/*-------------------------------------------------------------------------
 *
 * misc.c
 *
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
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
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <math.h>
#include <unistd.h>

#include "access/sysattr.h"
#include "access/table.h"
#include "catalog/catalog.h"
#include "catalog/pg_tablespace.h"
#include "catalog/pg_type.h"
#include "catalog/system_fk_info.h"
#include "commands/dbcommands.h"
#include "commands/tablespace.h"
#include "common/keywords.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "parser/scansup.h"
#include "pgstat.h"
#include "postmaster/syslogger.h"
#include "rewrite/rewriteHandler.h"
#include "storage/fd.h"
#include "storage/latch.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/ruleutils.h"
#include "utils/timestamp.h"

/*
 * Common subroutine for num_nulls() and num_nonnulls().
 * Returns true if successful, false if function should return NULL.
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

/* Function to find out which databases make use of a tablespace */

Datum
pg_tablespace_databases(PG_FUNCTION_ARGS)
{
	Oid			tablespaceOid = PG_GETARG_OID(0);
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	char	   *location;
	DIR		   *dirdesc;
	struct dirent *de;

	SetSingleFuncCall(fcinfo, SRF_SINGLE_USE_EXPECTED);

	if (tablespaceOid == GLOBALTABLESPACE_OID)
	{
		ereport(WARNING,
				(errmsg("global tablespace never has databases")));
		/* return empty tuplestore */
		return (Datum) 0;
	}

	if (tablespaceOid == DEFAULTTABLESPACE_OID)
		location = psprintf("base");
	else
		location = psprintf("pg_tblspc/%u/%s", tablespaceOid,
							TABLESPACE_VERSION_DIRECTORY);

	dirdesc = AllocateDir(location);

	if (!dirdesc)
	{
		/* the only expected error is ENOENT */
		if (errno != ENOENT)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not open directory \"%s\": %m",
							location)));
		ereport(WARNING,
				(errmsg("%u is not a tablespace OID", tablespaceOid)));
		/* return empty tuplestore */
		return (Datum) 0;
	}

	while ((de = ReadDir(dirdesc, location)) != NULL)
	{
		Oid			datOid = atooid(de->d_name);
		char	   *subdir;
		bool		isempty;
		Datum		values[1];
		bool		nulls[1];

		/* this test skips . and .., but is awfully weak */
		if (!datOid)
			continue;

		/* if database subdir is empty, don't report tablespace as used */

		subdir = psprintf("%s/%s", location, de->d_name);
		isempty = directory_is_empty(subdir);
		pfree(subdir);

		if (isempty)
			continue;			/* indeed, nothing in it */

		values[0] = ObjectIdGetDatum(datOid);
		nulls[0] = false;

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc,
							 values, nulls);
	}

	FreeDir(dirdesc);
	return (Datum) 0;
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
#ifndef WIN32
	struct stat st;
#endif

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

	/*
	 * Before reading the link, check if the source path is a link or a
	 * junction point.  Note that a directory is possible for a tablespace
	 * created with allow_in_place_tablespaces enabled.  If a directory is
	 * found, a relative path to the data directory is returned.
	 */
#ifdef WIN32
	if (!pgwin32_is_junction(sourcepath))
		PG_RETURN_TEXT_P(cstring_to_text(sourcepath));
#else
	if (lstat(sourcepath, &st) < 0)
	{
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not stat file \"%s\": %m",
						sourcepath)));
	}

	if (!S_ISLNK(st.st_mode))
		PG_RETURN_TEXT_P(cstring_to_text(sourcepath));
#endif

	/*
	 * In presence of a link or a junction point, return the path pointing to.
	 */
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
	 * non-query-canceling signal such as SIGHUP.
	 */
