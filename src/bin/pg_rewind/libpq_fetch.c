/*-------------------------------------------------------------------------
 *
 * libpq_fetch.c
 *	  Functions for fetching files from a remote server.
 *
 * Copyright (c) 2013-2020, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#include "catalog/pg_type_d.h"
#include "common/connect.h"
#include "datapagemap.h"
#include "fetch.h"
#include "file_ops.h"
#include "filemap.h"
#include "pg_rewind.h"
#include "port/pg_bswap.h"

PGconn	   *conn = NULL;

/*
 * Files are fetched max CHUNKSIZE bytes at a time.
 *
 * (This only applies to files that are copied in whole, or for truncated
 * files where we copy the tail. Relation files, where we know the individual
 * blocks that need to be fetched, are fetched in BLCKSZ chunks.)
 */
#define CHUNKSIZE 1000000

static void receiveFileChunks(const char *sql);
static void execute_pagemap(datapagemap_t *pagemap, const char *path);
static char *run_simple_query(const char *sql);
static void run_simple_command(const char *sql);

void
libpqConnect(const char *connstr)
{
	char	   *str;
	PGresult   *res;

	conn = PQconnectdb(connstr);
	if (PQstatus(conn) == CONNECTION_BAD)
		pg_fatal("%s", PQerrorMessage(conn));

	if (showprogress)
		pg_log_info("connected to server");

	/* disable all types of timeouts */
	run_simple_command("SET statement_timeout = 0");
	run_simple_command("SET lock_timeout = 0");
	run_simple_command("SET idle_in_transaction_session_timeout = 0");

	res = PQexec(conn, ALWAYS_SECURE_SEARCH_PATH_SQL);
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
		pg_fatal("could not clear search_path: %s",
				 PQresultErrorMessage(res));
	PQclear(res);

	/*
	 * Check that the server is not in hot standby mode. There is no
	 * fundamental reason that couldn't be made to work, but it doesn't
	 * currently because we use a temporary table. Better to check for it
	 * explicitly than error out, for a better error message.
	 */
	str = run_simple_query("SELECT pg_is_in_recovery()");
	if (strcmp(str, "f") != 0)
		pg_fatal("source server must not be in recovery mode");
	pg_free(str);

	/*
	 * Also check that full_page_writes is enabled.  We can get torn pages if
	 * a page is modified while we read it with pg_read_binary_file(), and we
	 * rely on full page images to fix them.
	 */
	str = run_simple_query("SHOW full_page_writes");
	if (strcmp(str, "on") != 0)
		pg_fatal("full_page_writes must be enabled in the source server");
	pg_free(str);

	/*
	 * Although we don't do any "real" updates, we do work with a temporary
	 * table. We don't care about synchronous commit for that. It doesn't
	 * otherwise matter much, but if the server is using synchronous
	 * replication, and replication isn't working for some reason, we don't
	 * want to get stuck, waiting for it to start working again.
	 */
	run_simple_command("SET synchronous_commit = off");
}

/*
 * Runs a query that returns a single value.
 * The result should be pg_free'd after use.
 */
static char *
run_simple_query(const char *sql)
{
	PGresult   *res;
	char	   *result;

	res = PQexec(conn, sql);

	if (PQresultStatus(res) != PGRES_TUPLES_OK)
		pg_fatal("error running query (%s) on source server: %s",
				 sql, PQresultErrorMessage(res));

	/* sanity check the result set */
	if (PQnfields(res) != 1 || PQntuples(res) != 1 || PQgetisnull(res, 0, 0))
		pg_fatal("unexpected result set from query");

	result = pg_strdup(PQgetvalue(res, 0, 0));

	PQclear(res);

	return result;
}

/*
 * Runs a command.
 * In the event of a failure, exit immediately.
 */
static void
run_simple_command(const char *sql)
{
	PGresult   *res;

	res = PQexec(conn, sql);

	if (PQresultStatus(res) != PGRES_COMMAND_OK)
		pg_fatal("error running query (%s) in source server: %s",
				 sql, PQresultErrorMessage(res));

	PQclear(res);
}

