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
 *	  $PostgreSQL: pgsql/src/backend/commands/variable.c,v 1.97 2004/05/26 04:41:13 neilc Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <ctype.h>

#include "access/xact.h"
#include "catalog/pg_shadow.h"
#include "commands/variable.h"
#include "miscadmin.h"
#include "parser/scansup.h"
#include "pgtime.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/syscache.h"
#include "utils/tqual.h"
#include "mb/pg_wchar.h"

/*
 * DATESTYLE
 */

/*
 * assign_datestyle: GUC assign_hook for datestyle
 */
const char *
assign_datestyle(const char *value, bool doit, GucSource source)
{
	int			newDateStyle = DateStyle;
	int			newDateOrder = DateOrder;
	bool		ok = true;
	int			scnt = 0,
				ocnt = 0;
	char	   *rawstring;
	char	   *result;
	List	   *elemlist;
	ListCell   *l;

	/* Need a modifiable copy of string */
	rawstring = pstrdup(value);

	/* Parse string into list of identifiers */
	if (!SplitIdentifierString(rawstring, ',', &elemlist))
	{
		/* syntax error in list */
		pfree(rawstring);
		list_free(elemlist);
		if (source >= PGC_S_INTERACTIVE)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("invalid list syntax for parameter \"datestyle\"")));
		return NULL;
	}

	foreach(l, elemlist)
	{
		char	   *tok = (char *) lfirst(l);

		/* Ugh. Somebody ought to write a table driven version -- mjl */

		if (pg_strcasecmp(tok, "ISO") == 0)
		{
			newDateStyle = USE_ISO_DATES;
			scnt++;
		}
		else if (pg_strcasecmp(tok, "SQL") == 0)
		{
			newDateStyle = USE_SQL_DATES;
			scnt++;
		}
		else if (pg_strncasecmp(tok, "POSTGRES", 8) == 0)
		{
			newDateStyle = USE_POSTGRES_DATES;
			scnt++;
		}
		else if (pg_strcasecmp(tok, "GERMAN") == 0)
		{
			newDateStyle = USE_GERMAN_DATES;
			scnt++;
			/* GERMAN also sets DMY, unless explicitly overridden */
			if (ocnt == 0)
				newDateOrder = DATEORDER_DMY;
		}
		else if (pg_strcasecmp(tok, "YMD") == 0)
		{
			newDateOrder = DATEORDER_YMD;
			ocnt++;
		}
		else if (pg_strcasecmp(tok, "DMY") == 0 ||
				 pg_strncasecmp(tok, "EURO", 4) == 0)
		{
			newDateOrder = DATEORDER_DMY;
			ocnt++;
		}
		else if (pg_strcasecmp(tok, "MDY") == 0 ||
				 pg_strcasecmp(tok, "US") == 0 ||
				 pg_strncasecmp(tok, "NONEURO", 7) == 0)
		{
			newDateOrder = DATEORDER_MDY;
			ocnt++;
		}
		else if (pg_strcasecmp(tok, "DEFAULT") == 0)
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
									  true, source);
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
			if (source >= PGC_S_INTERACTIVE)
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
	list_free(elemlist);

	if (!ok)
	{
		if (source >= PGC_S_INTERACTIVE)
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
 * assign_timezone: GUC assign_hook for timezone
 */
const char *
assign_timezone(const char *value, bool doit, GucSource source)
{
	char	   *result;
	char	   *endptr;
	double		hours;

	/*
	 * Check for INTERVAL 'foo'
	 */
	if (pg_strncasecmp(value, "interval", 8) == 0)
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
			if (source >= PGC_S_INTERACTIVE)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("invalid interval value for time zone: month not allowed")));
			pfree(interval);
			return NULL;
		}
		if (doit)
		{
			/* Here we change from SQL to Unix sign convention */
			CTimeZone = -interval->time;
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
		else if (pg_strcasecmp(value, "UNKNOWN") == 0)
		{
			/*
			 * UNKNOWN is the value shown as the "default" for TimeZone in
			 * guc.c.  We interpret it as being a complete no-op; we don't
			 * change the timezone setting.  Note that if there is a known
			 * timezone setting, we will return that name rather than
			 * UNKNOWN as the canonical spelling.
			 *
			 * During GUC initialization, since the timezone library isn't
			 * set up yet, pg_get_current_timezone will return NULL and we
			 * will leave the setting as UNKNOWN.  If this isn't overridden
			 * from the config file then pg_timezone_initialize() will
			 * eventually select a default value from the environment.
			 */
			const char *curzone = pg_get_current_timezone();

			if (curzone)
				value = curzone;
		}
		else
		{
			/*
			 * Otherwise assume it is a timezone name.
			 *
			 * We have to actually apply the change before we can have any
			 * hope of checking it.  So, save the old value in case we have
			 * to back out.  We have to copy since pg_get_current_timezone
			 * returns a pointer to its static state.
			 *
			 * This would all get a lot simpler if the TZ library had a better
			 * API that would let us look up and test a timezone name without
			 * making it the default.
			 */
			const char *cur_tz;
			char	   *save_tz;
			bool		known,
						acceptable;

			cur_tz = pg_get_current_timezone();
			if (cur_tz)
				save_tz = pstrdup(cur_tz);
			else
				save_tz = NULL;

			known = pg_tzset(value);
			acceptable = known ? tz_acceptable() : false;

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
					pg_tzset(save_tz);
				else
				{
					/*
					 * TZ library wasn't initialized yet.  Annoyingly, we will
					 * come here during startup because guc-file.l checks
					 * the value with doit = false before actually applying.
					 * The best approach seems to be as follows:
					 *
					 * 1. known && acceptable: leave the setting in place,
					 * since we'll apply it soon anyway.  This is mainly
					 * so that any log messages printed during this interval
					 * are timestamped with the user's requested timezone.
					 *
					 * 2. known && !acceptable: revert to GMT for lack of
					 * any better idea.  (select_default_timezone() may get
					 * called later to undo this.)
					 *
					 * 3. !known: no need to do anything since TZ library
					 * did not change its state.
					 *
					 * Again, this should all go away sometime soon.
					 */
					if (known && !acceptable)
						pg_tzset("GMT");
				}
				/* Complain if it was bad */
				if (!known)
				{
					ereport((source >= PGC_S_INTERACTIVE) ? ERROR : LOG,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("unrecognized time zone name: \"%s\"",
									value)));
					return NULL;
				}
				if (!acceptable)
				{
					ereport((source >= PGC_S_INTERACTIVE) ? ERROR : LOG,
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
	if (HasCTZSet)
	{
		result = (char *) malloc(64);
		if (!result)
			return NULL;
		snprintf(result, 64, "%.5f",
				 (double) (-CTimeZone) / 3600.0);
	}
	else
		result = strdup(value);

	return result;
}

/*
 * show_timezone: GUC show_hook for timezone
 */
const char *
show_timezone(void)
{
	const char	   *tzn;

	if (HasCTZSet)
	{
		Interval	interval;

		interval.month = 0;
		interval.time = -CTimeZone;

		tzn = DatumGetCString(DirectFunctionCall1(interval_out,
										  IntervalPGetDatum(&interval)));
	}
	else
		tzn = pg_get_current_timezone();

	if (tzn != NULL)
		return tzn;

	return "unknown";
}


/*
 * SET TRANSACTION ISOLATION LEVEL
 */

const char *
assign_XactIsoLevel(const char *value, bool doit, GucSource source)
{
	if (doit && source >= PGC_S_INTERACTIVE && SerializableSnapshot != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_ACTIVE_SQL_TRANSACTION),
				 errmsg("SET TRANSACTION ISOLATION LEVEL must be called before any query")));

	if (strcmp(value, "serializable") == 0)
	{
		if (doit)
			XactIsoLevel = XACT_SERIALIZABLE;
	}
	else if (strcmp(value, "repeatable read") == 0)
	{
		if (doit)
			XactIsoLevel = XACT_REPEATABLE_READ;
	}
	else if (strcmp(value, "read committed") == 0)
	{
		if (doit)
			XactIsoLevel = XACT_READ_COMMITTED;
	}
	else if (strcmp(value, "read uncommitted") == 0)
	{
		if (doit)
			XactIsoLevel = XACT_READ_UNCOMMITTED;
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
	switch (XactIsoLevel)
	{
		case XACT_READ_UNCOMMITTED:
			return "read uncommitted";
		case XACT_READ_COMMITTED:
			return "read committed";
		case XACT_REPEATABLE_READ:
			return "repeatable read";
		case XACT_SERIALIZABLE:
			return "serializable";
		default:
			return "bogus";
	}
}


/*
 * Random number seed
 */

bool
assign_random_seed(double value, bool doit, GucSource source)
{
	/* Can't really roll back on error, so ignore non-interactive setting */
	if (doit && source >= PGC_S_INTERACTIVE)
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
assign_client_encoding(const char *value, bool doit, GucSource source)
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
		if (source >= PGC_S_INTERACTIVE)
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
 * by the numeric userid --- this cannot conflict with any valid user name,
 * because of the NAMEDATALEN limit on names.
 */
const char *
assign_session_authorization(const char *value, bool doit, GucSource source)
{
	AclId		usesysid = 0;
	bool		is_superuser = false;
	char	   *result;

	if (strspn(value, "x") == NAMEDATALEN &&
		(value[NAMEDATALEN] == 'T' || value[NAMEDATALEN] == 'F'))
	{
		/* might be a saved numeric userid */
		char	   *endptr;

		usesysid = (AclId) strtoul(value + NAMEDATALEN + 1, &endptr, 10);

		if (endptr != value + NAMEDATALEN + 1 && *endptr == '\0')
		{
			/* syntactically valid, so use the numeric user ID and flag */
			is_superuser = (value[NAMEDATALEN] == 'T');
		}
		else
			usesysid = 0;
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
			if (source >= PGC_S_INTERACTIVE)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("user \"%s\" does not exist", value)));
			return NULL;
		}

		usesysid = ((Form_pg_shadow) GETSTRUCT(userTup))->usesysid;
		is_superuser = ((Form_pg_shadow) GETSTRUCT(userTup))->usesuper;

		ReleaseSysCache(userTup);
	}

	if (doit)
		SetSessionAuthorization(usesysid, is_superuser);

	result = (char *) malloc(NAMEDATALEN + 32);
	if (!result)
		return NULL;

	memset(result, 'x', NAMEDATALEN);

	snprintf(result + NAMEDATALEN, 32, "%c%lu",
			 is_superuser ? 'T' : 'F',
			 (unsigned long) usesysid);

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
