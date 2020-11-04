/*-------------------------------------------------------------------------
 *
 * libpq_source.c
 *	  Functions for fetching files from a remote server via libpq.
 *
 * Copyright (c) 2013-2020, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include "catalog/pg_type_d.h"
#include "common/connect.h"
#include "datapagemap.h"
#include "file_ops.h"
#include "filemap.h"
#include "pg_rewind.h"
#include "port/pg_bswap.h"
#include "rewind_source.h"

/*
 * Files are fetched max CHUNKSIZE bytes at a time.
 *
 * (This only applies to files that are copied in whole, or for truncated
 * files where we copy the tail. Relation files, where we know the individual
 * blocks that need to be fetched, are fetched in BLCKSZ chunks.)
 */
#define CHUNKSIZE 1000000

typedef struct
{
	rewind_source common;		/* common interface functions */

	PGconn	   *conn;
	bool		copy_started;
} libpq_source;

static void init_libpq_conn(PGconn *conn);
static char *run_simple_query(PGconn *conn, const char *sql);
static void run_simple_command(PGconn *conn, const char *sql);

/* public interface functions */
static void libpq_traverse_files(rewind_source *source,
								 process_file_callback_t callback);
static void libpq_queue_fetch_range(rewind_source *source, const char *path,
									off_t off, size_t len);
static void libpq_finish_fetch(rewind_source *source);
static char *libpq_fetch_file(rewind_source *source, const char *path,
							  size_t *filesize);
static XLogRecPtr libpq_get_current_wal_insert_lsn(rewind_source *source);
static void libpq_destroy(rewind_source *source);

/*
 * Create a new libpq source.
 *
 * The caller has already established the connection, but should not try
 * to use it while the source is active.
 */
rewind_source *
init_libpq_source(PGconn *conn)
{
	libpq_source *src;

	init_libpq_conn(conn);

	src = pg_malloc0(sizeof(libpq_source));

	src->common.traverse_files = libpq_traverse_files;
	src->common.fetch_file = libpq_fetch_file;
	src->common.queue_fetch_range = libpq_queue_fetch_range;
	src->common.finish_fetch = libpq_finish_fetch;
	src->common.get_current_wal_insert_lsn = libpq_get_current_wal_insert_lsn;
	src->common.destroy = libpq_destroy;

	src->conn = conn;

	return &src->common;
}

/*
 * Initialize a libpq connection for use.
 */
static void
init_libpq_conn(PGconn *conn)
{
	PGresult   *res;
	char	   *str;

	/* disable all types of timeouts */
	run_simple_command(conn, "SET statement_timeout = 0");
	run_simple_command(conn, "SET lock_timeout = 0");
	run_simple_command(conn, "SET idle_in_transaction_session_timeout = 0");

	/* secure search_path */
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
	str = run_simple_query(conn, "SELECT pg_is_in_recovery()");
	if (strcmp(str, "f") != 0)
		pg_fatal("source server must not be in recovery mode");
	pg_free(str);

	/*
	 * Also check that full_page_writes is enabled.  We can get torn pages if
	 * a page is modified while we read it with pg_read_binary_file(), and we
	 * rely on full page images to fix them.
	 */
	str = run_simple_query(conn, "SHOW full_page_writes");
	if (strcmp(str, "on") != 0)
		pg_fatal("full_page_writes must be enabled in the source server");
	pg_free(str);
}

/*
 * Run a query that returns a single value.
 *
 * The result should be pg_free'd after use.
 */
static char *
run_simple_query(PGconn *conn, const char *sql)
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
 * Run a command.
 *
 * In the event of a failure, exit immediately.
 */
static void
run_simple_command(PGconn *conn, const char *sql)
{
	PGresult   *res;

	res = PQexec(conn, sql);

	if (PQresultStatus(res) != PGRES_COMMAND_OK)
		pg_fatal("error running query (%s) in source server: %s",
				 sql, PQresultErrorMessage(res));

	PQclear(res);
}

/*
 * Call the pg_current_wal_insert_lsn() function in the remote system.
 */
static XLogRecPtr
libpq_get_current_wal_insert_lsn(rewind_source *source)
{
	PGconn	   *conn = ((libpq_source *) source)->conn;
	XLogRecPtr	result;
	uint32		hi;
	uint32		lo;
	char	   *val;

	val = run_simple_query(conn, "SELECT pg_current_wal_insert_lsn()");

	if (sscanf(val, "%X/%X", &hi, &lo) != 2)
		pg_fatal("unrecognized result \"%s\" for current WAL insert location", val);

	result = ((uint64) hi) << 32 | lo;

	pg_free(val);

	return result;
}

/*
 * Get a list of all files in the data directory.
 */
