#include "postgres.h"

#include <sys/types.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

#include "access/heapam.h"
#include "catalog/catalog.h"
#include "catalog/namespace.h"
#include "commands/dbcommands.h"
#include "fmgr.h"
#include "utils/builtins.h"


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

Datum database_size(PG_FUNCTION_ARGS);

Datum
database_size(PG_FUNCTION_ARGS)
{
	Name		dbname = PG_GETARG_NAME(0);

	Oid			dbid;
	char	   *dbpath;
	DIR		   *dirdesc;
	struct dirent *direntry;
	int64		totalsize;

	dbid = get_database_oid(NameStr(*dbname));
	if (!OidIsValid(dbid))
		elog(ERROR, "database %s does not exist", NameStr(*dbname));

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
 * SQL function: relation_size(text) returns bigint
 */

PG_FUNCTION_INFO_V1(relation_size);

Datum relation_size(PG_FUNCTION_ARGS);

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
				elog(ERROR, "could not stat %s: %m", fullname);
		}
		totalsize += statbuf.st_size;
		pfree(fullname);
		segcount++;
	}

	heap_close(relation, AccessShareLock);

	PG_RETURN_INT64(totalsize);
}
