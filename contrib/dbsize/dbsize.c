#include "postgres.h"
#include "fmgr.h"

#include "access/heapam.h"
#include "catalog/catalog.h"
#include "catalog/catname.h"
#include "catalog/pg_database.h"
#include "utils/fmgroids.h"

#include <stdlib.h>
#include <sys/types.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>


static char *
psnprintf(size_t len, const char *fmt,...)
{
	va_list		ap;
	char	   *buf;

	buf = palloc(len);

	va_start(ap, fmt);
	vsnprintf(buf, len, fmt, ap);
	va_end(ap);

	return buf;
}



/*
 * SQL function: database_size(name) returns bigint
 */

PG_FUNCTION_INFO_V1(database_size);

Datum
database_size(PG_FUNCTION_ARGS)
{
	Name		dbname = PG_GETARG_NAME(0);

	HeapTuple	tuple;
	Relation	relation;
	ScanKeyData	scanKey;
	HeapScanDesc scan;
	Oid			dbid;
	char	   *dbpath;
	DIR		   *dirdesc;
	struct dirent *direntry;
	int64		totalsize;

	relation = heap_openr(DatabaseRelationName, AccessShareLock);
	ScanKeyEntryInitialize(&scanKey, 0, Anum_pg_database_datname,
						   F_NAMEEQ, NameGetDatum(dbname));
	scan = heap_beginscan(relation, 0, SnapshotNow, 1, &scanKey);
	tuple = heap_getnext(scan, 0);

	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "database %s does not exist", NameStr(*dbname));

	dbid = tuple->t_data->t_oid;
	if (dbid == InvalidOid)
		elog(ERROR, "invalid database id");

	heap_endscan(scan);
	heap_close(relation, NoLock);

	dbpath = GetDatabasePath(dbid);

	dirdesc = opendir(dbpath);
	if (!dirdesc)
		elog(ERROR, "could not open directory %s: %s", dbpath, strerror(errno));

	totalsize = 0;
	for (;;)
	{
		char	   *fullname;
		struct stat statbuf;

		errno = 0;
		direntry = readdir(dirdesc);
		if (!direntry)
		{
			if (errno)
				elog(ERROR, "error reading directory: %s", strerror(errno));
			else
				break;
		}

		fullname = psnprintf(strlen(dbpath) + 1 + strlen(direntry->d_name) + 1,
							 "%s/%s", dbpath, direntry->d_name);
		if (stat(fullname, &statbuf) == -1)
			elog(ERROR, "could not stat %s: %s", fullname, strerror(errno));
		totalsize += statbuf.st_size;
		pfree(fullname);
	}

	closedir(dirdesc);

	PG_RETURN_INT64(totalsize);
}



/*
 * SQL function: relation_size(name) returns bigint
 */

PG_FUNCTION_INFO_V1(relation_size);

Datum
relation_size(PG_FUNCTION_ARGS)
{
	Name		relname = PG_GETARG_NAME(0);

	HeapTuple	tuple;
	Relation	relation;
	ScanKeyData	scanKey;
	HeapScanDesc scan;
	Oid			relnode;
	int64		totalsize;
	unsigned int segcount;

	relation = heap_openr(RelationRelationName, AccessShareLock);
	ScanKeyEntryInitialize(&scanKey, 0, Anum_pg_class_relname,
						   F_NAMEEQ, NameGetDatum(relname));
	scan = heap_beginscan(relation, 0, SnapshotNow, 1, &scanKey);
	tuple = heap_getnext(scan, 0);

	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "relation %s does not exist", NameStr(*relname));

	relnode = ((Form_pg_class) GETSTRUCT(tuple))->relfilenode;
	if (relnode == InvalidOid)
		elog(ERROR, "invalid relation node id");

	heap_endscan(scan);
	heap_close(relation, NoLock);

	totalsize = 0;
	segcount = 0;
	for (;;)
	{
		char	   *fullname;
		struct stat statbuf;

		if (segcount == 0)
			fullname = psnprintf(25, "%u", (unsigned) relnode);
		else
			fullname = psnprintf(50, "%u.%u", (unsigned) relnode, segcount);

		if (stat(fullname, &statbuf) == -1)
		{
			if (errno == ENOENT)
				break;
			else
				elog(ERROR, "could not stat %s: %s", fullname, strerror(errno));
		}
		totalsize += statbuf.st_size;
		pfree(fullname);
		segcount++;
	}

	PG_RETURN_INT64(totalsize);
}
