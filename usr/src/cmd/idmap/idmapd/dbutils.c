/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2007 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#pragma ident	"%Z%%M%	%I%	%E% SMI"

/*
 * Database related utility routines
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <rpc/rpc.h>
#include <sys/sid.h>
#include <time.h>
#include <pwd.h>
#include <grp.h>
#include <pthread.h>
#include <assert.h>

#include "idmapd.h"
#include "adutils.h"
#include "string.h"
#include "idmap_priv.h"


static idmap_retcode sql_compile_n_step_once(sqlite *, char *,
		sqlite_vm **, int *, int, const char ***);
static idmap_retcode lookup_wksids_name2sid(const char *, char **,
		idmap_rid_t *, int *);

#define	EMPTY_NAME(name)	(*name == 0 || strcmp(name, "\"\"") == 0)

#define	EMPTY_STRING(str)	(str == NULL || *str == 0)

#define	DO_NOT_ALLOC_NEW_ID_MAPPING(req)\
		(req->flag & IDMAP_REQ_FLG_NO_NEW_ID_ALLOC)

#define	AVOID_NAMESERVICE(req)\
		(req->flag & IDMAP_REQ_FLG_NO_NAMESERVICE)

#define	IS_EPHEMERAL(pid)	(pid > INT32_MAX)

#define	LOCALRID_MIN	1000



typedef enum init_db_option {
	FAIL_IF_CORRUPT = 0,
	REMOVE_IF_CORRUPT = 1
} init_db_option_t;

/*
 * Thread specfic data to hold the database handles so that the
 * databaes are not opened and closed for every request. It also
 * contains the sqlite busy handler structure.
 */

struct idmap_busy {
	const char *name;
	const int *delays;
	int delay_size;
	int total;
	int sec;
};


typedef struct idmap_tsd {
	sqlite *db_db;
	sqlite *cache_db;
	struct idmap_busy cache_busy;
	struct idmap_busy db_busy;
} idmap_tsd_t;



static const int cache_delay_table[] =
		{ 1, 2, 5, 10, 15, 20, 25, 30,  35,  40,
		50,  50, 60, 70, 80, 90, 100};

static const int db_delay_table[] =
		{ 5, 10, 15, 20, 30,  40,  55,  70, 100};


static pthread_key_t	idmap_tsd_key;

void
idmap_tsd_destroy(void *key)
{

	idmap_tsd_t	*tsd = (idmap_tsd_t *)key;
	if (tsd) {
		if (tsd->db_db)
			(void) sqlite_close(tsd->db_db);
		if (tsd->cache_db)
			(void) sqlite_close(tsd->cache_db);
		free(tsd);
	}
}

int
idmap_init_tsd_key(void) {

	return (pthread_key_create(&idmap_tsd_key, idmap_tsd_destroy));
}



idmap_tsd_t *
idmap_get_tsd(void)
{
	idmap_tsd_t	*tsd;

	if ((tsd = pthread_getspecific(idmap_tsd_key)) == NULL) {
		/* No thread specific data so create it */
		if ((tsd = malloc(sizeof (*tsd))) != NULL) {
			/* Initialize thread specific data */
			(void) memset(tsd, 0, sizeof (*tsd));
			/* save the trhread specific data */
			if (pthread_setspecific(idmap_tsd_key, tsd) != 0) {
				/* Can't store key */
				free(tsd);
				tsd = NULL;
			}
		} else {
			tsd = NULL;
		}
	}

	return (tsd);
}



/*
 * Initialize 'dbname' using 'sql'
 */
static int
init_db_instance(const char *dbname, const char *sql, init_db_option_t opt,
		int *new_db_created)
{
	int rc = 0;
	int tries = 0;
	sqlite *db = NULL;
	char *str = NULL;

	if (new_db_created != NULL)
		*new_db_created = 0;

	db = sqlite_open(dbname, 0600, &str);
	while (db == NULL) {
		idmapdlog(LOG_ERR,
		    "Error creating database %s (%s)",
		    dbname, CHECK_NULL(str));
		sqlite_freemem(str);
		if (opt == FAIL_IF_CORRUPT || opt != REMOVE_IF_CORRUPT ||
		    tries > 0)
			return (-1);

		tries++;
		(void) unlink(dbname);
		db = sqlite_open(dbname, 0600, &str);
	}

	sqlite_busy_timeout(db, 3000);
	rc = sqlite_exec(db, "BEGIN TRANSACTION;", NULL, NULL, &str);
	if (SQLITE_OK != rc) {
		idmapdlog(LOG_ERR, "Cannot begin database transaction (%s)",
		    str);
		sqlite_freemem(str);
		sqlite_close(db);
		return (1);
	}

	switch (sqlite_exec(db, sql, NULL, NULL, &str)) {
	case SQLITE_ERROR:
/*
 * This is the normal situation: CREATE probably failed because tables
 * already exist. It may indicate an error in SQL as well, but we cannot
 * tell.
 */
		sqlite_freemem(str);
		rc =  sqlite_exec(db, "ROLLBACK TRANSACTION",
		    NULL, NULL, &str);
		break;
	case SQLITE_OK:
		rc =  sqlite_exec(db, "COMMIT TRANSACTION",
		    NULL, NULL, &str);
		idmapdlog(LOG_INFO,
		    "Database created at %s", dbname);

		if (new_db_created != NULL)
			*new_db_created = 1;
		break;
	default:
		idmapdlog(LOG_ERR,
		    "Error initializing database %s (%s)",
		    dbname, str);
		sqlite_freemem(str);
		rc =  sqlite_exec(db, "ROLLBACK TRANSACTION",
		    NULL, NULL, &str);
		break;
	}

	if (SQLITE_OK != rc) {
		/* this is bad - database may be left in a locked state */
		idmapdlog(LOG_ERR,
		    "Error closing transaction (%s)", str);
		sqlite_freemem(str);
	}

	(void) sqlite_close(db);
	return (rc);
}


/*
 * This is the SQLite database busy handler that retries the SQL
 * operation until it is successful.
 */
int
/* LINTED E_FUNC_ARG_UNUSED */
idmap_sqlite_busy_handler(void *arg, const char *table_name, int count)
{
	struct idmap_busy	*busy = arg;
	int			delay;
	struct timespec		rqtp;

	if (count == 1)  {
		busy->total = 0;
		busy->sec = 2;
	}
	if (busy->total > 1000 * busy->sec) {
		idmapdlog(LOG_ERR,
		    "Thread %d waited %d sec for the %s database",
		    pthread_self(), busy->sec, busy->name);
		busy->sec++;
	}

	if (count <= busy->delay_size) {
		delay = busy->delays[count-1];
	} else {
		delay = busy->delays[busy->delay_size - 1];
	}
	busy->total += delay;
	rqtp.tv_sec = 0;
	rqtp.tv_nsec = delay * (NANOSEC / MILLISEC);
	(void) nanosleep(&rqtp, NULL);
	return (1);
}


/*
 * Get the database handle
 */
idmap_retcode
get_db_handle(sqlite **db) {
	char	*errmsg;
	idmap_tsd_t *tsd;

	/*
	 * Retrieve the db handle from thread-specific storage
	 * If none exists, open and store in thread-specific storage.
	 */
	if ((tsd = idmap_get_tsd()) == NULL) {
		idmapdlog(LOG_ERR,
			"Error getting thread specific data for %s",
			IDMAP_DBNAME);
		return (IDMAP_ERR_MEMORY);
	}

	if (tsd->db_db == NULL) {
		tsd->db_db = sqlite_open(IDMAP_DBNAME, 0, &errmsg);
		if (tsd->db_db == NULL) {
			idmapdlog(LOG_ERR,
				"Error opening database %s (%s)",
				IDMAP_DBNAME, CHECK_NULL(errmsg));
			sqlite_freemem(errmsg);
			return (IDMAP_ERR_INTERNAL);
		}
		tsd->db_busy.name = IDMAP_DBNAME;
		tsd->db_busy.delays = db_delay_table;
		tsd->db_busy.delay_size = sizeof (db_delay_table) /
		    sizeof (int);
		sqlite_busy_handler(tsd->db_db, idmap_sqlite_busy_handler,
		    &tsd->db_busy);
	}
	*db = tsd->db_db;
	return (IDMAP_SUCCESS);
}

/*
 * Get the cache handle
 */
idmap_retcode
get_cache_handle(sqlite **cache) {
	char	*errmsg;
	idmap_tsd_t *tsd;

	/*
	 * Retrieve the db handle from thread-specific storage
	 * If none exists, open and store in thread-specific storage.
	 */
	if ((tsd = idmap_get_tsd()) == NULL) {
		idmapdlog(LOG_ERR,
			"Error getting thread specific data for %s",
			IDMAP_DBNAME);
		return (IDMAP_ERR_MEMORY);
	}

	if (tsd->cache_db == NULL) {
		tsd->cache_db = sqlite_open(IDMAP_CACHENAME, 0, &errmsg);
		if (tsd->cache_db == NULL) {
			idmapdlog(LOG_ERR,
				"Error opening database %s (%s)",
				IDMAP_CACHENAME, CHECK_NULL(errmsg));
			sqlite_freemem(errmsg);
			return (IDMAP_ERR_INTERNAL);
		}
		tsd->cache_busy.name = IDMAP_CACHENAME;
		tsd->cache_busy.delays = cache_delay_table;
		tsd->cache_busy.delay_size = sizeof (cache_delay_table) /
		    sizeof (int);
		sqlite_busy_handler(tsd->cache_db, idmap_sqlite_busy_handler,
		    &tsd->cache_busy);
	}
	*cache = tsd->cache_db;
	return (IDMAP_SUCCESS);
}

#define	CACHE_SQL\
	"CREATE TABLE idmap_cache ("\
	"	sidprefix TEXT,"\
	"	rid INTEGER,"\
	"	windomain TEXT,"\
	"	winname TEXT,"\
	"	pid INTEGER,"\
	"	unixname TEXT,"\
	"	is_user INTEGER,"\
	"	w2u INTEGER,"\
	"	u2w INTEGER,"\
	"	expiration INTEGER"\
	");"\
	"CREATE UNIQUE INDEX idmap_cache_sid_w2u ON idmap_cache"\
	"		(sidprefix, rid, w2u);"\
	"CREATE UNIQUE INDEX idmap_cache_pid_u2w ON idmap_cache"\
	"		(pid, is_user, u2w);"\
	"CREATE TABLE name_cache ("\
	"	sidprefix TEXT,"\
	"	rid INTEGER,"\
	"	name TEXT,"\
	"	domain TEXT,"\
	"	type INTEGER,"\
	"	expiration INTEGER"\
	");"\
	"CREATE UNIQUE INDEX name_cache_sid ON name_cache"\
	"		(sidprefix, rid);"

#define	DB_SQL\
	"CREATE TABLE namerules ("\
	"	is_user INTEGER NOT NULL,"\
	"	windomain TEXT,"\
	"	winname TEXT NOT NULL,"\
	"	is_nt4 INTEGER NOT NULL,"\
	"	unixname NOT NULL,"\
	"	w2u_order INTEGER,"\
	"	u2w_order INTEGER"\
	");"\
	"CREATE UNIQUE INDEX namerules_w2u ON namerules"\
	"		(winname, windomain, is_user, w2u_order);"\
	"CREATE UNIQUE INDEX namerules_u2w ON namerules"\
	"		(unixname, is_user, u2w_order);"

/*
 * Initialize cache and db
 */
int
init_dbs() {
	/* name-based mappings; probably OK to blow away in a pinch(?) */
	if (init_db_instance(IDMAP_DBNAME, DB_SQL, FAIL_IF_CORRUPT, NULL) < 0)
		return (-1);

	/* mappings, name/SID lookup cache + ephemeral IDs; OK to blow away */
	if (init_db_instance(IDMAP_CACHENAME, CACHE_SQL, REMOVE_IF_CORRUPT,
			&_idmapdstate.new_eph_db) < 0)
		return (-1);

	return (0);
}

/*
 * Finalize databases
 */
void
fini_dbs() {
}

/*
 * This table is a listing of status codes that will returned to the
 * client when a SQL command fails with the corresponding error message.
 */
static msg_table_t sqlmsgtable[] = {
	{IDMAP_ERR_U2W_NAMERULE_CONFLICT,
	"columns unixname, is_user, u2w_order are not unique"},
	{IDMAP_ERR_W2U_NAMERULE_CONFLICT,
	"columns winname, windomain, is_user, w2u_order are not unique"},
	{-1, NULL}
};

/*
 * idmapd's version of string2stat to map SQLite messages to
 * status codes
 */
