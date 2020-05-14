/*-------------------------------------------------------------------------
 *
 * adminpack.c
 *
 *
 * Copyright (c) 2002-2020, PostgreSQL Global Development Group
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

#include "catalog/pg_authid.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "postmaster/syslogger.h"
#include "storage/fd.h"
#include "utils/acl.h"
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

PG_FUNCTION_INFO_V1(pg_file_write);
PG_FUNCTION_INFO_V1(pg_file_write_v1_1);
PG_FUNCTION_INFO_V1(pg_file_sync);
PG_FUNCTION_INFO_V1(pg_file_rename);
PG_FUNCTION_INFO_V1(pg_file_rename_v1_1);
PG_FUNCTION_INFO_V1(pg_file_unlink);
PG_FUNCTION_INFO_V1(pg_file_unlink_v1_1);
PG_FUNCTION_INFO_V1(pg_logdir_ls);
PG_FUNCTION_INFO_V1(pg_logdir_ls_v1_1);

static int64 pg_file_write_internal(text *file, text *data, bool replace);
static bool pg_file_rename_internal(text *file1, text *file2, text *file3);
static Datum pg_logdir_ls_internal(FunctionCallInfo fcinfo);


/*-----------------------
 * some helper functions
 */

/*
 * Convert a "text" filename argument to C string, and check it's allowable.
 *
 * Filename may be absolute or relative to the DataDir, but we only allow
 * absolute paths that match DataDir.
 */
static char *
convert_and_check_filename(text *arg)
{
	char	   *filename = text_to_cstring(arg);

	canonicalize_path(filename);	/* filename can change length here */

	/*
	 * Members of the 'pg_write_server_files' role are allowed to access any
	 * files on the server as the PG user, so no need to do any further checks
	 * here.
	 */
	if (is_member_of_role(GetUserId(), DEFAULT_ROLE_WRITE_SERVER_FILES))
		return filename;

	/* User isn't a member of the default role, so check if it's allowable */
	if (is_absolute_path(filename))
	{
		/* Disallow '/a/b/data/..' */
		if (path_contains_parent_reference(filename))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("reference to parent directory (\"..\") not allowed")));

		/* Allow absolute paths if within DataDir */
		if (!path_is_prefix_of_path(DataDir, filename))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("absolute path not allowed")));
	}
	else if (!path_is_relative_and_below_cwd(filename))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("path must be in or below the current directory")));

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
				 errmsg("only superuser may access generic file functions")));
}



/* ------------------------------------
 * pg_file_write - old version
 *
 * The superuser() check here must be kept as the library might be upgraded
 * without the extension being upgraded, meaning that in pre-1.1 installations
 * these functions could be called by any user.
 */
Datum
pg_file_write(PG_FUNCTION_ARGS)
{
	text	   *file = PG_GETARG_TEXT_PP(0);
	text	   *data = PG_GETARG_TEXT_PP(1);
	bool		replace = PG_GETARG_BOOL(2);
	int64		count = 0;

	requireSuperuser();

	count = pg_file_write_internal(file, data, replace);

	PG_RETURN_INT64(count);
}

/* ------------------------------------
 * pg_file_write_v1_1 - Version 1.1
 *
 * As of adminpack version 1.1, we no longer need to check if the user
 * is a superuser because we REVOKE EXECUTE on the function from PUBLIC.
 * Users can then grant access to it based on their policies.
 *
 * Otherwise identical to pg_file_write (above).
 */
Datum
pg_file_write_v1_1(PG_FUNCTION_ARGS)
{
	text	   *file = PG_GETARG_TEXT_PP(0);
	text	   *data = PG_GETARG_TEXT_PP(1);
	bool		replace = PG_GETARG_BOOL(2);
	int64		count = 0;

	count = pg_file_write_internal(file, data, replace);

	PG_RETURN_INT64(count);
}

/* ------------------------------------
 * pg_file_write_internal - Workhorse for pg_file_write functions.
 *
 * This handles the actual work for pg_file_write.
 */
