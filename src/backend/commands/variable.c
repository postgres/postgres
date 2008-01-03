/*-------------------------------------------------------------------------
 *
 * variable.c
 *		Routines for handling specialized SET variables.
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/commands/variable.c,v 1.119.2.1 2008/01/03 21:23:45 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <ctype.h>

#include "access/xact.h"
#include "catalog/pg_authid.h"
#include "commands/variable.h"
#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/builtins.h"
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
	bool		have_style = false;
	bool		have_order = false;
	bool		ok = true;
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
			if (have_style && newDateStyle != USE_ISO_DATES)
				ok = false;		/* conflicting styles */
			newDateStyle = USE_ISO_DATES;
			have_style = true;
		}
		else if (pg_strcasecmp(tok, "SQL") == 0)
		{
			if (have_style && newDateStyle != USE_SQL_DATES)
				ok = false;		/* conflicting styles */
			newDateStyle = USE_SQL_DATES;
			have_style = true;
		}
		else if (pg_strncasecmp(tok, "POSTGRES", 8) == 0)
		{
			if (have_style && newDateStyle != USE_POSTGRES_DATES)
				ok = false;		/* conflicting styles */
			newDateStyle = USE_POSTGRES_DATES;
			have_style = true;
		}
		else if (pg_strcasecmp(tok, "GERMAN") == 0)
		{
			if (have_style && newDateStyle != USE_GERMAN_DATES)
				ok = false;		/* conflicting styles */
			newDateStyle = USE_GERMAN_DATES;
			have_style = true;
			/* GERMAN also sets DMY, unless explicitly overridden */
			if (!have_order)
				newDateOrder = DATEORDER_DMY;
		}
		else if (pg_strcasecmp(tok, "YMD") == 0)
		{
			if (have_order && newDateOrder != DATEORDER_YMD)
				ok = false;		/* conflicting orders */
			newDateOrder = DATEORDER_YMD;
			have_order = true;
		}
		else if (pg_strcasecmp(tok, "DMY") == 0 ||
				 pg_strncasecmp(tok, "EURO", 4) == 0)
		{
			if (have_order && newDateOrder != DATEORDER_DMY)
				ok = false;		/* conflicting orders */
			newDateOrder = DATEORDER_DMY;
			have_order = true;
		}
		else if (pg_strcasecmp(tok, "MDY") == 0 ||
				 pg_strcasecmp(tok, "US") == 0 ||
				 pg_strncasecmp(tok, "NONEURO", 7) == 0)
		{
			if (have_order && newDateOrder != DATEORDER_MDY)
				ok = false;		/* conflicting orders */
			newDateOrder = DATEORDER_MDY;
			have_order = true;
		}
		else if (pg_strcasecmp(tok, "DEFAULT") == 0)
		{
			/*
			 * Easiest way to get the current DEFAULT state is to fetch the
			 * DEFAULT string from guc.c and recursively parse it.
			 *
			 * We can't simply "return assign_datestyle(...)" because we need
			 * to handle constructs like "DEFAULT, ISO".
			 */
			int			saveDateStyle = DateStyle;
			int			saveDateOrder = DateOrder;
			const char *subval;

			subval = assign_datestyle(GetConfigOptionResetString("datestyle"),
									  true, source);
			if (!have_style)
				newDateStyle = DateStyle;
			if (!have_order)
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
	 * Finally, it's safe to assign to the global variables; the assignment
	 * cannot fail now.
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
		 * ereport, which is not desirable for GUC.  We did what we could to
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
			if (source >= PGC_S_INTERACTIVE)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("invalid interval value for time zone: month not allowed")));
			pfree(interval);
			return NULL;
		}
		if (interval->day != 0)
		{
			if (source >= PGC_S_INTERACTIVE)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("invalid interval value for time zone: day not allowed")));
			pfree(interval);
			return NULL;
		}
		if (doit)
		{
			/* Here we change from SQL to Unix sign convention */
#ifdef HAVE_INT64_TIMESTAMP
			CTimeZone = -(interval->time / USECS_PER_SEC);
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
				CTimeZone = -hours * SECS_PER_HOUR;
				HasCTZSet = true;
			}
		}
		else if (pg_strcasecmp(value, "UNKNOWN") == 0)
		{
			/*
			 * UNKNOWN is the value shown as the "default" for TimeZone in
			 * guc.c.  We interpret it as being a complete no-op; we don't
			 * change the timezone setting.  Note that if there is a known
			 * timezone setting, we will return that name rather than UNKNOWN
			 * as the canonical spelling.
			 *
			 * During GUC initialization, since the timezone library isn't set
			 * up yet, pg_get_timezone_name will return NULL and we will leave
			 * the setting as UNKNOWN.	If this isn't overridden from the
			 * config file then pg_timezone_initialize() will eventually
			 * select a default value from the environment.
			 */
			if (doit)
			{
				const char *curzone = pg_get_timezone_name(global_timezone);

				if (curzone)
					value = curzone;
			}
		}
		else
		{
			/*
			 * Otherwise assume it is a timezone name, and try to load it.
			 */
			pg_tz	   *new_tz;

			new_tz = pg_tzset(value);

			if (!new_tz)
			{
				ereport((source >= PGC_S_INTERACTIVE) ? ERROR : LOG,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("unrecognized time zone name: \"%s\"",
								value)));
				return NULL;
			}

			if (!tz_acceptable(new_tz))
			{
				ereport((source >= PGC_S_INTERACTIVE) ? ERROR : LOG,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					   errmsg("time zone \"%s\" appears to use leap seconds",
							  value),
					errdetail("PostgreSQL does not support leap seconds.")));
				return NULL;
			}

			if (doit)
			{
				/* Save the changed TZ */
				global_timezone = new_tz;
				HasCTZSet = false;
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
				 (double) (-CTimeZone) / (double) SECS_PER_HOUR);
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
	const char *tzn;

	if (HasCTZSet)
	{
		Interval	interval;

		interval.month = 0;
		interval.day = 0;
#ifdef HAVE_INT64_TIMESTAMP
		interval.time = -(CTimeZone * USECS_PER_SEC);
#else
		interval.time = -CTimeZone;
#endif

		tzn = DatumGetCString(DirectFunctionCall1(interval_out,
											  IntervalPGetDatum(&interval)));
	}
	else
		tzn = pg_get_timezone_name(global_timezone);

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
	if (SerializableSnapshot != NULL)
	{
		if (source >= PGC_S_INTERACTIVE)
			ereport(ERROR,
					(errcode(ERRCODE_ACTIVE_SQL_TRANSACTION),
					 errmsg("SET TRANSACTION ISOLATION LEVEL must be called before any query")));
		/* source == PGC_S_OVERRIDE means do it anyway, eg at xact abort */
		else if (source != PGC_S_OVERRIDE)
			return NULL;
	}
	if (IsSubTransaction())
	{
		if (source >= PGC_S_INTERACTIVE)
			ereport(ERROR,
					(errcode(ERRCODE_ACTIVE_SQL_TRANSACTION),
					 errmsg("SET TRANSACTION ISOLATION LEVEL must not be called in a subtransaction")));
		/* source == PGC_S_OVERRIDE means do it anyway, eg at xact abort */
		else if (source != PGC_S_OVERRIDE)
			return NULL;
	}

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
	 * Note: if we are in startup phase then SetClientEncoding may not be able
	 * to really set the encoding.	In this case we will assume that the
	 * encoding is okay, and InitializeClientEncoding() will fix things once
	 * initialization is complete.
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
 * lookups.  Hence, the stored form of the value must provide a numeric oid
 * that can be re-used directly.  We store the string in the form of
 * NAMEDATALEN 'x's, followed by T or F to indicate superuserness, followed
 * by the numeric oid, followed by a comma, followed by the role name.
 * This cannot be confused with a plain role name because of the NAMEDATALEN
 * limit on names, so we can tell whether we're being passed an initial
 * role name or a saved/restored value.  (NOTE: we rely on guc.c to have
 * properly truncated any incoming value, but not to truncate already-stored
 * values.	See GUC_IS_NAME processing.)
 */