#define GetNowFloat()	((float8) GetCurrentTimestamp() / 1000000.0)

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
						 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 delay_ms,
						 WAIT_EVENT_PG_SLEEP);
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

		tupdesc = CreateTemplateTupleDesc(5);
		TupleDescInitEntry(tupdesc, (AttrNumber) 1, "word",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 2, "catcode",
						   CHAROID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 3, "barelabel",
						   BOOLOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 4, "catdesc",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 5, "baredesc",
						   TEXTOID, -1, 0);

		funcctx->attinmeta = TupleDescGetAttInMetadata(tupdesc);

		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();

	if (funcctx->call_cntr < ScanKeywords.num_keywords)
	{
		char	   *values[5];
		HeapTuple	tuple;

		/* cast-away-const is ugly but alternatives aren't much better */
		values[0] = unconstify(char *,
							   GetScanKeyword(funcctx->call_cntr,
											  &ScanKeywords));

		switch (ScanKeywordCategories[funcctx->call_cntr])
		{
			case UNRESERVED_KEYWORD:
				values[1] = "U";
				values[3] = _("unreserved");
				break;
			case COL_NAME_KEYWORD:
				values[1] = "C";
				values[3] = _("unreserved (cannot be function or type name)");
				break;
			case TYPE_FUNC_NAME_KEYWORD:
				values[1] = "T";
				values[3] = _("reserved (can be function or type name)");
				break;
			case RESERVED_KEYWORD:
				values[1] = "R";
				values[3] = _("reserved");
				break;
			default:			/* shouldn't be possible */
				values[1] = NULL;
				values[3] = NULL;
				break;
		}

		if (ScanKeywordBareLabel[funcctx->call_cntr])
		{
			values[2] = "true";
			values[4] = _("can be bare label");
		}
		else
		{
			values[2] = "false";
			values[4] = _("requires AS");
		}

		tuple = BuildTupleFromCStrings(funcctx->attinmeta, values);

		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
	}

	SRF_RETURN_DONE(funcctx);
}


/* Function to return the list of catalog foreign key relationships */
Datum
pg_get_catalog_foreign_keys(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	FmgrInfo   *arrayinp;

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext;
		TupleDesc	tupdesc;

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		tupdesc = CreateTemplateTupleDesc(6);
		TupleDescInitEntry(tupdesc, (AttrNumber) 1, "fktable",
						   REGCLASSOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 2, "fkcols",
						   TEXTARRAYOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 3, "pktable",
						   REGCLASSOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 4, "pkcols",
						   TEXTARRAYOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 5, "is_array",
						   BOOLOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 6, "is_opt",
						   BOOLOID, -1, 0);

		funcctx->tuple_desc = BlessTupleDesc(tupdesc);

		/*
		 * We use array_in to convert the C strings in sys_fk_relationships[]
		 * to text arrays.  But we cannot use DirectFunctionCallN to call
		 * array_in, and it wouldn't be very efficient if we could.  Fill an
		 * FmgrInfo to use for the call.
		 */
		arrayinp = (FmgrInfo *) palloc(sizeof(FmgrInfo));
		fmgr_info(F_ARRAY_IN, arrayinp);
		funcctx->user_fctx = arrayinp;

		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	arrayinp = (FmgrInfo *) funcctx->user_fctx;

	if (funcctx->call_cntr < lengthof(sys_fk_relationships))
	{
		const SysFKRelationship *fkrel = &sys_fk_relationships[funcctx->call_cntr];
		Datum		values[6];
		bool		nulls[6];
		HeapTuple	tuple;

		memset(nulls, false, sizeof(nulls));

		values[0] = ObjectIdGetDatum(fkrel->fk_table);
		values[1] = FunctionCall3(arrayinp,
								  CStringGetDatum(fkrel->fk_columns),
								  ObjectIdGetDatum(TEXTOID),
								  Int32GetDatum(-1));
		values[2] = ObjectIdGetDatum(fkrel->pk_table);
		values[3] = FunctionCall3(arrayinp,
								  CStringGetDatum(fkrel->pk_columns),
								  ObjectIdGetDatum(TEXTOID),
								  Int32GetDatum(-1));
		values[4] = BoolGetDatum(fkrel->is_array);
		values[5] = BoolGetDatum(fkrel->is_opt);

		tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);

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

	PG_RETURN_INT32(relation_is_updatable(reloid, NIL, include_triggers, NULL));
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

	events = relation_is_updatable(reloid, NIL, include_triggers,
								   bms_make_singleton(col));

	/* We require both updatability and deletability of the relation */
#define REQ_EVENTS ((1 << CMD_UPDATE) | (1 << CMD_DELETE))

	PG_RETURN_BOOL((events & REQ_EVENTS) == REQ_EVENTS);
}


