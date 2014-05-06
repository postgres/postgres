/*
 * dbsize.c
 *		Database object size functions, and related inquiries
 *
 * Copyright (c) 2002-2010, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/adt/dbsize.c,v 1.31 2010/02/26 02:01:07 momjian Exp $
 *
 */

#include "postgres.h"

#include <sys/types.h>
#include <sys/stat.h>

#include "access/heapam.h"
#include "catalog/catalog.h"
#include "catalog/namespace.h"
#include "catalog/pg_tablespace.h"
#include "commands/dbcommands.h"
#include "commands/tablespace.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "utils/relmapper.h"
#include "utils/syscache.h"


/* Return physical size of directory contents, or 0 if dir doesn't exist */
static int64
db_dir_size(const char *path)
{
	int64		dirsize = 0;
	struct dirent *direntry;
	DIR		   *dirdesc;
	char		filename[MAXPGPATH];

	dirdesc = AllocateDir(path);

	if (!dirdesc)
		return 0;

	while ((direntry = ReadDir(dirdesc, path)) != NULL)
	{
		struct stat fst;

		CHECK_FOR_INTERRUPTS();

		if (strcmp(direntry->d_name, ".") == 0 ||
			strcmp(direntry->d_name, "..") == 0)
			continue;

		snprintf(filename, MAXPGPATH, "%s/%s", path, direntry->d_name);

		if (stat(filename, &fst) < 0)
		{
			if (errno == ENOENT)
				continue;
			else
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not stat file \"%s\": %m", filename)));
		}
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
	DIR		   *dirdesc;
	struct dirent *direntry;
	char		dirpath[MAXPGPATH];
	char		pathname[MAXPGPATH];
	AclResult	aclresult;

	/* User must have connect privilege for target database */
	aclresult = pg_database_aclcheck(dbOid, GetUserId(), ACL_CONNECT);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, ACL_KIND_DATABASE,
					   get_database_name(dbOid));

	/* Shared storage in pg_global is not counted */

	/* Include pg_default storage */
	snprintf(pathname, MAXPGPATH, "base/%u", dbOid);
	totalsize = db_dir_size(pathname);

	/* Scan the non-default tablespaces */
	snprintf(dirpath, MAXPGPATH, "pg_tblspc");
	dirdesc = AllocateDir(dirpath);
	if (!dirdesc)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open tablespace directory \"%s\": %m",
						dirpath)));

	while ((direntry = ReadDir(dirdesc, dirpath)) != NULL)
	{
		CHECK_FOR_INTERRUPTS();

		if (strcmp(direntry->d_name, ".") == 0 ||
			strcmp(direntry->d_name, "..") == 0)
			continue;

		snprintf(pathname, MAXPGPATH, "pg_tblspc/%s/%s/%u",
				 direntry->d_name, TABLESPACE_VERSION_DIRECTORY, dbOid);
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
	Oid			dbOid = PG_GETARG_OID(0);

	PG_RETURN_INT64(calculate_database_size(dbOid));
}

