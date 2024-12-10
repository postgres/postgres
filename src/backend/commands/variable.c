/*-------------------------------------------------------------------------
 *
 * variable.c
 *		Routines for handling specialized SET variables.
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/variable.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <ctype.h>

#include "access/htup_details.h"
#include "access/parallel.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "access/xlogprefetcher.h"
#include "catalog/pg_authid.h"
#include "common/string.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "postmaster/postmaster.h"
#include "postmaster/syslogger.h"
#include "storage/bufmgr.h"
#include "utils/acl.h"
#include "utils/backend_status.h"
#include "utils/datetime.h"
#include "utils/fmgrprotos.h"
#include "utils/guc_hooks.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/tzparser.h"
#include "utils/varlena.h"

/*
 * DATESTYLE
 */

/*
 * check_datestyle: GUC check_hook for datestyle
 */
bool
check_datestyle(char **newval, void **extra, GucSource source)
{
	int			newDateStyle = DateStyle;
	int			newDateOrder = DateOrder;
	bool		have_style = false;
	bool		have_order = false;
	bool		ok = true;
	char	   *rawstring;
	int		   *myextra;
	char	   *result;
	List	   *elemlist;
	ListCell   *l;

	/* Need a modifiable copy of string */
	rawstring = pstrdup(*newval);

	/* Parse string into list of identifiers */
	if (!SplitIdentifierString(rawstring, ',', &elemlist))
	{
		/* syntax error in list */
		GUC_check_errdetail("List syntax is invalid.");
		pfree(rawstring);
		list_free(elemlist);
		return false;
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
			 * We can't simply "return check_datestyle(...)" because we need
			 * to handle constructs like "DEFAULT, ISO".
			 */
			char	   *subval;
			void	   *subextra = NULL;

			subval = guc_strdup(LOG, GetConfigOptionResetString("datestyle"));
			if (!subval)
			{
				ok = false;
				break;
			}
			if (!check_datestyle(&subval, &subextra, source))
			{
				guc_free(subval);
				ok = false;
				break;
			}
			myextra = (int *) subextra;
			if (!have_style)
				newDateStyle = myextra[0];
			if (!have_order)
				newDateOrder = myextra[1];
			guc_free(subval);
			guc_free(subextra);
		}
		else
		{
			GUC_check_errdetail("Unrecognized key word: \"%s\".", tok);
			pfree(rawstring);
			list_free(elemlist);
			return false;
		}
	}

	pfree(rawstring);
	list_free(elemlist);

	if (!ok)
	{
		GUC_check_errdetail("Conflicting \"DateStyle\" specifications.");
		return false;
	}

	/*
	 * Prepare the canonical string to return.  GUC wants it guc_malloc'd.
	 */
	result = (char *) guc_malloc(LOG, 32);
	if (!result)
		return false;

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

	guc_free(*newval);
	*newval = result;

	/*
	 * Set up the "extra" struct actually used by assign_datestyle.
	 */
	myextra = (int *) guc_malloc(LOG, 2 * sizeof(int));
	if (!myextra)
		return false;
	myextra[0] = newDateStyle;
	myextra[1] = newDateOrder;
	*extra = myextra;

	return true;
}

/*
 * assign_datestyle: GUC assign_hook for datestyle
 */
void
assign_datestyle(const char *newval, void *extra)
{
	int		   *myextra = (int *) extra;

	DateStyle = myextra[0];
	DateOrder = myextra[1];
}


/*
 * TIMEZONE
 */

/*
 * check_timezone: GUC check_hook for timezone
 */