idmap_retcode
idmapd_string2stat(const char *msg) {
	int i;
	for (i = 0; sqlmsgtable[i].msg; i++) {
		if (strcasecmp(sqlmsgtable[i].msg, msg) == 0)
			return (sqlmsgtable[i].retcode);
	}
	return (IDMAP_ERR_OTHER);
}

/*
 * Execute the given SQL statment without using any callbacks
 */
idmap_retcode
sql_exec_no_cb(sqlite *db, char *sql) {
	char		*errmsg = NULL;
	int		r;
	idmap_retcode	retcode;

	r = sqlite_exec(db, sql, NULL, NULL, &errmsg);
	assert(r != SQLITE_LOCKED && r != SQLITE_BUSY);

	if (r != SQLITE_OK) {
		idmapdlog(LOG_ERR, "Database error during %s (%s)",
			sql, CHECK_NULL(errmsg));
		retcode = idmapd_string2stat(errmsg);
		if (errmsg != NULL)
			sqlite_freemem(errmsg);
		return (retcode);
	}

	return (IDMAP_SUCCESS);
}

/*
 * Generate expression that can be used in WHERE statements.
 * Examples:
 * <prefix> <col>      <op> <value>   <suffix>
 * ""       "unixuser" "="  "foo" "AND"
 */
idmap_retcode
gen_sql_expr_from_utf8str(const char *prefix, const char *col,
		const char *op, idmap_utf8str *value,
		const char *suffix, char **out) {
	char		*str;
	idmap_stat	retcode;

	if (out == NULL)
		return (IDMAP_ERR_ARG);

	if (value == NULL)
		return (IDMAP_SUCCESS);

	retcode = idmap_utf82str(&str, 0, value);
	if (retcode != IDMAP_SUCCESS)
		return (retcode);

	if (prefix == NULL)
		prefix = "";
	if (suffix == NULL)
		suffix = "";

	*out = sqlite_mprintf("%s %s %s %Q %s",
			prefix, col, op, str, suffix);
	idmap_free(str);
	if (*out == NULL)
		return (IDMAP_ERR_MEMORY);
	return (IDMAP_SUCCESS);
}

/*
 * Generate and execute SQL statement for LIST RPC calls
 */
idmap_retcode
process_list_svc_sql(sqlite *db, char *sql, uint64_t limit,
		list_svc_cb cb, void *result) {
	list_cb_data_t	cb_data;
	char		*errmsg = NULL;
	int		r;
	idmap_retcode	retcode = IDMAP_ERR_INTERNAL;

	(void) memset(&cb_data, 0, sizeof (cb_data));
	cb_data.result = result;
	cb_data.limit = limit;


	r = sqlite_exec(db, sql, cb, &cb_data, &errmsg);
	assert(r != SQLITE_LOCKED && r != SQLITE_BUSY);
	switch (r) {
	case SQLITE_OK:
		retcode = IDMAP_SUCCESS;
		break;

	default:
		retcode = IDMAP_ERR_INTERNAL;
		idmapdlog(LOG_ERR,
			"Database error during %s (%s)",
			sql, CHECK_NULL(errmsg));
		break;
	}
	if (errmsg != NULL)
		sqlite_freemem(errmsg);
	return (retcode);
}

/*
 * This routine is called by callbacks that process the results of
 * LIST RPC calls to validate data and to allocate memory for
 * the result array.
 */
idmap_retcode
validate_list_cb_data(list_cb_data_t *cb_data, int argc, char **argv,
		int ncol, uchar_t **list, size_t valsize) {
	size_t	nsize;
	void	*tmplist;

	if (cb_data->limit > 0 && cb_data->next == cb_data->limit)
		return (IDMAP_NEXT);

	if (argc < ncol || argv == NULL) {
		idmapdlog(LOG_ERR, "Invalid data");
		return (IDMAP_ERR_INTERNAL);
	}

	/* alloc in bulk to reduce number of reallocs */
	if (cb_data->next >= cb_data->len) {
		nsize = (cb_data->len + SIZE_INCR) * valsize;
		tmplist = realloc(*list, nsize);
		if (tmplist == NULL) {
			idmapdlog(LOG_ERR, "Out of memory");
			return (IDMAP_ERR_MEMORY);
		}
		*list = tmplist;
		(void) memset(*list + (cb_data->len * valsize), 0,
			SIZE_INCR * valsize);
		cb_data->len += SIZE_INCR;
	}
	return (IDMAP_SUCCESS);
}

static idmap_retcode
get_namerule_order(char *winname, char *windomain, char *unixname,
		int direction, int *w2u_order, int *u2w_order) {

	*w2u_order = 0;
	*u2w_order = 0;

	/*
	 * Windows to UNIX lookup order:
	 *  1. winname@domain (or winname) to ""
	 *  2. winname@domain (or winname) to unixname
	 *  3. winname@* to ""
	 *  4. winname@* to unixname
	 *  5. *@domain (or *) to *
	 *  6. *@domain (or *) to ""
	 *  7. *@domain (or *) to unixname
	 *  8. *@* to *
	 *  9. *@* to ""
	 * 10. *@* to unixname
	 *
	 * winname is a special case of winname@domain when domain is the
	 * default domain. Similarly * is a special case of *@domain when
	 * domain is the default domain.
	 *
	 * Note that "" has priority over specific names because "" inhibits
	 * mappings and traditionally deny rules always had higher priority.
	 */
	if (direction != IDMAP_DIRECTION_U2W) {
		/* bi-directional or from windows to unix */
		if (winname == NULL)
			return (IDMAP_ERR_W2U_NAMERULE);
		else if (unixname == NULL)
			return (IDMAP_ERR_W2U_NAMERULE);
		else if (EMPTY_NAME(winname))
			return (IDMAP_ERR_W2U_NAMERULE);
		else if (*winname == '*' && windomain && *windomain == '*') {
			if (*unixname == '*')
				*w2u_order = 8;
			else if (EMPTY_NAME(unixname))
				*w2u_order = 9;
			else /* unixname == name */
				*w2u_order = 10;
		} else if (*winname == '*') {
			if (*unixname == '*')
				*w2u_order = 5;
			else if (EMPTY_NAME(unixname))
				*w2u_order = 6;
			else /* name */
				*w2u_order = 7;
		} else if (windomain != NULL && *windomain == '*') {
			/* winname == name */
			if (*unixname == '*')
				return (IDMAP_ERR_W2U_NAMERULE);
			else if (EMPTY_NAME(unixname))
				*w2u_order = 3;
			else /* name */
				*w2u_order = 4;
		} else  {
			/* winname == name && windomain == null or name */
			if (*unixname == '*')
				return (IDMAP_ERR_W2U_NAMERULE);
			else if (EMPTY_NAME(unixname))
				*w2u_order = 1;
			else /* name */
				*w2u_order = 2;
		}
	}

	/*
	 * 1. unixname to ""
	 * 2. unixname to winname@domain (or winname)
	 * 3. * to *@domain (or *)
	 * 4. * to ""
	 * 5. * to winname@domain (or winname)
	 */
	if (direction != IDMAP_DIRECTION_W2U) {
		/* bi-directional or from unix to windows */
		if (unixname == NULL || EMPTY_NAME(unixname))
			return (IDMAP_ERR_U2W_NAMERULE);
		else if (winname == NULL)
			return (IDMAP_ERR_U2W_NAMERULE);
		else if (windomain != NULL && *windomain == '*')
			return (IDMAP_ERR_U2W_NAMERULE);
		else if (*unixname == '*') {
			if (*winname == '*')
				*u2w_order = 3;
			else if (EMPTY_NAME(winname))
				*u2w_order = 4;
			else
				*u2w_order = 5;
		} else {
			if (*winname == '*')
				return (IDMAP_ERR_U2W_NAMERULE);
			else if (EMPTY_NAME(winname))
				*u2w_order = 1;
			else
				*u2w_order = 2;
		}
	}
	return (IDMAP_SUCCESS);
}

/*
 * Generate and execute SQL statement to add name-based mapping rule
 */
idmap_retcode
add_namerule(sqlite *db, idmap_namerule *rule) {
	char		*sql = NULL;
	idmap_stat	retcode;
	char		*windomain = NULL, *winname = NULL, *dom = NULL;
	char		*unixname = NULL;
	int		w2u_order, u2w_order;
	char		w2ubuf[11], u2wbuf[11];

	retcode = idmap_utf82str(&windomain, 0, &rule->windomain);
	if (retcode != IDMAP_SUCCESS)
		goto out;
	retcode = idmap_utf82str(&winname, 0, &rule->winname);
	if (retcode != IDMAP_SUCCESS)
		goto out;
	retcode = idmap_utf82str(&unixname, 0, &rule->unixname);
	if (retcode != IDMAP_SUCCESS)
		goto out;

	retcode = get_namerule_order(winname, windomain, unixname,
			rule->direction, &w2u_order, &u2w_order);
	if (retcode != IDMAP_SUCCESS)
		goto out;

	if (w2u_order)
		(void) snprintf(w2ubuf, sizeof (w2ubuf), "%d", w2u_order);
	if (u2w_order)
		(void) snprintf(u2wbuf, sizeof (u2wbuf), "%d", u2w_order);

	/*
	 * For the triggers on namerules table to work correctly:
	 * 1) Use NULL instead of 0 for w2u_order and u2w_order
	 * 2) Use "" instead of NULL for "no domain"
	 */

	if (windomain != NULL)
		dom = windomain;
	else if (lookup_wksids_name2sid(winname, NULL, NULL, NULL)
	    == IDMAP_SUCCESS) {
		/* well-known SIDs don't need domain */
		dom = "";
	}

	RDLOCK_CONFIG();
	if (dom == NULL) {
		if (_idmapdstate.cfg->pgcfg.mapping_domain)
			dom = _idmapdstate.cfg->pgcfg.mapping_domain;
		else
			dom = "";
	}
	sql = sqlite_mprintf("INSERT into namerules "
		"(is_user, windomain, winname, is_nt4, "
		"unixname, w2u_order, u2w_order) "
		"VALUES(%d, %Q, %Q, %d, %Q, %q, %q);",
		rule->is_user?1:0,
		dom,
		winname, rule->is_nt4?1:0,
		unixname,
		w2u_order?w2ubuf:NULL,
		u2w_order?u2wbuf:NULL);
	UNLOCK_CONFIG();

	if (sql == NULL) {
		retcode = IDMAP_ERR_INTERNAL;
		idmapdlog(LOG_ERR, "Out of memory");
		goto out;
	}

	retcode = sql_exec_no_cb(db, sql);

	if (retcode == IDMAP_ERR_OTHER)
		retcode = IDMAP_ERR_CFG;

out:
	if (windomain != NULL)
		idmap_free(windomain);
	if (winname != NULL)
		idmap_free(winname);
	if (unixname != NULL)
		idmap_free(unixname);
	if (sql != NULL)
		sqlite_freemem(sql);
	return (retcode);
}

/*
 * Flush name-based mapping rules
 */
idmap_retcode
flush_namerules(sqlite *db, bool_t is_user) {
	char		*sql = NULL;
	idmap_stat	retcode;

	sql = sqlite_mprintf("DELETE FROM namerules WHERE "
		"is_user = %d;", is_user?1:0);

	if (sql == NULL) {
		idmapdlog(LOG_ERR, "Out of memory");
		return (IDMAP_ERR_MEMORY);
	}

	retcode = sql_exec_no_cb(db, sql);

	sqlite_freemem(sql);
	return (retcode);
}

/*
 * Generate and execute SQL statement to remove a name-based mapping rule
 */