Datum
pg_database_size_name(PG_FUNCTION_ARGS)
{
	Name		dbName = PG_GETARG_NAME(0);
	Oid			dbOid = get_database_oid(NameStr(*dbName));

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
	char		tblspcPath[MAXPGPATH];
	char		pathname[MAXPGPATH];
	int64		totalsize = 0;
	DIR		   *dirdesc;
	struct dirent *direntry;
	AclResult	aclresult;

	/*
	 * User must have CREATE privilege for target tablespace, either
	 * explicitly granted or implicitly because it is default for current
	 * database.
	 */
	if (tblspcOid != MyDatabaseTableSpace)
	{
		aclresult = pg_tablespace_aclcheck(tblspcOid, GetUserId(), ACL_CREATE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, ACL_KIND_TABLESPACE,
						   get_tablespace_name(tblspcOid));
	}

	if (tblspcOid == DEFAULTTABLESPACE_OID)
		snprintf(tblspcPath, MAXPGPATH, "base");
	else if (tblspcOid == GLOBALTABLESPACE_OID)
		snprintf(tblspcPath, MAXPGPATH, "global");
	else
		snprintf(tblspcPath, MAXPGPATH, "pg_tblspc/%u/%s", tblspcOid,
				 TABLESPACE_VERSION_DIRECTORY);

	dirdesc = AllocateDir(tblspcPath);

	if (!dirdesc)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open tablespace directory \"%s\": %m",
						tblspcPath)));

	while ((direntry = ReadDir(dirdesc, tblspcPath)) != NULL)
	{
		struct stat fst;

		CHECK_FOR_INTERRUPTS();

		if (strcmp(direntry->d_name, ".") == 0 ||
			strcmp(direntry->d_name, "..") == 0)
			continue;

		snprintf(pathname, MAXPGPATH, "%s/%s", tblspcPath, direntry->d_name);

		if (stat(pathname, &fst) < 0)
		{
			if (errno == ENOENT)
				continue;
			else
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not stat file \"%s\": %m", pathname)));
		}

		if (S_ISDIR(fst.st_mode))
			totalsize += db_dir_size(pathname);

		totalsize += fst.st_size;
	}

	FreeDir(dirdesc);

	return totalsize;
}

Datum
pg_tablespace_size_oid(PG_FUNCTION_ARGS)
{
	Oid			tblspcOid = PG_GETARG_OID(0);

	PG_RETURN_INT64(calculate_tablespace_size(tblspcOid));
}

Datum
pg_tablespace_size_name(PG_FUNCTION_ARGS)
{
	Name		tblspcName = PG_GETARG_NAME(0);
	Oid			tblspcOid = get_tablespace_oid(NameStr(*tblspcName));

	if (!OidIsValid(tblspcOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("tablespace \"%s\" does not exist",
						NameStr(*tblspcName))));

	PG_RETURN_INT64(calculate_tablespace_size(tblspcOid));
}


/*
 * calculate size of (one fork of) a relation
 */
static int64
calculate_relation_size(RelFileNode *rfn, ForkNumber forknum)
{
	int64		totalsize = 0;
	char	   *relationpath;
	char		pathname[MAXPGPATH];
	unsigned int segcount = 0;

	relationpath = relpath(*rfn, forknum);

	for (segcount = 0;; segcount++)
	{
		struct stat fst;

		CHECK_FOR_INTERRUPTS();

		if (segcount == 0)
			snprintf(pathname, MAXPGPATH, "%s",
					 relationpath);
		else
			snprintf(pathname, MAXPGPATH, "%s.%u",
					 relationpath, segcount);

		if (stat(pathname, &fst) < 0)
		{
			if (errno == ENOENT)
				break;
			else
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not stat file \"%s\": %m", pathname)));
		}
		totalsize += fst.st_size;
	}

	return totalsize;
}

Datum
pg_relation_size(PG_FUNCTION_ARGS)
{
	Oid			relOid = PG_GETARG_OID(0);
	text	   *forkName = PG_GETARG_TEXT_P(1);
	Relation	rel;
	int64		size;

	rel = relation_open(relOid, AccessShareLock);

	size = calculate_relation_size(&(rel->rd_node),
							  forkname_to_number(text_to_cstring(forkName)));

	relation_close(rel, AccessShareLock);

	PG_RETURN_INT64(size);
}

/*
 * Calculate total on-disk size of a TOAST relation, including its index.
 * Must not be applied to non-TOAST relations.
 */
static int64
calculate_toast_table_size(Oid toastrelid)
{
	int64		size = 0;
	Relation	toastRel;
	Relation	toastIdxRel;
	ForkNumber	forkNum;

	toastRel = relation_open(toastrelid, AccessShareLock);

	/* toast heap size, including FSM and VM size */
	for (forkNum = 0; forkNum <= MAX_FORKNUM; forkNum++)
		size += calculate_relation_size(&(toastRel->rd_node), forkNum);

	/* toast index size, including FSM and VM size */
	toastIdxRel = relation_open(toastRel->rd_rel->reltoastidxid, AccessShareLock);
	for (forkNum = 0; forkNum <= MAX_FORKNUM; forkNum++)
		size += calculate_relation_size(&(toastIdxRel->rd_node), forkNum);

	relation_close(toastIdxRel, AccessShareLock);
	relation_close(toastRel, AccessShareLock);

	return size;
}

