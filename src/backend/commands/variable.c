/*-------------------------------------------------------------------------
 *
 * variable.c
 *		Routines for handling specialized SET variables.
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/commands/variable.c,v 1.88.2.3 2006/02/12 22:33:28 tgl Exp $
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
#ifndef tzname					/* For SGI.  */
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
	int			newDateOrder = DateOrder;
	bool		ok = true;
	int			scnt = 0,
				ocnt = 0;
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
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("invalid list syntax for parameter \"datestyle\"")));
		return NULL;
	}

	foreach(l, elemlist)
	{
		char	   *tok = (char *) lfirst(l);

		/* Ugh. Somebody ought to write a table driven version -- mjl */

		if (strcasecmp(tok, "ISO") == 0)
		{
			newDateStyle = USE_ISO_DATES;
			scnt++;
		}
		else if (strcasecmp(tok, "SQL") == 0)
		{
			newDateStyle = USE_SQL_DATES;
			scnt++;
		}
		else if (strncasecmp(tok, "POSTGRES", 8) == 0)
		{
			newDateStyle = USE_POSTGRES_DATES;
			scnt++;
		}
		else if (strcasecmp(tok, "GERMAN") == 0)
		{
			newDateStyle = USE_GERMAN_DATES;
			scnt++;
			/* GERMAN also sets DMY, unless explicitly overridden */
			if (ocnt == 0)
				newDateOrder = DATEORDER_DMY;
		}
		else if (strcasecmp(tok, "YMD") == 0)
		{
			newDateOrder = DATEORDER_YMD;
			ocnt++;
		}
		else if (strcasecmp(tok, "DMY") == 0 ||
				 strncasecmp(tok, "EURO", 4) == 0)
		{
			newDateOrder = DATEORDER_DMY;
			ocnt++;
		}
		else if (strcasecmp(tok, "MDY") == 0 ||
				 strcasecmp(tok, "US") == 0 ||
				 strncasecmp(tok, "NONEURO", 7) == 0)
		{
			newDateOrder = DATEORDER_MDY;
			ocnt++;
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
			int			saveDateOrder = DateOrder;
			const char *subval;

			subval = assign_datestyle(GetConfigOptionResetString("datestyle"),
									  true, interactive);
			if (scnt == 0)
				newDateStyle = DateStyle;
			if (ocnt == 0)
				newDateOrder = DateOrder;
			DateStyle = saveDateStyle;
			DateOrder = saveDateOrder;
			if (!subval)
			{
				ok = false;
				break;
			}
			/* Here we know that our own return value is always malloc'd */
			/* when doit is true */
			free((char *) subval);
		}
		else
		{
			if (interactive)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("unrecognized \"datestyle\" key word: \"%s\"",
								tok)));
			ok = false;
			break;
		}
	}

	if (scnt > 1 || ocnt > 1)
		ok = false;

	pfree(rawstring);
	freeList(elemlist);

	if (!ok)
	{
		if (interactive)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("conflicting \"datestyle\" specifications")));
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
			strcpy(result, "German");
			break;
		default:
			strcpy(result, "Postgres");
			break;
	}
	switch (newDateOrder)
	{
		case DATEORDER_YMD:
			strcat(result, ", YMD");
			break;
		case DATEORDER_DMY:
			strcat(result, ", DMY");
			break;
		default:
			strcat(result, ", MDY");
			break;
	}

	/*
	 * Finally, it's safe to assign to the global variables; the
	 * assignment cannot fail now.
	 */
	DateStyle = newDateStyle;
	DateOrder = newDateOrder;

	return result;
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
		elog(LOG, "could not set TZ environment variable");
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
	 * unsetenv() works fine, but is BSD, not POSIX, and is not available
	 * under Solaris, among others. Apparently putenv() called as below
	 * clears the process-specific environment variables.  Other
	 * reasonable arguments to putenv() (e.g. "TZ=", "TZ", "") result in a
	 * core dump (under Linux anyway). - thomas 1998-01-26
	 */
	if (tzbuf[0] == 'T')
	{
		strcpy(tzbuf, "=");
		if (putenv(tzbuf) != 0)
			elog(LOG, "could not clear TZ environment variable");
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
 * different from UTC.	Return true.
 *
 * Otherwise, check to see if the TZ name is a known spelling of "UTC"
 * (ie, appears in our internal tables as a timezone equivalent to UTC).
 * If so, accept it.
 *
 * This will reject nonstandard spellings of UTC unless tzset() chose to
 * set tzname[1] as well as tzname[0].	The glibc version of tzset() will
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
 * NB: this must NOT ereport(ERROR).  The caller must get control back so that
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
	 * Check for known spellings of "UTC".	Note we must downcase the
	 * input before passing it to DecodePosixTimezone().
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
 * NB: this must NOT ereport(ERROR).  The caller must get control back so that
 * it can restore the old value of TZ if we don't like the new one.
 */
static bool
tz_acceptable(void)
{
	struct tm	tt;
	time_t		time2000;

	/*
	 * To detect leap-second timekeeping, compute the time_t value for
	 * local midnight, 2000-01-01.	Insist that this be a multiple of 60;
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
		char	   *orig_tz = getenv("TZ");

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
		 * ereport, which is not desirable for GUC.  We did what we could
		 * to guard against this in flatten_set_variable_args, but a
		 * string coming in from postgresql.conf might contain anything.
		 */
		interval = DatumGetIntervalP(DirectFunctionCall3(interval_in,
													CStringGetDatum(val),
											ObjectIdGetDatum(InvalidOid),
													 Int32GetDatum(-1)));
		pfree(val);
		if (interval->month != 0)
		{
			if (interactive)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("invalid interval value for time zone: month not allowed")));
			pfree(interval);
			return NULL;
		}
		if (doit)
		{
			/* Here we change from SQL to Unix sign convention */
#ifdef HAVE_INT64_TIMESTAMP
			CTimeZone = -(interval->time / INT64CONST(1000000));
#else
			CTimeZone = -interval->time;
#endif
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
				/* Here we change from SQL to Unix sign convention */
				CTimeZone = -hours * 3600;
				HasCTZSet = true;
			}
		}
		else if (strcasecmp(value, "UNKNOWN") == 0)
		{
			/*
			 * UNKNOWN is the value shown as the "default" for TimeZone in
			 * guc.c.  We interpret it as meaning the original TZ
			 * inherited from the environment.	Note that if there is an
			 * original TZ setting, we will return that rather than
			 * UNKNOWN as the canonical spelling.
			 */
			if (doit)
			{
				bool		ok;

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
			 * hope of checking it.  So, save the old value in case we
			 * have to back out.  Note that it's possible the old setting
			 * is in tzbuf, so we'd better copy it.
			 */
			char		save_tzbuf[TZBUF_LEN];
			char	   *save_tz;
			bool		known,
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
					ereport(interactive ? ERROR : LOG,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("unrecognized time zone name: \"%s\"",
									value)));
					return NULL;
				}
				if (!acceptable)
				{
					ereport(interactive ? ERROR : LOG,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					errmsg("time zone \"%s\" appears to use leap seconds",
						   value),
							 errdetail("PostgreSQL does not support leap seconds.")));
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
		snprintf(result, sizeof(tzbuf), "%.5f",
				 (double) (-CTimeZone) / 3600.0);
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
#ifdef HAVE_INT64_TIMESTAMP
		interval.time = -(CTimeZone * INT64CONST(1000000));
