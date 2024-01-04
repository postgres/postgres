/*-------------------------------------------------------------------------
 *
 * pgtz.c
 *	  Timezone Library Integration Functions
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/timezone/pgtz.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>
#include <fcntl.h>
#include <time.h>

#include "common/file_utils.h"
#include "datatype/timestamp.h"
#include "miscadmin.h"
#include "pgtz.h"
#include "storage/fd.h"
#include "utils/hsearch.h"


/* Current session timezone (controlled by TimeZone GUC) */
pg_tz	   *session_timezone = NULL;

/* Current log timezone (controlled by log_timezone GUC) */
pg_tz	   *log_timezone = NULL;


static bool scan_directory_ci(const char *dirname,
							  const char *fname, int fnamelen,
							  char *canonname, int canonnamelen);


/*
 * Return full pathname of timezone data directory
 */
static const char *
pg_TZDIR(void)
{
#ifndef SYSTEMTZDIR
	/* normal case: timezone stuff is under our share dir */
	static bool done_tzdir = false;
	static char tzdir[MAXPGPATH];

	if (done_tzdir)
		return tzdir;

	get_share_path(my_exec_path, tzdir);
	strlcpy(tzdir + strlen(tzdir), "/timezone", MAXPGPATH - strlen(tzdir));

	done_tzdir = true;
	return tzdir;
#else
	/* we're configured to use system's timezone database */
	return SYSTEMTZDIR;
#endif
}


/*
 * Given a timezone name, open() the timezone data file.  Return the
 * file descriptor if successful, -1 if not.
 *
 * The input name is searched for case-insensitively (we assume that the
 * timezone database does not contain case-equivalent names).
 *
 * If "canonname" is not NULL, then on success the canonical spelling of the
 * given name is stored there (the buffer must be > TZ_STRLEN_MAX bytes!).
 */
int
pg_open_tzfile(const char *name, char *canonname)
{
	const char *fname;
	char		fullname[MAXPGPATH];
	int			fullnamelen;
	int			orignamelen;

	/* Initialize fullname with base name of tzdata directory */
	strlcpy(fullname, pg_TZDIR(), sizeof(fullname));
	orignamelen = fullnamelen = strlen(fullname);

	if (fullnamelen + 1 + strlen(name) >= MAXPGPATH)
		return -1;				/* not gonna fit */

	/*
	 * If the caller doesn't need the canonical spelling, first just try to
	 * open the name as-is.  This can be expected to succeed if the given name
	 * is already case-correct, or if the filesystem is case-insensitive; and
	 * we don't need to distinguish those situations if we aren't tasked with
	 * reporting the canonical spelling.
	 */
	if (canonname == NULL)
	{
		int			result;

		fullname[fullnamelen] = '/';
		/* test above ensured this will fit: */
		strcpy(fullname + fullnamelen + 1, name);
		result = open(fullname, O_RDONLY | PG_BINARY, 0);
		if (result >= 0)
			return result;
		/* If that didn't work, fall through to do it the hard way */
		fullname[fullnamelen] = '\0';
	}

	/*
	 * Loop to split the given name into directory levels; for each level,
	 * search using scan_directory_ci().
	 */
	fname = name;
	for (;;)
	{
		const char *slashptr;
		int			fnamelen;

		slashptr = strchr(fname, '/');
		if (slashptr)
			fnamelen = slashptr - fname;
		else
			fnamelen = strlen(fname);
		if (!scan_directory_ci(fullname, fname, fnamelen,
							   fullname + fullnamelen + 1,
							   MAXPGPATH - fullnamelen - 1))
			return -1;
		fullname[fullnamelen++] = '/';
		fullnamelen += strlen(fullname + fullnamelen);
		if (slashptr)
			fname = slashptr + 1;
		else
			break;
	}

	if (canonname)
		strlcpy(canonname, fullname + orignamelen + 1, TZ_STRLEN_MAX + 1);

	return open(fullname, O_RDONLY | PG_BINARY, 0);
}


/*
 * Scan specified directory for a case-insensitive match to fname
 * (of length fnamelen --- fname may not be null terminated!).  If found,
 * copy the actual filename into canonname and return true.
 */
static bool
scan_directory_ci(const char *dirname, const char *fname, int fnamelen,
				  char *canonname, int canonnamelen)
{
	bool		found = false;
	DIR		   *dirdesc;
	struct dirent *direntry;

	dirdesc = AllocateDir(dirname);

	while ((direntry = ReadDirExtended(dirdesc, dirname, LOG)) != NULL)
	{
		/*
		 * Ignore . and .., plus any other "hidden" files.  This is a security
		 * measure to prevent access to files outside the timezone directory.
		 */
		if (direntry->d_name[0] == '.')
			continue;

		if (strlen(direntry->d_name) == fnamelen &&
			pg_strncasecmp(direntry->d_name, fname, fnamelen) == 0)
		{
			/* Found our match */
			strlcpy(canonname, direntry->d_name, canonnamelen);
			found = true;
			break;
		}
	}

	FreeDir(dirdesc);

	return found;
}


