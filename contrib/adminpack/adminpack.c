/*-------------------------------------------------------------------------
 *
 * adminpack.c
 *
 *
 * Copyright (c) 2002-2013, PostgreSQL Global Development Group
 *
 * Author: Andreas Pflug <pgadmin@pse-consulting.de>
 *
 * IDENTIFICATION
 *	  contrib/adminpack/adminpack.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include "catalog/pg_type.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "postmaster/syslogger.h"
#include "storage/fd.h"
#include "utils/builtins.h"
#include "utils/datetime.h"


#ifdef WIN32

#ifdef rename
#undef rename
#endif

#ifdef unlink
#undef unlink
#endif
#endif

PG_MODULE_MAGIC;

Datum		pg_file_write(PG_FUNCTION_ARGS);
Datum		pg_file_rename(PG_FUNCTION_ARGS);
Datum		pg_file_unlink(PG_FUNCTION_ARGS);
Datum		pg_logdir_ls(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(pg_file_write);
PG_FUNCTION_INFO_V1(pg_file_rename);
PG_FUNCTION_INFO_V1(pg_file_unlink);
PG_FUNCTION_INFO_V1(pg_logdir_ls);

typedef struct
{
	char	   *location;
	DIR		   *dirdesc;
} directory_fctx;

/*-----------------------
 * some helper functions
 */

/*
 * Convert a "text" filename argument to C string, and check it's allowable.
 *
 * Filename may be absolute or relative to the DataDir, but we only allow
 * absolute paths that match DataDir or Log_directory.
 */
static char *
convert_and_check_filename(text *arg, bool logAllowed)
{
	char	   *filename = text_to_cstring(arg);

	canonicalize_path(filename);	/* filename can change length here */

	if (is_absolute_path(filename))
	{
		/* Disallow '/a/b/data/..' */
		if (path_contains_parent_reference(filename))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
			(errmsg("reference to parent directory (\"..\") not allowed"))));

		/*
		 * Allow absolute paths if within DataDir or Log_directory, even
		 * though Log_directory might be outside DataDir.
		 */
		if (!path_is_prefix_of_path(DataDir, filename) &&
			(!logAllowed || !is_absolute_path(Log_directory) ||
			 !path_is_prefix_of_path(Log_directory, filename)))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 (errmsg("absolute path not allowed"))));
	}
	else if (!path_is_relative_and_below_cwd(filename))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 (errmsg("path must be in or below the current directory"))));

	return filename;
}


/*
 * check for superuser, bark if not.
 */
static void
requireSuperuser(void)
{
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
			  (errmsg("only superuser may access generic file functions"))));
}



/* ------------------------------------
 * generic file handling functions
 */

Datum
pg_file_write(PG_FUNCTION_ARGS)
{
	FILE	   *f;
	char	   *filename;
	text	   *data;
	int64		count = 0;

	requireSuperuser();

	filename = convert_and_check_filename(PG_GETARG_TEXT_P(0), false);
	data = PG_GETARG_TEXT_P(1);

	if (!PG_GETARG_BOOL(2))
	{
		struct stat fst;

		if (stat(filename, &fst) >= 0)
			ereport(ERROR,
					(ERRCODE_DUPLICATE_FILE,
					 errmsg("file \"%s\" exists", filename)));

		f = fopen(filename, "wb");
	}
	else
		f = fopen(filename, "ab");

	if (!f)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\" for writing: %m",
						filename)));

	if (VARSIZE(data) != 0)
	{
		count = fwrite(VARDATA(data), 1, VARSIZE(data) - VARHDRSZ, f);

		if (count != VARSIZE(data) - VARHDRSZ)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not write file \"%s\": %m", filename)));
	}
	fclose(f);

	PG_RETURN_INT64(count);
}


