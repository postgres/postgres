/*-------------------------------------------------------------------------
 *
 * genfile.c
 *
 *
 * Copyright (c) 2004, PostgreSQL Global Development Group
 * 
 * Author: Andreas Pflug <pgadmin@pse-consulting.de>
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/adt/genfile.c,v 1.1 2005/08/12 03:24:08 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>

#include "utils/builtins.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "catalog/pg_type.h"
#include "funcapi.h"

extern  char *Log_directory;

typedef struct 
{
	char	*location;
	DIR		*dirdesc;
} directory_fctx;

/*
 * Return an absolute path. Argument may be absolute or 
 * relative to the DataDir.
 */
static char *check_and_make_absolute(text *arg)
{
	int datadir_len = strlen(DataDir);
	int filename_len = VARSIZE(arg) - VARHDRSZ;
	char *filename = palloc(filename_len + 1);
	
	memcpy(filename, VARDATA(arg), filename_len);
	filename[filename_len] = '\0';

	canonicalize_path(filename);
	filename_len = strlen(filename);	/* recompute */

	/*
	 *	Prevent reference to the parent directory.
	 *	"..a.." is a valid file name though.
	 */
	if (strcmp(filename, "..") == 0 ||							/* beginning */
		strncmp(filename, "../", 3) == 0 ||						/* beginning */
		strcmp(filename, "/..") == 0 ||							/* beginning */
		strncmp(filename, "../", 3) == 0 ||						/* beginning */
		strstr(filename, "/../") != NULL ||						/* middle */
		strncmp(filename + filename_len - 3, "/..", 3) == 0)	/* end */
			ereport(ERROR,
				  (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				   (errmsg("Reference to a parent directory (\"..\") not allowed"))));

	if (is_absolute_path(filename))
	{
		/* The log directory might be outside our datadir, but allow it */
	    if (is_absolute_path(Log_directory) &&
			strncmp(filename, Log_directory, strlen(Log_directory)) == 0 &&
			(filename[strlen(Log_directory)] == '/' ||
			 filename[strlen(Log_directory)] == '\0'))
			return filename;

	    ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 (errmsg("Absolute paths not allowed"))));
		return NULL;
	}
	else
	{
	    char *absname = palloc(datadir_len + filename_len + 2);
		sprintf(absname, "%s/%s", DataDir, filename);
		pfree(filename);
		return absname;
	}
}


Datum pg_read_file(PG_FUNCTION_ARGS)
{
	int64		bytes_to_read = PG_GETARG_INT64(2);
	int64		seek_offset = PG_GETARG_INT64(1);
	char 		*buf = 0;
	size_t		nbytes;
	FILE		*file;
	char		*filename;

	if (!superuser())
	    ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 (errmsg("must be superuser to read files"))));

	filename = check_and_make_absolute(PG_GETARG_TEXT_P(0));

	if ((file = AllocateFile(filename, PG_BINARY_R)) == NULL)
	{
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file %s for reading: %m", filename)));
		PG_RETURN_NULL();
	}

	if (fseeko(file, (off_t)seek_offset,
		(seek_offset >= 0) ? SEEK_SET : SEEK_END) != 0)
	{
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not seek in file %s: %m", filename)));
		PG_RETURN_NULL();
	}

	if (bytes_to_read < 0)
	{
		ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("length cannot be negative")));
	}
	
	buf = palloc(bytes_to_read + VARHDRSZ);

	nbytes = fread(VARDATA(buf), 1, bytes_to_read, file);

	if (nbytes < 0)
	{
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read file %s: %m", filename)));
		PG_RETURN_NULL();
	}
	VARATT_SIZEP(buf) = nbytes + VARHDRSZ;

	pfree(filename);
	FreeFile(file);
	PG_RETURN_TEXT_P(buf);
}


Datum pg_stat_file(PG_FUNCTION_ARGS)
{
	AttInMetadata *attinmeta;
	char		*filename = check_and_make_absolute(PG_GETARG_TEXT_P(0));
	struct stat fst;
	char		lenbuf[30], cbuf[30], abuf[30], mbuf[30], dirbuf[2];
	char		*values[5] = {lenbuf, cbuf, abuf, mbuf, dirbuf};
	pg_time_t	timestamp;
	HeapTuple	tuple;
	TupleDesc	tupdesc = CreateTemplateTupleDesc(5, false);

	if (!superuser())
	    ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 (errmsg("must be superuser to get file information"))));

	TupleDescInitEntry(tupdesc, (AttrNumber) 1, "length", INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 2, "atime", TIMESTAMPOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 3, "mtime", TIMESTAMPOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 4, "ctime", TIMESTAMPOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 5, "isdir", BOOLOID, -1, 0);
	attinmeta = TupleDescGetAttInMetadata(tupdesc);

	if (stat(filename, &fst) < 0)
	{
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not stat file %s: %m", filename)));
		PG_RETURN_NULL();
	}
	else
	{
		snprintf(lenbuf, 30, INT64_FORMAT, (int64)fst.st_size);

		timestamp = fst.st_atime;
		pg_strftime(abuf, 30, "%F %T", pg_localtime(&timestamp, global_timezone));

		timestamp = fst.st_mtime;
		pg_strftime(mbuf, 30, "%F %T", pg_localtime(&timestamp, global_timezone));

		timestamp = fst.st_ctime;
		pg_strftime(cbuf, 30, "%F %T", pg_localtime(&timestamp, global_timezone));

		if (fst.st_mode & S_IFDIR)
			strcpy(dirbuf, "t");
		else
			strcpy(dirbuf, "f");

		tuple = BuildTupleFromCStrings(attinmeta, values);
		pfree(filename);
		PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
	}
}


Datum pg_ls_dir(PG_FUNCTION_ARGS)
{
	FuncCallContext	*funcctx;
	struct dirent	*de;
	directory_fctx	*fctx;

	if (!superuser())
	    ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 (errmsg("must be superuser to get directory listings"))));

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext;

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		fctx = palloc(sizeof(directory_fctx));
		fctx->location = check_and_make_absolute(PG_GETARG_TEXT_P(0));

		fctx->dirdesc = AllocateDir(fctx->location);

		if (!fctx->dirdesc)
		    ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("%s is not browsable: %m", fctx->location)));

		funcctx->user_fctx = fctx;
		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	fctx = (directory_fctx*) funcctx->user_fctx;

	if (!fctx->dirdesc)  /* not a readable directory  */
		SRF_RETURN_DONE(funcctx);

	while ((de = ReadDir(fctx->dirdesc, fctx->location)) != NULL)
	{
		int			len = strlen(de->d_name);
		text		*result = palloc(len + VARHDRSZ);

		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
		    continue;

		VARATT_SIZEP(result) = len + VARHDRSZ;
		memcpy(VARDATA(result), de->d_name, len);

		SRF_RETURN_NEXT(funcctx, PointerGetDatum(result));
	}

	FreeDir(fctx->dirdesc);
	SRF_RETURN_DONE(funcctx);
}