/*
 * We keep loaded timezones in a hashtable so we don't have to
 * load and parse the TZ definition file every time one is selected.
 * Because we want timezone names to be found case-insensitively,
 * the hash key is the uppercased name of the zone.
 */
typedef struct
{
	/* tznameupper contains the all-upper-case name of the timezone */
	char		tznameupper[TZ_STRLEN_MAX + 1];
	pg_tz		tz;
} pg_tz_cache;

static HTAB *timezone_cache = NULL;


static bool
init_timezone_hashtable(void)
{
	HASHCTL		hash_ctl;

	hash_ctl.keysize = TZ_STRLEN_MAX + 1;
	hash_ctl.entrysize = sizeof(pg_tz_cache);

	timezone_cache = hash_create("Timezones",
								 4,
								 &hash_ctl,
								 HASH_ELEM | HASH_STRINGS);
	if (!timezone_cache)
		return false;

	return true;
}

/*
 * Load a timezone from file or from cache.
 * Does not verify that the timezone is acceptable!
 *
 * "GMT" is always interpreted as the tzparse() definition, without attempting
 * to load a definition from the filesystem.  This has a number of benefits:
 * 1. It's guaranteed to succeed, so we don't have the failure mode wherein
 * the bootstrap default timezone setting doesn't work (as could happen if
 * the OS attempts to supply a leap-second-aware version of "GMT").
 * 2. Because we aren't accessing the filesystem, we can safely initialize
 * the "GMT" zone definition before my_exec_path is known.
 * 3. It's quick enough that we don't waste much time when the bootstrap
 * default timezone setting is later overridden from postgresql.conf.
 */
pg_tz *
pg_tzset(const char *tzname)
{
	pg_tz_cache *tzp;
	struct state tzstate;
	char		uppername[TZ_STRLEN_MAX + 1];
	char		canonname[TZ_STRLEN_MAX + 1];
	char	   *p;

	if (strlen(tzname) > TZ_STRLEN_MAX)
		return NULL;			/* not going to fit */

	if (!timezone_cache)
		if (!init_timezone_hashtable())
			return NULL;

	/*
	 * Upcase the given name to perform a case-insensitive hashtable search.
	 * (We could alternatively downcase it, but we prefer upcase so that we
	 * can get consistently upcased results from tzparse() in case the name is
	 * a POSIX-style timezone spec.)
	 */
	p = uppername;
	while (*tzname)
		*p++ = pg_toupper((unsigned char) *tzname++);
	*p = '\0';

	tzp = (pg_tz_cache *) hash_search(timezone_cache,
									  uppername,
									  HASH_FIND,
									  NULL);
	if (tzp)
	{
		/* Timezone found in cache, nothing more to do */
		return &tzp->tz;
	}

	/*
	 * "GMT" is always sent to tzparse(), as per discussion above.
	 */
	if (strcmp(uppername, "GMT") == 0)
	{
		if (!tzparse(uppername, &tzstate, true))
		{
			/* This really, really should not happen ... */
			elog(ERROR, "could not initialize GMT time zone");
		}
		/* Use uppercase name as canonical */
		strcpy(canonname, uppername);
	}
	else if (tzload(uppername, canonname, &tzstate, true) != 0)
	{
		if (uppername[0] == ':' || !tzparse(uppername, &tzstate, false))
		{
			/* Unknown timezone. Fail our call instead of loading GMT! */
			return NULL;
		}
		/* For POSIX timezone specs, use uppercase name as canonical */
		strcpy(canonname, uppername);
	}

	/* Save timezone in the cache */
	tzp = (pg_tz_cache *) hash_search(timezone_cache,
									  uppername,
									  HASH_ENTER,
									  NULL);

	/* hash_search already copied uppername into the hash key */
	strcpy(tzp->tz.TZname, canonname);
	memcpy(&tzp->tz.state, &tzstate, sizeof(tzstate));

	return &tzp->tz;
}

/*
 * Load a fixed-GMT-offset timezone.
 * This is used for SQL-spec SET TIME ZONE INTERVAL 'foo' cases.
 * It's otherwise equivalent to pg_tzset().
 *
 * The GMT offset is specified in seconds, positive values meaning west of
 * Greenwich (ie, POSIX not ISO sign convention).  However, we use ISO
 * sign convention in the displayable abbreviation for the zone.
 *
 * Caution: this can fail (return NULL) if the specified offset is outside
 * the range allowed by the zic library.
 */