extern char *session_authorization_string;		/* in guc.c */

const char *
assign_session_authorization(const char *value, bool doit, GucSource source)
{
	Oid			roleid = InvalidOid;
	bool		is_superuser = false;
	const char *actual_rolename = NULL;
	char	   *result;

	if (strspn(value, "x") == NAMEDATALEN &&
		(value[NAMEDATALEN] == 'T' || value[NAMEDATALEN] == 'F'))
	{
		/* might be a saved userid string */
		Oid			savedoid;
		char	   *endptr;

		savedoid = (Oid) strtoul(value + NAMEDATALEN + 1, &endptr, 10);

		if (endptr != value + NAMEDATALEN + 1 && *endptr == ',')
		{
			/* syntactically valid, so break out the data */
			roleid = savedoid;
			is_superuser = (value[NAMEDATALEN] == 'T');
			actual_rolename = endptr + 1;
		}
	}

	if (roleid == InvalidOid)
	{
		/* not a saved ID, so look it up */
		HeapTuple	roleTup;

		if (InSecurityDefinerContext())
		{
			/*
			 * Disallow SET SESSION AUTHORIZATION inside a security definer
			 * context.  We need to do this because when we exit the context,
			 * GUC won't be notified, leaving things out of sync.  Note that
			 * this test is positioned so that restoring a previously saved
			 * setting isn't prevented.
			 */
			if (source >= PGC_S_INTERACTIVE)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot set session authorization within security-definer function")));
			return NULL;
		}

		if (!IsTransactionState())
		{
			/*
			 * Can't do catalog lookups, so fail.  The upshot of this is that
			 * session_authorization cannot be set in postgresql.conf, which
			 * seems like a good thing anyway.
			 */
			return NULL;
		}

		roleTup = SearchSysCache(AUTHNAME,
								 PointerGetDatum(value),
								 0, 0, 0);
		if (!HeapTupleIsValid(roleTup))
		{
			if (source >= PGC_S_INTERACTIVE)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("role \"%s\" does not exist", value)));
			return NULL;
		}

		roleid = HeapTupleGetOid(roleTup);
		is_superuser = ((Form_pg_authid) GETSTRUCT(roleTup))->rolsuper;
		actual_rolename = value;

		ReleaseSysCache(roleTup);
	}

	if (doit)
		SetSessionAuthorization(roleid, is_superuser);

	result = (char *) malloc(NAMEDATALEN + 32 + strlen(actual_rolename));
	if (!result)
		return NULL;

	memset(result, 'x', NAMEDATALEN);

	sprintf(result + NAMEDATALEN, "%c%u,%s",
			is_superuser ? 'T' : 'F',
			roleid,
			actual_rolename);

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
	Oid			savedoid;
	char	   *endptr;

	Assert(strspn(value, "x") == NAMEDATALEN &&
		   (value[NAMEDATALEN] == 'T' || value[NAMEDATALEN] == 'F'));

	savedoid = (Oid) strtoul(value + NAMEDATALEN + 1, &endptr, 10);

	Assert(endptr != value + NAMEDATALEN + 1 && *endptr == ',');

	return endptr + 1;
}


