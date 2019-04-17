/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 */

/*
 * Copyright 2015, Joyent, Inc.
 */

/*
 * The microfind() routine is used to calibrate the delay provided by
 * tenmicrosec().  Early in boot gethrtime() is not yet configured and
 * available for accurate delays, but some drivers still need to be able to
 * pause execution for rough increments of ten microseconds.  To that end,
 * microfind() will measure the wall time elapsed during a simple delay loop
 * using the Intel 8254 Programmable Interval Timer (PIT), and attempt to find
 * a loop count that approximates a ten microsecond delay.
 *
 * This mechanism is accurate enough when running unvirtualised on real CPUs,
 * but is somewhat less efficacious in a virtual machine.  In a virtualised
 * guest the relationship between instruction completion and elapsed wall time
 * is, at best, variable; on such machines the calibration is merely a rough
 * guess.
 */

#include <sys/types.h>
#include <sys/dl.h>
#include <sys/param.h>
#include <sys/pit.h>
#include <sys/inline.h>
#include <sys/machlock.h>
#include <sys/avintr.h>
#include <sys/smp_impldefs.h>
#include <sys/archsystm.h>
#include <sys/systm.h>
#include <sys/machsystm.h>
#include <sys/promif.h>

/*
 * Loop count for 10 microsecond wait.  MUST be initialized for those who
 * insist on calling "tenmicrosec" before the clock has been initialized.
 */
unsigned int microdata = 50;

/*
 * These values, used later in microfind(), are stored in globals to allow them
 * to be adjusted more easily via kmdb.
 */
unsigned int microdata_trial_count = 7;
unsigned int microdata_allowed_failures = 3;


static void
microfind_pit_reprogram_for_bios(void)
{
	/*
	 * Restore PIT counter 0 for BIOS use in mode 3 -- "Square Wave
	 * Generator".
	 */
	outb(PITCTL_PORT, PIT_C0 | PIT_LOADMODE | PIT_SQUAREMODE);

	/*
	 * Load an initial counter value of zero.
	 */
	outb(PITCTR0_PORT, 0);
	outb(PITCTR0_PORT, 0);
}

/*
 * Measure the run time of tenmicrosec() using the Intel 8254 Programmable
 * Interval Timer.  The timer operates at 1.193182 Mhz, so each timer tick
 * represents 0.8381 microseconds of wall time.  This function returns the
 * number of such ticks that passed while tenmicrosec() was running, or
 * -1 if the delay was too long to measure with the PIT.
 */
