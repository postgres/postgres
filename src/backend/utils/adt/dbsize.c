/*
 * dbsize.c
 *		object size functions
 *
 * Copyright (c) 2002-2005, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/adt/dbsize.c,v 1.4 2005/09/16 05:35:40 neilc Exp $
 *
 */

#include "postgres.h"

#include <sys/types.h>
#include <sys/stat.h>

#include "access/heapam.h"
#include "catalog/namespace.h"
#include "catalog/pg_tablespace.h"
#include "commands/dbcommands.h"
#include "commands/tablespace.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "utils/builtins.h"
#include "utils/syscache.h"
#include "utils/relcache.h"


/* Return physical size of directory contents, or 0 if dir doesn't exist */
static int64
db_dir_size(const char *path)
{
	int64		dirsize = 0;
    struct dirent *direntry;
	DIR         *dirdesc;
	char filename[MAXPGPATH];

	dirdesc = AllocateDir(path);

	if (!dirdesc)
	    return 0;

	while ((direntry = ReadDir(dirdesc, path)) != NULL)
	{
	    struct stat fst;

	    if (strcmp(direntry->d_name, ".") == 0 ||
			strcmp(direntry->d_name, "..") == 0)
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

/*
 * calculate size of database in all tablespaces
 */
static int64
calculate_database_size(Oid dbOid)
{
	int64		totalsize;
	DIR         *dirdesc;
    struct dirent *direntry;
	char dirpath[MAXPGPATH];
	char pathname[MAXPGPATH];

	/* Shared storage in pg_global is not counted */

	/* Include pg_default storage */
	snprintf(pathname, MAXPGPATH, "%s/base/%u", DataDir, dbOid);
	totalsize = db_dir_size(pathname);

	/* Scan the non-default tablespaces */
	snprintf(dirpath, MAXPGPATH, "%s/pg_tblspc", DataDir);
	dirdesc = AllocateDir(dirpath);
	if (!dirdesc)
	    ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open tablespace directory \"%s\": %m",
						dirpath)));

	while ((direntry = ReadDir(dirdesc, dirpath)) != NULL)
	{
	    if (strcmp(direntry->d_name, ".") == 0 ||
			strcmp(direntry->d_name, "..") == 0)
		    continue;

		snprintf(pathname, MAXPGPATH, "%s/pg_tblspc/%s/%u",
				 DataDir, direntry->d_name, dbOid);
		totalsize += db_dir_size(pathname);
	}

	FreeDir(dirdesc);

	/* Complain if we found no trace of the DB at all */
	if (!totalsize)
	    ereport(ERROR,
				(ERRCODE_UNDEFINED_DATABASE,
				 errmsg("database with OID %u does not exist", dbOid)));

	return totalsize;
}

Datum
pg_database_size_oid(PG_FUNCTION_ARGS)
{
    Oid dbOid = PG_GETARG_OID(0);

	PG_RETURN_INT64(calculate_database_size(dbOid));
}

Datum
pg_database_size_name(PG_FUNCTION_ARGS)
{
	Name dbName = PG_GETARG_NAME(0);
	Oid dbOid = get_database_oid(NameStr(*dbName));

	if (!OidIsValid(dbOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_DATABASE),
				 errmsg("database \"%s\" does not exist",
						NameStr(*dbName))));

	PG_RETURN_INT64(calculate_database_size(dbOid));
}


/*
 * calculate total size of tablespace
 */
static int64
calculate_tablespace_size(Oid tblspcOid)
{
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
		snprintf(tblspcPath, MAXPGPATH, "%s/pg_tblspc/%u", DataDir, tblspcOid);

	dirdesc = AllocateDir(tblspcPath);

	if (!dirdesc)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open tablespace directory \"%s\": %m",
						tblspcPath)));

	while ((direntry = ReadDir(dirdesc, tblspcPath)) != NULL)
	{
	    struct stat fst;

	    if (strcmp(direntry->d_name, ".") == 0 ||
			strcmp(direntry->d_name, "..") == 0)
		    continue;

		snprintf(pathname, MAXPGPATH, "%s/%s", tblspcPath, direntry->d_name);

		if (stat(pathname, &fst) < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not stat \"%s\": %m", pathname)));

		if (fst.st_mode & S_IFDIR)
		    totalsize += db_dir_size(pathname);
        
        totalsize += fst.st_size;
	}

	FreeDir(dirdesc);
    
	return totalsize;
}

