/*-------------------------------------------------------------------------
 *
 * misc.c
 *
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/misc.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/file.h>
#include <signal.h>
#include <dirent.h>
#include <math.h>
#include <unistd.h>

#include "access/sysattr.h"
#include "catalog/catalog.h"
#include "catalog/pg_tablespace.h"
#include "catalog/pg_type.h"
#include "commands/dbcommands.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "parser/scansup.h"
#include "parser/keywords.h"
#include "postmaster/syslogger.h"
#include "rewrite/rewriteHandler.h"
#include "storage/fd.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "utils/lsyscache.h"
#include "utils/ruleutils.h"
#include "tcop/tcopprot.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/timestamp.h"

#define atooid(x)  ((Oid) strtoul((x), NULL, 10))


/*
 * Common subroutine for num_nulls() and num_nonnulls().
 * Returns TRUE if successful, FALSE if function should return NULL.
 * If successful, total argument count and number of nulls are
 * returned into *nargs and *nulls.
 */
static bool
count_nulls(FunctionCallInfo fcinfo,
			int32 *nargs, int32 *nulls)
{
	int32		count = 0;
	int			i;

	/* Did we get a VARIADIC array argument, or separate arguments? */
	if (get_fn_expr_variadic(fcinfo->flinfo))
	{
		ArrayType  *arr;
		int			ndims,
					nitems,
				   *dims;
		bits8	   *bitmap;

		Assert(PG_NARGS() == 1);

		/*
		 * If we get a null as VARIADIC array argument, we can't say anything
		 * useful about the number of elements, so return NULL.  This behavior
		 * is consistent with other variadic functions - see concat_internal.
		 */
		if (PG_ARGISNULL(0))
			return false;

		/*
		 * Non-null argument had better be an array.  We assume that any call
		 * context that could let get_fn_expr_variadic return true will have
		 * checked that a VARIADIC-labeled parameter actually is an array.  So
		 * it should be okay to just Assert that it's an array rather than
		 * doing a full-fledged error check.
		 */
		Assert(OidIsValid(get_base_element_type(get_fn_expr_argtype(fcinfo->flinfo, 0))));

		/* OK, safe to fetch the array value */
		arr = PG_GETARG_ARRAYTYPE_P(0);

		/* Count the array elements */
		ndims = ARR_NDIM(arr);
		dims = ARR_DIMS(arr);
		nitems = ArrayGetNItems(ndims, dims);

		/* Count those that are NULL */
		bitmap = ARR_NULLBITMAP(arr);
		if (bitmap)
		{
			int			bitmask = 1;

			for (i = 0; i < nitems; i++)
			{
				if ((*bitmap & bitmask) == 0)
					count++;

				bitmask <<= 1;
				if (bitmask == 0x100)
				{
					bitmap++;
					bitmask = 1;
				}
			}
		}

		*nargs = nitems;
		*nulls = count;
	}
	else
	{
		/* Separate arguments, so just count 'em */
		for (i = 0; i < PG_NARGS(); i++)
		{
			if (PG_ARGISNULL(i))
				count++;
		}

		*nargs = PG_NARGS();
		*nulls = count;
	}

	return true;
}

/*
 * num_nulls()
 *	Count the number of NULL arguments
 */
Datum
pg_num_nulls(PG_FUNCTION_ARGS)
{
	int32		nargs,
				nulls;

	if (!count_nulls(fcinfo, &nargs, &nulls))
		PG_RETURN_NULL();

	PG_RETURN_INT32(nulls);
}

/*
 * num_nonnulls()
 *	Count the number of non-NULL arguments
 */
Datum
pg_num_nonnulls(PG_FUNCTION_ARGS)
{
	int32		nargs,
				nulls;

	if (!count_nulls(fcinfo, &nargs, &nulls))
		PG_RETURN_NULL();

	PG_RETURN_INT32(nargs - nulls);
}


/*
 * current_database()
 *	Expose the current database to the user
 */
Datum
current_database(PG_FUNCTION_ARGS)
{
	Name		db;

	db = (Name) palloc(NAMEDATALEN);

	namestrcpy(db, get_database_name(MyDatabaseId));
	PG_RETURN_NAME(db);
}