/*
 * SET ROLE
 *
 * When resetting session auth after an error, we can't expect to do catalog
 * lookups.  Hence, the stored form of the value must provide a numeric oid
 * that can be re-used directly.  We implement this exactly like SET
 * SESSION AUTHORIZATION.
 *
 * The SQL spec requires "SET ROLE NONE" to unset the role, so we hardwire
 * a translation of "none" to InvalidOid.
 */
extern char *role_string;		/* in guc.c */

const char *
assign_role(const char *value, bool doit, GucSource source)
{
	Oid			roleid = InvalidOid;
	bool		is_superuser = false;
	const char *actual_rolename = value;
	char	   *result;

	if (strspn(value, "x") == NAMEDATALEN &&
		(value[NAMEDATALEN] == 'T' || value[NAMEDATALEN] == 'F'))
	{
		/* might be a saved userid string */
		Oid			savedoid;
		char	   *endptr;

		savedoid = (Oid) strtoul(value + NAMEDATALEN + 1, &endptr, 10);

		if (endptr != value + NAMEDATALEN + 1 && *endptr == ',')
		{
			/* syntactically valid, so break out the data */
			roleid = savedoid;
			is_superuser = (value[NAMEDATALEN] == 'T');
			actual_rolename = endptr + 1;
		}
	}

	if (roleid == InvalidOid && InSecurityDefinerContext())
	{
		/*
		 * Disallow SET ROLE inside a security definer context.  We need to do
		 * this because when we exit the context, GUC won't be notified,
		 * leaving things out of sync.  Note that this test is arranged so
		 * that restoring a previously saved setting isn't prevented.
		 *
		 * XXX it would be nice to allow this case in future, with the
		 * behavior being that the SET ROLE's effects end when the security
		 * definer context is exited.
		 */
		if (source >= PGC_S_INTERACTIVE)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot set role within security-definer function")));
		return NULL;
	}

	if (roleid == InvalidOid &&
		strcmp(actual_rolename, "none") != 0)
	{
		/* not a saved ID, so look it up */
		HeapTuple	roleTup;

		if (!IsTransactionState())
		{
			/*
			 * Can't do catalog lookups, so fail.  The upshot of this is that
			 * role cannot be set in postgresql.conf, which seems like a good
			 * thing anyway.
			 */
			return NULL;
		}

		roleTup = SearchSysCache(AUTHNAME,
								 PointerGetDatum(value),
								 0, 0, 0);
		if (!HeapTupleIsValid(roleTup))
		{
			if (source >= PGC_S_INTERACTIVE)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("role \"%s\" does not exist", value)));
			return NULL;
		}

		roleid = HeapTupleGetOid(roleTup);
		is_superuser = ((Form_pg_authid) GETSTRUCT(roleTup))->rolsuper;

		ReleaseSysCache(roleTup);

		/*
		 * Verify that session user is allowed to become this role
		 */
		if (!is_member_of_role(GetSessionUserId(), roleid))
		{
			if (source >= PGC_S_INTERACTIVE)
				ereport(ERROR,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						 errmsg("permission denied to set role \"%s\"",
								value)));
			return NULL;
		}
	}

	if (doit)
		SetCurrentRoleId(roleid, is_superuser);

	result = (char *) malloc(NAMEDATALEN + 32 + strlen(actual_rolename));
	if (!result)
		return NULL;

	memset(result, 'x', NAMEDATALEN);

	sprintf(result + NAMEDATALEN, "%c%u,%s",
			is_superuser ? 'T' : 'F',
			roleid,
			actual_rolename);

	return result;
}

const char *
show_role(void)
{
	/*
	 * Extract the role name from the stored string; see assign_role
	 */
	const char *value = role_string;
	Oid			savedoid;
	char	   *endptr;

	/* This special case only applies if no SET ROLE has been done */
	if (value == NULL || strcmp(value, "none") == 0)
		return "none";

	Assert(strspn(value, "x") == NAMEDATALEN &&
		   (value[NAMEDATALEN] == 'T' || value[NAMEDATALEN] == 'F'));

	savedoid = (Oid) strtoul(value + NAMEDATALEN + 1, &endptr, 10);

	Assert(endptr != value + NAMEDATALEN + 1 && *endptr == ',');

	/*
	 * Check that the stored string still matches the effective setting, else
	 * return "none".  This is a kluge to deal with the fact that SET SESSION
	 * AUTHORIZATION logically resets SET ROLE to NONE, but we cannot set the
	 * GUC role variable from assign_session_authorization (because we haven't
	 * got enough info to call set_config_option).
	 */
	if (savedoid != GetCurrentRoleId())
		return "none";

	return endptr + 1;
}