/*
 * Is character a valid identifier start?
 * Must match scan.l's {ident_start} character class.
 */
static bool
is_ident_start(unsigned char c)
{
	/* Underscores and ASCII letters are OK */
	if (c == '_')
		return true;
	if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
		return true;
	/* Any high-bit-set character is OK (might be part of a multibyte char) */
	if (IS_HIGHBIT_SET(c))
		return true;
	return false;
}

/*
 * Is character a valid identifier continuation?
 * Must match scan.l's {ident_cont} character class.
 */
static bool
is_ident_cont(unsigned char c)
{
	/* Can be digit or dollar sign ... */
	if ((c >= '0' && c <= '9') || c == '$')
		return true;
	/* ... or an identifier start character */
	return is_ident_start(c);
}

/*
 * parse_ident - parse a SQL qualified identifier into separate identifiers.
 * When strict mode is active (second parameter), then any chars after
 * the last identifier are disallowed.
 */
Datum
parse_ident(PG_FUNCTION_ARGS)
{
	text	   *qualname = PG_GETARG_TEXT_PP(0);
	bool		strict = PG_GETARG_BOOL(1);
	char	   *qualname_str = text_to_cstring(qualname);
	ArrayBuildState *astate = NULL;
	char	   *nextp;
	bool		after_dot = false;

	/*
	 * The code below scribbles on qualname_str in some cases, so we should
	 * reconvert qualname if we need to show the original string in error
	 * messages.
	 */
	nextp = qualname_str;

	/* skip leading whitespace */
	while (scanner_isspace(*nextp))
		nextp++;

	for (;;)
	{
		char	   *curname;
		bool		missing_ident = true;

		if (*nextp == '"')
		{
			char	   *endp;

			curname = nextp + 1;
			for (;;)
			{
				endp = strchr(nextp + 1, '"');
				if (endp == NULL)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("string is not a valid identifier: \"%s\"",
									text_to_cstring(qualname)),
							 errdetail("String has unclosed double quotes.")));
				if (endp[1] != '"')
					break;
				memmove(endp, endp + 1, strlen(endp));
				nextp = endp;
			}
			nextp = endp + 1;
			*endp = '\0';

			if (endp - curname == 0)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("string is not a valid identifier: \"%s\"",
								text_to_cstring(qualname)),
						 errdetail("Quoted identifier must not be empty.")));

			astate = accumArrayResult(astate, CStringGetTextDatum(curname),
									  false, TEXTOID, CurrentMemoryContext);
			missing_ident = false;
		}
		else if (is_ident_start((unsigned char) *nextp))
		{
			char	   *downname;
			int			len;
			text	   *part;

			curname = nextp++;
			while (is_ident_cont((unsigned char) *nextp))
				nextp++;

			len = nextp - curname;

			/*
			 * We don't implicitly truncate identifiers. This is useful for
			 * allowing the user to check for specific parts of the identifier
			 * being too long. It's easy enough for the user to get the
			 * truncated names by casting our output to name[].
			 */
			downname = downcase_identifier(curname, len, false, false);
			part = cstring_to_text_with_len(downname, len);
			astate = accumArrayResult(astate, PointerGetDatum(part), false,
									  TEXTOID, CurrentMemoryContext);
			missing_ident = false;
		}

		if (missing_ident)
		{
			/* Different error messages based on where we failed. */
			if (*nextp == '.')
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("string is not a valid identifier: \"%s\"",
								text_to_cstring(qualname)),
						 errdetail("No valid identifier before \".\".")));
			else if (after_dot)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("string is not a valid identifier: \"%s\"",
								text_to_cstring(qualname)),
						 errdetail("No valid identifier after \".\".")));
			else
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("string is not a valid identifier: \"%s\"",
								text_to_cstring(qualname))));
		}

		while (scanner_isspace(*nextp))
			nextp++;

		if (*nextp == '.')
		{
			after_dot = true;
			nextp++;
			while (scanner_isspace(*nextp))
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
						 errmsg("string is not a valid identifier: \"%s\"",
								text_to_cstring(qualname))));
			break;
		}
	}

	PG_RETURN_DATUM(makeArrayResult(astate, CurrentMemoryContext));
}

