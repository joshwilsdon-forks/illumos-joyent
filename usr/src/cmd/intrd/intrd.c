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
 * Copyright 2019 Joyent, Inc.
 */

#define __EXTENSIONS__

#include <err.h>
#include <errno.h>
#include <inttypes.h>
#include <kstat.h>
#include <libcmdutils.h>
#include <libcustr.h>
#include <limits.h>
#include <priv.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <syslog.h>
#include <unistd.h>
#include <umem.h>
#include <sys/debug.h>
#include <sys/errno.h>
#include <sys/kstat.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "intrd.h"

static int intrd_daemonize(void);
#if 0
static void intrd_dfatal(int, const char *, ...);
#endif

static void setup(kstat_ctl_t **restrict, config_t *restrict);
static void loop(const config_t *restrict, kstat_ctl_t *restrict);
static void delta_save(stats_t **, size_t, stats_t *, uint_t);

uint_t cfg_interval = 10;
uint_t cfg_retry_interval = 1;
uint_t cfg_idle_interval = 45;

uint_t max_cpu;

extern const char *__progname;

#ifdef DEBUG
const char *
_umem_debug_init(void)
{
	return ("default,verbose");
}

const char *
_umem_logging_init(void)
{
	return ("fail,contents");
}
#endif

static int
nomem(void)
{
	(void) fprintf(stderr, "Out of memory\n");
	return (UMEM_CALLBACK_EXIT(255));
}

static void __NORETURN
usage(void)
{
	(void) fprintf(stderr,
	    "Usage: %s [-d]\n"
	    "\t-d don't fork in the background\n",
	    __progname);
	exit(EXIT_FAILURE);
}

int
main(int argc, char **argv)
{
	kstat_ctl_t *kcp;
	config_t cfg = {
		.cfg_interval = 10,
		.cfg_idle_interval = 45,
		.cfg_retry_interval = 1,
		.cfg_avginterval = 60,
		.cfg_statslen = 120,
		.cfg_tooslow = 0.05
	};
	int c;
	boolean_t opt_d = B_FALSE;

	while ((c = getopt(argc, argv, "d")) != -1) {
		switch (c) {
		case 'd':
			opt_d = B_TRUE;
			break;
		case '?':
			(void) fprintf(stderr, "Unrecognized option -%c\n",
			    optopt);
			usage();
		}
	}

	int dfd, status;

	umem_nofail_callback(nomem);

	if (!opt_d)
		dfd = intrd_daemonize();

	setup(&kcp, &cfg);

	if (!opt_d) {
		status = 0;
		(void) write(dfd, &status, sizeof (status));
		(void) close(dfd);
	}

	loop(&cfg, kcp);

	(void) kstat_close(kcp);
	return (0);
}

