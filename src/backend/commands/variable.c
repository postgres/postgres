/*-------------------------------------------------------------------------
 *
 * variable.c
 *		Routines for handling specialized SET variables.
 *
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/commands/variable.c,v 1.77 2003/05/22 17:13:08 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <ctype.h>
#include <time.h>

#include "access/xact.h"
#include "catalog/pg_shadow.h"
#include "commands/variable.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/syscache.h"
#include "utils/tqual.h"
#include "mb/pg_wchar.h"

/*
 * Some systems have tzname[] but don't declare it in <time.h>.  Use this
 * to duplicate the test in AC_STRUCT_TIMEZONE.
 */
#ifdef HAVE_TZNAME
#ifndef tzname /* For SGI.  */
extern char *tzname[];
#endif
#endif


/*
 * DATESTYLE
 */

/*
 * assign_datestyle: GUC assign_hook for datestyle
 */
const char *
assign_datestyle(const char *value, bool doit, bool interactive)
{
	int			newDateStyle = DateStyle;
	bool		newEuroDates = EuroDates;
	bool		ok = true;
	int			dcnt = 0,
				ecnt = 0;
	char	   *rawstring;
	char	   *result;
	List	   *elemlist;
	List	   *l;

	/* Need a modifiable copy of string */
	rawstring = pstrdup(value);

	/* Parse string into list of identifiers */
	if (!SplitIdentifierString(rawstring, ',', &elemlist))
	{
		/* syntax error in list */
		pfree(rawstring);
		freeList(elemlist);
		if (interactive)
			elog(ERROR, "SET DATESTYLE: invalid list syntax");
		return NULL;
	}

	foreach(l, elemlist)
	{
		char	   *tok = (char *) lfirst(l);

		/* Ugh. Somebody ought to write a table driven version -- mjl */

		if (strcasecmp(tok, "ISO") == 0)
		{
			newDateStyle = USE_ISO_DATES;
			dcnt++;
		}
		else if (strcasecmp(tok, "SQL") == 0)
		{
			newDateStyle = USE_SQL_DATES;
			dcnt++;
		}
		else if (strncasecmp(tok, "POSTGRESQL", 8) == 0)
		{
			newDateStyle = USE_POSTGRES_DATES;
			dcnt++;
		}
		else if (strcasecmp(tok, "GERMAN") == 0)
		{
			newDateStyle = USE_GERMAN_DATES;
			dcnt++;
			if ((ecnt > 0) && (!newEuroDates))
				ok = false;
			newEuroDates = TRUE;
		}
		else if (strncasecmp(tok, "EURO", 4) == 0)
		{
			newEuroDates = TRUE;
			ecnt++;
		}
		else if (strcasecmp(tok, "US") == 0
				 || strncasecmp(tok, "NONEURO", 7) == 0)
		{
			newEuroDates = FALSE;
			ecnt++;
			if ((dcnt > 0) && (newDateStyle == USE_GERMAN_DATES))
				ok = false;
		}
		else if (strcasecmp(tok, "DEFAULT") == 0)
		{
			/*
			 * Easiest way to get the current DEFAULT state is to fetch
			 * the DEFAULT string from guc.c and recursively parse it.
			 *
			 * We can't simply "return assign_datestyle(...)" because we need
			 * to handle constructs like "DEFAULT, ISO".
			 */
			int			saveDateStyle = DateStyle;
			bool		saveEuroDates = EuroDates;
			const char *subval;

			subval = assign_datestyle(GetConfigOptionResetString("datestyle"),
									  true, interactive);
			newDateStyle = DateStyle;
			newEuroDates = EuroDates;
			DateStyle = saveDateStyle;
			EuroDates = saveEuroDates;
			if (!subval)
			{
				ok = false;
				break;
			}
			/* Here we know that our own return value is always malloc'd */
			/* when doit is true */
			free((char *) subval);
			dcnt++;
			ecnt++;
		}
		else
		{
			if (interactive)
				elog(ERROR, "SET DATESTYLE: unrecognized keyword %s", tok);
			ok = false;
			break;
		}
	}

	if (dcnt > 1 || ecnt > 1)
		ok = false;

	pfree(rawstring);
	freeList(elemlist);

	if (!ok)
	{
		if (interactive)
			elog(ERROR, "SET DATESTYLE: conflicting specifications");
		return NULL;
	}

	/*
	 * If we aren't going to do the assignment, just return OK indicator.
	 */
	if (!doit)
		return value;

	/*
	 * Prepare the canonical string to return.	GUC wants it malloc'd.
	 */
	result = (char *) malloc(32);
	if (!result)
		return NULL;

	switch (newDateStyle)
	{
		case USE_ISO_DATES:
			strcpy(result, "ISO");
			break;
		case USE_SQL_DATES:
			strcpy(result, "SQL");
			break;
		case USE_GERMAN_DATES:
			strcpy(result, "GERMAN");
			break;
		default:
			strcpy(result, "POSTGRESQL");
			break;
	}
	strcat(result, newEuroDates ? ", EURO" : ", US");

	/*
	 * Finally, it's safe to assign to the global variables; the
	 * assignment cannot fail now.
	 */
	DateStyle = newDateStyle;
	EuroDates = newEuroDates;

	return result;
}