idmap_retcode
rm_namerule(sqlite *db, idmap_namerule *rule) {
	char		*sql = NULL;
	idmap_stat	retcode;
	char		*s_windomain = NULL, *s_winname = NULL;
	char		*s_unixname = NULL;
	char		buf[80];

	if (rule->direction < 0 &&
			rule->windomain.idmap_utf8str_len < 1 &&
			rule->winname.idmap_utf8str_len < 1 &&
			rule->unixname.idmap_utf8str_len < 1)
		return (IDMAP_SUCCESS);

	if (rule->direction < 0) {
		buf[0] = 0;
	} else if (rule->direction == IDMAP_DIRECTION_BI) {
		(void) snprintf(buf, sizeof (buf), "AND w2u_order > 0"
				" AND u2w_order > 0");
	} else if (rule->direction == IDMAP_DIRECTION_W2U) {
		(void) snprintf(buf, sizeof (buf), "AND w2u_order > 0"
				" AND (u2w_order = 0 OR u2w_order ISNULL)");
	} else if (rule->direction == IDMAP_DIRECTION_U2W) {
		(void) snprintf(buf, sizeof (buf), "AND u2w_order > 0"
				" AND (w2u_order = 0 OR w2u_order ISNULL)");
	}

	retcode = IDMAP_ERR_INTERNAL;
	if (rule->windomain.idmap_utf8str_len > 0) {
		if (gen_sql_expr_from_utf8str("AND", "windomain", "=",
				&rule->windomain,
				"", &s_windomain) != IDMAP_SUCCESS)
			goto out;
	}

	if (rule->winname.idmap_utf8str_len > 0) {
		if (gen_sql_expr_from_utf8str("AND", "winname", "=",
				&rule->winname,
				"", &s_winname) != IDMAP_SUCCESS)
			goto out;
	}

	if (rule->unixname.idmap_utf8str_len > 0) {
		if (gen_sql_expr_from_utf8str("AND", "unixname", "=",
				&rule->unixname,
				"", &s_unixname) != IDMAP_SUCCESS)
			goto out;
	}

	sql = sqlite_mprintf("DELETE FROM namerules WHERE "
		"is_user = %d %s %s %s %s;",
		rule->is_user?1:0,
		s_windomain?s_windomain:"",
		s_winname?s_winname:"",
		s_unixname?s_unixname:"",
		buf);

	if (sql == NULL) {
		retcode = IDMAP_ERR_INTERNAL;
		idmapdlog(LOG_ERR, "Out of memory");
		goto out;
	}

	retcode = sql_exec_no_cb(db, sql);

out:
	if (s_windomain != NULL)
		sqlite_freemem(s_windomain);
	if (s_winname != NULL)
		sqlite_freemem(s_winname);
	if (s_unixname != NULL)
		sqlite_freemem(s_unixname);
	if (sql != NULL)
		sqlite_freemem(sql);
	return (retcode);
}

/*
 * Compile the given SQL query and step just once.
 *
 * Input:
 * db  - db handle
 * sql - SQL statement
 *
 * Output:
 * vm     -  virtual SQL machine
 * ncol   - number of columns in the result
 * values - column values
 *
 * Return values:
 * IDMAP_SUCCESS
 * IDMAP_ERR_NOTFOUND
 * IDMAP_ERR_INTERNAL
 */

static idmap_retcode
sql_compile_n_step_once(sqlite *db, char *sql, sqlite_vm **vm, int *ncol,
		int reqcol, const char ***values) {
	char		*errmsg = NULL;
	int		r;

	if ((r = sqlite_compile(db, sql, NULL, vm, &errmsg)) != SQLITE_OK) {
		idmapdlog(LOG_ERR,
			"Database error during %s (%s)",
			sql, CHECK_NULL(errmsg));
		sqlite_freemem(errmsg);
		return (IDMAP_ERR_INTERNAL);
	}

	r = sqlite_step(*vm, ncol, values, NULL);
	assert(r != SQLITE_LOCKED && r != SQLITE_BUSY);

	if (r == SQLITE_ROW) {
		if (ncol != NULL && *ncol < reqcol) {
			(void) sqlite_finalize(*vm, NULL);
			*vm = NULL;
			return (IDMAP_ERR_INTERNAL);
		}
		/* Caller will call finalize after using the results */
		return (IDMAP_SUCCESS);
	} else if (r == SQLITE_DONE) {
		(void) sqlite_finalize(*vm, NULL);
		*vm = NULL;
		return (IDMAP_ERR_NOTFOUND);
	}

	(void) sqlite_finalize(*vm, &errmsg);
	*vm = NULL;
	idmapdlog(LOG_ERR, "Database error during %s (%s)",
	    sql, CHECK_NULL(errmsg));
	sqlite_freemem(errmsg);
	return (IDMAP_ERR_INTERNAL);
}

/*
 * Table for well-known SIDs.
 *
 * Background:
 *
 * These well-known principals are stored (as of Windows Server 2003) under:
 * cn=WellKnown Security Principals, cn=Configuration, dc=<forestRootDomain>
 * They belong to objectClass "foreignSecurityPrincipal". They don't have
 * "samAccountName" nor "userPrincipalName" attributes. Their names are
 * available in "cn" and "name" attributes. Some of these principals have a
 * second entry under CN=ForeignSecurityPrincipals,dc=<forestRootDomain> and
 * these duplicate entries have the stringified SID in the "name" and "cn"
 * attributes instead of the actual name.
 *
 * These principals remain constant across all operating systems. Using
 * a hard-coded table here improves performance and avoids additional
 * complexity in the AD lookup code in adutils.c
 *
 * Currently we don't support localization of well-known SID names,
 * unlike Windows.
 *
 * Note that other well-known SIDs (i.e. S-1-5-<domain>-<w-k RID> and
 * S-1-5-32-<w-k RID>) are not stored here because AD does have normal
 * user/group objects for these objects and can be looked up using the
 * existing AD lookup code.
 */
static wksids_table_t wksids[] = {
	{"S-1-1", 0, "Everyone", 0, SENTINEL_PID, -1},
	{"S-1-3", 0, "Creator Owner", 1, IDMAP_WK_CREATOR_OWNER_UID, 0},
	{"S-1-3", 1, "Creator Group", 0, IDMAP_WK_CREATOR_GROUP_GID, 0},
	{"S-1-3", 2, "Creator Owner Server", 1, SENTINEL_PID, -1},
	{"S-1-3", 3, "Creator Group Server", 0, SENTINEL_PID, -1},
	{"S-1-5", 1, "Dialup", 0, SENTINEL_PID, -1},
	{"S-1-5", 2, "Network", 0, SENTINEL_PID, -1},
	{"S-1-5", 3, "Batch", 0, SENTINEL_PID, -1},
	{"S-1-5", 4, "Interactive", 0, SENTINEL_PID, -1},
	{"S-1-5", 6, "Service", 0, SENTINEL_PID, -1},
	{"S-1-5", 7, "Anonymous Logon", 0, GID_NOBODY, 0},
	{"S-1-5", 8, "Proxy", 0, SENTINEL_PID, -1},
	{"S-1-5", 9, "Enterprise Domain Controllers", 0, SENTINEL_PID, -1},
	{"S-1-5", 10, "Self", 0, SENTINEL_PID, -1},
	{"S-1-5", 11, "Authenticated Users", 0, SENTINEL_PID, -1},
	{"S-1-5", 12, "Restricted Code", 0, SENTINEL_PID, -1},
	{"S-1-5", 13, "Terminal Server User", 0, SENTINEL_PID, -1},
	{"S-1-5", 14, "Remote Interactive Logon", 0, SENTINEL_PID, -1},
	{"S-1-5", 15, "This Organization", 0, SENTINEL_PID, -1},
	{"S-1-5", 18, "Local System", 0, IDMAP_WK_LOCAL_SYSTEM_GID, 0},
	{"S-1-5", 19, "Local Service", 0, SENTINEL_PID, -1},
	{"S-1-5", 20, "Network Service", 0, SENTINEL_PID, -1},
	{"S-1-5", 1000, "Other Organization", 0, SENTINEL_PID, -1},
	{"S-1-5-64", 21, "Digest Authentication", 0, SENTINEL_PID, -1},
	{"S-1-5-64", 10, "NTLM Authentication", 0, SENTINEL_PID, -1},
	{"S-1-5-64", 14, "SChannel Authentication", 0, SENTINEL_PID, -1},
	{NULL, UINT32_MAX, NULL, -1, SENTINEL_PID, -1}
};

static idmap_retcode
lookup_wksids_sid2pid(idmap_mapping *req, idmap_id_res *res) {
	int i;
	for (i = 0; wksids[i].sidprefix != NULL; i++) {
		if (wksids[i].rid == req->id1.idmap_id_u.sid.rid &&
		    (strcasecmp(wksids[i].sidprefix,
		    req->id1.idmap_id_u.sid.prefix) == 0)) {

			if (wksids[i].pid == SENTINEL_PID)
				/* Not mapped, break */
				break;
			else if (wksids[i].direction == IDMAP_DIRECTION_U2W)
				continue;

			switch (req->id2.idtype) {
			case IDMAP_UID:
				if (wksids[i].is_user == 0)
					continue;
				res->id.idmap_id_u.uid = wksids[i].pid;
				res->direction = wksids[i].direction;
				return (IDMAP_SUCCESS);
			case IDMAP_GID:
				if (wksids[i].is_user == 1)
					continue;
				res->id.idmap_id_u.gid = wksids[i].pid;
				res->direction = wksids[i].direction;
				return (IDMAP_SUCCESS);
			case IDMAP_POSIXID:
				res->id.idmap_id_u.uid = wksids[i].pid;
				res->id.idtype = (!wksids[i].is_user)?
						IDMAP_GID:IDMAP_UID;
				res->direction = wksids[i].direction;
				return (IDMAP_SUCCESS);
			default:
				return (IDMAP_ERR_NOTSUPPORTED);
			}
		}
	}
	return (IDMAP_ERR_NOTFOUND);
}

static idmap_retcode
lookup_wksids_pid2sid(idmap_mapping *req, idmap_id_res *res, int is_user) {
	int i;
	if (req->id2.idtype != IDMAP_SID)
		return (IDMAP_ERR_NOTSUPPORTED);
	for (i = 0; wksids[i].sidprefix != NULL; i++) {
		if (wksids[i].pid == req->id1.idmap_id_u.uid &&
		    wksids[i].is_user == is_user &&
		    wksids[i].direction != IDMAP_DIRECTION_W2U) {
			res->id.idmap_id_u.sid.rid = wksids[i].rid;
			res->id.idmap_id_u.sid.prefix =
				strdup(wksids[i].sidprefix);
			if (res->id.idmap_id_u.sid.prefix == NULL) {
				idmapdlog(LOG_ERR, "Out of memory");
				return (IDMAP_ERR_MEMORY);
			}
			res->direction = wksids[i].direction;
			return (IDMAP_SUCCESS);
		}
	}
	return (IDMAP_ERR_NOTFOUND);
}

static idmap_retcode
lookup_wksids_sid2name(const char *sidprefix, idmap_rid_t rid, char **name,
		int *type) {
	int i;
	for (i = 0; wksids[i].sidprefix != NULL; i++) {
		if ((strcasecmp(wksids[i].sidprefix, sidprefix) == 0) &&
		    wksids[i].rid == rid) {
			if ((*name = strdup(wksids[i].winname)) == NULL) {
				idmapdlog(LOG_ERR, "Out of memory");
				return (IDMAP_ERR_MEMORY);
			}
			*type = (wksids[i].is_user)?
			    _IDMAP_T_USER:_IDMAP_T_GROUP;
			return (IDMAP_SUCCESS);
		}
	}
	return (IDMAP_ERR_NOTFOUND);
}

static idmap_retcode
lookup_wksids_name2sid(const char *name, char **sidprefix, idmap_rid_t *rid,
		int *type) {
	int i;
	for (i = 0; wksids[i].sidprefix != NULL; i++) {
		if (strcasecmp(wksids[i].winname, name) == 0) {
			if (sidprefix != NULL && (*sidprefix =
			    strdup(wksids[i].sidprefix)) == NULL) {
				idmapdlog(LOG_ERR, "Out of memory");
				return (IDMAP_ERR_MEMORY);
			}
			if (type != NULL)
				*type = (wksids[i].is_user)?
				    _IDMAP_T_USER:_IDMAP_T_GROUP;
			if (rid != NULL)
				*rid = wksids[i].rid;
			return (IDMAP_SUCCESS);
		}
	}
	return (IDMAP_ERR_NOTFOUND);
}