/*
 * Calculate total on-disk size of a given table,
 * including FSM and VM, plus TOAST table if any.
 * Indexes other than the TOAST table's index are not included.
 *
 * Note that this also behaves sanely if applied to an index or toast table;
 * those won't have attached toast tables, but they can have multiple forks.
 */
static int64
calculate_table_size(Oid relOid)
{
	int64		size = 0;
	Relation	rel;
	ForkNumber	forkNum;

	rel = relation_open(relOid, AccessShareLock);

	/*
	 * heap size, including FSM and VM
	 */
	for (forkNum = 0; forkNum <= MAX_FORKNUM; forkNum++)
		size += calculate_relation_size(&(rel->rd_node), forkNum);

	/*
	 * Size of toast relation
	 */
	if (OidIsValid(rel->rd_rel->reltoastrelid))
		size += calculate_toast_table_size(rel->rd_rel->reltoastrelid);

	relation_close(rel, AccessShareLock);

	return size;
}

/*
 * Calculate total on-disk size of all indexes attached to the given table.
 *
 * Can be applied safely to an index, but you'll just get zero.
 */
static int64
calculate_indexes_size(Oid relOid)
{
	int64		size = 0;
	Relation	rel;

	rel = relation_open(relOid, AccessShareLock);

	/*
	 * Aggregate all indexes on the given relation
	 */
	if (rel->rd_rel->relhasindex)
	{
		List	   *index_oids = RelationGetIndexList(rel);
		ListCell   *cell;

		foreach(cell, index_oids)
		{
			Oid			idxOid = lfirst_oid(cell);
			Relation	idxRel;
			ForkNumber	forkNum;

			idxRel = relation_open(idxOid, AccessShareLock);

			for (forkNum = 0; forkNum <= MAX_FORKNUM; forkNum++)
				size += calculate_relation_size(&(idxRel->rd_node), forkNum);

			relation_close(idxRel, AccessShareLock);
		}

		list_free(index_oids);
	}

	relation_close(rel, AccessShareLock);

	return size;
}

Datum
pg_table_size(PG_FUNCTION_ARGS)
{
	Oid			relOid = PG_GETARG_OID(0);

	PG_RETURN_INT64(calculate_table_size(relOid));
}

Datum
pg_indexes_size(PG_FUNCTION_ARGS)
{
	Oid			relOid = PG_GETARG_OID(0);

	PG_RETURN_INT64(calculate_indexes_size(relOid));
}

/*
 *	Compute the on-disk size of all files for the relation,
 *	including heap data, index data, toast data, FSM, VM.
 */
static int64
calculate_total_relation_size(Oid Relid)
{
	int64		size;

	/*
	 * Aggregate the table size, this includes size of the heap, toast and
	 * toast index with free space and visibility map
	 */
	size = calculate_table_size(Relid);

	/*
	 * Add size of all attached indexes as well
	 */
	size += calculate_indexes_size(Relid);

	return size;
}

Datum
pg_total_relation_size(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);

	PG_RETURN_INT64(calculate_total_relation_size(relid));
}

/*
 * formatting with size units
 */