static int
microfind_pit_delta(boolean_t use_readback)
{
	unsigned char status;
	unsigned int count;

	/*
	 * Configure PIT counter 0 in mode 0 -- "Interrupt On Terminal Count".
	 * In this mode, the PIT will count down from the loaded value and
	 * set its output bit high once it reaches zero.  The PIT will pause
	 * until we write the low byte and then the high byte to the counter
	 * port.
	 */
	outb(PITCTL_PORT, PIT_LOADMODE);

	/*
	 * Load the maximum counter value, 0xffff, into the counter port.
	 */
	outb(PITCTR0_PORT, 0xff);
	outb(PITCTR0_PORT, 0xff);

	/*
	 * Run the delay function.
	 */
	tenmicrosec();

	if (use_readback) {
		/*
		 * Latch the counter value and status for counter 0 with the
		 * read back command.
		 */
		outb(PITCTL_PORT, PIT_READBACK | PIT_READBACKC0 | PIT_RB_NOCOUNT);
		outb(PITCTL_PORT, PIT_READBACK | PIT_READBACKC0 | PIT_RB_NOSTATUS);
	} else {
		/*
		 * Use the less reliable method of latching the counter
		 * value without reading the status byte.
		 */
		outb(PITCTL_PORT, PIT_LATCH | PIT_LATCHC0);
	}

	/*
	 * In read back mode, three values are read from the counter port in
	 * order: the status byte, followed by the low byte and high byte of
	 * the counter value.  In latch mode, the status byte is not available.
	 */
	if (use_readback) {
		status = inb(PITCTR0_PORT);
	}
	count = inb(PITCTR0_PORT);
	count |= inb(PITCTR0_PORT) << 8;

	if (!use_readback) {
		/*
		 * When not using the read back command, the status byte is not
		 * available to us; we have to assume the counter value is
		 * useful.
		 */
		if (count >= 0xffff || count == 0) {
			prom_printf("microfind: latch: invalid count %x\n", count);
			return (-1);
		}
		goto out;
	}

	/*
	 * Verify that the counter started counting down.  The null count
	 * flag in the status byte is set when we load a value, and cleared
	 * when counting operation begins.
	 */
	if (status & (1 << PITSTAT_NULLCNT)) {
		/*
		 * The counter did not begin.  This means the loop count
		 * used by tenmicrosec is too small for this CPU.  We return
		 * a zero count to represent that the delay was too small
		 * to measure.
		 */
		prom_printf("microfind: did not begin (status %x count %x)\n", (int)status, count);
		return (0);
	}

	/*
	 * Verify that the counter did not wrap around.  The output pin is
	 * reset when we load a new counter value, and set once the counter
	 * reaches zero.
	 */
	if (status & (1 << PITSTAT_OUTPUT)) {
		/*
		 * The counter reached zero before we were able to read the
		 * value.  This means the loop count used by tenmicrosec is too
		 * large for this CPU.
		 */
		prom_printf("microfind: zero too fast (status %x count %x)\n", (int)status, count);
		return (-1);
	}

	/*
	 * The PIT counts from our initial load value of 0xffff down to zero.
	 * Return the number of timer ticks that passed while tenmicrosec was
	 * running.
	 */
out:
	VERIFY3U(count, <=, 0xffff);
	return (0xffff - count);
}

static int
microfind_pit_delta_avg(boolean_t readback, int trials, int allowed_failures)
{
	int tc = 0;
	int failures = 0;
	long long int total = 0;

	while (tc < trials) {
		int d;

		if ((d = microfind_pit_delta(readback)) < 0) {
			/*
			 * If the counter wrapped, we cannot use this
			 * data point in the average.  Record the failure
			 * and try again.
			 */
			if (++failures > allowed_failures) {
				/*
				 * Too many failures.
				 */
				return (-1);
			}
			continue;
		}

		total += d;
		tc++;
	}

	return (total / tc);
}

void
microfind(void)
{
	boolean_t use_readback = B_TRUE;
	int ticks;
	ulong_t s;

	prom_printf("microfind: starting\n");

	/*
	 * Disable interrupts while we measure the speed of the CPU.
	 */
	s = clear_int_flag();

	/*
	 * Start at the smallest loop count, i.e. 1, and keep doubling
	 * until a delay of ~10ms can be measured.
	 */
again:
	ticks = -1;
	microdata = 1;
	for (;;) {
		int ticksprev = ticks;

		prom_printf("microfind: loop microdata %d ticks %d\n",
		    microdata, ticks);

		/*
		 * We use a trial count of 7 to attempt to smooth out jitter
		 * caused by the scheduling of virtual machines.  We only allow
		 * three failures, as each failure represents a wrapped counter
		 * and an expired wall time of at least ~55ms.
		 */
		if ((ticks = microfind_pit_delta_avg(use_readback,
		    microdata_trial_count, microdata_allowed_failures)) < 0) {
			if (use_readback) {
				/*
				 * In case this is a system with a PIT that
				 * does not correctly implement the read back
				 * command, try again with the more pedestrian
				 * counter latch command.
				 */
				prom_printf("microfind: try again w/ latch\n");
				use_readback = B_FALSE;
				goto again;
			}

			/*
			 * The counter wrapped.  Halve the counter, restore the
			 * previous ticks count and break out of the loop.
			 */
			if (microdata <= 1) {
				/*
				 * If the counter wrapped on the first try,
				 * then we have some serious problems.
				 */
				panic("microfind: pit counter always wrapped");
			}
			microdata = microdata >> 1;
			ticks = ticksprev;
			break;
		}

		if (ticks > 0x3000) {
			/*
			 * The loop ran for at least ~10ms worth of 0.8381us
			 * PIT ticks.
			 */
			break;
		} else if (microdata > (UINT_MAX >> 1)) {
			/*
			 * Doubling the loop count again would cause an
			 * overflow.  Use what we have.
			 */
			break;
		} else {
			/*
			 * Double and try again.
			 */
			microdata = microdata << 1;
		}
	}

	prom_printf("microfind: after loop microdata %d ticks %d\n",
	    microdata, ticks);

	if (ticks < 1) {
		/*
		 * If we were unable to measure a positive PIT tick count, then
		 * we will be unable to scale the value of "microdata"
		 * correctly.
		 */
		panic("microfind: could not calibrate delay loop");
	}

	/*
	 * Calculate the loop count based on the final PIT tick count and the
	 * loop count.  Each PIT tick represents a duration of ~0.8381us, so we
	 * want to adjust microdata to represent a duration of 12 ticks, or
	 * ~10us.
	 */
	microdata = (long long)microdata * 12LL / (long long)ticks;

	prom_printf("microfind: final microdata value %d\n", microdata);

	/*
	 * Try and leave things as we found them.
	 */
	microfind_pit_reprogram_for_bios();

	/*
	 * Restore previous interrupt state.
	 */
	restore_int_flag(s);
}