static idmap_retcode
lookup_cache_sid2pid(sqlite *cache, idmap_mapping *req, idmap_id_res *res) {
	char		*end;
	char		*sql = NULL;
	const char	**values;
	sqlite_vm	*vm = NULL;
	int		ncol, is_user;
	uid_t		pid;
	idmap_utf8str	*str;
	time_t		curtime, exp;
	idmap_retcode	retcode;

	/* Current time */
	errno = 0;
	if ((curtime = time(NULL)) == (time_t)-1) {
		idmapdlog(LOG_ERR,
			"Failed to get current time (%s)",
			strerror(errno));
		retcode = IDMAP_ERR_INTERNAL;
		goto out;
	}

	/* SQL to lookup the cache */
	sql = sqlite_mprintf("SELECT pid, is_user, expiration, unixname, u2w "
			"FROM idmap_cache WHERE "
			"sidprefix = %Q AND rid = %u AND w2u = 1 AND "
			"(pid >= 2147483648 OR "
			"(expiration = 0 OR expiration ISNULL OR "
			"expiration > %d));",
			req->id1.idmap_id_u.sid.prefix,
			req->id1.idmap_id_u.sid.rid,
			curtime);
	if (sql == NULL) {
		idmapdlog(LOG_ERR, "Out of memory");
		retcode = IDMAP_ERR_MEMORY;
		goto out;
	}
	retcode = sql_compile_n_step_once(cache, sql, &vm, &ncol, 5, &values);
	sqlite_freemem(sql);

	if (retcode == IDMAP_ERR_NOTFOUND) {
		goto out;
	} else if (retcode == IDMAP_SUCCESS) {
		/* sanity checks */
		if (values[0] == NULL || values[1] == NULL) {
			retcode = IDMAP_ERR_CACHE;
			goto out;
		}

		pid = strtoul(values[0], &end, 10);
		is_user = strncmp(values[1], "0", 2)?1:0;

		/*
		 * We may have an expired ephemeral mapping. Consider
		 * the expired entry as valid if we are not going to
		 * perform name-based mapping. But do not renew the
		 * expiration.
		 * If we will be doing name-based mapping then store the
		 * ephemeral pid in the result so that we can use it
		 * if we end up doing dynamic mapping again.
		 */
		if (!DO_NOT_ALLOC_NEW_ID_MAPPING(req) &&
				!AVOID_NAMESERVICE(req)) {
			if (IS_EPHEMERAL(pid) && values[2] != NULL) {
				exp = strtoll(values[2], &end, 10);
				if (exp && exp <= curtime) {
					/* Store the ephemeral pid */
					res->id.idmap_id_u.uid = pid;
					res->id.idtype = is_user?
						IDMAP_UID:IDMAP_GID;
					res->direction = IDMAP_DIRECTION_BI;
					req->direction |= is_user?
						_IDMAP_F_EXP_EPH_UID:
						_IDMAP_F_EXP_EPH_GID;
					retcode = IDMAP_ERR_NOTFOUND;
					goto out;
				}
			}
		}

		switch (req->id2.idtype) {
		case IDMAP_UID:
			if (!is_user)
				retcode = IDMAP_ERR_NOTUSER;
			else
				res->id.idmap_id_u.uid = pid;
			break;
		case IDMAP_GID:
			if (is_user)
				retcode = IDMAP_ERR_NOTGROUP;
			else
				res->id.idmap_id_u.gid = pid;
			break;
		case IDMAP_POSIXID:
			res->id.idmap_id_u.uid = pid;
			res->id.idtype = (is_user)?IDMAP_UID:IDMAP_GID;
			break;
		default:
			retcode = IDMAP_ERR_NOTSUPPORTED;
			break;
		}
	}

out:
	if (retcode == IDMAP_SUCCESS) {
		if (values[4] != NULL)
			res->direction =
			    (strtol(values[4], &end, 10) == 0)?
			    IDMAP_DIRECTION_W2U:IDMAP_DIRECTION_BI;
		else
			res->direction = IDMAP_DIRECTION_W2U;

		if (values[3] != NULL) {
			str = &req->id2name;
			retcode = idmap_str2utf8(&str, values[3], 0);
			if (retcode != IDMAP_SUCCESS) {
				idmapdlog(LOG_ERR, "Out of memory");
				retcode = IDMAP_ERR_MEMORY;
			}
		}
	}
	if (vm != NULL)
		(void) sqlite_finalize(vm, NULL);
	return (retcode);
}

static idmap_retcode
lookup_cache_sid2name(sqlite *cache, const char *sidprefix, idmap_rid_t rid,
		char **name, char **domain, int *type) {
	char		*end;
	char		*sql = NULL;
	const char	**values;
	sqlite_vm	*vm = NULL;
	int		ncol;
	time_t		curtime;
	idmap_retcode	retcode = IDMAP_SUCCESS;

	/* Get current time */
	errno = 0;
	if ((curtime = time(NULL)) == (time_t)-1) {
		idmapdlog(LOG_ERR,
			"Failed to get current time (%s)",
			strerror(errno));
		retcode = IDMAP_ERR_INTERNAL;
		goto out;
	}

	/* SQL to lookup the cache */
	sql = sqlite_mprintf("SELECT name, domain, type FROM name_cache WHERE "
			"sidprefix = %Q AND rid = %u AND "
			"(expiration = 0 OR expiration ISNULL OR "
			"expiration > %d);",
			sidprefix, rid, curtime);
	if (sql == NULL) {
		idmapdlog(LOG_ERR, "Out of memory");
		retcode = IDMAP_ERR_MEMORY;
		goto out;
	}
	retcode = sql_compile_n_step_once(cache, sql, &vm, &ncol, 3, &values);
	sqlite_freemem(sql);

	if (retcode == IDMAP_SUCCESS) {
		if (type != NULL) {
			if (values[2] == NULL) {
				retcode = IDMAP_ERR_CACHE;
				goto out;
			}
			*type = strtol(values[2], &end, 10);
		}

		if (name != NULL && values[0] != NULL) {
			if ((*name = strdup(values[0])) == NULL) {
				idmapdlog(LOG_ERR, "Out of memory");
				retcode = IDMAP_ERR_MEMORY;
				goto out;
			}
		}

		if (domain != NULL && values[1] != NULL) {
			if ((*domain = strdup(values[1])) == NULL) {
				if (name != NULL && *name) {
					free(*name);
					*name = NULL;
				}
				idmapdlog(LOG_ERR, "Out of memory");
				retcode = IDMAP_ERR_MEMORY;
				goto out;
			}
		}
	}

out:
	if (vm != NULL)
		(void) sqlite_finalize(vm, NULL);
	return (retcode);
}

static idmap_retcode
verify_type(idmap_id_type idtype, int type, idmap_id_res *res) {
	switch (idtype) {
	case IDMAP_UID:
		if (type != _IDMAP_T_USER)
			return (IDMAP_ERR_NOTUSER);
		res->id.idtype = IDMAP_UID;
		break;
	case IDMAP_GID:
		if (type != _IDMAP_T_GROUP)
			return (IDMAP_ERR_NOTGROUP);
		res->id.idtype = IDMAP_GID;
		break;
	case IDMAP_POSIXID:
		if (type == _IDMAP_T_USER)
			res->id.idtype = IDMAP_UID;
		else if (type == _IDMAP_T_GROUP)
			res->id.idtype = IDMAP_GID;
		else
			return (IDMAP_ERR_SID);
		break;
	default:
		return (IDMAP_ERR_NOTSUPPORTED);
	}
	return (IDMAP_SUCCESS);
}

/*
 * Lookup sid to name locally
 */
static idmap_retcode
lookup_local_sid2name(sqlite *cache, idmap_mapping *req, idmap_id_res *res) {
	int		type = -1;
	idmap_retcode	retcode;
	char		*sidprefix;
	idmap_rid_t	rid;
	char		*name = NULL, *domain = NULL;
	idmap_utf8str	*str;

	sidprefix = req->id1.idmap_id_u.sid.prefix;
	rid = req->id1.idmap_id_u.sid.rid;

	/* Lookup sids to name in well-known sids table */
	retcode = lookup_wksids_sid2name(sidprefix, rid, &name, &type);
	if (retcode != IDMAP_ERR_NOTFOUND)
		goto out;

	/* Lookup sid to name in cache */
	retcode = lookup_cache_sid2name(cache, sidprefix, rid, &name,
		&domain, &type);
	if (retcode != IDMAP_SUCCESS)
		goto out;

out:
	if (retcode == IDMAP_SUCCESS) {
		/* Verify that the sid type matches the request */
		retcode = verify_type(req->id2.idtype, type, res);

		/* update state in 'req' */
		if (name != NULL) {
			str = &req->id1name;
			(void) idmap_str2utf8(&str, name, 1);
		}
		if (domain != NULL) {
			str = &req->id1domain;
			(void) idmap_str2utf8(&str, domain, 1);
		}
	} else {
		if (name != NULL)
			free(name);
		if (domain != NULL)
			free(domain);
	}
	return (retcode);
}

idmap_retcode
lookup_win_batch_sid2name(lookup_state_t *state, idmap_mapping_batch *batch,
		idmap_ids_res *result) {
	idmap_retcode	retcode;
	int		ret, i;
	int		retries = 0;
	idmap_mapping	*req;
	idmap_id_res	*res;

	if (state->ad_nqueries == 0)
		return (IDMAP_SUCCESS);

retry:
	ret = idmap_lookup_batch_start(_idmapdstate.ad, state->ad_nqueries,
		&state->ad_lookup);
	if (ret != 0) {
		idmapdlog(LOG_ERR,
		"Failed to create sid2name batch for AD lookup");
		return (IDMAP_ERR_INTERNAL);
	}

	for (i = 0; i < batch->idmap_mapping_batch_len; i++) {
		req = &batch->idmap_mapping_batch_val[i];
		res = &result->ids.ids_val[i];

		if (req->id1.idtype == IDMAP_SID &&
				req->direction & _IDMAP_F_S2N_AD) {
			if (retries == 0)
				res->retcode = IDMAP_ERR_RETRIABLE_NET_ERR;
			else if (res->retcode != IDMAP_ERR_RETRIABLE_NET_ERR)
				continue;
			retcode = idmap_sid2name_batch_add1(
					state->ad_lookup,
					req->id1.idmap_id_u.sid.prefix,
					&req->id1.idmap_id_u.sid.rid,
					&req->id1name.idmap_utf8str_val,
					&req->id1domain.idmap_utf8str_val,
					(int *)&res->id.idtype,
					&res->retcode);

			if (retcode == IDMAP_ERR_RETRIABLE_NET_ERR)
				break;
			if (retcode != IDMAP_SUCCESS)
				goto out;
		}
	}

	if (retcode == IDMAP_ERR_RETRIABLE_NET_ERR)
		idmap_lookup_release_batch(&state->ad_lookup);
	else
		retcode = idmap_lookup_batch_end(&state->ad_lookup, NULL);

	if (retcode == IDMAP_ERR_RETRIABLE_NET_ERR && retries++ < 2)
		goto retry;

	return (retcode);

out:
	idmapdlog(LOG_NOTICE, "Windows SID to user/group name lookup failed");
	idmap_lookup_release_batch(&state->ad_lookup);
	return (retcode);
}

idmap_retcode
sid2pid_first_pass(lookup_state_t *state, sqlite *cache, idmap_mapping *req,
		idmap_id_res *res) {
	idmap_retcode	retcode;

	/*
	 * The req->direction field is used to maintain state of the
	 * sid2pid request.
	 */
	req->direction = _IDMAP_F_DONE;

	if (req->id1.idmap_id_u.sid.prefix == NULL) {
		retcode = IDMAP_ERR_SID;
		goto out;
	}
	res->id.idtype = req->id2.idtype;
	res->id.idmap_id_u.uid = UID_NOBODY;

	/* Lookup well-known sid to pid mapping */
	retcode = lookup_wksids_sid2pid(req, res);
	if (retcode != IDMAP_ERR_NOTFOUND)
		goto out;

	/* Lookup sid to pid in cache */
	retcode = lookup_cache_sid2pid(cache, req, res);
	if (retcode != IDMAP_ERR_NOTFOUND)
		goto out;

	if (DO_NOT_ALLOC_NEW_ID_MAPPING(req) || AVOID_NAMESERVICE(req)) {
		res->id.idmap_id_u.uid = SENTINEL_PID;
		goto out;
	}

	/*
	 * Failed to find non-expired entry in cache. Tell the caller
	 * that we are not done yet.
	 */
	state->sid2pid_done = FALSE;

	/*
	 * Our next step is name-based mapping. To lookup name-based
	 * mapping rules, we need the windows name and domain-name
	 * associated with the SID.
	 */

	/*
	 * Check if we already have the name (i.e name2pid lookups)
	 */
	if (req->id1name.idmap_utf8str_val != NULL &&
	    req->id1domain.idmap_utf8str_val != NULL) {
		retcode = IDMAP_SUCCESS;
		req->direction |= _IDMAP_F_S2N_CACHE;
		goto out;
	}

	/* Lookup sid to winname@domain locally first */
	retcode = lookup_local_sid2name(cache, req, res);
	if (retcode == IDMAP_SUCCESS) {
		req->direction |= _IDMAP_F_S2N_CACHE;
	} else if (retcode == IDMAP_ERR_NOTFOUND) {
		/* Batch sid to name AD lookup request */
		retcode = IDMAP_SUCCESS;
		req->direction |= _IDMAP_F_S2N_AD;
		state->ad_nqueries++;
		goto out;
	}


out:
	res->retcode = idmap_stat4prot(retcode);
	return (retcode);
}

/*
 * Generate SID using the following convention
 * 	<machine-sid-prefix>-<1000 + uid>
 * 	<machine-sid-prefix>-<2^31 + gid>
 */