static int64
pg_file_write_internal(text *file, text *data, bool replace)
{
	FILE	   *f;
	char	   *filename;
	int64		count = 0;

	filename = convert_and_check_filename(file);

	if (!replace)
	{
		struct stat fst;

		if (stat(filename, &fst) >= 0)
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_FILE),
					 errmsg("file \"%s\" exists", filename)));

		f = AllocateFile(filename, "wb");
	}
	else
		f = AllocateFile(filename, "ab");

	if (!f)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\" for writing: %m",
						filename)));

	count = fwrite(VARDATA_ANY(data), 1, VARSIZE_ANY_EXHDR(data), f);
	if (count != VARSIZE_ANY_EXHDR(data) || FreeFile(f))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write file \"%s\": %m", filename)));

	return (count);
}

/* ------------------------------------
 * pg_file_sync
 *
 * We REVOKE EXECUTE on the function from PUBLIC.
 * Users can then grant access to it based on their policies.
 */
Datum
pg_file_sync(PG_FUNCTION_ARGS)
{
	char	   *filename;
	struct stat fst;

	filename = convert_and_check_filename(PG_GETARG_TEXT_PP(0));

	if (stat(filename, &fst) < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not stat file \"%s\": %m", filename)));

	fsync_fname_ext(filename, S_ISDIR(fst.st_mode), false, ERROR);

	PG_RETURN_VOID();
}

/* ------------------------------------
 * pg_file_rename - old version
 *
 * The superuser() check here must be kept as the library might be upgraded
 * without the extension being upgraded, meaning that in pre-1.1 installations
 * these functions could be called by any user.
 */
Datum
pg_file_rename(PG_FUNCTION_ARGS)
{
	text	   *file1;
	text	   *file2;
	text	   *file3;
	bool		result;

	requireSuperuser();

	if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
		PG_RETURN_NULL();

	file1 = PG_GETARG_TEXT_PP(0);
	file2 = PG_GETARG_TEXT_PP(1);

	if (PG_ARGISNULL(2))
		file3 = NULL;
	else
		file3 = PG_GETARG_TEXT_PP(2);

	result = pg_file_rename_internal(file1, file2, file3);

	PG_RETURN_BOOL(result);
}

/* ------------------------------------
 * pg_file_rename_v1_1 - Version 1.1
 *
 * As of adminpack version 1.1, we no longer need to check if the user
 * is a superuser because we REVOKE EXECUTE on the function from PUBLIC.
 * Users can then grant access to it based on their policies.
 *
 * Otherwise identical to pg_file_write (above).
 */
Datum
pg_file_rename_v1_1(PG_FUNCTION_ARGS)
{
	text	   *file1;
	text	   *file2;
	text	   *file3;
	bool		result;

	if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
		PG_RETURN_NULL();

	file1 = PG_GETARG_TEXT_PP(0);
	file2 = PG_GETARG_TEXT_PP(1);

	if (PG_ARGISNULL(2))
		file3 = NULL;
	else
		file3 = PG_GETARG_TEXT_PP(2);

	result = pg_file_rename_internal(file1, file2, file3);

	PG_RETURN_BOOL(result);
}

/* ------------------------------------
 * pg_file_rename_internal - Workhorse for pg_file_rename functions.
 *
 * This handles the actual work for pg_file_rename.
 */
static bool
pg_file_rename_internal(text *file1, text *file2, text *file3)
{
	char	   *fn1,
			   *fn2,
			   *fn3;
	int			rc;

	fn1 = convert_and_check_filename(file1);
	fn2 = convert_and_check_filename(file2);

	if (file3 == NULL)
		fn3 = NULL;
	else
		fn3 = convert_and_check_filename(file3);

	if (access(fn1, W_OK) < 0)
	{
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("file \"%s\" is not accessible: %m", fn1)));

		return false;
	}

	if (fn3 && access(fn2, W_OK) < 0)
	{
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("file \"%s\" is not accessible: %m", fn2)));

		return false;
	}

	rc = access(fn3 ? fn3 : fn2, W_OK);
	if (rc >= 0 || errno != ENOENT)
	{
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_FILE),
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
						(errcode(ERRCODE_UNDEFINED_FILE),
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

	return true;
}