static int
microfind_pit_delta_tsc(boolean_t readback, uint64_t *tscs)
{
	int d;
	hrtime_t start, end;

	start = tsc_read();
	if ((d = microfind_pit_delta(readback)) < 0) {
		/*
		 * If the counter wrapped, we cannot use this
		 * data point in the average.
		 * and try again.
		 */
		return (-1);
	}
	end = tsc_read();

	*tscs = end - start;
	return (d);
}


uint64_t
microfind_freq_tsc(uint32_t *pit_counter)
{
	int ticks;
	ulong_t s;
	uint64_t tscs = 0;
	unsigned int save_microdata = microdata; /* XXX */

	prom_printf("microfind_freq_tsc: starting\n");

	/*
	 * Start at the smallest loop count, i.e. 1, and keep doubling
	 * until a delay of ~10ms can be measured.
	 */
again:
	ticks = -1;
	microdata = 1;
	for (;;) {
		int ticksprev = ticks;

		prom_printf("microfind_freq_tsc: loop microdata %d ticks %d\n",
		    microdata, ticks);

		/*
		 * We use a trial count of 7 to attempt to smooth out jitter
		 * caused by the scheduling of virtual machines.  We only allow
		 * three failures, as each failure represents a wrapped counter
		 * and an expired wall time of at least ~55ms.
		 */
		if ((ticks = microfind_pit_delta_tsc(B_FALSE, &tscs)) < 0) {
			/*
			 * The counter wrapped.  Halve the counter, restore the
			 * previous ticks count and break out of the loop.
			 */
			if (microdata <= 1) {
				/*
				 * If the counter wrapped on the first try,
				 * then we have some serious problems.
				 */
				panic("microfind_freq_tsc: pit counter always wrapped");
			}
			microdata = microdata >> 1;
			ticks = ticksprev;
			break;
		}

		if (ticks > 0x3000) {
			/*
			 * The loop ran for at least ~10ms worth of 0.8381us
			 * PIT ticks.
			 */
			break;
		} else if (microdata > (UINT_MAX >> 1)) {
			/*
			 * Doubling the loop count again would cause an
			 * overflow.  Use what we have.
			 */
			break;
		} else {
			/*
			 * Double and try again.
			 */
			microdata = microdata << 1;
		}
	}

	prom_printf("microfind_freq_tsc: after loop microdata %d ticks %d tscs %llu\n",
	    microdata, ticks, (long long unsigned)tscs);

	if (ticks < 1) {
		/*
		 * If we were unable to measure a positive PIT tick count, then
		 * we will be unable to scale the value of "microdata"
		 * correctly.
		 */
		panic("microfind_freq_tsc: could not calibrate delay loop");
	}

	microdata = save_microdata;

	/*
	 * Try and leave things as we found them.
	 */
	microfind_pit_reprogram_for_bios();

	*pit_counter = ticks;
	return (tscs);
}