static idmap_retcode
generate_localsid(idmap_mapping *req, idmap_id_res *res, int is_user) {

	if (_idmapdstate.cfg->pgcfg.machine_sid != NULL) {
		/* Skip 1000 UIDs */
		if (is_user && req->id1.idmap_id_u.uid >
				(INT32_MAX - LOCALRID_MIN))
			return (IDMAP_ERR_NOMAPPING);

		RDLOCK_CONFIG();
		res->id.idmap_id_u.sid.prefix =
			strdup(_idmapdstate.cfg->pgcfg.machine_sid);
		if (res->id.idmap_id_u.sid.prefix == NULL) {
			UNLOCK_CONFIG();
			idmapdlog(LOG_ERR, "Out of memory");
			return (IDMAP_ERR_MEMORY);
		}
		UNLOCK_CONFIG();
		res->id.idmap_id_u.sid.rid =
			(is_user)?req->id1.idmap_id_u.uid + LOCALRID_MIN:
			req->id1.idmap_id_u.gid + INT32_MAX + 1;
		res->direction = IDMAP_DIRECTION_BI;

		/*
		 * Don't update name_cache because local sids don't have
		 * valid windows names.
		 * We mark the entry as being found in the namecache so that
		 * the cache update routine doesn't update namecache.
		 */
		req->direction = _IDMAP_F_S2N_CACHE;
		return (IDMAP_SUCCESS);
	}

	return (IDMAP_ERR_NOMAPPING);
}

static idmap_retcode
lookup_localsid2pid(idmap_mapping *req, idmap_id_res *res) {
	char		*sidprefix;
	uint32_t	rid;
	int		s;

	/*
	 * If the sidprefix == localsid then UID = last RID - 1000 or
	 * GID = last RID - 2^31.
	 */
	sidprefix = req->id1.idmap_id_u.sid.prefix;
	rid = req->id1.idmap_id_u.sid.rid;

	RDLOCK_CONFIG();
	s = (_idmapdstate.cfg->pgcfg.machine_sid)?
		strcasecmp(sidprefix,
		_idmapdstate.cfg->pgcfg.machine_sid):1;
	UNLOCK_CONFIG();

	if (s == 0) {
		switch (req->id2.idtype) {
		case IDMAP_UID:
			if (rid > INT32_MAX) {
				return (IDMAP_ERR_NOTUSER);
			} else if (rid < LOCALRID_MIN) {
				return (IDMAP_ERR_NOTFOUND);
			}
			res->id.idmap_id_u.uid = rid - LOCALRID_MIN;
			res->id.idtype = IDMAP_UID;
			break;
		case IDMAP_GID:
			if (rid <= INT32_MAX) {
				return (IDMAP_ERR_NOTGROUP);
			}
			res->id.idmap_id_u.gid = rid - INT32_MAX - 1;
			res->id.idtype = IDMAP_GID;
			break;
		case IDMAP_POSIXID:
			if (rid > INT32_MAX) {
				res->id.idmap_id_u.gid =
					rid - INT32_MAX - 1;
				res->id.idtype = IDMAP_GID;
			} else if (rid < LOCALRID_MIN) {
				return (IDMAP_ERR_NOTFOUND);
			} else {
				res->id.idmap_id_u.uid = rid - LOCALRID_MIN;
				res->id.idtype = IDMAP_UID;
			}
			break;
		default:
			return (IDMAP_ERR_NOTSUPPORTED);
		}
		return (IDMAP_SUCCESS);
	}

	return (IDMAP_ERR_NOTFOUND);
}

static idmap_retcode
ns_lookup_byname(int is_user, const char *name, idmap_id_res *res) {
	struct passwd	pwd;
	struct group	grp;
	char		buf[1024];
	int		errnum;
	const char	*me = "ns_lookup_byname";

	if (is_user) {
		if (getpwnam_r(name, &pwd, buf, sizeof (buf)) == NULL) {
			errnum = errno;
			idmapdlog(LOG_WARNING,
			"%s: getpwnam_r(%s) failed (%s).",
				me, name,
				errnum?strerror(errnum):"not found");
			if (errnum == 0)
				return (IDMAP_ERR_NOTFOUND);
			else
				return (IDMAP_ERR_INTERNAL);
		}
		res->id.idmap_id_u.uid = pwd.pw_uid;
		res->id.idtype = IDMAP_UID;
	} else {
		if (getgrnam_r(name, &grp, buf, sizeof (buf)) == NULL) {
			errnum = errno;
			idmapdlog(LOG_WARNING,
			"%s: getgrnam_r(%s) failed (%s).",
				me, name,
				errnum?strerror(errnum):"not found");
			if (errnum == 0)
				return (IDMAP_ERR_NOTFOUND);
			else
				return (IDMAP_ERR_INTERNAL);
		}
		res->id.idmap_id_u.gid = grp.gr_gid;
		res->id.idtype = IDMAP_GID;
	}
	return (IDMAP_SUCCESS);
}

/*
 * Name-based mapping
 *
 * Case 1: If no rule matches do ephemeral
 *
 * Case 2: If rule matches and unixname is "" then return no mapping.
 *
 * Case 3: If rule matches and unixname is specified then lookup name
 *  service using the unixname. If unixname not found then return no mapping.
 *
 * Case 4: If rule matches and unixname is * then lookup name service
 *  using winname as the unixname. If unixname not found then process
 *  other rules using the lookup order. If no other rule matches then do
 *  ephemeral. Otherwise, based on the matched rule do Case 2 or 3 or 4.
 *  This allows us to specify a fallback unixname per _domain_ or no mapping
 *  instead of the default behaviour of doing ephemeral mapping.
 *
 * Example 1:
 * *@sfbay == *
 * If looking up windows users foo@sfbay and foo does not exists in
 * the name service then foo@sfbay will be mapped to an ephemeral id.
 *
 * Example 2:
 * *@sfbay == *
 * *@sfbay => guest
 * If looking up windows users foo@sfbay and foo does not exists in
 * the name service then foo@sfbay will be mapped to guest.
 *
 * Example 3:
 * *@sfbay == *
 * *@sfbay => ""
 * If looking up windows users foo@sfbay and foo does not exists in
 * the name service then we will return no mapping for foo@sfbay.
 *
 */
static idmap_retcode
name_based_mapping_sid2pid(sqlite *db, idmap_mapping *req, idmap_id_res *res) {
	const char	*unixname, *winname, *windomain;
	char		*sql = NULL, *errmsg = NULL;
	idmap_retcode	retcode;
	char		*end;
	const char	**values;
	sqlite_vm	*vm = NULL;
	idmap_utf8str	*str;
	int		ncol, r, i, is_user;
	const char	*me = "name_based_mapping_sid2pid";

	winname = req->id1name.idmap_utf8str_val;
	windomain = req->id1domain.idmap_utf8str_val;
	is_user = (res->id.idtype == IDMAP_UID)?1:0;

	i = 0;
	if (windomain == NULL) {
		windomain = "";
	} else {
		RDLOCK_CONFIG();
		if (_idmapdstate.cfg->pgcfg.mapping_domain != NULL) {
			if (strcasecmp(_idmapdstate.cfg->pgcfg.mapping_domain,
			    windomain) == 0)
				i = 1;
		}
		UNLOCK_CONFIG();
	}

	sql = sqlite_mprintf(
		"SELECT unixname, u2w_order FROM namerules WHERE "
		"w2u_order > 0 AND is_user = %d AND "
		"(winname = %Q OR winname = '*') AND "
		"(windomain = %Q OR windomain = '*' %s) "
		"ORDER BY w2u_order ASC;",
		is_user, winname,
		windomain,
		i?"OR windomain ISNULL OR windomain = ''":"");
	if (sql == NULL) {
		idmapdlog(LOG_ERR, "Out of memory");
		retcode = IDMAP_ERR_MEMORY;
		goto out;
	}

	if (sqlite_compile(db, sql, NULL, &vm, &errmsg) != SQLITE_OK) {
		retcode = IDMAP_ERR_INTERNAL;
		idmapdlog(LOG_ERR,
			"%s: database error (%s)",
			me, CHECK_NULL(errmsg));
		sqlite_freemem(errmsg);
		goto out;
	}

	for (; ; ) {
		r = sqlite_step(vm, &ncol, &values, NULL);
		assert(r != SQLITE_LOCKED && r != SQLITE_BUSY);

		if (r == SQLITE_ROW) {
			if (ncol < 2) {
				retcode = IDMAP_ERR_INTERNAL;
				goto out;
			}
			if (values[0] == NULL) {
				retcode = IDMAP_ERR_INTERNAL;
				goto out;
			}

			if (EMPTY_NAME(values[0])) {
				retcode = IDMAP_ERR_NOMAPPING;
				goto out;
			}
			unixname = (values[0][0] == '*')?winname:values[0];
			retcode = ns_lookup_byname(is_user, unixname, res);
			if (retcode == IDMAP_ERR_NOTFOUND) {
				if (unixname == winname)
					/* Case 4 */
					continue;
				else
					/* Case 3 */
					retcode = IDMAP_ERR_NOMAPPING;
			}
			goto out;
		} else if (r == SQLITE_DONE) {
			retcode = IDMAP_ERR_NOTFOUND;
			goto out;
		} else {
			(void) sqlite_finalize(vm, &errmsg);
			vm = NULL;
			idmapdlog(LOG_ERR,
				"%s: database error (%s)",
				me, CHECK_NULL(errmsg));
			sqlite_freemem(errmsg);
			retcode = IDMAP_ERR_INTERNAL;
			goto out;
		}
	}

out:
	if (sql != NULL)
		sqlite_freemem(sql);
	if (retcode == IDMAP_SUCCESS) {
		if (values[1] != NULL)
			res->direction =
			    (strtol(values[1], &end, 10) == 0)?
			    IDMAP_DIRECTION_W2U:IDMAP_DIRECTION_BI;
		else
			res->direction = IDMAP_DIRECTION_W2U;
		str = &req->id2name;
		retcode = idmap_str2utf8(&str, unixname, 0);
	}
	if (vm != NULL)
		(void) sqlite_finalize(vm, NULL);
	return (retcode);
}

static
int
get_next_eph_uid(uid_t *next_uid)
{
	uid_t uid;
	gid_t gid;
	int err;

	*next_uid = (uid_t)-1;
	uid = _idmapdstate.next_uid++;
	if (uid >= _idmapdstate.limit_uid) {
		if ((err = allocids(0, 8192, &uid, 0, &gid)) != 0)
			return (err);

		_idmapdstate.limit_uid = uid + 8192;
		_idmapdstate.next_uid = uid;
	}
	*next_uid = uid;

	return (0);
}

static
int
get_next_eph_gid(gid_t *next_gid)
{
	uid_t uid;
	gid_t gid;
	int err;

	*next_gid = (uid_t)-1;
	gid = _idmapdstate.next_gid++;
	if (gid >= _idmapdstate.limit_gid) {
		if ((err = allocids(0, 0, &uid, 8192, &gid)) != 0)
			return (err);

		_idmapdstate.limit_gid = gid + 8192;
		_idmapdstate.next_gid = gid;
	}
	*next_gid = gid;

	return (0);
}

static
int
gethash(const char *str, uint32_t num, uint_t htsize) {
	uint_t  hval, i, len;

	if (str == NULL)
		return (0);
	for (len = strlen(str), hval = 0, i = 0; i < len; i++) {
		hval += str[i];
		hval += (hval << 10);
		hval ^= (hval >> 6);
	}
	for (str = (const char *)&num, i = 0; i < sizeof (num); i++) {
		hval += str[i];
		hval += (hval << 10);
		hval ^= (hval >> 6);
	}
	hval += (hval << 3);
	hval ^= (hval >> 11);
	hval += (hval << 15);
	return (hval % htsize);
}

static
int
get_from_sid_history(lookup_state_t *state, const char *prefix, uint32_t rid,
		uid_t *pid) {
	uint_t		next, key;
	uint_t		htsize = state->sid_history_size;
	idmap_sid	*sid;

	next = gethash(prefix, rid, htsize);
	while (next != htsize) {
		key = state->sid_history[next].key;
		if (key == htsize)
			return (0);
		sid = &state->batch->idmap_mapping_batch_val[key].id1.
		    idmap_id_u.sid;
		if (sid->rid == rid && strcmp(sid->prefix, prefix) == 0) {
			*pid = state->result->ids.ids_val[key].id.
			    idmap_id_u.uid;
			return (1);
		}
		next = state->sid_history[next].next;
	}
	return (0);
}

static
void
add_to_sid_history(lookup_state_t *state, const char *prefix, uint32_t rid) {
	uint_t		hash, next;
	uint_t		htsize = state->sid_history_size;

	hash = next = gethash(prefix, rid, htsize);
	while (state->sid_history[next].key != htsize) {
		next++;
		next %= htsize;
	}
	state->sid_history[next].key = state->curpos;
	if (hash == next)
		return;
	state->sid_history[next].next = state->sid_history[hash].next;
	state->sid_history[hash].next = next;
}