/*
 * current_query()
 *	Expose the current query to the user (useful in stored procedures)
 *	We might want to use ActivePortal->sourceText someday.
 */
Datum
current_query(PG_FUNCTION_ARGS)
{
	/* there is no easy way to access the more concise 'query_string' */
	if (debug_query_string)
		PG_RETURN_TEXT_P(cstring_to_text(debug_query_string));
	else
		PG_RETURN_NULL();
}

/*
 * Send a signal to another backend.
 *
 * The signal is delivered if the user is either a superuser or the same
 * role as the backend being signaled. For "dangerous" signals, an explicit
 * check for superuser needs to be done prior to calling this function.
 *
 * Returns 0 on success, 1 on general failure, 2 on normal permission error
 * and 3 if the caller needs to be a superuser.
 *
 * In the event of a general failure (return code 1), a warning message will
 * be emitted. For permission errors, doing that is the responsibility of
 * the caller.
 */
#define SIGNAL_BACKEND_SUCCESS 0
#define SIGNAL_BACKEND_ERROR 1
#define SIGNAL_BACKEND_NOPERMISSION 2
#define SIGNAL_BACKEND_NOSUPERUSER 3
static int
pg_signal_backend(int pid, int sig)
{
	PGPROC	   *proc = BackendPidGetProc(pid);

	/*
	 * BackendPidGetProc returns NULL if the pid isn't valid; but by the time
	 * we reach kill(), a process for which we get a valid proc here might
	 * have terminated on its own.  There's no way to acquire a lock on an
	 * arbitrary process to prevent that. But since so far all the callers of
	 * this mechanism involve some request for ending the process anyway, that
	 * it might end on its own first is not a problem.
	 */
	if (proc == NULL)
	{
		/*
		 * This is just a warning so a loop-through-resultset will not abort
		 * if one backend terminated on its own during the run.
		 */
		ereport(WARNING,
				(errmsg("PID %d is not a PostgreSQL server process", pid)));
		return SIGNAL_BACKEND_ERROR;
	}

	/* Only allow superusers to signal superuser-owned backends. */
	if (superuser_arg(proc->roleId) && !superuser())
		return SIGNAL_BACKEND_NOSUPERUSER;

	/* Users can signal backends they have role membership in. */
	if (!has_privs_of_role(GetUserId(), proc->roleId))
		return SIGNAL_BACKEND_NOPERMISSION;

	/*
	 * Can the process we just validated above end, followed by the pid being
	 * recycled for a new process, before reaching here?  Then we'd be trying
	 * to kill the wrong thing.  Seems near impossible when sequential pid
	 * assignment and wraparound is used.  Perhaps it could happen on a system
	 * where pid re-use is randomized.  That race condition possibility seems
	 * too unlikely to worry about.
	 */

	/* If we have setsid(), signal the backend's whole process group */
#ifdef HAVE_SETSID
	if (kill(-pid, sig))
#else
	if (kill(pid, sig))
#endif
	{
		/* Again, just a warning to allow loops */
		ereport(WARNING,
				(errmsg("could not send signal to process %d: %m", pid)));
		return SIGNAL_BACKEND_ERROR;
	}
	return SIGNAL_BACKEND_SUCCESS;
}

/*
 * Signal to cancel a backend process.  This is allowed if you are a member of
 * the role whose process is being canceled.
 *
 * Note that only superusers can signal superuser-owned processes.
 */
Datum
pg_cancel_backend(PG_FUNCTION_ARGS)
{
	int			r = pg_signal_backend(PG_GETARG_INT32(0), SIGINT);

	if (r == SIGNAL_BACKEND_NOSUPERUSER)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 (errmsg("must be a superuser to cancel superuser query"))));

	if (r == SIGNAL_BACKEND_NOPERMISSION)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 (errmsg("must be a member of the role whose query is being canceled"))));

	PG_RETURN_BOOL(r == SIGNAL_BACKEND_SUCCESS);
}

/*
 * Signal to terminate a backend process.  This is allowed if you are a member
 * of the role whose process is being terminated.
 *
 * Note that only superusers can signal superuser-owned processes.
 */