bool
check_timezone(char **newval, void **extra, GucSource source)
{
	pg_tz	   *new_tz;
	long		gmtoffset;
	char	   *endptr;
	double		hours;

	if (pg_strncasecmp(*newval, "interval", 8) == 0)
	{
		/*
		 * Support INTERVAL 'foo'.  This is for SQL spec compliance, not
		 * because it has any actual real-world usefulness.
		 */
		const char *valueptr = *newval;
		char	   *val;
		Interval   *interval;

		valueptr += 8;
		while (isspace((unsigned char) *valueptr))
			valueptr++;
		if (*valueptr++ != '\'')
			return false;
		val = pstrdup(valueptr);
		/* Check and remove trailing quote */
		endptr = strchr(val, '\'');
		if (!endptr || endptr[1] != '\0')
		{
			pfree(val);
			return false;
		}
		*endptr = '\0';

		/*
		 * Try to parse it.  XXX an invalid interval format will result in
		 * ereport(ERROR), which is not desirable for GUC.  We did what we
		 * could to guard against this in flatten_set_variable_args, but a
		 * string coming in from postgresql.conf might contain anything.
		 */
		interval = DatumGetIntervalP(DirectFunctionCall3(interval_in,
														 CStringGetDatum(val),
														 ObjectIdGetDatum(InvalidOid),
														 Int32GetDatum(-1)));

		pfree(val);
		if (interval->month != 0)
		{
			GUC_check_errdetail("Cannot specify months in time zone interval.");
			pfree(interval);
			return false;
		}
		if (interval->day != 0)
		{
			GUC_check_errdetail("Cannot specify days in time zone interval.");
			pfree(interval);
			return false;
		}

		/* Here we change from SQL to Unix sign convention */
		gmtoffset = -(interval->time / USECS_PER_SEC);
		new_tz = pg_tzset_offset(gmtoffset);

		pfree(interval);
	}
	else
	{
		/*
		 * Try it as a numeric number of hours (possibly fractional).
		 */
		hours = strtod(*newval, &endptr);
		if (endptr != *newval && *endptr == '\0')
		{
			/* Here we change from SQL to Unix sign convention */
			gmtoffset = -hours * SECS_PER_HOUR;
			new_tz = pg_tzset_offset(gmtoffset);
		}
		else
		{
			/*
			 * Otherwise assume it is a timezone name, and try to load it.
			 */
			new_tz = pg_tzset(*newval);

			if (!new_tz)
			{
				/* Doesn't seem to be any great value in errdetail here */
				return false;
			}

			if (!pg_tz_acceptable(new_tz))
			{
				GUC_check_errmsg("time zone \"%s\" appears to use leap seconds",
								 *newval);
				GUC_check_errdetail("PostgreSQL does not support leap seconds.");
				return false;
			}
		}
	}

	/* Test for failure in pg_tzset_offset, which we assume is out-of-range */
	if (!new_tz)
	{
		GUC_check_errdetail("UTC timezone offset is out of range.");
		return false;
	}

	/*
	 * Pass back data for assign_timezone to use
	 */
	*extra = guc_malloc(LOG, sizeof(pg_tz *));
	if (!*extra)
		return false;
	*((pg_tz **) *extra) = new_tz;

	return true;
}

/*
 * assign_timezone: GUC assign_hook for timezone
 */
void
assign_timezone(const char *newval, void *extra)
{
	session_timezone = *((pg_tz **) extra);
}

/*
 * show_timezone: GUC show_hook for timezone
 */
const char *
show_timezone(void)
{
	const char *tzn;

	/* Always show the zone's canonical name */
	tzn = pg_get_timezone_name(session_timezone);

	if (tzn != NULL)
		return tzn;

	return "unknown";
}


/*
 * LOG_TIMEZONE
 *
 * For log_timezone, we don't support the interval-based methods of setting a
 * zone, which are only there for SQL spec compliance not because they're
 * actually useful.
 */

/*
 * check_log_timezone: GUC check_hook for log_timezone
 */