#else
		interval.time = -CTimeZone;
#endif

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
		ereport(ERROR,
				(errcode(ERRCODE_ACTIVE_SQL_TRANSACTION),
				 errmsg("SET TRANSACTION ISOLATION LEVEL must be called before any query")));

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
		return "serializable";
	else
		return "read committed";
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
	 * the encoding is okay, and InitializeClientEncoding() will fix
	 * things once initialization is complete.
	 */
	if (SetClientEncoding(encoding, doit) < 0)
	{
		if (interactive)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				  errmsg("conversion between %s and %s is not supported",
						 value, GetDatabaseEncodingName())));
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
 * NAMEDATALEN 'x's, followed by T or F to indicate superuserness, followed
 * by the numeric userid, followed by a comma, followed by the user name.
 * This cannot be confused with a plain user name because of the NAMEDATALEN
 * limit on names, so we can tell whether we're being passed an initial
 * username or a saved/restored value.  (NOTE: we rely on guc.c to have
 * properly truncated any incoming value, but not to truncate already-stored
 * values.  See GUC_IS_NAME processing.)
 */
extern char *session_authorization_string; /* in guc.c */

const char *
assign_session_authorization(const char *value, bool doit, bool interactive)
{
	AclId		usesysid = 0;
	bool		is_superuser = false;
	const char *actual_username = NULL;
	char	   *result;

	if (strspn(value, "x") == NAMEDATALEN &&
		(value[NAMEDATALEN] == 'T' || value[NAMEDATALEN] == 'F'))
	{
		/* might be a saved userid string */
		AclId		savedsysid;
		char	   *endptr;

		savedsysid = (AclId) strtoul(value + NAMEDATALEN + 1, &endptr, 10);

		if (endptr != value + NAMEDATALEN + 1 && *endptr == ',')
		{
			/* syntactically valid, so break out the data */
			usesysid = savedsysid;
			is_superuser = (value[NAMEDATALEN] == 'T');
			actual_username = endptr + 1;
		}
	}

	if (usesysid == 0)
	{
		/* not a saved ID, so look it up */
		HeapTuple	userTup;

		if (!IsTransactionState())
		{
			/*
			 * Can't do catalog lookups, so fail.  The upshot of this is
			 * that session_authorization cannot be set in
			 * postgresql.conf, which seems like a good thing anyway.
			 */
			return NULL;
		}

		userTup = SearchSysCache(SHADOWNAME,
								 PointerGetDatum(value),
								 0, 0, 0);
		if (!HeapTupleIsValid(userTup))
		{
			if (interactive)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("user \"%s\" does not exist", value)));
			return NULL;
		}

		usesysid = ((Form_pg_shadow) GETSTRUCT(userTup))->usesysid;
		is_superuser = ((Form_pg_shadow) GETSTRUCT(userTup))->usesuper;
		actual_username = value;

		ReleaseSysCache(userTup);
	}

	if (doit)
		SetSessionAuthorization(usesysid, is_superuser);

	result = (char *) malloc(NAMEDATALEN + 32 + strlen(actual_username));
	if (!result)
		return NULL;

	memset(result, 'x', NAMEDATALEN);

	sprintf(result + NAMEDATALEN, "%c%lu,%s",
			is_superuser ? 'T' : 'F',
			(unsigned long) usesysid,
			actual_username);

	return result;
}

const char *
show_session_authorization(void)
{
	/*
	 * Extract the user name from the stored string; see
	 * assign_session_authorization
	 */
	const char *value = session_authorization_string;
	AclId		savedsysid;
	char	   *endptr;

	Assert(strspn(value, "x") == NAMEDATALEN &&
		   (value[NAMEDATALEN] == 'T' || value[NAMEDATALEN] == 'F'));

	savedsysid = (AclId) strtoul(value + NAMEDATALEN + 1, &endptr, 10);

	Assert(endptr != value + NAMEDATALEN + 1 && *endptr == ',');

	return endptr + 1;
}
