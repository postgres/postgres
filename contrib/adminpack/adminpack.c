/*-------------------------------------------------------------------------
 *
 * admin81.c
 *
 *
 * Copyright (c) 2002 - 2006, PostgreSQL Global Development Group
 * 
 * Author: Andreas Pflug <pgadmin@pse-consulting.de>
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/contrib/adminpack/adminpack.c,v 1.1 2006/05/30 12:07:31 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>

#include "miscadmin.h"
#include "storage/fd.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "utils/datetime.h"


#ifdef WIN32

#ifdef rename
#undef rename
#endif

#ifdef unlink
#undef unlink
#endif

#endif

extern DLLIMPORT char *DataDir;
extern DLLIMPORT char *Log_directory;
extern DLLIMPORT char *Log_filename;

Datum pg_file_write(PG_FUNCTION_ARGS);
Datum pg_file_rename(PG_FUNCTION_ARGS);
Datum pg_file_unlink(PG_FUNCTION_ARGS);
Datum pg_logdir_ls(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(pg_file_write);
PG_FUNCTION_INFO_V1(pg_file_rename);
PG_FUNCTION_INFO_V1(pg_file_unlink);
PG_FUNCTION_INFO_V1(pg_logdir_ls);

typedef struct 
{
	char *location;
	DIR *dirdesc;
} directory_fctx;

/*-----------------------
 * some helper functions
 */

/*
 * Return an absolute path. Argument may be absolute or 
 * relative to the DataDir.
 */
static char *absClusterPath(text *arg, bool logAllowed)
{
	char *filename;
	int len=VARSIZE(arg) - VARHDRSZ;
	int dlen = strlen(DataDir);

	filename = palloc(len+1);
	memcpy(filename, VARDATA(arg), len);
	filename[len] = 0;

	if (strstr(filename, "..") != NULL)
	  ereport(ERROR,
			  (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
			   (errmsg("No .. allowed in filenames"))));
	
	if (is_absolute_path(filename))
	{
	    if (logAllowed && !strncmp(filename, Log_directory, strlen(Log_directory)))
		    return filename;
		if (strncmp(filename, DataDir, dlen))
		    ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 (errmsg("Absolute path not allowed"))));

		return filename;
	}
	else
	{
	    char *absname = palloc(dlen+len+2);
		sprintf(absname, "%s/%s", DataDir, filename);
		pfree(filename);
		return absname;
	}
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

Datum pg_file_write(PG_FUNCTION_ARGS)
{
	FILE *f;
	char *filename;
	text *data;
	int64 count = 0;

	requireSuperuser();

	filename = absClusterPath(PG_GETARG_TEXT_P(0), false);
	data = PG_GETARG_TEXT_P(1);

	if (PG_ARGISNULL(2) || !PG_GETARG_BOOL(2))
	{
	    struct stat fst;
		if (stat(filename, &fst) >= 0)
		    ereport(ERROR,
					(ERRCODE_DUPLICATE_FILE,
					 errmsg("file %s exists", filename)));

	    f = fopen(filename, "wb");
	}
	else
	    f = fopen(filename, "ab");

	if (!f)
	{
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could open file %s for writing: %m", filename)));
	}

	if (VARSIZE(data) != 0)
	{
		count = fwrite(VARDATA(data), 1, VARSIZE(data) - VARHDRSZ, f);

		if (count != VARSIZE(data) - VARHDRSZ)
		    ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("error writing file %s: %m", filename)));
	}
	fclose(f);

	PG_RETURN_INT64(count);
}


Datum pg_file_rename(PG_FUNCTION_ARGS)
{
    char *fn1, *fn2, *fn3;
	int rc;

	requireSuperuser();

	if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
		PG_RETURN_NULL();

	fn1=absClusterPath(PG_GETARG_TEXT_P(0), false);
	fn2=absClusterPath(PG_GETARG_TEXT_P(1), false);
	if (PG_ARGISNULL(2))
	    fn3=0;
	else
	    fn3=absClusterPath(PG_GETARG_TEXT_P(2), false);

	if (access(fn1, W_OK) < 0)
	{
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("file %s not accessible: %m", fn1)));

	    PG_RETURN_BOOL(false);
	}

	if (fn3 && access(fn2, W_OK) < 0)
	{
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("file %s not accessible: %m", fn2)));

	    PG_RETURN_BOOL(false);
	}


	rc = access(fn3 ? fn3 : fn2, 2);
	if (rc >= 0 || errno != ENOENT)
	{
		ereport(ERROR,
				(ERRCODE_DUPLICATE_FILE,
				 errmsg("cannot rename to target file %s", fn3 ? fn3 : fn2)));
	}
	
	if (fn3)
	{
	    if (rename(fn2, fn3) != 0)
		{
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not rename %s to %s: %m", fn2, fn3)));
		}
		if (rename(fn1, fn2) != 0)
		{
			ereport(WARNING,
					(errcode_for_file_access(),
					 errmsg("could not rename %s to %s: %m", fn1, fn2)));

			if (rename(fn3, fn2) != 0)
			{
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not rename %s back to %s: %m", fn3, fn2)));
			}
			else
			{
				ereport(ERROR,
						(ERRCODE_UNDEFINED_FILE,
						 errmsg("renaming %s to %s was reverted", fn2, fn3)));

			}
		}
	}
	else if (rename(fn1, fn2) != 0)
	{
			ereport(WARNING,
					(errcode_for_file_access(),
					 errmsg("renaming %s to %s %m", fn1, fn2)));
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not rename %s to %s: %m", fn1, fn2)));
	}

	PG_RETURN_BOOL(true);
}