/*
 * show_datestyle: GUC show_hook for datestyle
 */
const char *
show_datestyle(void)
{
	static char buf[64];

	switch (DateStyle)
	{
		case USE_ISO_DATES:
			strcpy(buf, "ISO");
			break;
		case USE_SQL_DATES:
			strcpy(buf, "SQL");
			break;
		case USE_GERMAN_DATES:
			strcpy(buf, "German");
			break;
		default:
			strcpy(buf, "Postgres");
			break;
	};
	strcat(buf, " with ");
	strcat(buf, ((EuroDates) ? "European" : "US (NonEuropean)"));
	strcat(buf, " conventions");

	return buf;
}


/*
 * TIMEZONE
 */

/*
 * Storage for TZ env var is allocated with an arbitrary size of 64 bytes.
 */
#define TZBUF_LEN	64

static char tzbuf[TZBUF_LEN];

/*
 * First time through, we remember the original environment TZ value, if any.
 */
static bool have_saved_tz = false;
static char orig_tzbuf[TZBUF_LEN];

/*
 * Convenience subroutine for assigning the value of TZ
 */
static void
set_tz(const char *tz)
{
	strcpy(tzbuf, "TZ=");
	strncpy(tzbuf + 3, tz, sizeof(tzbuf) - 4);
	if (putenv(tzbuf) != 0)		/* shouldn't happen? */
		elog(LOG, "Unable to set TZ environment variable");
	tzset();
}

/*
 * Remove any value of TZ we have established
 *
 * Note: this leaves us with *no* value of TZ in the environment, and
 * is therefore only appropriate for reverting to that state, not for
 * reverting to a state where TZ was set to something else.
 */
static void
clear_tz(void)
{
	/*
	 * unsetenv() works fine, but is BSD, not POSIX, and is not
	 * available under Solaris, among others. Apparently putenv()
	 * called as below clears the process-specific environment
	 * variables.  Other reasonable arguments to putenv() (e.g.
	 * "TZ=", "TZ", "") result in a core dump (under Linux
	 * anyway). - thomas 1998-01-26
	 */
	if (tzbuf[0] == 'T')
	{
		strcpy(tzbuf, "=");
		if (putenv(tzbuf) != 0)
			elog(LOG, "Unable to clear TZ environment variable");
		tzset();
	}
}

/*
 * Check whether tzset() succeeded
 *
 * Unfortunately, tzset doesn't offer any well-defined way to detect that the
 * value of TZ was bad.  Often it will just select UTC (GMT) as the effective
 * timezone.  We use the following heuristics:
 *
 * If tzname[1] is a nonempty string, *or* the global timezone variable is
 * not zero, then tzset must have recognized the TZ value as something
 * different from UTC.  Return true.
 *
 * Otherwise, check to see if the TZ name is a known spelling of "UTC"
 * (ie, appears in our internal tables as a timezone equivalent to UTC).
 * If so, accept it.
 *
 * This will reject nonstandard spellings of UTC unless tzset() chose to
 * set tzname[1] as well as tzname[0].  The glibc version of tzset() will
 * do so, but on other systems we may be tightening the spec a little.
 *
 * Another problem is that on some platforms (eg HPUX), if tzset thinks the
 * input is bogus then it will adopt the system default timezone, which we
 * really can't tell is not the intended translation of the input.
 *
 * Still, it beats failing to detect bad TZ names at all, and a silent
 * failure mode of adopting the system-wide default is much better than
 * a silent failure mode of adopting UTC.
 *
 * NB: this must NOT elog(ERROR).  The caller must get control back so that
 * it can restore the old value of TZ if we don't like the new one.
 */