bool
check_log_timezone(char **newval, void **extra, GucSource source)
{
	pg_tz	   *new_tz;

	/*
	 * Assume it is a timezone name, and try to load it.
	 */
	new_tz = pg_tzset(*newval);

	if (!new_tz)
	{
		/* Doesn't seem to be any great value in errdetail here */
		return false;
	}

	if (!pg_tz_acceptable(new_tz))
	{
		GUC_check_errmsg("time zone \"%s\" appears to use leap seconds",
						 *newval);
		GUC_check_errdetail("PostgreSQL does not support leap seconds.");
		return false;
	}

	/*
	 * Pass back data for assign_log_timezone to use
	 */
	*extra = guc_malloc(LOG, sizeof(pg_tz *));
	if (!*extra)
		return false;
	*((pg_tz **) *extra) = new_tz;

	return true;
}

/*
 * assign_log_timezone: GUC assign_hook for log_timezone
 */
void
assign_log_timezone(const char *newval, void *extra)
{
	log_timezone = *((pg_tz **) extra);
}

/*
 * show_log_timezone: GUC show_hook for log_timezone
 */
const char *
show_log_timezone(void)
{
	const char *tzn;

	/* Always show the zone's canonical name */
	tzn = pg_get_timezone_name(log_timezone);

	if (tzn != NULL)
		return tzn;

	return "unknown";
}


/*
 * TIMEZONE_ABBREVIATIONS
 */

/*
 * GUC check_hook for timezone_abbreviations
 */
bool
check_timezone_abbreviations(char **newval, void **extra, GucSource source)
{
	/*
	 * The boot_val for timezone_abbreviations is NULL.  When we see that we
	 * just do nothing.  If the value isn't overridden from the config file
	 * then pg_timezone_abbrev_initialize() will eventually replace it with
	 * "Default".  This hack has two purposes: to avoid wasting cycles loading
	 * values that might soon be overridden from the config file, and to avoid
	 * trying to read the timezone abbrev files during InitializeGUCOptions().
	 * The latter doesn't work in an EXEC_BACKEND subprocess because
	 * my_exec_path hasn't been set yet and so we can't locate PGSHAREDIR.
	 */
	if (*newval == NULL)
	{
		Assert(source == PGC_S_DEFAULT);
		return true;
	}

	/* OK, load the file and produce a guc_malloc'd TimeZoneAbbrevTable */
	*extra = load_tzoffsets(*newval);

	/* tzparser.c returns NULL on failure, reporting via GUC_check_errmsg */
	if (!*extra)
		return false;

	return true;
}

/*
 * GUC assign_hook for timezone_abbreviations
 */
void
assign_timezone_abbreviations(const char *newval, void *extra)
{
	/* Do nothing for the boot_val default of NULL */
	if (!extra)
		return;

	InstallTimeZoneAbbrevs((TimeZoneAbbrevTable *) extra);
}


/*
 * SET TRANSACTION READ ONLY and SET TRANSACTION READ WRITE
 *
 * We allow idempotent changes (r/w -> r/w and r/o -> r/o) at any time, and
 * we also always allow changes from read-write to read-only.  However,
 * read-only may be changed to read-write only when in a top-level transaction
 * that has not yet taken an initial snapshot.  Can't do it in a hot standby,
 * either.
 *
 * If we are not in a transaction at all, just allow the change; it means
 * nothing since XactReadOnly will be reset by the next StartTransaction().
 * The IsTransactionState() test protects us against trying to check
 * RecoveryInProgress() in contexts where shared memory is not accessible.
 * (Similarly, if we're restoring state in a parallel worker, just allow
 * the change.)
 */
bool
check_transaction_read_only(bool *newval, void **extra, GucSource source)
{
	if (*newval == false && XactReadOnly && IsTransactionState() && !InitializingParallelWorker)
	{
		/* Can't go to r/w mode inside a r/o transaction */
		if (IsSubTransaction())
		{
			GUC_check_errcode(ERRCODE_ACTIVE_SQL_TRANSACTION);
			GUC_check_errmsg("cannot set transaction read-write mode inside a read-only transaction");
			return false;
		}
		/* Top level transaction can't change to r/w after first snapshot. */
		if (FirstSnapshotSet)
		{
			GUC_check_errcode(ERRCODE_ACTIVE_SQL_TRANSACTION);
			GUC_check_errmsg("transaction read-write mode must be set before any query");
			return false;
		}
		/* Can't go to r/w mode while recovery is still active */
		if (RecoveryInProgress())
		{
			GUC_check_errcode(ERRCODE_FEATURE_NOT_SUPPORTED);
			GUC_check_errmsg("cannot set transaction read-write mode during recovery");
			return false;
		}
	}

	return true;
}