Datum
pg_terminate_backend(PG_FUNCTION_ARGS)
{
	int			r = pg_signal_backend(PG_GETARG_INT32(0), SIGTERM);

	if (r == SIGNAL_BACKEND_NOSUPERUSER)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
			(errmsg("must be a superuser to terminate superuser process"))));

	if (r == SIGNAL_BACKEND_NOPERMISSION)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 (errmsg("must be a member of the role whose process is being terminated"))));

	PG_RETURN_BOOL(r == SIGNAL_BACKEND_SUCCESS);
}

/*
 * Signal to reload the database configuration
 */
Datum
pg_reload_conf(PG_FUNCTION_ARGS)
{
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 (errmsg("must be superuser to signal the postmaster"))));

	if (kill(PostmasterPid, SIGHUP))
	{
		ereport(WARNING,
				(errmsg("failed to send signal to postmaster: %m")));
		PG_RETURN_BOOL(false);
	}

	PG_RETURN_BOOL(true);
}


/*
 * Rotate log file
 */
Datum
pg_rotate_logfile(PG_FUNCTION_ARGS)
{
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 (errmsg("must be superuser to rotate log files"))));

	if (!Logging_collector)
	{
		ereport(WARNING,
		(errmsg("rotation not possible because log collection not active")));
		PG_RETURN_BOOL(false);
	}

	SendPostmasterSignal(PMSIGNAL_ROTATE_LOGFILE);
	PG_RETURN_BOOL(true);
}

/* Function to find out which databases make use of a tablespace */

typedef struct
{
	char	   *location;
	DIR		   *dirdesc;
} ts_db_fctx;

Datum
pg_tablespace_databases(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	struct dirent *de;
	ts_db_fctx *fctx;

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext;
		Oid			tablespaceOid = PG_GETARG_OID(0);

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		fctx = palloc(sizeof(ts_db_fctx));

		if (tablespaceOid == GLOBALTABLESPACE_OID)
		{
			fctx->dirdesc = NULL;
			ereport(WARNING,
					(errmsg("global tablespace never has databases")));
		}
		else
		{
			if (tablespaceOid == DEFAULTTABLESPACE_OID)
				fctx->location = psprintf("base");
			else
				fctx->location = psprintf("pg_tblspc/%u/%s", tablespaceOid,
										  TABLESPACE_VERSION_DIRECTORY);

			fctx->dirdesc = AllocateDir(fctx->location);

			if (!fctx->dirdesc)
			{
				/* the only expected error is ENOENT */
				if (errno != ENOENT)
					ereport(ERROR,
							(errcode_for_file_access(),
							 errmsg("could not open directory \"%s\": %m",
									fctx->location)));
				ereport(WARNING,
					  (errmsg("%u is not a tablespace OID", tablespaceOid)));
			}
		}
		funcctx->user_fctx = fctx;
		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	fctx = (ts_db_fctx *) funcctx->user_fctx;

	if (!fctx->dirdesc)			/* not a tablespace */
		SRF_RETURN_DONE(funcctx);

	while ((de = ReadDir(fctx->dirdesc, fctx->location)) != NULL)
	{
		char	   *subdir;
		DIR		   *dirdesc;
		Oid			datOid = atooid(de->d_name);

		/* this test skips . and .., but is awfully weak */
		if (!datOid)
			continue;

		/* if database subdir is empty, don't report tablespace as used */

		subdir = psprintf("%s/%s", fctx->location, de->d_name);
		dirdesc = AllocateDir(subdir);
		while ((de = ReadDir(dirdesc, subdir)) != NULL)
		{
			if (strcmp(de->d_name, ".") != 0 && strcmp(de->d_name, "..") != 0)
				break;
		}
		FreeDir(dirdesc);
		pfree(subdir);

		if (!de)
			continue;			/* indeed, nothing in it */

		SRF_RETURN_NEXT(funcctx, ObjectIdGetDatum(datOid));
	}

	FreeDir(fctx->dirdesc);
	SRF_RETURN_DONE(funcctx);
}


/*
 * pg_tablespace_location - get location for a tablespace
 */