Datum
pg_tablespace_size_oid(PG_FUNCTION_ARGS)
{
    Oid tblspcOid = PG_GETARG_OID(0);
    
	PG_RETURN_INT64(calculate_tablespace_size(tblspcOid));
}

Datum
pg_tablespace_size_name(PG_FUNCTION_ARGS)
{
	Name tblspcName = PG_GETARG_NAME(0);
	Oid tblspcOid = get_tablespace_oid(NameStr(*tblspcName));

	if (!OidIsValid(tblspcOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("tablespace \"%s\" does not exist",
						NameStr(*tblspcName))));

	PG_RETURN_INT64(calculate_tablespace_size(tblspcOid));
}


/*
 * calculate size of a relation
 */
static int64
calculate_relation_size(Oid tblspcOid, Oid relnodeOid)
{
	int64		totalsize=0;
	unsigned int segcount=0;
	char dirpath[MAXPGPATH];
	char pathname[MAXPGPATH];

	if (!tblspcOid)
		tblspcOid = MyDatabaseTableSpace;

	if (tblspcOid == DEFAULTTABLESPACE_OID)
	    snprintf(dirpath, MAXPGPATH, "%s/base/%u", DataDir, MyDatabaseId);
	else if (tblspcOid == GLOBALTABLESPACE_OID)
	    snprintf(dirpath, MAXPGPATH, "%s/global", DataDir);
	else
	    snprintf(dirpath, MAXPGPATH, "%s/pg_tblspc/%u/%u",
				 DataDir, tblspcOid, MyDatabaseId);

	for (segcount = 0 ;; segcount++)
	{
		struct stat fst;

		if (segcount == 0)
		    snprintf(pathname, MAXPGPATH, "%s/%u",
					 dirpath, relnodeOid);
		else
		    snprintf(pathname, MAXPGPATH, "%s/%u.%u",
					 dirpath, relnodeOid, segcount);

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

Datum
pg_relation_size_oid(PG_FUNCTION_ARGS)
{
	Oid         relOid=PG_GETARG_OID(0);
	HeapTuple   tuple;
	Form_pg_class pg_class;
	Oid			relnodeOid;
	Oid         tblspcOid;

	tuple = SearchSysCache(RELOID,
						   ObjectIdGetDatum(relOid),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u", relOid);

	pg_class = (Form_pg_class) GETSTRUCT(tuple);
	relnodeOid = pg_class->relfilenode;
	tblspcOid = pg_class->reltablespace;

	ReleaseSysCache(tuple);

	PG_RETURN_INT64(calculate_relation_size(tblspcOid, relnodeOid));
}

Datum
pg_relation_size_name(PG_FUNCTION_ARGS)
{
	text	   *relname = PG_GETARG_TEXT_P(0);
	RangeVar   *relrv;
	Relation	relation;
	Oid			relnodeOid;
	Oid         tblspcOid;
    
	relrv = makeRangeVarFromNameList(textToQualifiedNameList(relname));    
	relation = relation_openrv(relrv, AccessShareLock);
    
	tblspcOid  = relation->rd_rel->reltablespace;             
	relnodeOid = relation->rd_rel->relfilenode;
             
	relation_close(relation, AccessShareLock);

	PG_RETURN_INT64(calculate_relation_size(tblspcOid, relnodeOid));
}


/*
 *  Compute the on-disk size of files for 'relation' according to the
 *  stat function, optionally including heap data, index data, and/or
 *  toast data.
 */
static int64
calculate_total_relation_size(Oid tblspcOid, Oid relnodeOid)
{
    Relation        heapRelation;
	Relation        idxRelation;
	Relation        toastRelation;
    Oid             idxOid;
    Oid             idxTblspcOid;
	Oid             toastOid;
	Oid             toastTblspcOid;
	bool            hasIndices;
	int64           size;
	List            *indexoidlist;
	ListCell        *idx;

    heapRelation = relation_open(relnodeOid, AccessShareLock);
	toastOid = heapRelation->rd_rel->reltoastrelid;
	hasIndices = heapRelation->rd_rel->relhasindex;

    /* Get the heap size */
    size = calculate_relation_size(tblspcOid, relnodeOid);

    /* Get index size */
	if (hasIndices)
	{
		/* recursively include any dependent indexes */
		indexoidlist = RelationGetIndexList(heapRelation);

		foreach(idx, indexoidlist)
		{
            idxOid = lfirst_oid(idx);
			idxRelation = relation_open(idxOid, AccessShareLock);
            idxTblspcOid = idxRelation->rd_rel->reltablespace;
 			size += calculate_relation_size(idxTblspcOid, idxOid);
			relation_close(idxRelation, AccessShareLock);
		}
		list_free(indexoidlist);
	}

    relation_close(heapRelation, AccessShareLock);

    /* Get toast table size */
	if (toastOid != 0)
	{
		/* recursively include any toast relations */
		toastRelation = relation_open(toastOid, AccessShareLock);
		toastTblspcOid = toastRelation->rd_rel->reltablespace;
		size += calculate_relation_size(toastTblspcOid, toastOid);
		relation_close(toastRelation, AccessShareLock);
	}

	return size;
}

/*
 *  Compute on-disk size of files for 'relation' including 
 *  heap data, index data, and toasted data.
 */
Datum
pg_total_relation_size_oid(PG_FUNCTION_ARGS)
{
	Oid		relOid=PG_GETARG_OID(0);
	HeapTuple	tuple;
	Form_pg_class	pg_class;
	Oid		relnodeOid;
	Oid		tblspcOid;

	tuple = SearchSysCache(RELOID,
						   ObjectIdGetDatum(relOid),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u", relOid);

	pg_class = (Form_pg_class) GETSTRUCT(tuple);
	relnodeOid = pg_class->relfilenode;
	tblspcOid = pg_class->reltablespace;

	ReleaseSysCache(tuple);

	PG_RETURN_INT64(calculate_total_relation_size(tblspcOid, relnodeOid));
}

Datum
pg_total_relation_size_name(PG_FUNCTION_ARGS)
{
	text		*relname = PG_GETARG_TEXT_P(0);
	RangeVar	*relrv;
	Relation	relation;
	Oid		relnodeOid;
	Oid		tblspcOid;
    
	relrv = makeRangeVarFromNameList(textToQualifiedNameList(relname));    
	relation = relation_openrv(relrv, AccessShareLock);
    
	tblspcOid  = relation->rd_rel->reltablespace;             
	relnodeOid = relation->rd_rel->relfilenode;
             
	relation_close(relation, AccessShareLock);

	PG_RETURN_INT64(calculate_total_relation_size(tblspcOid, relnodeOid));
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
	    snprintf(VARDATA(result), 50, INT64_FORMAT" bytes",
				 size);
	else
	{
		mult *= 1024;
		if (size < limit*mult)
		     snprintf(VARDATA(result), 50, INT64_FORMAT " kB",
					  (size+mult/2) / mult);
		else
		{
			mult *= 1024;
			if (size < limit*mult)
			    snprintf(VARDATA(result), 50, INT64_FORMAT " MB",
						 (size+mult/2) / mult);
			else
			{
				mult *= 1024;
				if (size < limit*mult)
				    snprintf(VARDATA(result), 50, INT64_FORMAT " GB",
							 (size+mult/2) / mult);
				else
				{
				    mult *= 1024;
				    snprintf(VARDATA(result), 50, INT64_FORMAT " TB",
							 (size+mult/2) / mult);
				}
			}
		}
	}

	VARATT_SIZEP(result) = strlen(VARDATA(result)) + VARHDRSZ;

	PG_RETURN_TEXT_P(result);
}