/*
 * SET TRANSACTION ISOLATION LEVEL
 *
 * We allow idempotent changes at any time, but otherwise this can only be
 * changed in a toplevel transaction that has not yet taken a snapshot.
 *
 * As in check_transaction_read_only, allow it if not inside a transaction,
 * or if restoring state in a parallel worker.
 */
bool
check_transaction_isolation(int *newval, void **extra, GucSource source)
{
	int			newXactIsoLevel = *newval;

	if (newXactIsoLevel != XactIsoLevel &&
		IsTransactionState() && !InitializingParallelWorker)
	{
		if (FirstSnapshotSet)
		{
			GUC_check_errcode(ERRCODE_ACTIVE_SQL_TRANSACTION);
			GUC_check_errmsg("SET TRANSACTION ISOLATION LEVEL must be called before any query");
			return false;
		}
		/* We ignore a subtransaction setting it to the existing value. */
		if (IsSubTransaction())
		{
			GUC_check_errcode(ERRCODE_ACTIVE_SQL_TRANSACTION);
			GUC_check_errmsg("SET TRANSACTION ISOLATION LEVEL must not be called in a subtransaction");
			return false;
		}
		/* Can't go to serializable mode while recovery is still active */
		if (newXactIsoLevel == XACT_SERIALIZABLE && RecoveryInProgress())
		{
			GUC_check_errcode(ERRCODE_FEATURE_NOT_SUPPORTED);
			GUC_check_errmsg("cannot use serializable mode in a hot standby");
			GUC_check_errhint("You can use REPEATABLE READ instead.");
			return false;
		}
	}

	return true;
}

/*
 * SET TRANSACTION [NOT] DEFERRABLE
 */

bool
check_transaction_deferrable(bool *newval, void **extra, GucSource source)
{
	/* Just accept the value when restoring state in a parallel worker */
	if (InitializingParallelWorker)
		return true;

	if (IsSubTransaction())
	{
		GUC_check_errcode(ERRCODE_ACTIVE_SQL_TRANSACTION);
		GUC_check_errmsg("SET TRANSACTION [NOT] DEFERRABLE cannot be called within a subtransaction");
		return false;
	}
	if (FirstSnapshotSet)
	{
		GUC_check_errcode(ERRCODE_ACTIVE_SQL_TRANSACTION);
		GUC_check_errmsg("SET TRANSACTION [NOT] DEFERRABLE must be called before any query");
		return false;
	}

	return true;
}

/*
 * Random number seed
 *
 * We can't roll back the random sequence on error, and we don't want
 * config file reloads to affect it, so we only want interactive SET SEED
 * commands to set it.  We use the "extra" storage to ensure that rollbacks
 * don't try to do the operation again.
 */

bool
check_random_seed(double *newval, void **extra, GucSource source)
{
	*extra = guc_malloc(LOG, sizeof(int));
	if (!*extra)
		return false;
	/* Arm the assign only if source of value is an interactive SET */
	*((int *) *extra) = (source >= PGC_S_INTERACTIVE);

	return true;
}

void
assign_random_seed(double newval, void *extra)
{
	/* We'll do this at most once for any setting of the GUC variable */
	if (*((int *) extra))
		DirectFunctionCall1(setseed, Float8GetDatum(newval));
	*((int *) extra) = 0;
}

const char *
show_random_seed(void)
{
	return "unavailable";
}


/*
 * SET CLIENT_ENCODING
 */