pg_tz *
pg_tzset_offset(long gmtoffset)
{
	long		absoffset = (gmtoffset < 0) ? -gmtoffset : gmtoffset;
	char		offsetstr[64];
	char		tzname[128];

	snprintf(offsetstr, sizeof(offsetstr),
			 "%02ld", absoffset / SECS_PER_HOUR);
	absoffset %= SECS_PER_HOUR;
	if (absoffset != 0)
	{
		snprintf(offsetstr + strlen(offsetstr),
				 sizeof(offsetstr) - strlen(offsetstr),
				 ":%02ld", absoffset / SECS_PER_MINUTE);
		absoffset %= SECS_PER_MINUTE;
		if (absoffset != 0)
			snprintf(offsetstr + strlen(offsetstr),
					 sizeof(offsetstr) - strlen(offsetstr),
					 ":%02ld", absoffset);
	}
	if (gmtoffset > 0)
		snprintf(tzname, sizeof(tzname), "<-%s>+%s",
				 offsetstr, offsetstr);
	else
		snprintf(tzname, sizeof(tzname), "<+%s>-%s",
				 offsetstr, offsetstr);

	return pg_tzset(tzname);
}


/*
 * Initialize timezone library
 *
 * This is called before GUC variable initialization begins.  Its purpose
 * is to ensure that log_timezone has a valid value before any logging GUC
 * variables could become set to values that require elog.c to provide
 * timestamps (e.g., log_line_prefix).  We may as well initialize
 * session_timezone to something valid, too.
 */
void
pg_timezone_initialize(void)
{
	/*
	 * We may not yet know where PGSHAREDIR is (in particular this is true in
	 * an EXEC_BACKEND subprocess).  So use "GMT", which pg_tzset forces to be
	 * interpreted without reference to the filesystem.  This corresponds to
	 * the bootstrap default for these variables in guc_tables.c, although in
	 * principle it could be different.
	 */
	session_timezone = pg_tzset("GMT");
	log_timezone = session_timezone;
}


/*
 * Functions to enumerate available timezones
 *
 * Note that pg_tzenumerate_next() will return a pointer into the pg_tzenum
 * structure, so the data is only valid up to the next call.
 *
 * All data is allocated using palloc in the current context.
 */
#define MAX_TZDIR_DEPTH 10

struct pg_tzenum
{
	int			baselen;
	int			depth;
	DIR		   *dirdesc[MAX_TZDIR_DEPTH];
	char	   *dirname[MAX_TZDIR_DEPTH];
	struct pg_tz tz;
};

/* typedef pg_tzenum is declared in pgtime.h */

pg_tzenum *
pg_tzenumerate_start(void)
{
	pg_tzenum  *ret = (pg_tzenum *) palloc0(sizeof(pg_tzenum));
	char	   *startdir = pstrdup(pg_TZDIR());

	ret->baselen = strlen(startdir) + 1;
	ret->depth = 0;
	ret->dirname[0] = startdir;
	ret->dirdesc[0] = AllocateDir(startdir);
	if (!ret->dirdesc[0])
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open directory \"%s\": %m", startdir)));
	return ret;
}

void
pg_tzenumerate_end(pg_tzenum *dir)
{
	while (dir->depth >= 0)
	{
		FreeDir(dir->dirdesc[dir->depth]);
		pfree(dir->dirname[dir->depth]);
		dir->depth--;
	}
	pfree(dir);
}

pg_tz *
pg_tzenumerate_next(pg_tzenum *dir)
{
	while (dir->depth >= 0)
	{
		struct dirent *direntry;
		char		fullname[MAXPGPATH * 2];

		direntry = ReadDir(dir->dirdesc[dir->depth], dir->dirname[dir->depth]);

		if (!direntry)
		{
			/* End of this directory */
			FreeDir(dir->dirdesc[dir->depth]);
			pfree(dir->dirname[dir->depth]);
			dir->depth--;
			continue;
		}

		if (direntry->d_name[0] == '.')
			continue;

		snprintf(fullname, sizeof(fullname), "%s/%s",
				 dir->dirname[dir->depth], direntry->d_name);

		if (get_dirent_type(fullname, direntry, true, ERROR) == PGFILETYPE_DIR)
		{
			/* Step into the subdirectory */
			if (dir->depth >= MAX_TZDIR_DEPTH - 1)
				ereport(ERROR,
						(errmsg_internal("timezone directory stack overflow")));
			dir->depth++;
			dir->dirname[dir->depth] = pstrdup(fullname);
			dir->dirdesc[dir->depth] = AllocateDir(fullname);
			if (!dir->dirdesc[dir->depth])
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not open directory \"%s\": %m",
								fullname)));

			/* Start over reading in the new directory */
			continue;
		}

		/*
		 * Load this timezone using tzload() not pg_tzset(), so we don't fill
		 * the cache.  Also, don't ask for the canonical spelling: we already
		 * know it, and pg_open_tzfile's way of finding it out is pretty
		 * inefficient.
		 */
		if (tzload(fullname + dir->baselen, NULL, &dir->tz.state, true) != 0)
		{
			/* Zone could not be loaded, ignore it */
			continue;
		}

		if (!pg_tz_acceptable(&dir->tz))
		{
			/* Ignore leap-second zones */
			continue;
		}

		/* OK, return the canonical zone name spelling. */
		strlcpy(dir->tz.TZname, fullname + dir->baselen,
				sizeof(dir->tz.TZname));

		/* Timezone loaded OK. */
		return &dir->tz;
	}

	/* Nothing more found */
	return NULL;
}
