/*
 * dbsize.c
 * object size functions
 *
 * Copyright (c) 2004, PostgreSQL Global Development Group
 *
 * Author: Andreas Pflug <pgadmin@pse-consulting.de>
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/contrib/dbsize/dbsize.c,v 1.13 2004/09/02 00:55:22 momjian Exp $
 *
 */


#include "postgres.h"

#include <sys/types.h>
#include <sys/stat.h>

#include "access/heapam.h"
#include "storage/fd.h"
#include "utils/syscache.h"
#include "utils/builtins.h"
#include "catalog/namespace.h"
#include "catalog/pg_tablespace.h"
#include "commands/dbcommands.h"
#include "miscadmin.h"


extern DLLIMPORT char *DataDir;

Datum pg_tablespace_size(PG_FUNCTION_ARGS);
Datum pg_database_size(PG_FUNCTION_ARGS);
Datum pg_relation_size(PG_FUNCTION_ARGS);
Datum pg_size_pretty(PG_FUNCTION_ARGS);

Datum database_size(PG_FUNCTION_ARGS);
Datum relation_size(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(pg_tablespace_size);
PG_FUNCTION_INFO_V1(pg_database_size);
PG_FUNCTION_INFO_V1(pg_relation_size);
PG_FUNCTION_INFO_V1(pg_size_pretty);

PG_FUNCTION_INFO_V1(database_size);
PG_FUNCTION_INFO_V1(relation_size);



static int64
db_dir_size(char *path)
{
    int64 dirsize=0;
    struct dirent *direntry;
	DIR         *dirdesc;
	char filename[MAXPGPATH];

	dirdesc=AllocateDir(path);

	if (!dirdesc)
	    return 0;

	while ((direntry = readdir(dirdesc)) != 0)
	{
	    struct stat fst;

	    if (!strcmp(direntry->d_name, ".") || !strcmp(direntry->d_name, ".."))
		    continue;

		snprintf(filename, MAXPGPATH, "%s/%s", path, direntry->d_name);

		if (stat(filename, &fst) < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not stat \"%s\": %m", filename)));
		dirsize += fst.st_size;
	}

	FreeDir(dirdesc);
	return dirsize;
}


static int64
calculate_database_size(Oid dbOid)
{
	int64 totalsize=0;
	DIR         *dirdesc;
    struct dirent *direntry;
	char pathname[MAXPGPATH];

	snprintf(pathname, MAXPGPATH, "%s/global/%u", DataDir, (unsigned)dbOid);
	totalsize += db_dir_size(pathname);
	snprintf(pathname, MAXPGPATH, "%s/base/%u", DataDir, (unsigned)dbOid);
	totalsize += db_dir_size(pathname);

	snprintf(pathname, MAXPGPATH, "%s/pg_tblspc", DataDir);
	dirdesc = AllocateDir(pathname);

	if (!dirdesc)
	    ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open tablespace directory: %m")));

	while ((direntry = readdir(dirdesc)) != 0)
	{
	    if (!strcmp(direntry->d_name, ".") || !strcmp(direntry->d_name, ".."))
		    continue;

		snprintf(pathname, MAXPGPATH, "%s/pg_tblspc/%s/%u", DataDir, direntry->d_name, (unsigned)dbOid);
		totalsize += db_dir_size(pathname);
	}

	FreeDir(dirdesc);

	if (!totalsize)
	    ereport(ERROR,
				(ERRCODE_UNDEFINED_DATABASE,
				 errmsg("Database OID %u unknown.", (unsigned)dbOid)));

	return totalsize;
}

/*
 * calculate total size of tablespace
 */
Datum
pg_tablespace_size(PG_FUNCTION_ARGS)
{
    Oid tblspcOid = PG_GETARG_OID(0);

	char tblspcPath[MAXPGPATH];
	char pathname[MAXPGPATH];
	int64		totalsize=0;
	DIR         *dirdesc;
    struct dirent *direntry;

	if (tblspcOid == DEFAULTTABLESPACE_OID)
	    snprintf(tblspcPath, MAXPGPATH, "%s/base", DataDir);
	else if (tblspcOid == GLOBALTABLESPACE_OID)
	    snprintf(tblspcPath, MAXPGPATH, "%s/global", DataDir);
	else
		snprintf(tblspcPath, MAXPGPATH, "%s/pg_tblspc/%u", DataDir, (unsigned)tblspcOid);

	dirdesc = AllocateDir(tblspcPath);

	if (!dirdesc)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("No such tablespace OID: %u: %m", (unsigned)tblspcOid)));

	while ((direntry = readdir(dirdesc)) != 0)
	{
	    struct stat fst;

	    if (!strcmp(direntry->d_name, ".") || !strcmp(direntry->d_name, ".."))
		    continue;

		snprintf(pathname, MAXPGPATH, "%s/%s", tblspcPath, direntry->d_name);
		if (stat(pathname, &fst) < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not stat \"%s\": %m", pathname)));
		totalsize += fst.st_size;

		if (fst.st_mode & S_IFDIR)
		    totalsize += db_dir_size(pathname);
	}

	FreeDir(dirdesc);

	PG_RETURN_INT64(totalsize);
}


/*
 * calculate size of databases in all tablespaces
 */