bool
check_client_encoding(char **newval, void **extra, GucSource source)
{
	int			encoding;
	const char *canonical_name;

	/* Look up the encoding by name */
	encoding = pg_valid_client_encoding(*newval);
	if (encoding < 0)
		return false;

	/* Get the canonical name (no aliases, uniform case) */
	canonical_name = pg_encoding_to_char(encoding);

	/*
	 * Parallel workers send data to the leader, not the client.  They always
	 * send data using the database encoding; therefore, we should never
	 * actually change the client encoding in a parallel worker.  However,
	 * during parallel worker startup, we want to accept the leader's
	 * client_encoding setting so that anyone who looks at the value in the
	 * worker sees the same value that they would see in the leader.  A change
	 * other than during startup, for example due to a SET clause attached to
	 * a function definition, should be rejected, as there is nothing we can
	 * do inside the worker to make it take effect.
	 */
	if (IsParallelWorker() && !InitializingParallelWorker)
	{
		GUC_check_errcode(ERRCODE_INVALID_TRANSACTION_STATE);
		GUC_check_errdetail("Cannot change \"client_encoding\" during a parallel operation.");
		return false;
	}

	/*
	 * If we are not within a transaction then PrepareClientEncoding will not
	 * be able to look up the necessary conversion procs.  If we are still
	 * starting up, it will return "OK" anyway, and InitializeClientEncoding
	 * will fix things once initialization is far enough along.  After
	 * startup, we'll fail.  This would only happen if someone tries to change
	 * client_encoding in postgresql.conf and then SIGHUP existing sessions.
	 * It seems like a bad idea for client_encoding to change that way anyhow,
	 * so we don't go out of our way to support it.
	 *
	 * In a parallel worker, we might as well skip PrepareClientEncoding since
	 * we're not going to use its results.
	 *
	 * Note: in the postmaster, or any other process that never calls
	 * InitializeClientEncoding, PrepareClientEncoding will always succeed,
	 * and so will SetClientEncoding; but they won't do anything, which is OK.
	 */
	if (!IsParallelWorker() &&
		PrepareClientEncoding(encoding) < 0)
	{
		if (IsTransactionState())
		{
			/* Must be a genuine no-such-conversion problem */
			GUC_check_errcode(ERRCODE_FEATURE_NOT_SUPPORTED);
			GUC_check_errdetail("Conversion between %s and %s is not supported.",
								canonical_name,
								GetDatabaseEncodingName());
		}
		else
		{
			/* Provide a useful complaint */
			GUC_check_errdetail("Cannot change \"client_encoding\" now.");
		}
		return false;
	}

	/*
	 * Replace the user-supplied string with the encoding's canonical name.
	 * This gets rid of aliases and case-folding variations.
	 *
	 * XXX Although canonicalizing seems like a good idea in the abstract, it
	 * breaks pre-9.1 JDBC drivers, which expect that if they send "UNICODE"
	 * as the client_encoding setting then it will read back the same way. As
	 * a workaround, don't replace the string if it's "UNICODE".  Remove that
	 * hack when pre-9.1 JDBC drivers are no longer in use.
	 */
	if (strcmp(*newval, canonical_name) != 0 &&
		strcmp(*newval, "UNICODE") != 0)
	{
		guc_free(*newval);
		*newval = guc_strdup(LOG, canonical_name);
		if (!*newval)
			return false;
	}

	/*
	 * Save the encoding's ID in *extra, for use by assign_client_encoding.
	 */
	*extra = guc_malloc(LOG, sizeof(int));
	if (!*extra)
		return false;
	*((int *) *extra) = encoding;

	return true;
}

void
assign_client_encoding(const char *newval, void *extra)
{
	int			encoding = *((int *) extra);

	/*
	 * In a parallel worker, we never override the client encoding that was
	 * set by ParallelWorkerMain().
	 */
	if (IsParallelWorker())
		return;

	/* We do not expect an error if PrepareClientEncoding succeeded */
	if (SetClientEncoding(encoding) < 0)
		elog(LOG, "SetClientEncoding(%d) failed", encoding);
}