/*
 * Calls pg_current_wal_insert_lsn() function
 */
XLogRecPtr
libpqGetCurrentXlogInsertLocation(void)
{
	XLogRecPtr	result;
	uint32		hi;
	uint32		lo;
	char	   *val;

	val = run_simple_query("SELECT pg_current_wal_insert_lsn()");

	if (sscanf(val, "%X/%X", &hi, &lo) != 2)
		pg_fatal("unrecognized result \"%s\" for current WAL insert location", val);

	result = ((uint64) hi) << 32 | lo;

	pg_free(val);

	return result;
}

/*
 * Get a list of all files in the data directory.
 */
void
libpqProcessFileList(void)
{
	PGresult   *res;
	const char *sql;
	int			i;

	/*
	 * Create a recursive directory listing of the whole data directory.
	 *
	 * The WITH RECURSIVE part does most of the work. The second part gets the
	 * targets of the symlinks in pg_tblspc directory.
	 *
	 * XXX: There is no backend function to get a symbolic link's target in
	 * general, so if the admin has put any custom symbolic links in the data
	 * directory, they won't be copied correctly.
	 */
	sql =
		"WITH RECURSIVE files (path, filename, size, isdir) AS (\n"
		"  SELECT '' AS path, filename, size, isdir FROM\n"
		"  (SELECT pg_ls_dir('.', true, false) AS filename) AS fn,\n"
		"        pg_stat_file(fn.filename, true) AS this\n"
		"  UNION ALL\n"
		"  SELECT parent.path || parent.filename || '/' AS path,\n"
		"         fn, this.size, this.isdir\n"
		"  FROM files AS parent,\n"
		"       pg_ls_dir(parent.path || parent.filename, true, false) AS fn,\n"
		"       pg_stat_file(parent.path || parent.filename || '/' || fn, true) AS this\n"
		"       WHERE parent.isdir = 't'\n"
		")\n"
		"SELECT path || filename, size, isdir,\n"
		"       pg_tablespace_location(pg_tablespace.oid) AS link_target\n"
		"FROM files\n"
		"LEFT OUTER JOIN pg_tablespace ON files.path = 'pg_tblspc/'\n"
		"                             AND oid::text = files.filename\n";
	res = PQexec(conn, sql);

	if (PQresultStatus(res) != PGRES_TUPLES_OK)
		pg_fatal("could not fetch file list: %s",
				 PQresultErrorMessage(res));

	/* sanity check the result set */
	if (PQnfields(res) != 4)
		pg_fatal("unexpected result set while fetching file list");

	/* Read result to local variables */
	for (i = 0; i < PQntuples(res); i++)
	{
		char	   *path;
		int64		filesize;
		bool		isdir;
		char	   *link_target;
		file_type_t type;

		if (PQgetisnull(res, i, 1))
		{
			/*
			 * The file was removed from the server while the query was
			 * running. Ignore it.
			 */
			continue;
		}

		path = PQgetvalue(res, i, 0);
		filesize = atol(PQgetvalue(res, i, 1));
		isdir = (strcmp(PQgetvalue(res, i, 2), "t") == 0);
		link_target = PQgetvalue(res, i, 3);

		if (link_target[0])
			type = FILE_TYPE_SYMLINK;
		else if (isdir)
			type = FILE_TYPE_DIRECTORY;
		else
			type = FILE_TYPE_REGULAR;

		process_source_file(path, type, filesize, link_target);
	}
	PQclear(res);
}

/*----
 * Runs a query, which returns pieces of files from the remote source data
 * directory, and overwrites the corresponding parts of target files with
 * the received parts. The result set is expected to be of format:
 *
 * path		text	-- path in the data directory, e.g "base/1/123"
 * begin	int8	-- offset within the file
 * chunk	bytea	-- file content
 *----
 */