Datum
pg_file_rename(PG_FUNCTION_ARGS)
{
	char	   *fn1,
			   *fn2,
			   *fn3;
	int			rc;

	requireSuperuser();

	if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
		PG_RETURN_NULL();

	fn1 = convert_and_check_filename(PG_GETARG_TEXT_P(0), false);
	fn2 = convert_and_check_filename(PG_GETARG_TEXT_P(1), false);
	if (PG_ARGISNULL(2))
		fn3 = 0;
	else
		fn3 = convert_and_check_filename(PG_GETARG_TEXT_P(2), false);

	if (access(fn1, W_OK) < 0)
	{
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("file \"%s\" is not accessible: %m", fn1)));

		PG_RETURN_BOOL(false);
	}

	if (fn3 && access(fn2, W_OK) < 0)
	{
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("file \"%s\" is not accessible: %m", fn2)));

		PG_RETURN_BOOL(false);
	}

	rc = access(fn3 ? fn3 : fn2, 2);
	if (rc >= 0 || errno != ENOENT)
	{
		ereport(ERROR,
				(ERRCODE_DUPLICATE_FILE,
				 errmsg("cannot rename to target file \"%s\"",
						fn3 ? fn3 : fn2)));
	}

	if (fn3)
	{
		if (rename(fn2, fn3) != 0)
		{
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not rename \"%s\" to \"%s\": %m",
							fn2, fn3)));
		}
		if (rename(fn1, fn2) != 0)
		{
			ereport(WARNING,
					(errcode_for_file_access(),
					 errmsg("could not rename \"%s\" to \"%s\": %m",
							fn1, fn2)));

			if (rename(fn3, fn2) != 0)
			{
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not rename \"%s\" back to \"%s\": %m",
								fn3, fn2)));
			}
			else
			{
				ereport(ERROR,
						(ERRCODE_UNDEFINED_FILE,
						 errmsg("renaming \"%s\" to \"%s\" was reverted",
								fn2, fn3)));
			}
		}
	}
	else if (rename(fn1, fn2) != 0)
	{
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not rename \"%s\" to \"%s\": %m", fn1, fn2)));
	}

	PG_RETURN_BOOL(true);
}


Datum
pg_file_unlink(PG_FUNCTION_ARGS)
{
	char	   *filename;

	requireSuperuser();

	filename = convert_and_check_filename(PG_GETARG_TEXT_P(0), false);

	if (access(filename, W_OK) < 0)
	{
		if (errno == ENOENT)
			PG_RETURN_BOOL(false);
		else
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("file \"%s\" is not accessible: %m", filename)));
	}

	if (unlink(filename) < 0)
	{
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("could not unlink file \"%s\": %m", filename)));

		PG_RETURN_BOOL(false);
	}
	PG_RETURN_BOOL(true);
}


Datum
pg_logdir_ls(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	struct dirent *de;
	directory_fctx *fctx;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 (errmsg("only superuser can list the log directory"))));

	if (strcmp(Log_filename, "postgresql-%Y-%m-%d_%H%M%S.log") != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 (errmsg("the log_filename parameter must equal 'postgresql-%%Y-%%m-%%d_%%H%%M%%S.log'"))));

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext;
		TupleDesc	tupdesc;

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		fctx = palloc(sizeof(directory_fctx));

		tupdesc = CreateTemplateTupleDesc(2, false);
		TupleDescInitEntry(tupdesc, (AttrNumber) 1, "starttime",
						   TIMESTAMPOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 2, "filename",
						   TEXTOID, -1, 0);

		funcctx->attinmeta = TupleDescGetAttInMetadata(tupdesc);

		fctx->location = pstrdup(Log_directory);
		fctx->dirdesc = AllocateDir(fctx->location);

		if (!fctx->dirdesc)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read directory \"%s\": %m",
							fctx->location)));

		funcctx->user_fctx = fctx;
		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	fctx = (directory_fctx *) funcctx->user_fctx;

	while ((de = ReadDir(fctx->dirdesc, fctx->location)) != NULL)
	{
		char	   *values[2];
		HeapTuple	tuple;
		char		timestampbuf[32];
		char	   *field[MAXDATEFIELDS];
		char		lowstr[MAXDATELEN + 1];
		int			dtype;
		int			nf,
					ftype[MAXDATEFIELDS];
		fsec_t		fsec;
		int			tz = 0;
		struct pg_tm date;

		/*
		 * Default format: postgresql-YYYY-MM-DD_HHMMSS.log
		 */
		if (strlen(de->d_name) != 32
			|| strncmp(de->d_name, "postgresql-", 11) != 0
			|| de->d_name[21] != '_'
			|| strcmp(de->d_name + 28, ".log") != 0)
			continue;

		/* extract timestamp portion of filename */
		strcpy(timestampbuf, de->d_name + 11);
		timestampbuf[17] = '\0';

		/* parse and decode expected timestamp to verify it's OK format */
		if (ParseDateTime(timestampbuf, lowstr, MAXDATELEN, field, ftype, MAXDATEFIELDS, &nf))
			continue;

		if (DecodeDateTime(field, ftype, nf, &dtype, &date, &fsec, &tz))
			continue;

		/* Seems the timestamp is OK; prepare and return tuple */

		values[0] = timestampbuf;
		values[1] = palloc(strlen(fctx->location) + strlen(de->d_name) + 2);
		sprintf(values[1], "%s/%s", fctx->location, de->d_name);

		tuple = BuildTupleFromCStrings(funcctx->attinmeta, values);

		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
	}

	FreeDir(fctx->dirdesc);
	SRF_RETURN_DONE(funcctx);
}