/*
 * SET SESSION AUTHORIZATION
 */

typedef struct
{
	/* This is the "extra" state for both SESSION AUTHORIZATION and ROLE */
	Oid			roleid;
	bool		is_superuser;
} role_auth_extra;

bool
check_session_authorization(char **newval, void **extra, GucSource source)
{
	HeapTuple	roleTup;
	Form_pg_authid roleform;
	Oid			roleid;
	bool		is_superuser;
	role_auth_extra *myextra;

	/* Do nothing for the boot_val default of NULL */
	if (*newval == NULL)
		return true;

	if (InitializingParallelWorker)
	{
		/*
		 * In parallel worker initialization, we want to copy the leader's
		 * state even if it no longer matches the catalogs. ParallelWorkerMain
		 * already installed the correct role OID and superuser state.
		 */
		roleid = GetSessionUserId();
		is_superuser = GetSessionUserIsSuperuser();
	}
	else
	{
		if (!IsTransactionState())
		{
			/*
			 * Can't do catalog lookups, so fail.  The result of this is that
			 * session_authorization cannot be set in postgresql.conf, which
			 * seems like a good thing anyway, so we don't work hard to avoid
			 * it.
			 */
			return false;
		}

		/*
		 * When source == PGC_S_TEST, we don't throw a hard error for a
		 * nonexistent user name or insufficient privileges, only a NOTICE.
		 * See comments in guc.h.
		 */

		/* Look up the username */
		roleTup = SearchSysCache1(AUTHNAME, PointerGetDatum(*newval));
		if (!HeapTupleIsValid(roleTup))
		{
			if (source == PGC_S_TEST)
			{
				ereport(NOTICE,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("role \"%s\" does not exist", *newval)));
				return true;
			}
			GUC_check_errmsg("role \"%s\" does not exist", *newval);
			return false;
		}

		roleform = (Form_pg_authid) GETSTRUCT(roleTup);
		roleid = roleform->oid;
		is_superuser = roleform->rolsuper;

		ReleaseSysCache(roleTup);

		/*
		 * Only superusers may SET SESSION AUTHORIZATION a role other than
		 * itself. Note that in case of multiple SETs in a single session, the
		 * original authenticated user's superuserness is what matters.
		 */
		if (roleid != GetAuthenticatedUserId() &&
			!superuser_arg(GetAuthenticatedUserId()))
		{
			if (source == PGC_S_TEST)
			{
				ereport(NOTICE,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						 errmsg("permission will be denied to set session authorization \"%s\"",
								*newval)));
				return true;
			}
			GUC_check_errcode(ERRCODE_INSUFFICIENT_PRIVILEGE);
			GUC_check_errmsg("permission denied to set session authorization \"%s\"",
							 *newval);
			return false;
		}
	}

	/* Set up "extra" struct for assign_session_authorization to use */
	myextra = (role_auth_extra *) guc_malloc(LOG, sizeof(role_auth_extra));
	if (!myextra)
		return false;
	myextra->roleid = roleid;
	myextra->is_superuser = is_superuser;
	*extra = myextra;

	return true;
}

void
assign_session_authorization(const char *newval, void *extra)
{
	role_auth_extra *myextra = (role_auth_extra *) extra;

	/* Do nothing for the boot_val default of NULL */
	if (!myextra)
		return;

	SetSessionAuthorization(myextra->roleid, myextra->is_superuser);
}


/*
 * SET ROLE
 *
 * The SQL spec requires "SET ROLE NONE" to unset the role, so we hardwire
 * a translation of "none" to InvalidOid.  Otherwise this is much like
 * SET SESSION AUTHORIZATION.
 */