Datum
pg_tablespace_location(PG_FUNCTION_ARGS)
{
	Oid			tablespaceOid = PG_GETARG_OID(0);
	char		sourcepath[MAXPGPATH];
	char		targetpath[MAXPGPATH];
	int			rllen;

	/*
	 * It's useful to apply this function to pg_class.reltablespace, wherein
	 * zero means "the database's default tablespace".  So, rather than
	 * throwing an error for zero, we choose to assume that's what is meant.
	 */
	if (tablespaceOid == InvalidOid)
		tablespaceOid = MyDatabaseTableSpace;

	/*
	 * Return empty string for the cluster's default tablespaces
	 */
	if (tablespaceOid == DEFAULTTABLESPACE_OID ||
		tablespaceOid == GLOBALTABLESPACE_OID)
		PG_RETURN_TEXT_P(cstring_to_text(""));

#if defined(HAVE_READLINK) || defined(WIN32)

	/*
	 * Find the location of the tablespace by reading the symbolic link that
	 * is in pg_tblspc/<oid>.
	 */
	snprintf(sourcepath, sizeof(sourcepath), "pg_tblspc/%u", tablespaceOid);

	rllen = readlink(sourcepath, targetpath, sizeof(targetpath));
	if (rllen < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read symbolic link \"%s\": %m",
						sourcepath)));
	if (rllen >= sizeof(targetpath))
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("symbolic link \"%s\" target is too long",
						sourcepath)));
	targetpath[rllen] = '\0';

	PG_RETURN_TEXT_P(cstring_to_text(targetpath));
#else
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("tablespaces are not supported on this platform")));
	PG_RETURN_NULL();
#endif
}

/*
 * pg_sleep - delay for N seconds
 */
Datum
pg_sleep(PG_FUNCTION_ARGS)
{
	float8		secs = PG_GETARG_FLOAT8(0);
	float8		endtime;

	/*
	 * We sleep using WaitLatch, to ensure that we'll wake up promptly if an
	 * important signal (such as SIGALRM or SIGINT) arrives.  Because
	 * WaitLatch's upper limit of delay is INT_MAX milliseconds, and the user
	 * might ask for more than that, we sleep for at most 10 minutes and then
	 * loop.
	 *
	 * By computing the intended stop time initially, we avoid accumulation of
	 * extra delay across multiple sleeps.  This also ensures we won't delay
	 * less than the specified time when WaitLatch is terminated early by a
	 * non-query-cancelling signal such as SIGHUP.
	 */

#ifdef HAVE_INT64_TIMESTAMP
#define GetNowFloat()	((float8) GetCurrentTimestamp() / 1000000.0)
#else
#define GetNowFloat()	GetCurrentTimestamp()
#endif

	endtime = GetNowFloat() + secs;

	for (;;)
	{
		float8		delay;
		long		delay_ms;

		CHECK_FOR_INTERRUPTS();

		delay = endtime - GetNowFloat();
		if (delay >= 600.0)
			delay_ms = 600000;
		else if (delay > 0.0)
			delay_ms = (long) ceil(delay * 1000.0);
		else
			break;

		(void) WaitLatch(MyLatch,
						 WL_LATCH_SET | WL_TIMEOUT,
						 delay_ms);
		ResetLatch(MyLatch);
	}

	PG_RETURN_VOID();
}

/* Function to return the list of grammar keywords */
Datum
pg_get_keywords(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext;
		TupleDesc	tupdesc;

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		tupdesc = CreateTemplateTupleDesc(3, false);
		TupleDescInitEntry(tupdesc, (AttrNumber) 1, "word",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 2, "catcode",
						   CHAROID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 3, "catdesc",
						   TEXTOID, -1, 0);

		funcctx->attinmeta = TupleDescGetAttInMetadata(tupdesc);

		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();

	if (funcctx->call_cntr < NumScanKeywords)
	{
		char	   *values[3];
		HeapTuple	tuple;

		/* cast-away-const is ugly but alternatives aren't much better */
		values[0] = (char *) ScanKeywords[funcctx->call_cntr].name;

		switch (ScanKeywords[funcctx->call_cntr].category)
		{
			case UNRESERVED_KEYWORD:
				values[1] = "U";
				values[2] = _("unreserved");
				break;
			case COL_NAME_KEYWORD:
				values[1] = "C";
				values[2] = _("unreserved (cannot be function or type name)");
				break;
			case TYPE_FUNC_NAME_KEYWORD:
				values[1] = "T";
				values[2] = _("reserved (can be function or type name)");
				break;
			case RESERVED_KEYWORD:
				values[1] = "R";
				values[2] = _("reserved");
				break;
			default:			/* shouldn't be possible */
				values[1] = NULL;
				values[2] = NULL;
				break;
		}

		tuple = BuildTupleFromCStrings(funcctx->attinmeta, values);

		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
	}

	SRF_RETURN_DONE(funcctx);
}