static bool
tzset_succeeded(const char *tz)
{
	char		tztmp[TZBUF_LEN];
	char	   *cp;
	int			tzval;

	/*
	 * Check first set of heuristics to say that tzset definitely worked.
	 */
#ifdef HAVE_TZNAME
	if (tzname[1] && tzname[1][0] != '\0')
		return true;
#endif
	if (TIMEZONE_GLOBAL != 0)
		return true;

	/*
	 * Check for known spellings of "UTC".  Note we must downcase the input
	 * before passing it to DecodePosixTimezone().
	 */
	StrNCpy(tztmp, tz, sizeof(tztmp));
	for (cp = tztmp; *cp; cp++)
		*cp = tolower((unsigned char) *cp);
	if (DecodePosixTimezone(tztmp, &tzval) == 0)
		if (tzval == 0)
			return true;

	return false;
}

/*
 * Check whether timezone is acceptable.
 *
 * What we are doing here is checking for leap-second-aware timekeeping.
 * We need to reject such TZ settings because they'll wreak havoc with our
 * date/time arithmetic.
 *
 * NB: this must NOT elog(ERROR).  The caller must get control back so that
 * it can restore the old value of TZ if we don't like the new one.
 */
static bool
tz_acceptable(void)
{
	struct tm	tt;
	time_t		time2000;

	/*
	 * To detect leap-second timekeeping, compute the time_t value for
	 * local midnight, 2000-01-01.  Insist that this be a multiple of 60;
	 * any partial-minute offset has to be due to leap seconds.
	 */
	MemSet(&tt, 0, sizeof(tt));
	tt.tm_year = 100;
	tt.tm_mon = 0;
	tt.tm_mday = 1;
	tt.tm_isdst = -1;
	time2000 = mktime(&tt);
	if ((time2000 % 60) != 0)
		return false;

	return true;
}

/*
 * assign_timezone: GUC assign_hook for timezone
 */
