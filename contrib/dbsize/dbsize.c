#include "postgres.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include "access/heapam.h"
#include "catalog/catalog.h"
#include "catalog/catname.h"
#include "catalog/namespace.h"
#include "catalog/pg_tablespace.h"
#include "commands/dbcommands.h"
#include "fmgr.h"
#include "storage/fd.h"
#include "utils/builtins.h"


static int64
get_tablespace_size(Oid dbid, Oid spcid, bool baddirOK);

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

Datum		database_size(PG_FUNCTION_ARGS);

Datum
database_size(PG_FUNCTION_ARGS)
{
	Name		dbname = PG_GETARG_NAME(0);

	Oid			dbid;
	int64		totalsize;
#ifdef SYMLINK
	Relation		dbrel;
	HeapScanDesc 	scan;
	HeapTuple		tuple;
#endif

	dbid = get_database_oid(NameStr(*dbname));
	if (!OidIsValid(dbid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_DATABASE),
			errmsg("database \"%s\" does not exist", NameStr(*dbname))));

#ifdef SYMLINK 

	dbrel = heap_openr(TableSpaceRelationName, AccessShareLock);	
	scan = heap_beginscan(dbrel, SnapshotNow, 0, (ScanKey) NULL);

	totalsize = 0;

	while((tuple = heap_getnext(scan, ForwardScanDirection)))
	{
		Oid spcid = HeapTupleGetOid(tuple);
		if(spcid != GLOBALTABLESPACE_OID)
			totalsize += get_tablespace_size(dbid, spcid, true);
	}
	heap_endscan(scan);
	heap_close(dbrel, AccessShareLock);
#else
	/* Same as always */
	totalsize = get_tablespace_size(dbid, DEFAULTTABLESPACE_OID, false);
#endif

	/*
	 * We need to keep in mind that we may not be called from the database
	 * whose size we're reporting so, we need to look in every tablespace
	 * to see if our database has data in there
	 */

	PG_RETURN_INT64(totalsize);
}

static int64
get_tablespace_size(Oid dbid, Oid spcid, bool baddirOK)
{
	char		*dbpath;
	DIR			*dirdesc;
	struct dirent *direntry;
	int64		totalsize;

	dbpath = GetDatabasePath(dbid, spcid);

	dirdesc = AllocateDir(dbpath);
	if (!dirdesc)
	{
		if(baddirOK)
			return 0;
		else
			ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open directory \"%s\": %m", dbpath)));
	}
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
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("error reading directory: %m")));
			else
				break;
		}

		fullname = psnprintf(strlen(dbpath) + 1 + strlen(direntry->d_name) + 1,
							 "%s/%s", dbpath, direntry->d_name);
		if (stat(fullname, &statbuf) == -1)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not stat \"%s\": %m", fullname)));
		totalsize += statbuf.st_size;
		pfree(fullname);
	}

	FreeDir(dirdesc);
	return (totalsize);
}

/*
 * SQL function: relation_size(text) returns bigint
 */

PG_FUNCTION_INFO_V1(relation_size);

Datum		relation_size(PG_FUNCTION_ARGS);

Datum
relation_size(PG_FUNCTION_ARGS)
{
	text	   *relname = PG_GETARG_TEXT_P(0);

	RangeVar   *relrv;
	Relation	relation;
	Oid			relnode;
	int64		totalsize;
	unsigned int segcount;

	relrv = makeRangeVarFromNameList(textToQualifiedNameList(relname,
													   "relation_size"));
	relation = heap_openrv(relrv, AccessShareLock);

	relnode = relation->rd_rel->relfilenode;

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
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not stat \"%s\": %m", fullname)));
		}
		totalsize += statbuf.st_size;
		pfree(fullname);
		segcount++;
	}

	heap_close(relation, AccessShareLock);

	PG_RETURN_INT64(totalsize);
}