/*
 * pg_current_logfile
 *
 * Report current log file used by log collector by scanning current_logfiles.
 */
Datum
pg_current_logfile(PG_FUNCTION_ARGS)
{
	FILE	   *fd;
	char		lbuffer[MAXPGPATH];
	char	   *logfmt;

	/* The log format parameter is optional */
	if (PG_NARGS() == 0 || PG_ARGISNULL(0))
		logfmt = NULL;
	else
	{
		logfmt = text_to_cstring(PG_GETARG_TEXT_PP(0));

		if (strcmp(logfmt, "stderr") != 0 &&
			strcmp(logfmt, "csvlog") != 0 &&
			strcmp(logfmt, "jsonlog") != 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("log format \"%s\" is not supported", logfmt),
					 errhint("The supported log formats are \"stderr\", \"csvlog\", and \"jsonlog\".")));
	}

	fd = AllocateFile(LOG_METAINFO_DATAFILE, "r");
	if (fd == NULL)
	{
		if (errno != ENOENT)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read file \"%s\": %m",
							LOG_METAINFO_DATAFILE)));
		PG_RETURN_NULL();
	}

#ifdef WIN32
	/* syslogger.c writes CRLF line endings on Windows */
	_setmode(_fileno(fd), _O_TEXT);
#endif

	/*
	 * Read the file to gather current log filename(s) registered by the
	 * syslogger.
	 */
	while (fgets(lbuffer, sizeof(lbuffer), fd) != NULL)
	{
		char	   *log_format;
		char	   *log_filepath;
		char	   *nlpos;

		/* Extract log format and log file path from the line. */
		log_format = lbuffer;
		log_filepath = strchr(lbuffer, ' ');
		if (log_filepath == NULL)
		{
			/* Uh oh.  No space found, so file content is corrupted. */
			elog(ERROR,
				 "missing space character in \"%s\"", LOG_METAINFO_DATAFILE);
			break;
		}

		*log_filepath = '\0';
		log_filepath++;
		nlpos = strchr(log_filepath, '\n');
		if (nlpos == NULL)
		{
			/* Uh oh.  No newline found, so file content is corrupted. */
			elog(ERROR,
				 "missing newline character in \"%s\"", LOG_METAINFO_DATAFILE);
			break;
		}
		*nlpos = '\0';

		if (logfmt == NULL || strcmp(logfmt, log_format) == 0)
		{
			FreeFile(fd);
			PG_RETURN_TEXT_P(cstring_to_text(log_filepath));
		}
	}

	/* Close the current log filename file. */
	FreeFile(fd);

	PG_RETURN_NULL();
}

/*
 * Report current log file used by log collector (1 argument version)
 *
 * note: this wrapper is necessary to pass the sanity check in opr_sanity,
 * which checks that all built-in functions that share the implementing C
 * function take the same number of arguments
 */
Datum
pg_current_logfile_1arg(PG_FUNCTION_ARGS)
{
	return pg_current_logfile(fcinfo);
}

/*
 * SQL wrapper around RelationGetReplicaIndex().
 */
Datum
pg_get_replica_identity_index(PG_FUNCTION_ARGS)
{
	Oid			reloid = PG_GETARG_OID(0);
	Oid			idxoid;
	Relation	rel;

	rel = table_open(reloid, AccessShareLock);
	idxoid = RelationGetReplicaIndex(rel);
	table_close(rel, AccessShareLock);

	if (OidIsValid(idxoid))
		PG_RETURN_OID(idxoid);
	else
		PG_RETURN_NULL();
}