/* ARGSUSED */
static
idmap_retcode
dynamic_ephemeral_mapping(lookup_state_t *state, sqlite *cache,
		idmap_mapping *req, idmap_id_res *res) {

	uid_t		next_pid;

	res->direction = IDMAP_DIRECTION_BI;

	if (IS_EPHEMERAL(res->id.idmap_id_u.uid))
		return (IDMAP_SUCCESS);

	if (state->sid_history != NULL &&
	    get_from_sid_history(state, req->id1.idmap_id_u.sid.prefix,
	    req->id1.idmap_id_u.sid.rid, &next_pid)) {
		res->id.idmap_id_u.uid = next_pid;
		return (IDMAP_SUCCESS);
	}

	if (res->id.idtype == IDMAP_UID) {
		if (get_next_eph_uid(&next_pid) != 0)
			return (IDMAP_ERR_INTERNAL);
		res->id.idmap_id_u.uid = next_pid;
	} else {
		if (get_next_eph_gid(&next_pid) != 0)
			return (IDMAP_ERR_INTERNAL);
		res->id.idmap_id_u.gid = next_pid;
	}

	if (state->sid_history != NULL)
		add_to_sid_history(state, req->id1.idmap_id_u.sid.prefix,
		    req->id1.idmap_id_u.sid.rid);

	return (IDMAP_SUCCESS);
}

idmap_retcode
sid2pid_second_pass(lookup_state_t *state, sqlite *cache, sqlite *db,
		idmap_mapping *req, idmap_id_res *res) {
	idmap_retcode	retcode;

	/*
	 * The req->direction field is used to maintain state of the
	 * sid2pid request.
	 */

	/* Check if second pass is needed */
	if (req->direction == _IDMAP_F_DONE)
		return (res->retcode);

	/* Get status from previous pass */
	retcode = (res->retcode == IDMAP_NEXT)?IDMAP_SUCCESS:res->retcode;

	if (retcode != IDMAP_SUCCESS) {
		/* Reset return type */
		res->id.idtype = req->id2.idtype;
		res->id.idmap_id_u.uid = UID_NOBODY;

		/* Check if this is a localsid */
		if (retcode == IDMAP_ERR_NOTFOUND &&
		    _idmapdstate.cfg->pgcfg.machine_sid) {
			retcode = lookup_localsid2pid(req, res);
			if (retcode == IDMAP_SUCCESS) {
				state->sid2pid_done = FALSE;
				req->direction = _IDMAP_F_S2N_CACHE;
			}
		}
		goto out;
	}

	/*
	 * Verify that the sid type matches the request if the
	 * SID was validated by an AD lookup.
	 */
	if (req->direction & _IDMAP_F_S2N_AD) {
		retcode = verify_type(req->id2.idtype,
			(int)res->id.idtype, res);
		if (retcode != IDMAP_SUCCESS) {
			res->id.idtype = req->id2.idtype;
			res->id.idmap_id_u.uid = UID_NOBODY;
			goto out;
		}
	}

	/* Name-based mapping */
	retcode = name_based_mapping_sid2pid(db, req, res);
	if (retcode == IDMAP_ERR_NOTFOUND)
		/* If not found, do ephemeral mapping */
		goto ephemeral;
	else if (retcode != IDMAP_SUCCESS)
		goto out;

	state->sid2pid_done = FALSE;
	goto out;


ephemeral:
	retcode = dynamic_ephemeral_mapping(state, cache, req, res);
	if (retcode == IDMAP_SUCCESS)
		state->sid2pid_done = FALSE;

out:
	res->retcode = idmap_stat4prot(retcode);
	return (retcode);
}

idmap_retcode
update_cache_pid2sid(lookup_state_t *state, sqlite *cache,
		idmap_mapping *req, idmap_id_res *res) {
	char		*sql = NULL;
	idmap_retcode	retcode;

	/* Check if we need to cache anything */
	if (req->direction == _IDMAP_F_DONE)
		return (IDMAP_SUCCESS);

	/* We don't cache negative entries */
	if (res->retcode != IDMAP_SUCCESS)
		return (IDMAP_SUCCESS);

	/*
	 * Using NULL for u2w instead of 0 so that our trigger allows
	 * the same pid to be the destination in multiple entries
	 */
	sql = sqlite_mprintf("INSERT OR REPLACE into idmap_cache "
		"(sidprefix, rid, windomain, winname, pid, unixname, "
		"is_user, expiration, w2u, u2w) "
		"VALUES(%Q, %u, %Q, %Q, %u, %Q, %d, "
		"strftime('%%s','now') + 600, %q, 1); ",
		res->id.idmap_id_u.sid.prefix,
		res->id.idmap_id_u.sid.rid,
		req->id2domain.idmap_utf8str_val,
		req->id2name.idmap_utf8str_val,
		req->id1.idmap_id_u.uid,
		req->id1name.idmap_utf8str_val,
		(req->id1.idtype == IDMAP_UID)?1:0,
		(res->direction == 0)?"1":NULL);

	if (sql == NULL) {
		retcode = IDMAP_ERR_INTERNAL;
		idmapdlog(LOG_ERR, "Out of memory");
		goto out;
	}

	retcode = sql_exec_no_cb(cache, sql);
	if (retcode != IDMAP_SUCCESS)
		goto out;

	state->pid2sid_done = FALSE;
	sqlite_freemem(sql);
	sql = NULL;

	/* If sid2name was found in the cache, no need to update namecache */
	if (req->direction & _IDMAP_F_S2N_CACHE)
		goto out;

	if (req->id2name.idmap_utf8str_val == NULL)
		goto out;

	sql = sqlite_mprintf("INSERT OR REPLACE into name_cache "
		"(sidprefix, rid, name, domain, type, expiration) "
		"VALUES(%Q, %u, %Q, %Q, %d, strftime('%%s','now') + 3600); ",
		res->id.idmap_id_u.sid.prefix,
		res->id.idmap_id_u.sid.rid,
		req->id2name.idmap_utf8str_val,
		req->id2domain.idmap_utf8str_val,
		(req->id1.idtype == IDMAP_UID)?_IDMAP_T_USER:_IDMAP_T_GROUP);

	if (sql == NULL) {
		retcode = IDMAP_ERR_INTERNAL;
		idmapdlog(LOG_ERR, "Out of memory");
		goto out;
	}

	retcode = sql_exec_no_cb(cache, sql);

out:
	if (sql != NULL)
		sqlite_freemem(sql);
	return (retcode);
}

idmap_retcode
update_cache_sid2pid(lookup_state_t *state, sqlite *cache,
		idmap_mapping *req, idmap_id_res *res) {
	char		*sql = NULL;
	idmap_retcode	retcode;
	int		is_eph_user;

	/* Check if we need to cache anything */
	if (req->direction == _IDMAP_F_DONE)
		return (IDMAP_SUCCESS);

	/* We don't cache negative entries */
	if (res->retcode != IDMAP_SUCCESS)
		return (IDMAP_SUCCESS);

	if (req->direction & _IDMAP_F_EXP_EPH_UID)
		is_eph_user = 1;
	else if (req->direction & _IDMAP_F_EXP_EPH_GID)
		is_eph_user = 0;
	else
		is_eph_user = -1;

	if (is_eph_user >= 0 && !IS_EPHEMERAL(res->id.idmap_id_u.uid)) {
		sql = sqlite_mprintf("UPDATE idmap_cache "
			"SET w2u = 0 WHERE "
			"sidprefix = %Q AND rid = %u AND w2u = 1 AND "
			"pid >= 2147483648 AND is_user = %d;",
			req->id1.idmap_id_u.sid.prefix,
			req->id1.idmap_id_u.sid.rid,
			is_eph_user);
		if (sql == NULL) {
			retcode = IDMAP_ERR_INTERNAL;
			idmapdlog(LOG_ERR, "Out of memory");
			goto out;
		}

		retcode = sql_exec_no_cb(cache, sql);
		if (retcode != IDMAP_SUCCESS)
			goto out;

		sqlite_freemem(sql);
		sql = NULL;
	}

	sql = sqlite_mprintf("INSERT OR REPLACE into idmap_cache "
		"(sidprefix, rid, windomain, winname, pid, unixname, "
		"is_user, expiration, w2u, u2w) "
		"VALUES(%Q, %u, %Q, %Q, %u, %Q, %d, "
		"strftime('%%s','now') + 600, 1, %q); ",
		req->id1.idmap_id_u.sid.prefix,
		req->id1.idmap_id_u.sid.rid,
		req->id1domain.idmap_utf8str_val,
		req->id1name.idmap_utf8str_val,
		res->id.idmap_id_u.uid,
		req->id2name.idmap_utf8str_val,
		(res->id.idtype == IDMAP_UID)?1:0,
		(res->direction == 0)?"1":NULL);

	if (sql == NULL) {
		retcode = IDMAP_ERR_INTERNAL;
		idmapdlog(LOG_ERR, "Out of memory");
		goto out;
	}

	retcode = sql_exec_no_cb(cache, sql);
	if (retcode != IDMAP_SUCCESS)
		goto out;

	state->sid2pid_done = FALSE;
	sqlite_freemem(sql);
	sql = NULL;

	/* If name2sid was found in the cache, no need to update namecache */
	if (req->direction & _IDMAP_F_S2N_CACHE)
		goto out;

	if (req->id1name.idmap_utf8str_val == NULL)
		goto out;

	sql = sqlite_mprintf("INSERT OR REPLACE into name_cache "
		"(sidprefix, rid, name, domain, type, expiration) "
		"VALUES(%Q, %u, %Q, %Q, %d, strftime('%%s','now') + 3600); ",
		req->id1.idmap_id_u.sid.prefix,
		req->id1.idmap_id_u.sid.rid,
		req->id1name.idmap_utf8str_val,
		req->id1domain.idmap_utf8str_val,
		(res->id.idtype == IDMAP_UID)?_IDMAP_T_USER:_IDMAP_T_GROUP);

	if (sql == NULL) {
		retcode = IDMAP_ERR_INTERNAL;
		idmapdlog(LOG_ERR, "Out of memory");
		goto out;
	}

	retcode = sql_exec_no_cb(cache, sql);

out:
	if (sql != NULL)
		sqlite_freemem(sql);
	return (retcode);
}

static idmap_retcode
lookup_cache_pid2sid(sqlite *cache, idmap_mapping *req, idmap_id_res *res,
		int is_user, int getname) {
	char		*end;
	char		*sql = NULL;
	const char	**values;
	sqlite_vm	*vm = NULL;
	int		ncol;
	idmap_retcode	retcode = IDMAP_SUCCESS;
	idmap_utf8str	*str;
	time_t		curtime;

	/* Current time */
	errno = 0;
	if ((curtime = time(NULL)) == (time_t)-1) {
		idmapdlog(LOG_ERR,
			"Failed to get current time (%s)",
			strerror(errno));
		retcode = IDMAP_ERR_INTERNAL;
		goto out;
	}

	/* SQL to lookup the cache */
	sql = sqlite_mprintf("SELECT sidprefix, rid, winname, windomain, w2u "
			"FROM idmap_cache WHERE "
			"pid = %u AND u2w = 1 AND is_user = %d AND "
			"(pid >= 2147483648 OR "
			"(expiration = 0 OR expiration ISNULL OR "
			"expiration > %d));",
			req->id1.idmap_id_u.uid, is_user, curtime);
	if (sql == NULL) {
		idmapdlog(LOG_ERR, "Out of memory");
		retcode = IDMAP_ERR_MEMORY;
		goto out;
	}
	retcode = sql_compile_n_step_once(cache, sql, &vm, &ncol, 5, &values);
	sqlite_freemem(sql);

	if (retcode == IDMAP_ERR_NOTFOUND)
		goto out;
	else if (retcode == IDMAP_SUCCESS) {
		/* sanity checks */
		if (values[0] == NULL || values[1] == NULL) {
			retcode = IDMAP_ERR_CACHE;
			goto out;
		}

		switch (req->id2.idtype) {
		case IDMAP_SID:
			res->id.idmap_id_u.sid.rid =
				strtoul(values[1], &end, 10);
			res->id.idmap_id_u.sid.prefix = strdup(values[0]);
			if (res->id.idmap_id_u.sid.prefix == NULL) {
				idmapdlog(LOG_ERR, "Out of memory");
				retcode = IDMAP_ERR_MEMORY;
				goto out;
			}

			if (values[4] != NULL)
				res->direction =
				    (strtol(values[4], &end, 10) == 0)?
				    IDMAP_DIRECTION_U2W:IDMAP_DIRECTION_BI;
			else
				res->direction = IDMAP_DIRECTION_U2W;

			if (getname == 0 || values[2] == NULL)
				break;
			str = &req->id2name;
			retcode = idmap_str2utf8(&str, values[2], 0);
			if (retcode != IDMAP_SUCCESS) {
				idmapdlog(LOG_ERR, "Out of memory");
				retcode = IDMAP_ERR_MEMORY;
				goto out;
			}

			if (values[3] == NULL)
				break;
			str = &req->id2domain;
			retcode = idmap_str2utf8(&str, values[3], 0);
			if (retcode != IDMAP_SUCCESS) {
				idmapdlog(LOG_ERR, "Out of memory");
				retcode = IDMAP_ERR_MEMORY;
				goto out;
			}
			break;
		default:
			retcode = IDMAP_ERR_NOTSUPPORTED;
			break;
		}
	}

out:
	if (vm != NULL)
		(void) sqlite_finalize(vm, NULL);
	return (retcode);
}