/* ------------------------------------
 * pg_file_unlink - old version
 *
 * The superuser() check here must be kept as the library might be upgraded
 * without the extension being upgraded, meaning that in pre-1.1 installations
 * these functions could be called by any user.
 */
Datum
pg_file_unlink(PG_FUNCTION_ARGS)
{
	char	   *filename;

	requireSuperuser();

	filename = convert_and_check_filename(PG_GETARG_TEXT_PP(0));

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


/* ------------------------------------
 * pg_file_unlink_v1_1 - Version 1.1
 *
 * As of adminpack version 1.1, we no longer need to check if the user
 * is a superuser because we REVOKE EXECUTE on the function from PUBLIC.
 * Users can then grant access to it based on their policies.
 *
 * Otherwise identical to pg_file_unlink (above).
 */
Datum
pg_file_unlink_v1_1(PG_FUNCTION_ARGS)
{
	char	   *filename;

	filename = convert_and_check_filename(PG_GETARG_TEXT_PP(0));

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

/* ------------------------------------
 * pg_logdir_ls - Old version
 *
 * The superuser() check here must be kept as the library might be upgraded
 * without the extension being upgraded, meaning that in pre-1.1 installations
 * these functions could be called by any user.
 */
Datum
pg_logdir_ls(PG_FUNCTION_ARGS)
{
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("only superuser can list the log directory")));

	return (pg_logdir_ls_internal(fcinfo));
}

/* ------------------------------------
 * pg_logdir_ls_v1_1 - Version 1.1
 *
 * As of adminpack version 1.1, we no longer need to check if the user
 * is a superuser because we REVOKE EXECUTE on the function from PUBLIC.
 * Users can then grant access to it based on their policies.
 *
 * Otherwise identical to pg_logdir_ls (above).
 */
Datum
pg_logdir_ls_v1_1(PG_FUNCTION_ARGS)
{
	return (pg_logdir_ls_internal(fcinfo));
}

static Datum
pg_logdir_ls_internal(FunctionCallInfo fcinfo)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	bool		randomAccess;
	TupleDesc	tupdesc;
	Tuplestorestate *tupstore;
	AttInMetadata *attinmeta;
	DIR		   *dirdesc;
	struct dirent *de;
	MemoryContext oldcontext;

	if (strcmp(Log_filename, "postgresql-%Y-%m-%d_%H%M%S.log") != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("the log_filename parameter must equal 'postgresql-%%Y-%%m-%%d_%%H%%M%%S.log'")));

	/* check to see if caller supports us returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("materialize mode required, but it is not allowed in this context")));

	/* The tupdesc and tuplestore must be created in ecxt_per_query_memory */
	oldcontext = MemoryContextSwitchTo(rsinfo->econtext->ecxt_per_query_memory);

	tupdesc = CreateTemplateTupleDesc(2);
	TupleDescInitEntry(tupdesc, (AttrNumber) 1, "starttime",
					   TIMESTAMPOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 2, "filename",
					   TEXTOID, -1, 0);

	randomAccess = (rsinfo->allowedModes & SFRM_Materialize_Random) != 0;
	tupstore = tuplestore_begin_heap(randomAccess, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	MemoryContextSwitchTo(oldcontext);

	attinmeta = TupleDescGetAttInMetadata(tupdesc);

	dirdesc = AllocateDir(Log_directory);
	while ((de = ReadDir(dirdesc, Log_directory)) != NULL)
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
		values[1] = psprintf("%s/%s", Log_directory, de->d_name);

		tuple = BuildTupleFromCStrings(attinmeta, values);

		tuplestore_puttuple(tupstore, tuple);
	}

	FreeDir(dirdesc);
	return (Datum) 0;
}