/*
 * Return the type of the argument.
 */
Datum
pg_typeof(PG_FUNCTION_ARGS)
{
	PG_RETURN_OID(get_fn_expr_argtype(fcinfo->flinfo, 0));
}


/*
 * Implementation of the COLLATE FOR expression; returns the collation
 * of the argument.
 */
Datum
pg_collation_for(PG_FUNCTION_ARGS)
{
	Oid			typeid;
	Oid			collid;

	typeid = get_fn_expr_argtype(fcinfo->flinfo, 0);
	if (!typeid)
		PG_RETURN_NULL();
	if (!type_is_collatable(typeid) && typeid != UNKNOWNOID)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("collations are not supported by type %s",
						format_type_be(typeid))));

	collid = PG_GET_COLLATION();
	if (!collid)
		PG_RETURN_NULL();
	PG_RETURN_TEXT_P(cstring_to_text(generate_collation_name(collid)));
}


/*
 * pg_relation_is_updatable - determine which update events the specified
 * relation supports.
 *
 * This relies on relation_is_updatable() in rewriteHandler.c, which see
 * for additional information.
 */
Datum
pg_relation_is_updatable(PG_FUNCTION_ARGS)
{
	Oid			reloid = PG_GETARG_OID(0);
	bool		include_triggers = PG_GETARG_BOOL(1);

	PG_RETURN_INT32(relation_is_updatable(reloid, include_triggers, NULL));
}

/*
 * pg_column_is_updatable - determine whether a column is updatable
 *
 * This function encapsulates the decision about just what
 * information_schema.columns.is_updatable actually means.  It's not clear
 * whether deletability of the column's relation should be required, so
 * we want that decision in C code where we could change it without initdb.
 */
Datum
pg_column_is_updatable(PG_FUNCTION_ARGS)
{
	Oid			reloid = PG_GETARG_OID(0);
	AttrNumber	attnum = PG_GETARG_INT16(1);
	AttrNumber	col = attnum - FirstLowInvalidHeapAttributeNumber;
	bool		include_triggers = PG_GETARG_BOOL(2);
	int			events;

	/* System columns are never updatable */
	if (attnum <= 0)
		PG_RETURN_BOOL(false);

	events = relation_is_updatable(reloid, include_triggers,
								   bms_make_singleton(col));

	/* We require both updatability and deletability of the relation */
#define REQ_EVENTS ((1 << CMD_UPDATE) | (1 << CMD_DELETE))

	PG_RETURN_BOOL((events & REQ_EVENTS) == REQ_EVENTS);
}


/*
 * This simple parser utility are compatible with lexer implementation,
 * used only in parse_ident function
 */
static bool
is_ident_start(unsigned char c)
{
	if (c == '_')
		return true;
	if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
		return true;

	if (c >= 0200 && c <= 0377)
		return true;

	return false;
}

static bool
is_ident_cont(unsigned char c)
{
	if (c >= '0' && c <= '9')
		return true;

	return is_ident_start(c);
}

/*
 * Sanitize SQL string for using in error message.
 */