static idmap_retcode
lookup_cache_name2sid(sqlite *cache, const char *name, const char *domain,
		char **sidprefix, idmap_rid_t *rid, int *type) {
	char		*end;
	char		*sql = NULL;
	const char	**values;
	sqlite_vm	*vm = NULL;
	int		ncol;
	time_t		curtime;
	idmap_retcode	retcode = IDMAP_SUCCESS;

	/* Get current time */
	errno = 0;
	if ((curtime = time(NULL)) == (time_t)-1) {
		idmapdlog(LOG_ERR,
			"Failed to get current time (%s)",
			strerror(errno));
		retcode = IDMAP_ERR_INTERNAL;
		goto out;
	}

	/* SQL to lookup the cache */
	sql = sqlite_mprintf("SELECT sidprefix, rid, type FROM name_cache "
			"WHERE name = %Q AND domain = %Q AND "
			"(expiration = 0 OR expiration ISNULL OR "
			"expiration > %d);",
			name, domain, curtime);
	if (sql == NULL) {
		idmapdlog(LOG_ERR, "Out of memory");
		retcode = IDMAP_ERR_MEMORY;
		goto out;
	}
	retcode = sql_compile_n_step_once(cache, sql, &vm, &ncol, 3, &values);
	sqlite_freemem(sql);

	if (retcode == IDMAP_SUCCESS) {
		if (type != NULL) {
			if (values[2] == NULL) {
				retcode = IDMAP_ERR_CACHE;
				goto out;
			}
			*type = strtol(values[2], &end, 10);
		}

		if (values[0] == NULL || values[1] == NULL) {
			retcode = IDMAP_ERR_CACHE;
			goto out;
		}
		if ((*sidprefix = strdup(values[0])) == NULL) {
			idmapdlog(LOG_ERR, "Out of memory");
			retcode = IDMAP_ERR_MEMORY;
			goto out;
		}
		*rid = strtoul(values[1], &end, 10);
	}

out:
	if (vm != NULL)
		(void) sqlite_finalize(vm, NULL);
	return (retcode);
}

static idmap_retcode
lookup_win_name2sid(const char *name, const char *domain, char **sidprefix,
		idmap_rid_t *rid, int *type) {
	int			ret;
	int			retries = 0;
	idmap_query_state_t	*qs = NULL;
	idmap_retcode		rc, retcode;

	retcode = IDMAP_ERR_NOTFOUND;

retry:
	ret = idmap_lookup_batch_start(_idmapdstate.ad, 1, &qs);
	if (ret != 0) {
		idmapdlog(LOG_ERR,
		"Failed to create name2sid batch for AD lookup");
		return (IDMAP_ERR_INTERNAL);
	}

	retcode = idmap_name2sid_batch_add1(qs, name, domain, sidprefix,
					rid, type, &rc);
	if (retcode == IDMAP_ERR_RETRIABLE_NET_ERR)
		goto out;

	if (retcode != IDMAP_SUCCESS) {
		idmapdlog(LOG_ERR,
		"Failed to batch name2sid for AD lookup");
		idmap_lookup_release_batch(&qs);
		return (IDMAP_ERR_INTERNAL);
	}

out:
	if (retcode == IDMAP_ERR_RETRIABLE_NET_ERR)
		idmap_lookup_release_batch(&qs);
	else
		retcode = idmap_lookup_batch_end(&qs, NULL);

	if (retcode == IDMAP_ERR_RETRIABLE_NET_ERR && retries++ < 2)
		goto retry;

	if (retcode != IDMAP_SUCCESS) {
		idmapdlog(LOG_NOTICE, "Windows user/group name to SID lookup "
		    "failed");
		return (retcode);
	} else
		return (rc);
	/* NOTREACHED */
}

static idmap_retcode
lookup_name2sid(sqlite *cache, const char *name, const char *domain,
		int *is_user, char **sidprefix, idmap_rid_t *rid,
		idmap_mapping *req) {
	int		type;
	idmap_retcode	retcode;

	/* Lookup name@domain to sid in the well-known sids table */
	retcode = lookup_wksids_name2sid(name, sidprefix, rid, &type);
	if (retcode == IDMAP_SUCCESS) {
		req->direction |= _IDMAP_F_S2N_CACHE;
		goto out;
	} else if (retcode != IDMAP_ERR_NOTFOUND) {
		return (retcode);
	}

	/* Lookup name@domain to sid in cache */
	retcode = lookup_cache_name2sid(cache, name, domain, sidprefix,
		rid, &type);
	if (retcode == IDMAP_ERR_NOTFOUND) {
		/* Lookup Windows NT/AD to map name@domain to sid */
		retcode = lookup_win_name2sid(name, domain, sidprefix, rid,
			&type);
		if (retcode != IDMAP_SUCCESS)
			return (retcode);
		req->direction |= _IDMAP_F_S2N_AD;
	} else if (retcode != IDMAP_SUCCESS) {
		return (retcode);
	} else {
		/* Set flag */
		req->direction |= _IDMAP_F_S2N_CACHE;
	}

out:
	/*
	 * Entry found (cache or Windows lookup)
	 * is_user is both input as well as output parameter
	 */
	if (*is_user == 1) {
		if (type != _IDMAP_T_USER)
			return (IDMAP_ERR_NOTUSER);
	} else if (*is_user == 0) {
		if (type != _IDMAP_T_GROUP)
			return (IDMAP_ERR_NOTGROUP);
	} else if (*is_user == -1) {
		/* Caller wants to know if its user or group */
		if (type == _IDMAP_T_USER)
			*is_user = 1;
		else if (type == _IDMAP_T_GROUP)
			*is_user = 0;
		else
			return (IDMAP_ERR_SID);
	}

	return (retcode);
}

static idmap_retcode
name_based_mapping_pid2sid(sqlite *db, sqlite *cache, const char *unixname,
		int is_user, idmap_mapping *req, idmap_id_res *res) {
	const char	*winname, *windomain;
	char		*mapping_domain = NULL;
	char		*sql = NULL, *errmsg = NULL;
	idmap_retcode	retcode;
	char		*end;
	const char	**values;
	sqlite_vm	*vm = NULL;
	idmap_utf8str	*str;
	int		ncol, r;
	const char	*me = "name_based_mapping_pid2sid";

	RDLOCK_CONFIG();
	if (_idmapdstate.cfg->pgcfg.mapping_domain != NULL) {
		mapping_domain =
			strdup(_idmapdstate.cfg->pgcfg.mapping_domain);
		if (mapping_domain == NULL) {
			UNLOCK_CONFIG();
			idmapdlog(LOG_ERR, "Out of memory");
			retcode = IDMAP_ERR_MEMORY;
			goto out;
		}
	}
	UNLOCK_CONFIG();

	sql = sqlite_mprintf(
		"SELECT winname, windomain, w2u_order FROM namerules WHERE "
		"u2w_order > 0 AND is_user = %d AND "
		"(unixname = %Q OR unixname = '*') "
		"ORDER BY u2w_order ASC;",
		is_user, unixname);
	if (sql == NULL) {
		idmapdlog(LOG_ERR, "Out of memory");
		retcode = IDMAP_ERR_MEMORY;
		goto out;
	}

	if (sqlite_compile(db, sql, NULL, &vm, &errmsg) != SQLITE_OK) {
		retcode = IDMAP_ERR_INTERNAL;
		idmapdlog(LOG_ERR,
			"%s: database error (%s)",
			me, CHECK_NULL(errmsg));
		sqlite_freemem(errmsg);
		goto out;
	}

	for (;;) {
		r = sqlite_step(vm, &ncol, &values, NULL);
		assert(r != SQLITE_LOCKED && r != SQLITE_BUSY);
		if (r == SQLITE_ROW) {
			if (ncol < 3) {
				retcode = IDMAP_ERR_INTERNAL;
				goto out;
			}
			if (values[0] == NULL) {
				/* values [1] and [2] can be null */
				retcode = IDMAP_ERR_INTERNAL;
				goto out;
			}
			if (EMPTY_NAME(values[0])) {
				retcode = IDMAP_ERR_NOMAPPING;
				goto out;
			}
			winname = (values[0][0] == '*')?unixname:values[0];
			if (values[1] != NULL)
				windomain = values[1];
			else if (mapping_domain != NULL)
				windomain = mapping_domain;
			else {
				idmapdlog(LOG_ERR,
					"%s: no domain", me);
				retcode = IDMAP_ERR_DOMAIN_NOTFOUND;
				goto out;
			}
			/* Lookup winname@domain to sid */
			retcode = lookup_name2sid(cache, winname, windomain,
				&is_user, &res->id.idmap_id_u.sid.prefix,
				&res->id.idmap_id_u.sid.rid, req);
			if (retcode == IDMAP_ERR_NOTFOUND) {
				if (winname == unixname)
					continue;
				else
					retcode = IDMAP_ERR_NOMAPPING;
			}
			goto out;
		} else if (r == SQLITE_DONE) {
			retcode = IDMAP_ERR_NOTFOUND;
			goto out;
		} else {
			(void) sqlite_finalize(vm, &errmsg);
			vm = NULL;
			idmapdlog(LOG_ERR,
				"%s: database error (%s)",
				me, CHECK_NULL(errmsg));
			sqlite_freemem(errmsg);
			retcode = IDMAP_ERR_INTERNAL;
			goto out;
		}
	}

out:
	if (sql != NULL)
		sqlite_freemem(sql);
	if (retcode == IDMAP_SUCCESS) {
		if (values[2] != NULL)
			res->direction =
			    (strtol(values[2], &end, 10) == 0)?
			    IDMAP_DIRECTION_U2W:IDMAP_DIRECTION_BI;
		else
			res->direction = IDMAP_DIRECTION_U2W;
		str = &req->id2name;
		retcode = idmap_str2utf8(&str, winname, 0);
		if (retcode == IDMAP_SUCCESS) {
			str = &req->id2domain;
			if (windomain == mapping_domain) {
				(void) idmap_str2utf8(&str, windomain, 1);
				mapping_domain = NULL;
			} else
				retcode = idmap_str2utf8(&str, windomain, 0);
		}
	}
	if (vm != NULL)
		(void) sqlite_finalize(vm, NULL);
	if (mapping_domain != NULL)
		free(mapping_domain);
	return (retcode);
}