Datum
pg_size_pretty(PG_FUNCTION_ARGS)
{
	int64		size = PG_GETARG_INT64(0);
	char		buf[64];
	int64		limit = 10 * 1024;
	int64		limit2 = limit * 2 - 1;

	if (size < limit)
		snprintf(buf, sizeof(buf), INT64_FORMAT " bytes", size);
	else
	{
		size >>= 9;				/* keep one extra bit for rounding */
		if (size < limit2)
			snprintf(buf, sizeof(buf), INT64_FORMAT " kB",
					 (size + 1) / 2);
		else
		{
			size >>= 10;
			if (size < limit2)
				snprintf(buf, sizeof(buf), INT64_FORMAT " MB",
						 (size + 1) / 2);
			else
			{
				size >>= 10;
				if (size < limit2)
					snprintf(buf, sizeof(buf), INT64_FORMAT " GB",
							 (size + 1) / 2);
				else
				{
					size >>= 10;
					snprintf(buf, sizeof(buf), INT64_FORMAT " TB",
							 (size + 1) / 2);
				}
			}
		}
	}

	PG_RETURN_TEXT_P(cstring_to_text(buf));
}

/*
 * Get the filenode of a relation
 *
 * This is expected to be used in queries like
 *		SELECT pg_relation_filenode(oid) FROM pg_class;
 * That leads to a couple of choices.  We work from the pg_class row alone
 * rather than actually opening each relation, for efficiency.  We don't
 * fail if we can't find the relation --- some rows might be visible in
 * the query's MVCC snapshot but already dead according to SnapshotNow.
 * (Note: we could avoid using the catcache, but there's little point
 * because the relation mapper also works "in the now".)  We also don't
 * fail if the relation doesn't have storage.  In all these cases it
 * seems better to quietly return NULL.
 */
Datum
pg_relation_filenode(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	Oid			result;
	HeapTuple	tuple;
	Form_pg_class relform;

	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tuple))
		PG_RETURN_NULL();
	relform = (Form_pg_class) GETSTRUCT(tuple);

	switch (relform->relkind)
	{
		case RELKIND_RELATION:
		case RELKIND_INDEX:
		case RELKIND_SEQUENCE:
		case RELKIND_TOASTVALUE:
			/* okay, these have storage */
			if (relform->relfilenode)
				result = relform->relfilenode;
			else	/* Consult the relation mapper */
				result = RelationMapOidToFilenode(relid,
												  relform->relisshared);
			break;

		default:
			/* no storage, return NULL */
			result = InvalidOid;
			break;
	}

	ReleaseSysCache(tuple);

	if (!OidIsValid(result))
		PG_RETURN_NULL();

	PG_RETURN_OID(result);
}

/*
 * Get the pathname (relative to $PGDATA) of a relation
 *
 * See comments for pg_relation_filenode.
 */
Datum
pg_relation_filepath(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	HeapTuple	tuple;
	Form_pg_class relform;
	RelFileNode rnode;
	char	   *path;

	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tuple))
		PG_RETURN_NULL();
	relform = (Form_pg_class) GETSTRUCT(tuple);

	switch (relform->relkind)
	{
		case RELKIND_RELATION:
		case RELKIND_INDEX:
		case RELKIND_SEQUENCE:
		case RELKIND_TOASTVALUE:
			/* okay, these have storage */

			/* This logic should match RelationInitPhysicalAddr */
			if (relform->reltablespace)
				rnode.spcNode = relform->reltablespace;
			else
				rnode.spcNode = MyDatabaseTableSpace;
			if (rnode.spcNode == GLOBALTABLESPACE_OID)
				rnode.dbNode = InvalidOid;
			else
				rnode.dbNode = MyDatabaseId;
			if (relform->relfilenode)
				rnode.relNode = relform->relfilenode;
			else	/* Consult the relation mapper */
				rnode.relNode = RelationMapOidToFilenode(relid,
													   relform->relisshared);
			break;

		default:
			/* no storage, return NULL */
			rnode.relNode = InvalidOid;
			/* some compilers generate warnings without these next two lines */
			rnode.dbNode = InvalidOid;
			rnode.spcNode = InvalidOid;
			break;
	}

	ReleaseSysCache(tuple);

	if (!OidIsValid(rnode.relNode))
		PG_RETURN_NULL();

	path = relpath(rnode, MAIN_FORKNUM);

	PG_RETURN_TEXT_P(cstring_to_text(path));
}