bool
check_role(char **newval, void **extra, GucSource source)
{
	HeapTuple	roleTup;
	Oid			roleid;
	bool		is_superuser;
	role_auth_extra *myextra;
	Form_pg_authid roleform;

	if (strcmp(*newval, "none") == 0)
	{
		/* hardwired translation */
		roleid = InvalidOid;
		is_superuser = false;
	}
	else if (InitializingParallelWorker)
	{
		/*
		 * In parallel worker initialization, we want to copy the leader's
		 * state even if it no longer matches the catalogs. ParallelWorkerMain
		 * already installed the correct role OID and superuser state.
		 */
		roleid = GetCurrentRoleId();
		is_superuser = current_role_is_superuser;
	}
	else
	{
		if (!IsTransactionState())
		{
			/*
			 * Can't do catalog lookups, so fail.  The result of this is that
			 * role cannot be set in postgresql.conf, which seems like a good
			 * thing anyway, so we don't work hard to avoid it.
			 */
			return false;
		}

		/*
		 * When source == PGC_S_TEST, we don't throw a hard error for a
		 * nonexistent user name or insufficient privileges, only a NOTICE.
		 * See comments in guc.h.
		 */

		/* Look up the username */
		roleTup = SearchSysCache1(AUTHNAME, PointerGetDatum(*newval));
		if (!HeapTupleIsValid(roleTup))
		{
			if (source == PGC_S_TEST)
			{
				ereport(NOTICE,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("role \"%s\" does not exist", *newval)));
				return true;
			}
			GUC_check_errmsg("role \"%s\" does not exist", *newval);
			return false;
		}

		roleform = (Form_pg_authid) GETSTRUCT(roleTup);
		roleid = roleform->oid;
		is_superuser = roleform->rolsuper;

		ReleaseSysCache(roleTup);

		/* Verify that session user is allowed to become this role */
		if (!member_can_set_role(GetSessionUserId(), roleid))
		{
			if (source == PGC_S_TEST)
			{
				ereport(NOTICE,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						 errmsg("permission will be denied to set role \"%s\"",
								*newval)));
				return true;
			}
			GUC_check_errcode(ERRCODE_INSUFFICIENT_PRIVILEGE);
			GUC_check_errmsg("permission denied to set role \"%s\"",
							 *newval);
			return false;
		}
	}

	/* Set up "extra" struct for assign_role to use */
	myextra = (role_auth_extra *) guc_malloc(LOG, sizeof(role_auth_extra));
	if (!myextra)
		return false;
	myextra->roleid = roleid;
	myextra->is_superuser = is_superuser;
	*extra = myextra;

	return true;
}

void
assign_role(const char *newval, void *extra)
{
	role_auth_extra *myextra = (role_auth_extra *) extra;

	SetCurrentRoleId(myextra->roleid, myextra->is_superuser);
}

const char *
show_role(void)
{
	/*
	 * Check whether SET ROLE is active; if not return "none".  This is a
	 * kluge to deal with the fact that SET SESSION AUTHORIZATION logically
	 * resets SET ROLE to NONE, but we cannot set the GUC role variable from
	 * assign_session_authorization (because we haven't got enough info to
	 * call set_config_option).
	 */
	if (!OidIsValid(GetCurrentRoleId()))
		return "none";

	/* Otherwise we can just use the GUC string */
	return role_string ? role_string : "none";
}


/*
 * PATH VARIABLES
 *
 * check_canonical_path is used for log_directory and some other GUCs where
 * all we want to do is canonicalize the represented path name.
 */

bool
check_canonical_path(char **newval, void **extra, GucSource source)
{
	/*
	 * Since canonicalize_path never enlarges the string, we can just modify
	 * newval in-place.  But watch out for NULL, which is the default value
	 * for external_pid_file.
	 */
	if (*newval)
		canonicalize_path(*newval);
	return true;
}


/*
 * MISCELLANEOUS
 */

/*
 * GUC check_hook for application_name
 */
bool
check_application_name(char **newval, void **extra, GucSource source)
{
	char	   *clean;
	char	   *ret;

	/* Only allow clean ASCII chars in the application name */
	clean = pg_clean_ascii(*newval, MCXT_ALLOC_NO_OOM);
	if (!clean)
		return false;

	ret = guc_strdup(WARNING, clean);
	if (!ret)
	{
		pfree(clean);
		return false;
	}

	guc_free(*newval);

	pfree(clean);
	*newval = ret;
	return true;
}