idmap_retcode
pid2sid_first_pass(lookup_state_t *state, sqlite *cache, sqlite *db,
		idmap_mapping *req, idmap_id_res *res, int is_user,
		int getname) {
	char		*unixname = NULL;
	struct passwd	pwd;
	struct group	grp;
	idmap_utf8str	*str;
	char		buf[1024];
	int		errnum;
	idmap_retcode	retcode = IDMAP_SUCCESS;
	const char	*me = "pid2sid";

	req->direction = _IDMAP_F_DONE;
	res->id.idtype = req->id2.idtype;

	/* Lookup well-known SIDs */
	retcode = lookup_wksids_pid2sid(req, res, is_user);
	if (retcode != IDMAP_ERR_NOTFOUND)
		goto out;

	/* Lookup pid to sid in cache */
	retcode = lookup_cache_pid2sid(cache, req, res, is_user, getname);
	if (retcode != IDMAP_ERR_NOTFOUND)
		goto out;

	/* Ephemeral ids cannot be allocated during pid2sid */
	if (IS_EPHEMERAL(req->id1.idmap_id_u.uid)) {
		retcode = IDMAP_ERR_NOMAPPING;
		goto out;
	}

	if (DO_NOT_ALLOC_NEW_ID_MAPPING(req) || AVOID_NAMESERVICE(req)) {
		retcode = IDMAP_ERR_NOMAPPING;
		goto out;
	}

	/* uid/gid to name */
	if (req->id1name.idmap_utf8str_val != NULL) {
		unixname = req->id1name.idmap_utf8str_val;
	} if (is_user) {
		errno = 0;
		if (getpwuid_r(req->id1.idmap_id_u.uid, &pwd, buf,
				sizeof (buf)) == NULL) {
			errnum = errno;
			idmapdlog(LOG_WARNING,
			"%s: getpwuid_r(%u) failed (%s).",
				me, req->id1.idmap_id_u.uid,
				errnum?strerror(errnum):"not found");
			retcode = (errnum == 0)?IDMAP_ERR_NOTFOUND:
					IDMAP_ERR_INTERNAL;
			goto fallback_localsid;
		}
		unixname = pwd.pw_name;
	} else {
		errno = 0;
		if (getgrgid_r(req->id1.idmap_id_u.gid, &grp, buf,
				sizeof (buf)) == NULL) {
			errnum = errno;
			idmapdlog(LOG_WARNING,
			"%s: getgrgid_r(%u) failed (%s).",
				me, req->id1.idmap_id_u.gid,
				errnum?strerror(errnum):"not found");
			retcode = (errnum == 0)?IDMAP_ERR_NOTFOUND:
					IDMAP_ERR_INTERNAL;
			goto fallback_localsid;
		}
		unixname = grp.gr_name;
	}

	/* Name-based mapping */
	retcode = name_based_mapping_pid2sid(db, cache, unixname, is_user,
		req, res);
	if (retcode == IDMAP_ERR_NOTFOUND) {
		retcode = generate_localsid(req, res, is_user);
		goto out;
	} else if (retcode == IDMAP_SUCCESS)
		goto out;

fallback_localsid:
	/*
	 * Here we generate localsid as fallback id on errors. Our
	 * return status is the error that's been previously assigned.
	 */
	(void) generate_localsid(req, res, is_user);

out:
	if (retcode == IDMAP_SUCCESS) {
		if (req->id1name.idmap_utf8str_val == NULL &&
		    unixname != NULL) {
			str = &req->id1name;
			retcode = idmap_str2utf8(&str, unixname, 0);
		}
	}
	if (req->direction != _IDMAP_F_DONE)
		state->pid2sid_done = FALSE;
	res->retcode = idmap_stat4prot(retcode);
	return (retcode);
}

static idmap_retcode
lookup_win_sid2name(const char *sidprefix, idmap_rid_t rid, char **name,
		char **domain, int *type) {
	int			ret;
	idmap_query_state_t	*qs = NULL;
	idmap_retcode		rc, retcode;

	retcode = IDMAP_ERR_NOTFOUND;

	ret = idmap_lookup_batch_start(_idmapdstate.ad, 1, &qs);
	if (ret != 0) {
		idmapdlog(LOG_ERR,
		"Failed to create sid2name batch for AD lookup");
		retcode = IDMAP_ERR_INTERNAL;
		goto out;
	}

	ret = idmap_sid2name_batch_add1(
			qs, sidprefix, &rid, name, domain, type, &rc);
	if (ret != 0) {
		idmapdlog(LOG_ERR,
		"Failed to batch sid2name for AD lookup");
		retcode = IDMAP_ERR_INTERNAL;
		goto out;
	}

out:
	if (qs != NULL) {
		ret = idmap_lookup_batch_end(&qs, NULL);
		if (ret != 0) {
			idmapdlog(LOG_ERR,
			"Failed to execute sid2name AD lookup");
			retcode = IDMAP_ERR_INTERNAL;
		} else
			retcode = rc;
	}

	return (retcode);
}

static int
copy_mapping_request(idmap_mapping *mapping, idmap_mapping *request)
{
	(void) memset(mapping, 0, sizeof (*mapping));

	mapping->flag = request->flag;
	mapping->direction = request->direction;
	mapping->id2.idtype = request->id2.idtype;

	mapping->id1.idtype = request->id1.idtype;
	if (request->id1.idtype == IDMAP_SID) {
		mapping->id1.idmap_id_u.sid.rid =
		    request->id1.idmap_id_u.sid.rid;
		if (!EMPTY_STRING(request->id1.idmap_id_u.sid.prefix)) {
			mapping->id1.idmap_id_u.sid.prefix =
			    strdup(request->id1.idmap_id_u.sid.prefix);
			if (mapping->id1.idmap_id_u.sid.prefix == NULL)
				return (-1);
		}
	} else {
		mapping->id1.idmap_id_u.uid = request->id1.idmap_id_u.uid;
	}

	mapping->id1domain.idmap_utf8str_len =
	    request->id1domain.idmap_utf8str_len;
	if (mapping->id1domain.idmap_utf8str_len) {
		mapping->id1domain.idmap_utf8str_val =
		    strdup(request->id1domain.idmap_utf8str_val);
		if (mapping->id1domain.idmap_utf8str_val == NULL)
			return (-1);
	}

	mapping->id1name.idmap_utf8str_len  =
	    request->id1name.idmap_utf8str_len;
	if (mapping->id1name.idmap_utf8str_len) {
		mapping->id1name.idmap_utf8str_val =
		    strdup(request->id1name.idmap_utf8str_val);
		if (mapping->id1name.idmap_utf8str_val == NULL)
			return (-1);
	}

	/* We don't need the rest of the request i.e request->id2 */
	return (0);

errout:
	if (mapping->id1.idmap_id_u.sid.prefix != NULL) {
		free(mapping->id1.idmap_id_u.sid.prefix);
		mapping->id1.idmap_id_u.sid.prefix = NULL;
	}

	if (mapping->id1domain.idmap_utf8str_val != NULL) {
		free(mapping->id1domain.idmap_utf8str_val);
		mapping->id1domain.idmap_utf8str_val = NULL;
		mapping->id1domain.idmap_utf8str_len = 0;
	}

	if (mapping->id1name.idmap_utf8str_val != NULL) {
		free(mapping->id1name.idmap_utf8str_val);
		mapping->id1name.idmap_utf8str_val = NULL;
		mapping->id1name.idmap_utf8str_len = 0;
	}

	(void) memset(mapping, 0, sizeof (*mapping));
	return (-1);
}


idmap_retcode
get_w2u_mapping(sqlite *cache, sqlite *db, idmap_mapping *request,
		idmap_mapping *mapping) {
	idmap_id_res	idres;
	lookup_state_t	state;
	idmap_utf8str	*str;
	char		*cp;
	int		is_user;
	idmap_retcode	retcode;
	const char	*winname, *windomain;

	(void) memset(&idres, 0, sizeof (idres));
	(void) memset(&state, 0, sizeof (state));

	if (request->id2.idtype == IDMAP_UID)
		is_user = 1;
	else if (request->id2.idtype == IDMAP_GID)
		is_user = 0;
	else if (request->id2.idtype == IDMAP_POSIXID)
		is_user = -1;
	else {
		retcode = IDMAP_ERR_IDTYPE;
		goto out;
	}

	/* Copy data from request to result */
	if (copy_mapping_request(mapping, request) < 0) {
		retcode = IDMAP_ERR_MEMORY;
		goto out;
	}

	winname = mapping->id1name.idmap_utf8str_val;
	windomain = mapping->id1domain.idmap_utf8str_val;

	if (winname == NULL && windomain != NULL) {
		retcode = IDMAP_ERR_ARG;
		goto out;
	}

	if (winname != NULL && windomain == NULL) {
		str = &mapping->id1domain;

		if ((cp = strchr(winname, '@')) != NULL) {
			/*
			 * if winname is qualified with a domain, use it.
			 */
			*cp = '\0';
			retcode = idmap_str2utf8(&str, cp + 1, 0);
		} else if (_idmapdstate.cfg->pgcfg.mapping_domain != NULL) {
			/*
			 * otherwise use the mapping domain
			 */
			RDLOCK_CONFIG();
			retcode = idmap_str2utf8(&str,
				_idmapdstate.cfg->pgcfg.mapping_domain, 0);
			UNLOCK_CONFIG();
		} else
			retcode = IDMAP_SUCCESS;

		if (retcode != IDMAP_SUCCESS) {
			idmapdlog(LOG_ERR, "Out of memory");
			goto out;
		}
		windomain = mapping->id1domain.idmap_utf8str_val;
	}

	if (winname != NULL && mapping->id1.idmap_id_u.sid.prefix == NULL) {
		retcode = lookup_name2sid(cache, winname, windomain,
			&is_user, &mapping->id1.idmap_id_u.sid.prefix,
			&mapping->id1.idmap_id_u.sid.rid, mapping);
		if (retcode != IDMAP_SUCCESS)
			goto out;
		if (mapping->id2.idtype == IDMAP_POSIXID)
			mapping->id2.idtype = is_user?IDMAP_UID:IDMAP_GID;
	}

	state.sid2pid_done = TRUE;
	retcode = sid2pid_first_pass(&state, cache, mapping, &idres);
	if (IDMAP_ERROR(retcode) || state.sid2pid_done == TRUE)
		goto out;

	if (state.ad_nqueries) {
		/* sid2name AD lookup */
		retcode = lookup_win_sid2name(
			mapping->id1.idmap_id_u.sid.prefix,
			mapping->id1.idmap_id_u.sid.rid,
			&mapping->id1name.idmap_utf8str_val,
			&mapping->id1domain.idmap_utf8str_val,
			(int *)&idres.id.idtype);

		idres.retcode = retcode;
	}

	state.sid2pid_done = TRUE;
	retcode = sid2pid_second_pass(&state, cache, db, mapping, &idres);
	if (IDMAP_ERROR(retcode) || state.sid2pid_done == TRUE)
		goto out;

	/* Update cache */
	(void) update_cache_sid2pid(&state, cache, mapping, &idres);

out:
	if (retcode == IDMAP_SUCCESS) {
		mapping->direction = idres.direction;
		mapping->id2 = idres.id;
		(void) memset(&idres, 0, sizeof (idres));
	} else {
		mapping->id2.idmap_id_u.uid = UID_NOBODY;
	}
	xdr_free(xdr_idmap_id_res, (caddr_t)&idres);
	return (retcode);
}

idmap_retcode
get_u2w_mapping(sqlite *cache, sqlite *db, idmap_mapping *request,
		idmap_mapping *mapping, int is_user) {
	idmap_id_res	idres;
	lookup_state_t	state;
	struct passwd	pwd;
	struct group	grp;
	char		buf[1024];
	int		errnum;
	idmap_retcode	retcode;
	const char	*unixname;
	const char	*me = "get_u2w_mapping";

	/*
	 * In order to re-use the pid2sid code, we convert
	 * our input data into structs that are expected by
	 * pid2sid_first_pass.
	 */

	(void) memset(&idres, 0, sizeof (idres));
	(void) memset(&state, 0, sizeof (state));

	/* Copy data from request to result */
	if (copy_mapping_request(mapping, request) < 0) {
		retcode = IDMAP_ERR_MEMORY;
		goto out;
	}

	unixname = mapping->id1name.idmap_utf8str_val;

	if (unixname == NULL && mapping->id1.idmap_id_u.uid == SENTINEL_PID) {
		retcode = IDMAP_ERR_ARG;
		goto out;
	}

	if (unixname != NULL && mapping->id1.idmap_id_u.uid == SENTINEL_PID) {
		/* Get uid/gid by name */
		if (is_user) {
			errno = 0;
			if (getpwnam_r(unixname, &pwd, buf,
					sizeof (buf)) == NULL) {
				errnum = errno;
				idmapdlog(LOG_WARNING,
				"%s: getpwnam_r(%s) failed (%s).",
					me, unixname,
					errnum?strerror(errnum):"not found");
				retcode = (errnum == 0)?IDMAP_ERR_NOTFOUND:
						IDMAP_ERR_INTERNAL;
				goto out;
			}
			mapping->id1.idmap_id_u.uid = pwd.pw_uid;
		} else {
			errno = 0;
			if (getgrnam_r(unixname, &grp, buf,
					sizeof (buf)) == NULL) {
				errnum = errno;
				idmapdlog(LOG_WARNING,
				"%s: getgrnam_r(%s) failed (%s).",
					me, unixname,
					errnum?strerror(errnum):"not found");
				retcode = (errnum == 0)?IDMAP_ERR_NOTFOUND:
						IDMAP_ERR_INTERNAL;
				goto out;
			}
			mapping->id1.idmap_id_u.gid = grp.gr_gid;
		}
	}

	state.pid2sid_done = TRUE;
	retcode = pid2sid_first_pass(&state, cache, db, mapping, &idres,
			is_user, 1);
	if (IDMAP_ERROR(retcode) || state.pid2sid_done == TRUE)
		goto out;

	/* Update cache */
	(void) update_cache_pid2sid(&state, cache, mapping, &idres);

out:
	mapping->direction = idres.direction;
	mapping->id2 = idres.id;
	(void) memset(&idres, 0, sizeof (idres));
	xdr_free(xdr_idmap_id_res, (caddr_t)&idres);
	return (retcode);
}