Datum
pg_database_size(PG_FUNCTION_ARGS)
{
    Oid dbOid = PG_GETARG_OID(0);

	PG_RETURN_INT64(calculate_database_size(dbOid));
}


Datum
database_size(PG_FUNCTION_ARGS)
{
	Name dbName = PG_GETARG_NAME(0);
	Oid dbOid = get_database_oid(NameStr(*dbName));

	if (!OidIsValid(dbOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_DATABASE),
			errmsg("database \"%s\" does not exist", NameStr(*dbName))));

	PG_RETURN_INT64(calculate_database_size(dbOid));
}

static int64
calculate_relation_size(Oid tblspcOid, Oid relnodeOid)
{
	int64		totalsize=0;
	unsigned int segcount=0;
	char dirpath[MAXPGPATH];
	char pathname[MAXPGPATH];

	if (tblspcOid == 0 || tblspcOid == DEFAULTTABLESPACE_OID)
	    snprintf(dirpath, MAXPGPATH, "%s/base/%u", DataDir, (unsigned)MyDatabaseId);
	else if (tblspcOid == GLOBALTABLESPACE_OID)
	    snprintf(dirpath, MAXPGPATH, "%s/global", DataDir);
	else
	    snprintf(dirpath, MAXPGPATH, "%s/pg_tblspc/%u/%u", DataDir, (unsigned)tblspcOid, (unsigned)MyDatabaseId);

	for (segcount = 0 ;; segcount++)
	{
		struct stat fst;

		if (segcount == 0)
		    snprintf(pathname, MAXPGPATH, "%s/%u", dirpath, (unsigned) relnodeOid);
		else
		    snprintf(pathname, MAXPGPATH, "%s/%u.%u", dirpath, (unsigned) relnodeOid, segcount);

		if (stat(pathname, &fst) < 0)
		{
			if (errno == ENOENT)
				break;
			else
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not stat \"%s\": %m", pathname)));
		}
		totalsize += fst.st_size;
	}

	return totalsize;
}

/*
 * calculate size of relation
 */
Datum
pg_relation_size(PG_FUNCTION_ARGS)
{
	Oid         relOid=PG_GETARG_OID(0);

	HeapTuple   tuple;
	Form_pg_class pg_class;
	Oid			relnodeOid;
	Oid         tblspcOid;
    char        relkind;

	tuple = SearchSysCache(RELOID, ObjectIdGetDatum(relOid), 0, 0, 0);
	if (!HeapTupleIsValid(tuple))
	    ereport(ERROR,
				(ERRCODE_UNDEFINED_TABLE,
				 errmsg("Relation OID %u does not exist", relOid)));

	pg_class = (Form_pg_class) GETSTRUCT(tuple);
	relnodeOid = pg_class->relfilenode;
	tblspcOid = pg_class->reltablespace;
	relkind = pg_class->relkind;

	ReleaseSysCache(tuple);

	switch(relkind)
	{
	    case RELKIND_INDEX:
	    case RELKIND_RELATION:
	    case RELKIND_TOASTVALUE:
		    break;
	    default:
		    ereport(ERROR,
					(ERRCODE_WRONG_OBJECT_TYPE,
					 errmsg("Relation kind %d not supported", relkind)));
	}

	PG_RETURN_INT64(calculate_relation_size(tblspcOid, relnodeOid));
}


Datum
relation_size(PG_FUNCTION_ARGS)
{
	text	   *relname = PG_GETARG_TEXT_P(0);

	RangeVar   *relrv;
	Relation	relation;
	Oid			relnodeOid;
	Oid         tblspcOid;

	relrv = makeRangeVarFromNameList(textToQualifiedNameList(relname,
													   "relation_size"));
	relation = heap_openrv(relrv, AccessShareLock);

	tblspcOid  = relation->rd_rel->reltablespace;
	relnodeOid = relation->rd_rel->relfilenode;

	heap_close(relation, AccessShareLock);

	PG_RETURN_INT64(calculate_relation_size(tblspcOid, relnodeOid));
}

/*
 * formatting with size units
 */
Datum
pg_size_pretty(PG_FUNCTION_ARGS)
{
    int64 size=PG_GETARG_INT64(0);
	char *result=palloc(50+VARHDRSZ);
	int64 limit = 10*1024;
	int64 mult=1;

	if (size < limit*mult)
	    snprintf(VARDATA(result), 50, INT64_FORMAT" bytes", size);
    else
	{
		mult *= 1024;
		if (size < limit*mult)
		     snprintf(VARDATA(result), 50, INT64_FORMAT " kB", (size+mult/2) / mult);
		else
		{
			mult *= 1024;
			if (size < limit*mult)
			    snprintf(VARDATA(result), 50, INT64_FORMAT " MB", (size+mult/2) / mult);
			else
			{
				mult *= 1024;
				if (size < limit*mult)
				    snprintf(VARDATA(result), 50, INT64_FORMAT " GB", (size+mult/2) / mult);
				else
				{
				    mult *= 1024;
				    snprintf(VARDATA(result), 50, INT64_FORMAT " TB", (size+mult/2) / mult);
				}
			}
		}
	}
	VARATT_SIZEP(result) = strlen(VARDATA(result)) + VARHDRSZ;

	PG_RETURN_TEXT_P(result);
}