/*
 * GUC assign_hook for application_name
 */
void
assign_application_name(const char *newval, void *extra)
{
	/* Update the pg_stat_activity view */
	pgstat_report_appname(newval);
}

/*
 * GUC check_hook for cluster_name
 */
bool
check_cluster_name(char **newval, void **extra, GucSource source)
{
	char	   *clean;
	char	   *ret;

	/* Only allow clean ASCII chars in the cluster name */
	clean = pg_clean_ascii(*newval, MCXT_ALLOC_NO_OOM);
	if (!clean)
		return false;

	ret = guc_strdup(WARNING, clean);
	if (!ret)
	{
		pfree(clean);
		return false;
	}

	guc_free(*newval);

	pfree(clean);
	*newval = ret;
	return true;
}

/*
 * GUC assign_hook for maintenance_io_concurrency
 */
void
assign_maintenance_io_concurrency(int newval, void *extra)
{
#ifdef USE_PREFETCH
	/*
	 * Reconfigure recovery prefetching, because a setting it depends on
	 * changed.
	 */
	maintenance_io_concurrency = newval;
	if (AmStartupProcess())
		XLogPrefetchReconfigure();
#endif
}


/*
 * These show hooks just exist because we want to show the values in octal.
 */

/*
 * GUC show_hook for data_directory_mode
 */
const char *
show_data_directory_mode(void)
{
	static char buf[12];

	snprintf(buf, sizeof(buf), "%04o", data_directory_mode);
	return buf;
}

/*
 * GUC show_hook for log_file_mode
 */
const char *
show_log_file_mode(void)
{
	static char buf[12];

	snprintf(buf, sizeof(buf), "%04o", Log_file_mode);
	return buf;
}

/*
 * GUC show_hook for unix_socket_permissions
 */
const char *
show_unix_socket_permissions(void)
{
	static char buf[12];

	snprintf(buf, sizeof(buf), "%04o", Unix_socket_permissions);
	return buf;
}


/*
 * These check hooks do nothing more than reject non-default settings
 * in builds that don't support them.
 */

bool
check_bonjour(bool *newval, void **extra, GucSource source)
{
#ifndef USE_BONJOUR
	if (*newval)
	{
		GUC_check_errmsg("Bonjour is not supported by this build");
		return false;
	}
#endif
	return true;
}

bool
check_default_with_oids(bool *newval, void **extra, GucSource source)
{
	if (*newval)
	{
		/* check the GUC's definition for an explanation */
		GUC_check_errcode(ERRCODE_FEATURE_NOT_SUPPORTED);
		GUC_check_errmsg("tables declared WITH OIDS are not supported");

		return false;
	}

	return true;
}

bool
check_effective_io_concurrency(int *newval, void **extra, GucSource source)
{
#ifndef USE_PREFETCH
	if (*newval != 0)
	{
		GUC_check_errdetail("\"%s\" must be set to 0 on platforms that lack support for issuing read-ahead advice.",
							"effective_io_concurrency");
		return false;
	}
#endif							/* USE_PREFETCH */
	return true;
}

bool
check_maintenance_io_concurrency(int *newval, void **extra, GucSource source)
{
#ifndef USE_PREFETCH
	if (*newval != 0)
	{
		GUC_check_errdetail("\"%s\" must be set to 0 on platforms that lack support for issuing read-ahead advice.",
							"maintenance_io_concurrency");
		return false;
	}
#endif							/* USE_PREFETCH */
	return true;
}

bool
check_ssl(bool *newval, void **extra, GucSource source)
{
#ifndef USE_SSL
	if (*newval)
	{
		GUC_check_errmsg("SSL is not supported by this build");
		return false;
	}
#endif
	return true;
}