Datum pg_file_unlink(PG_FUNCTION_ARGS)
{
    char *filename;

	requireSuperuser();

    filename = absClusterPath(PG_GETARG_TEXT_P(0), false);

	if (access(filename, W_OK) < 0)
	{
	    if (errno == ENOENT)
		    PG_RETURN_BOOL(false);
		else
		    ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("file %s not accessible: %m", filename)));

	}

	if (unlink(filename) < 0)
	{
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("could not unlink file %s: %m", filename)));

		PG_RETURN_BOOL(false);
	}
	PG_RETURN_BOOL(true);
}


Datum pg_logdir_ls(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	struct dirent *de;
	directory_fctx *fctx;

	if (!superuser()) 
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 (errmsg("only superuser can list the log directory"))));
	
	if (memcmp(Log_filename, "postgresql-%Y-%m-%d_%H%M%S.log", 30) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 (errmsg("the log_filename parameter must equal 'postgresql-%%Y-%%m-%%d_%%H%%M%%S.log'"))));

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext;
		TupleDesc tupdesc;

		funcctx=SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		fctx = palloc(sizeof(directory_fctx));
		if (is_absolute_path(Log_directory))
		    fctx->location = Log_directory;
		else
		{
			fctx->location = palloc(strlen(DataDir) + strlen(Log_directory) +2);
			sprintf(fctx->location, "%s/%s", DataDir, Log_directory);
		}
		tupdesc = CreateTemplateTupleDesc(2, false);
		TupleDescInitEntry(tupdesc, (AttrNumber) 1, "starttime",
						   TIMESTAMPOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 2, "filename",
						   TEXTOID, -1, 0);

		funcctx->attinmeta = TupleDescGetAttInMetadata(tupdesc);
		
		fctx->dirdesc = AllocateDir(fctx->location);

		if (!fctx->dirdesc)
		    ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("%s is not browsable: %m", fctx->location)));

		funcctx->user_fctx = fctx;
		MemoryContextSwitchTo(oldcontext);
	}

	funcctx=SRF_PERCALL_SETUP();
	fctx = (directory_fctx*) funcctx->user_fctx;

	if (!fctx->dirdesc)  /* not a readable directory  */
		SRF_RETURN_DONE(funcctx);

	while ((de = readdir(fctx->dirdesc)) != NULL)
	{
		char *values[2];
		HeapTuple tuple;
            
		char	   	*field[MAXDATEFIELDS];
		char		lowstr[MAXDATELEN + 1];
		int		dtype;
		int		nf, ftype[MAXDATEFIELDS];
		fsec_t		fsec;
		int		tz = 0;
		struct 		pg_tm date;

		/*
		 * Default format:
		 *        postgresql-YYYY-MM-DD_HHMMSS.log
		 */
		if (strlen(de->d_name) != 32
		    || memcmp(de->d_name, "postgresql-", 11)
			|| de->d_name[21] != '_'
			|| strcmp(de->d_name + 28, ".log"))
		      continue;

		values[1] = palloc(strlen(fctx->location) + strlen(de->d_name) + 2);
		sprintf(values[1], "%s/%s", fctx->location, de->d_name);

		values[0] = de->d_name + 11;       /* timestamp */
		values[0][17] = 0;

                    /* parse and decode expected timestamp */
		if (ParseDateTime(values[0], lowstr, MAXDATELEN, field, ftype, MAXDATEFIELDS, &nf))
		    continue;

		if (DecodeDateTime(field, ftype, nf, &dtype, &date, &fsec, &tz))
		    continue;

		/* Seems the format fits the expected format; feed it into the tuple */

		tuple = BuildTupleFromCStrings(funcctx->attinmeta, values);

		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
	}

	FreeDir(fctx->dirdesc);
	SRF_RETURN_DONE(funcctx);
}