static char *
sanitize_text(text *t)
{
	int			len = VARSIZE_ANY_EXHDR(t);
	const char *p = VARDATA_ANY(t);
	StringInfo	dstr;

	dstr = makeStringInfo();

	appendStringInfoChar(dstr, '"');

	while (len--)
	{
		switch (*p)
		{
			case '\b':
				appendStringInfoString(dstr, "\\b");
				break;
			case '\f':
				appendStringInfoString(dstr, "\\f");
				break;
			case '\n':
				appendStringInfoString(dstr, "\\n");
				break;
			case '\r':
				appendStringInfoString(dstr, "\\r");
				break;
			case '\t':
				appendStringInfoString(dstr, "\\t");
				break;
			case '\'':
				appendStringInfoString(dstr, "''");
				break;
			case '\\':
				appendStringInfoString(dstr, "\\\\");
				break;
			default:
				if ((unsigned char) *p < ' ')
					appendStringInfo(dstr, "\\u%04x", (int) *p);
				else
					appendStringInfoCharMacro(dstr, *p);
				break;
		}
		p++;
	}

	appendStringInfoChar(dstr, '"');

	return dstr->data;
}

/*
 * parse_ident - parse SQL composed identifier to separate identifiers.
 * When strict mode is active (second parameter), then any chars after
 * last identifiers are disallowed.
 */
Datum
parse_ident(PG_FUNCTION_ARGS)
{
	text	   *qualname;
	char	   *qualname_str;
	bool		strict;
	char	   *nextp;
	bool		after_dot = false;
	ArrayBuildState *astate = NULL;

	qualname = PG_GETARG_TEXT_PP(0);
	qualname_str = text_to_cstring(qualname);
	strict = PG_GETARG_BOOL(1);

	nextp = qualname_str;

	/* skip leading whitespace */
	while (isspace((unsigned char) *nextp))
		nextp++;

	for (;;)
	{
		char		*curname;
		char		*endp;
		bool		missing_ident;

		missing_ident = true;

		if (*nextp == '\"')
		{
			curname = nextp + 1;
			for (;;)
			{
				endp = strchr(nextp + 1, '\"');
				if (endp == NULL)
					ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("unclosed double quotes"),
						 errdetail("string %s is not valid identifier",
									sanitize_text(qualname))));
				if (endp[1] != '\"')
					break;
				memmove(endp, endp + 1, strlen(endp));
				nextp = endp;
			}
			nextp = endp + 1;
			*endp = '\0';

			/* Show complete input string in this case. */
			if (endp - curname == 0)
				ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("identifier should not be empty: %s",
							sanitize_text(qualname))));

			astate = accumArrayResult(astate, CStringGetTextDatum(curname),
									  false, TEXTOID, CurrentMemoryContext);
			missing_ident = false;
		}
		else
		{
			if (is_ident_start((unsigned char) *nextp))
			{
				char *downname;
				int	len;
				text	*part;

				curname = nextp++;
				while (is_ident_cont((unsigned char) *nextp))
					nextp++;

				len = nextp - curname;

				/*
				 * Unlike name, we don't implicitly truncate identifiers. This
				 * is useful for allowing the user to check for specific parts
				 * of the identifier being too long. It's easy enough for the
				 * user to get the truncated names by casting our output to
				 * name[].
				 */
				downname = downcase_identifier(curname, len, false, false);
				part = cstring_to_text_with_len(downname, len);
				astate = accumArrayResult(astate, PointerGetDatum(part), false,
										  TEXTOID, CurrentMemoryContext);
				missing_ident = false;
			}
		}

		if (missing_ident)
		{
			/* Different error messages based on where we failed. */
			if (*nextp == '.')
				ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("missing valid identifier before \".\" symbol: %s",
							sanitize_text(qualname))));
			else if (after_dot)
				ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("missing valid identifier after \".\" symbol: %s",
							sanitize_text(qualname))));
			else
				ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("missing valid identifier: %s",
							sanitize_text(qualname))));
		}

		while (isspace((unsigned char) *nextp))
			nextp++;

		if (*nextp == '.')
		{
			after_dot = true;
			nextp++;
			while (isspace((unsigned char) *nextp))
				nextp++;
		}
		else if (*nextp == '\0')
		{
			break;
		}
		else
		{
			if (strict)
				ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("identifier contains disallowed characters: %s",
							sanitize_text(qualname))));
			break;
		}
	}

	PG_RETURN_DATUM(makeArrayResult(astate, CurrentMemoryContext));
}