static void
libpq_traverse_files(rewind_source *source, process_file_callback_t callback)
{
	PGconn	   *conn = ((libpq_source *) source)->conn;
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

/*
 * Queue up a request to fetch a piece of a file from remote system.
 */
static void
libpq_queue_fetch_range(rewind_source *source, const char *path, off_t off,
						size_t len)
{
	libpq_source *src = (libpq_source *) source;
	uint64		begin = off;
	uint64		end = off + len;

	/*
	 * On first call, create a temporary table, and start COPYing to it.
	 * We will load it with the list of blocks that we need to fetch.
	 */
	if (!src->copy_started)
	{
		PGresult   *res;

		run_simple_command(src->conn, "CREATE TEMPORARY TABLE fetchchunks(path text, begin int8, len int4)");

		res = PQexec(src->conn, "COPY fetchchunks FROM STDIN");
		if (PQresultStatus(res) != PGRES_COPY_IN)
			pg_fatal("could not send file list: %s",
					 PQresultErrorMessage(res));
		PQclear(res);

		src->copy_started = true;
	}

	/*
	 * Write the file range to a temporary table in the server.
	 *
	 * The range is sent to the server as a COPY formatted line, to be inserted
	 * into the 'fetchchunks' temporary table. The libpq_finish_fetch() uses
	 * the temporary table to actually fetch the data.
	 */

	/* Split the range into CHUNKSIZE chunks */
	while (end - begin > 0)
	{
		char		linebuf[MAXPGPATH + 23];
		unsigned int len;

		/* Fine as long as CHUNKSIZE is not bigger than UINT32_MAX */
		if (end - begin > CHUNKSIZE)
			len = CHUNKSIZE;
		else
			len = (unsigned int) (end - begin);

		snprintf(linebuf, sizeof(linebuf), "%s\t" UINT64_FORMAT "\t%u\n", path, begin, len);

		if (PQputCopyData(src->conn, linebuf, strlen(linebuf)) != 1)
			pg_fatal("could not send COPY data: %s",
					 PQerrorMessage(src->conn));

		begin += len;
	}
}

/*
 * Receive all the queued chunks and write them to the target data directory.
 */
static void
libpq_finish_fetch(rewind_source *source)
{
	libpq_source *src = (libpq_source *) source;
	PGresult   *res;
	const char *sql;

	if (PQputCopyEnd(src->conn, NULL) != 1)
		pg_fatal("could not send end-of-COPY: %s",
				 PQerrorMessage(src->conn));

	while ((res = PQgetResult(src->conn)) != NULL)
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

	if (PQsendQueryParams(src->conn, sql, 0, NULL, NULL, NULL, NULL, 1) != 1)
		pg_fatal("could not send query: %s", PQerrorMessage(src->conn));

	pg_log_debug("getting file chunks");

	if (PQsetSingleRowMode(src->conn) != 1)
		pg_fatal("could not set libpq connection to single row mode");

	/*----
	 * The result set is of format:
	 *
	 * path		text	-- path in the data directory, e.g "base/1/123"
	 * begin	int8	-- offset within the file
	 * chunk	bytea	-- file content
	 *----
	 */
	while ((res = PQgetResult(src->conn)) != NULL)
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

		pg_log_debug("received chunk for file \"%s\", offset " INT64_FORMAT ", size %d",
					 filename, chunkoff, chunksize);

		open_target_file(filename, false);

		write_target_range(chunk, chunkoff, chunksize);

		pg_free(filename);

		PQclear(res);
	}
}

/*
 * Fetch a single file as a malloc'd buffer.
 */
static char *
libpq_fetch_file(rewind_source *source, const char *path, size_t *filesize)
{
	PGconn	   *conn = ((libpq_source *) source)->conn;
	PGresult   *res;
	char	   *result;
	int			len;
	const char *paramValues[1];

	paramValues[0] = path;
	res = PQexecParams(conn, "SELECT pg_read_binary_file($1)",
					   1, NULL, paramValues, NULL, NULL, 1);

	if (PQresultStatus(res) != PGRES_TUPLES_OK)
		pg_fatal("could not fetch remote file \"%s\": %s",
				 path, PQresultErrorMessage(res));

	/* sanity check the result set */
	if (PQntuples(res) != 1 || PQgetisnull(res, 0, 0))
		pg_fatal("unexpected result set while fetching remote file \"%s\"",
				 path);

	/* Read result to local variables */
	len = PQgetlength(res, 0, 0);
	result = pg_malloc(len + 1);
	memcpy(result, PQgetvalue(res, 0, 0), len);
	result[len] = '\0';

	PQclear(res);

	pg_log_debug("fetched file \"%s\", length %d", path, len);

	if (filesize)
		*filesize = len;
	return result;
}

/*
 * Close a libpq source.
 */
static void
libpq_destroy(rewind_source *source)
{
	pfree(source);
	/* NOTE: we don't close the connection here, as it was not opened by us. */
}