static void
receiveFileChunks(const char *sql)
{
	PGresult   *res;

	if (PQsendQueryParams(conn, sql, 0, NULL, NULL, NULL, NULL, 1) != 1)
		pg_fatal("could not send query: %s", PQerrorMessage(conn));

	pg_log_debug("getting file chunks");

	if (PQsetSingleRowMode(conn) != 1)
		pg_fatal("could not set libpq connection to single row mode");

	while ((res = PQgetResult(conn)) != NULL)
	{
		char	   *filename;
		int			filenamelen;
		int64		chunkoff;
		int			chunksize;
		char	   *chunk;

		switch (PQresultStatus(res))
		{
			case PGRES_SINGLE_TUPLE:
				break;

			case PGRES_TUPLES_OK:
				PQclear(res);
				continue;		/* final zero-row result */

			default:
				pg_fatal("unexpected result while fetching remote files: %s",
						 PQresultErrorMessage(res));
		}

		/* sanity check the result set */
		if (PQnfields(res) != 3 || PQntuples(res) != 1)
			pg_fatal("unexpected result set size while fetching remote files");

		if (PQftype(res, 0) != TEXTOID ||
			PQftype(res, 1) != INT8OID ||
			PQftype(res, 2) != BYTEAOID)
		{
			pg_fatal("unexpected data types in result set while fetching remote files: %u %u %u",
					 PQftype(res, 0), PQftype(res, 1), PQftype(res, 2));
		}

		if (PQfformat(res, 0) != 1 &&
			PQfformat(res, 1) != 1 &&
			PQfformat(res, 2) != 1)
		{
			pg_fatal("unexpected result format while fetching remote files");
		}

		if (PQgetisnull(res, 0, 0) ||
			PQgetisnull(res, 0, 1))
		{
			pg_fatal("unexpected null values in result while fetching remote files");
		}

		if (PQgetlength(res, 0, 1) != sizeof(int64))
			pg_fatal("unexpected result length while fetching remote files");

		/* Read result set to local variables */
		memcpy(&chunkoff, PQgetvalue(res, 0, 1), sizeof(int64));
		chunkoff = pg_ntoh64(chunkoff);
		chunksize = PQgetlength(res, 0, 2);

		filenamelen = PQgetlength(res, 0, 0);
		filename = pg_malloc(filenamelen + 1);
		memcpy(filename, PQgetvalue(res, 0, 0), filenamelen);
		filename[filenamelen] = '\0';

		chunk = PQgetvalue(res, 0, 2);

		/*
		 * If a file has been deleted on the source, remove it on the target
		 * as well.  Note that multiple unlink() calls may happen on the same
		 * file if multiple data chunks are associated with it, hence ignore
		 * unconditionally anything missing.  If this file is not a relation
		 * data file, then it has been already truncated when creating the
		 * file chunk list at the previous execution of the filemap.
		 */
		if (PQgetisnull(res, 0, 2))
		{
			pg_log_debug("received null value for chunk for file \"%s\", file has been deleted",
						 filename);
			remove_target_file(filename, true);
			pg_free(filename);
			PQclear(res);
			continue;
		}

		pg_log_debug("received chunk for file \"%s\", offset %lld, size %d",
					 filename, (long long int) chunkoff, chunksize);

		open_target_file(filename, false);

		write_target_range(chunk, chunkoff, chunksize);

		pg_free(filename);

		PQclear(res);
	}
}

/*
 * Receive a single file as a malloc'd buffer.
 */
char *
libpqGetFile(const char *filename, size_t *filesize)
{
	PGresult   *res;
	char	   *result;
	int			len;
	const char *paramValues[1];

	paramValues[0] = filename;
	res = PQexecParams(conn, "SELECT pg_read_binary_file($1)",
					   1, NULL, paramValues, NULL, NULL, 1);

	if (PQresultStatus(res) != PGRES_TUPLES_OK)
		pg_fatal("could not fetch remote file \"%s\": %s",
				 filename, PQresultErrorMessage(res));

	/* sanity check the result set */
	if (PQntuples(res) != 1 || PQgetisnull(res, 0, 0))
		pg_fatal("unexpected result set while fetching remote file \"%s\"",
				 filename);

	/* Read result to local variables */
	len = PQgetlength(res, 0, 0);
	result = pg_malloc(len + 1);
	memcpy(result, PQgetvalue(res, 0, 0), len);
	result[len] = '\0';

	PQclear(res);

	pg_log_debug("fetched file \"%s\", length %d", filename, len);

	if (filesize)
		*filesize = len;
	return result;
}