const char *
assign_timezone(const char *value, bool doit, bool interactive)
{
	char	   *result;
	char	   *endptr;
	double		hours;

	/*
	 * On first call, see if there is a TZ in the original environment.
	 * Save that value permanently.
	 */
	if (!have_saved_tz)
	{
		char   *orig_tz = getenv("TZ");

		if (orig_tz)
			StrNCpy(orig_tzbuf, orig_tz, sizeof(orig_tzbuf));
		else
			orig_tzbuf[0] = '\0';
		have_saved_tz = true;
	}

	/*
	 * Check for INTERVAL 'foo'
	 */
	if (strncasecmp(value, "interval", 8) == 0)
	{
		const char *valueptr = value;
		char	   *val;
		Interval   *interval;

		valueptr += 8;
		while (isspace((unsigned char) *valueptr))
			valueptr++;
		if (*valueptr++ != '\'')
			return NULL;
		val = pstrdup(valueptr);
		/* Check and remove trailing quote */
		endptr = strchr(val, '\'');
		if (!endptr || endptr[1] != '\0')
		{
			pfree(val);
			return NULL;
		}
		*endptr = '\0';

		/*
		 * Try to parse it.  XXX an invalid interval format will result in
		 * elog, which is not desirable for GUC.  We did what we could to
		 * guard against this in flatten_set_variable_args, but a string
		 * coming in from postgresql.conf might contain anything.
		 */
		interval = DatumGetIntervalP(DirectFunctionCall3(interval_in,
													CStringGetDatum(val),
											ObjectIdGetDatum(InvalidOid),
													 Int32GetDatum(-1)));
		pfree(val);
		if (interval->month != 0)
		{
			if (interactive)
				elog(ERROR, "SET TIME ZONE: illegal INTERVAL; month not allowed");
			pfree(interval);
			return NULL;
		}
		if (doit)
		{
			CTimeZone = interval->time;
			HasCTZSet = true;
		}
		pfree(interval);
	}
	else
	{
		/*
		 * Try it as a numeric number of hours (possibly fractional).
		 */
		hours = strtod(value, &endptr);
		if (endptr != value && *endptr == '\0')
		{
			if (doit)
			{
				CTimeZone = hours * 3600;
				HasCTZSet = true;
			}
		}
		else if (strcasecmp(value, "UNKNOWN") == 0)
		{
			/*
			 * UNKNOWN is the value shown as the "default" for TimeZone
			 * in guc.c.  We interpret it as meaning the original TZ
			 * inherited from the environment.  Note that if there is an
			 * original TZ setting, we will return that rather than UNKNOWN
			 * as the canonical spelling.
			 */
			if (doit)
			{
				bool	ok;

				/* Revert to original setting of TZ, whatever it was */
				if (orig_tzbuf[0])
				{
					set_tz(orig_tzbuf);
					ok = tzset_succeeded(orig_tzbuf) && tz_acceptable();
				}
				else
				{
					clear_tz();
					ok = tz_acceptable();
				}

				if (ok)
					HasCTZSet = false;
				else
				{
					/* Bogus, so force UTC (equivalent to INTERVAL 0) */
					CTimeZone = 0;
					HasCTZSet = true;
				}
			}
		}
		else
		{
			/*
			 * Otherwise assume it is a timezone name.
			 *
			 * We have to actually apply the change before we can have any
			 * hope of checking it.  So, save the old value in case we have
			 * to back out.  Note that it's possible the old setting is in
			 * tzbuf, so we'd better copy it.
			 */
			char	save_tzbuf[TZBUF_LEN];
			char   *save_tz;
			bool	known,
					acceptable;

			save_tz = getenv("TZ");
			if (save_tz)
				StrNCpy(save_tzbuf, save_tz, sizeof(save_tzbuf));

			set_tz(value);

			known = tzset_succeeded(value);
			acceptable = tz_acceptable();

			if (doit && known && acceptable)
			{
				/* Keep the changed TZ */
				HasCTZSet = false;
			}
			else
			{
				/*
				 * Revert to prior TZ setting; note we haven't changed
				 * HasCTZSet in this path, so if we were previously using
				 * a fixed offset, we still are.
				 */
				if (save_tz)
					set_tz(save_tzbuf);
				else
					clear_tz();
				/* Complain if it was bad */
				if (!known)
				{
					elog(interactive ? ERROR : LOG,
						 "unrecognized timezone name \"%s\"",
						 value);
					return NULL;
				}
				if (!acceptable)
				{
					elog(interactive ? ERROR : LOG,
						 "timezone \"%s\" appears to use leap seconds"
						 "\n\tPostgreSQL does not support leap seconds",
						 value);
					return NULL;
				}
			}
		}
	}

	/*
	 * If we aren't going to do the assignment, just return OK indicator.
	 */
	if (!doit)
		return value;

	/*
	 * Prepare the canonical string to return.	GUC wants it malloc'd.
	 */
	result = (char *) malloc(sizeof(tzbuf));
	if (!result)
		return NULL;

	if (HasCTZSet)
		snprintf(result, sizeof(tzbuf), "%.5f", (double) CTimeZone / 3600.0);
	else if (tzbuf[0] == 'T')
		strcpy(result, tzbuf + 3);
	else
		strcpy(result, "UNKNOWN");

	return result;
}

/*
 * show_timezone: GUC show_hook for timezone
 */
const char *
show_timezone(void)
{
	char	   *tzn;

	if (HasCTZSet)
	{
		Interval	interval;

		interval.month = 0;
		interval.time = CTimeZone;

		tzn = DatumGetCString(DirectFunctionCall1(interval_out,
										  IntervalPGetDatum(&interval)));
	}
	else
		tzn = getenv("TZ");

	if (tzn != NULL)
		return tzn;

	return "unknown";
}