static int
intrd_daemonize(void)
{
	sigset_t set, oset;
	int estatus, pfds[2];
	pid_t child;
	priv_set_t *pset;

	if (chdir("/") != 0)
		err(EXIT_FAILURE, "failed to chdir /");

	/*
	 * At this point, block all signals going in so we don't have the parent
	 * mistakingly exit when the child is running, but never block SIGABRT.
	 */
	if (sigfillset(&set) != 0)
		abort();
	if (sigdelset(&set, SIGABRT) != 0)
		abort();
	if (sigprocmask(SIG_BLOCK, &set, &oset) != 0)
		abort();

	/*
	 * Do the fork+setsid dance.
	 */
	if (pipe(pfds) != 0)
		err(EXIT_FAILURE, "failed to create pipe for daemonizing");

	if ((child = fork()) == -1)
		err(EXIT_FAILURE, "failed to fork for daemonizing");

	if (child != 0) {
		/* We'll be exiting shortly, so allow for silent failure */
		(void) close(pfds[1]);
		if (read(pfds[0], &estatus, sizeof (estatus)) ==
		    sizeof (estatus))
			_exit(estatus);

		if (waitpid(child, &estatus, 0) == child && WIFEXITED(estatus))
			_exit(WEXITSTATUS(estatus));

		_exit(EXIT_FAILURE);
	}

	/*
	 * Drop privileges.
	 * XXX: Should we run as nobody and maybe have SMF give us
	 * basic + PRIV_SYS_RES_CONFIG
	 */
	if (setgroups(0, NULL) != 0)
		abort();
	if ((pset = priv_allocset()) == NULL)
		abort();

	priv_basicset(pset);
	if (priv_delset(pset, PRIV_PROC_EXEC) == -1 ||
	    priv_delset(pset, PRIV_PROC_INFO) == -1 ||
	    priv_delset(pset, PRIV_PROC_FORK) == -1 ||
	    priv_delset(pset, PRIV_PROC_SESSION) == -1 ||
	    priv_delset(pset, PRIV_FILE_LINK_ANY) == -1 ||
	    priv_addset(pset, PRIV_SYS_RES_CONFIG) == -1) {
		abort();
	}

	if (setppriv(PRIV_SET, PRIV_PERMITTED, pset) == -1)
		abort();
	if (setppriv(PRIV_SET, PRIV_EFFECTIVE, pset) == -1)
		abort();

	priv_freeset(pset);

	if (close(pfds[0]) != 0)
		abort();
	if (setsid() == -1)
		abort();
	if (sigprocmask(SIG_SETMASK, &oset, NULL) != 0)
		abort();
	(void) umask(0022);

	return (pfds[1]);
}

static void
setup(kstat_ctl_t **restrict kcpp, config_t *restrict cfg)
{
	kstat_ctl_t *kcp;
	long val;

	if ((kcp = kstat_open()) == NULL)
		err(EXIT_FAILURE, "could not open /dev/kstat");
	*kcpp = kcp;

	if ((val = sysconf(_SC_NPROCESSORS_MAX)) == -1)
		err(EXIT_FAILURE, "sysconf(_SC_NPROCESSORS_MAX) failed");

	if (val > UINT32_MAX || val <= 0) {
		errx(EXIT_FAILURE, "max # of processors (%ld) of range "
		    "[1, %u]", val, UINT32_MAX);
	}
	max_cpu = (uint_t)val;

	/*
	 * This must happen after we've determined max_cpu so that
	 * stats_t->sts_cpu_byid can be sized correctly.
	 */
	intrd_kstat_init();

	// XXX: Initialize cfg
}

/*
 * Temporary functions to output data during development.  Maybe turn into
 * debug output later.
 */
static void
format_load(lgrp_id_t id, load_t *l, int indent)
{
	char buf[NN_NUMBUF_SZ];

	nanonicenum(l->ld_intrtotal, buf, sizeof (buf));

	(void) printf("%*sLGRP %2d %zu CPUs intr %6ss (%3.1f%%)",
	    indent * 2, "", id, l->ld_ncpu, buf, l->ld_avgload * 100.0);

	ivec_t *iv = l->ld_bigint;

	if (iv != NULL) {
		nanonicenum(iv->ivec_time, buf, sizeof (buf));
		(void) printf(" - %s int#%d %ss", custr_cstr(iv->ivec_name),
		    iv->ivec_ino, buf);
	}
	(void) fputc('\n', stdout);
}

static void
show_load(const cpugrp_t *grp, load_t *load, lgrp_id_t id, int indent)
{
	load_t *load_grp = LOAD_LGRP(load, id);
	const cpugrp_t *lgrp = &grp[id];

	format_load(id, load_grp, indent);
	for (size_t i = 0; i < lgrp->cg_nchildren; i++)
		show_load(grp, load, lgrp->cg_children[i], indent + 1);
}