/*
 * Write a file range to a temporary table in the server.
 *
 * The range is sent to the server as a COPY formatted line, to be inserted
 * into the 'fetchchunks' temporary table. It is used in receiveFileChunks()
 * function to actually fetch the data.
 */
static void
fetch_file_range(const char *path, uint64 begin, uint64 end)
{
	char		linebuf[MAXPGPATH + 23];

	/* Split the range into CHUNKSIZE chunks */
	while (end - begin > 0)
	{
		unsigned int len;

		/* Fine as long as CHUNKSIZE is not bigger than UINT32_MAX */
		if (end - begin > CHUNKSIZE)
			len = CHUNKSIZE;
		else
			len = (unsigned int) (end - begin);

		snprintf(linebuf, sizeof(linebuf), "%s\t" UINT64_FORMAT "\t%u\n", path, begin, len);

		if (PQputCopyData(conn, linebuf, strlen(linebuf)) != 1)
			pg_fatal("could not send COPY data: %s",
					 PQerrorMessage(conn));

		begin += len;
	}
}

/*
 * Fetch all changed blocks from remote source data directory.
 */
void
libpq_executeFileMap(filemap_t *map)
{
	file_entry_t *entry;
	const char *sql;
	PGresult   *res;
	int			i;

	/*
	 * First create a temporary table, and load it with the blocks that we
	 * need to fetch.
	 */
	sql = "CREATE TEMPORARY TABLE fetchchunks(path text, begin int8, len int4);";
	run_simple_command(sql);

	sql = "COPY fetchchunks FROM STDIN";
	res = PQexec(conn, sql);

	if (PQresultStatus(res) != PGRES_COPY_IN)
		pg_fatal("could not send file list: %s",
				 PQresultErrorMessage(res));
	PQclear(res);

	for (i = 0; i < map->narray; i++)
	{
		entry = map->array[i];

		/* If this is a relation file, copy the modified blocks */
		execute_pagemap(&entry->pagemap, entry->path);

		switch (entry->action)
		{
			case FILE_ACTION_NONE:
				/* nothing else to do */
				break;

			case FILE_ACTION_COPY:
				/* Truncate the old file out of the way, if any */
				open_target_file(entry->path, true);
				fetch_file_range(entry->path, 0, entry->newsize);
				break;

			case FILE_ACTION_TRUNCATE:
				truncate_target_file(entry->path, entry->newsize);
				break;

			case FILE_ACTION_COPY_TAIL:
				fetch_file_range(entry->path, entry->oldsize, entry->newsize);
				break;

			case FILE_ACTION_REMOVE:
				remove_target(entry);
				break;

			case FILE_ACTION_CREATE:
				create_target(entry);
				break;
		}
	}

	if (PQputCopyEnd(conn, NULL) != 1)
		pg_fatal("could not send end-of-COPY: %s",
				 PQerrorMessage(conn));

	while ((res = PQgetResult(conn)) != NULL)
	{
		if (PQresultStatus(res) != PGRES_COMMAND_OK)
			pg_fatal("unexpected result while sending file list: %s",
					 PQresultErrorMessage(res));
		PQclear(res);
	}

	/*
	 * We've now copied the list of file ranges that we need to fetch to the
	 * temporary table. Now, actually fetch all of those ranges.
	 */
	sql =
		"SELECT path, begin,\n"
		"  pg_read_binary_file(path, begin, len, true) AS chunk\n"
		"FROM fetchchunks\n";

	receiveFileChunks(sql);
}

static void
execute_pagemap(datapagemap_t *pagemap, const char *path)
{
	datapagemap_iterator_t *iter;
	BlockNumber blkno;
	off_t		offset;

	iter = datapagemap_iterate(pagemap);
	while (datapagemap_next(iter, &blkno))
	{
		offset = blkno * BLCKSZ;

		fetch_file_range(path, offset, offset + BLCKSZ);
	}
	pg_free(iter);
}