/*
 * SET TRANSACTION ISOLATION LEVEL
 */

const char *
assign_XactIsoLevel(const char *value, bool doit, bool interactive)
{
	if (doit && interactive && SerializableSnapshot != NULL)
		elog(ERROR, "SET TRANSACTION ISOLATION LEVEL must be called before any query");

	if (strcmp(value, "serializable") == 0)
	{
		if (doit)
			XactIsoLevel = XACT_SERIALIZABLE;
	}
	else if (strcmp(value, "read committed") == 0)
	{
		if (doit)
			XactIsoLevel = XACT_READ_COMMITTED;
	}
	else if (strcmp(value, "default") == 0)
	{
		if (doit)
			XactIsoLevel = DefaultXactIsoLevel;
	}
	else
		return NULL;

	return value;
}

const char *
show_XactIsoLevel(void)
{
	if (XactIsoLevel == XACT_SERIALIZABLE)
		return "SERIALIZABLE";
	else
		return "READ COMMITTED";
}


/*
 * Random number seed
 */

bool
assign_random_seed(double value, bool doit, bool interactive)
{
	/* Can't really roll back on error, so ignore non-interactive setting */
	if (doit && interactive)
		DirectFunctionCall1(setseed, Float8GetDatum(value));
	return true;
}

const char *
show_random_seed(void)
{
	return "unavailable";
}


/*
 * encoding handling functions
 */

const char *
assign_client_encoding(const char *value, bool doit, bool interactive)
{
	int			encoding;

	encoding = pg_valid_client_encoding(value);
	if (encoding < 0)
		return NULL;

	/*
	 * Note: if we are in startup phase then SetClientEncoding may not be
	 * able to really set the encoding.  In this case we will assume that
	 * the encoding is okay, and InitializeClientEncoding() will fix things
	 * once initialization is complete.
	 */
	if (SetClientEncoding(encoding, doit) < 0)
	{
		if (interactive)
			elog(ERROR, "Conversion between %s and %s is not supported",
				 value, GetDatabaseEncodingName());
		return NULL;
	}
	return value;
}


/*
 * SET SESSION AUTHORIZATION
 *
 * When resetting session auth after an error, we can't expect to do catalog
 * lookups.  Hence, the stored form of the value must provide a numeric userid
 * that can be re-used directly.  We store the string in the form of
 * NAMEDATALEN 'x's followed by the numeric userid --- this cannot conflict
 * with any valid user name, because of the NAMEDATALEN limit on names.
 */
const char *
assign_session_authorization(const char *value, bool doit, bool interactive)
{
	AclId		usesysid = 0;
	char	   *result;

	if (strspn(value, "x") == NAMEDATALEN)
	{
		/* might be a saved numeric userid */
		char	   *endptr;

		usesysid = (AclId) strtoul(value + NAMEDATALEN, &endptr, 10);

		if (endptr != value + NAMEDATALEN && *endptr == '\0')
		{
			/* syntactically valid, so use the numeric user ID */
		}
		else
			usesysid = 0;
	}

	if (usesysid == 0)
	{
		/* not a saved ID, so look it up */
		HeapTuple	userTup;

		userTup = SearchSysCache(SHADOWNAME,
								 PointerGetDatum(value),
								 0, 0, 0);
		if (!HeapTupleIsValid(userTup))
		{
			if (interactive)
				elog(ERROR, "user \"%s\" does not exist", value);
			return NULL;
		}

		usesysid = ((Form_pg_shadow) GETSTRUCT(userTup))->usesysid;

		ReleaseSysCache(userTup);
	}

	if (doit)
		SetSessionAuthorization(usesysid);

	result = (char *) malloc(NAMEDATALEN + 32);
	if (!result)
		return NULL;

	memset(result, 'x', NAMEDATALEN);

	snprintf(result + NAMEDATALEN, 32, "%lu", (unsigned long) usesysid);

	return result;
}

const char *
show_session_authorization(void)
{
	/*
	 * We can't use the stored string; see comments for
	 * assign_session_authorization
	 */
	return GetUserNameFromId(GetSessionUserId());
}