static void
loop(const config_t *restrict cfg, kstat_ctl_t *restrict kcp)
{
	const size_t deltas_sz = cfg->cfg_avginterval / cfg->cfg_interval + 1;

	stats_t *stats[2] = { 0 };
	stats_t **deltas = NULL;
	stats_t *delta = NULL, *sum = NULL;
	size_t ndeltas = 0;
	uint_t interval = cfg->cfg_interval;
	int gen = 0;

	deltas = xcalloc(deltas_sz, sizeof (stats_t *));

	for (;; (void) sleep(interval)) {
		stats_free(stats[gen]);

		/*
		 * If there was a temporary error, retry sooner than a
		 * regular interval.
		 */
		if ((stats[gen] = stats_get(cfg, kcp, interval)) == NULL) {
			interval = cfg_retry_interval;
			continue;
		}

		interval = cfg_interval;

		delta = stats_delta(stats[gen], stats[gen ^ 1]);
		gen ^= 1;

		if (delta == NULL) {
			/*
			 * Something changed between the current and previous
			 * stat collection.  Try again later.
			 */
			continue;
		}
		delta_save(deltas, deltas_sz, delta, cfg->cfg_statslen);
		sum = stats_sum(deltas, deltas_sz, &ndeltas);

		stats_dump(sum);

		/*  XXX: temporary just to show the data */
		{
			stats_t *st = (sum != NULL) ? sum : delta;
			load_t *load = load_calc(st);

			show_load(st->sts_lgrp, load, 0, 0);
			load_free(load);
		}
	}
}

/*
 * Add newdelta to the front of deltas, and remove any entries in delta
 * from more than statslen seconds ago.
 */
static void
delta_save(stats_t **deltas, size_t n, stats_t *newdelta, uint_t statslen)
{
	hrtime_t cutoff, prevtime = INT64_MAX;
	size_t i, j;

	VERIFY3U(n, >, 1);

	cutoff = newdelta->sts_maxtime - (hrtime_t)statslen * NANOSEC;

	for (i = 0; i < n; i++) {
		if (deltas[i] == NULL)
			continue;

		/* These should be in order from newest to oldest */
		VERIFY3S(prevtime, >, deltas[i]->sts_mintime);
		prevtime = deltas[i]->sts_mintime;

		if (deltas[i]->sts_mintime >= cutoff)
			continue;

		for (j = i; i < n; i++) {
			stats_free(deltas[j]);
			deltas[j] = NULL;
		}
		break;
	}

	/* If all the slots are full, drop the last entry */
	if (i == n) {
		i = n - 1;
		stats_free(deltas[i]);
	}

	/* Move everything over one slot */
	(void) memmove(deltas + 1, deltas, i * sizeof (stats_t *));
	deltas[0] = newdelta;
}

/*
 * Like nicenum, but assumes the value is * 10^(-9) units
 */
void
nanonicenum(uint64_t val, char *buf, size_t buflen)
{
	static const char units[] = "num KMGTPE";
	static const size_t index_max = 9;
	uint64_t divisor = 1;
	int index = 0;
	char u;

	while (index < index_max) {
		uint64_t newdiv = divisor * 1024;

		if (val < newdiv)
			break;
		divisor = newdiv;
		index++;
	}
	u = units[index];

	if (val % divisor == 0) {
		(void) snprintf(buf, buflen, "%llu%c", val / divisor, u);
	} else {
		for (int i = 2; i >= 0; i--) {
			if (snprintf(buf, buflen, "%.*f%c", i,
			    (double)val / divisor, u) <= 5)
				return;
		}
	}
}

char *
xstrdup(const char *s)
{
	char *p = strdup(s);

	if (p == NULL)
		err(EXIT_FAILURE, "strdup failed");
	return (p);
}

void *
xcalloc(size_t nelem, size_t eltsize)
{
	void *p = calloc(nelem, eltsize);

	if (p == NULL && nelem > 0)
		err(EXIT_FAILURE, "calloc failed");
	return (p);
}

void *
xreallocarray(void *p, size_t n, size_t elsize)
{
	void *newp = reallocarray(p, n, elsize);

	if (newp == NULL && n > 0)
		err(EXIT_FAILURE, "reallocarray failed");
	return (newp);
}

#if 0
static void
intrd_dfatal(int dfd, const char *fmt, ...)
{
	int status = EXIT_FAILURE;
	va_list ap;

	va_start(ap, fmt);
	(void) vfprintf(stderr, fmt, ap);
	va_end(ap);

	(void) write(dfd, &status, sizeof (status));
	exit(status);
}
#endif
