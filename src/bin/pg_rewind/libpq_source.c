/*-------------------------------------------------------------------------
 *
 * libpq_source.c
 *	  Functions for fetching files from a remote server via libpq.
 *
 * Copyright (c) 2013-2024, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include "catalog/pg_type_d.h"
#include "common/connect.h"
#include "file_ops.h"
#include "filemap.h"
#include "lib/stringinfo.h"
#include "pg_rewind.h"
#include "port/pg_bswap.h"
#include "rewind_source.h"

/*
 * Files are fetched MAX_CHUNK_SIZE bytes at a time, and with a
 * maximum of MAX_CHUNKS_PER_QUERY chunks in a single query.
 */
#define MAX_CHUNK_SIZE (1024 * 1024)
#define MAX_CHUNKS_PER_QUERY 1000

/* represents a request to fetch a piece of a file from the source */
typedef struct
{
	const char *path;			/* path relative to data directory root */
	off_t		offset;
	size_t		length;
} fetch_range_request;

typedef struct
{
	rewind_source common;		/* common interface functions */

	PGconn	   *conn;

	/*
	 * Queue of chunks that have been requested with the queue_fetch_range()
	 * function, but have not been fetched from the remote server yet.
	 */
	int			num_requests;
	fetch_range_request request_queue[MAX_CHUNKS_PER_QUERY];

	/* temporary space for process_queued_fetch_requests() */
	StringInfoData paths;
	StringInfoData offsets;
	StringInfoData lengths;
} libpq_source;

static void init_libpq_conn(PGconn *conn);
static char *run_simple_query(PGconn *conn, const char *sql);
static void run_simple_command(PGconn *conn, const char *sql);
static void appendArrayEscapedString(StringInfo buf, const char *str);

static void process_queued_fetch_requests(libpq_source *src);

/* public interface functions */
static void libpq_traverse_files(rewind_source *source,
								 process_file_callback_t callback);
static void libpq_queue_fetch_file(rewind_source *source, const char *path, size_t len);
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
	src->common.queue_fetch_file = libpq_queue_fetch_file;
	src->common.queue_fetch_range = libpq_queue_fetch_range;
	src->common.finish_fetch = libpq_finish_fetch;
	src->common.get_current_wal_insert_lsn = libpq_get_current_wal_insert_lsn;
	src->common.destroy = libpq_destroy;

	src->conn = conn;

	initStringInfo(&src->paths);
	initStringInfo(&src->offsets);
	initStringInfo(&src->lengths);

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
	run_simple_command(conn, "SET transaction_timeout = 0");

	/*
	 * we don't intend to do any updates, put the connection in read-only mode
	 * to keep us honest
	 */
	run_simple_command(conn, "SET default_transaction_read_only = on");

	/* secure search_path */
	res = PQexec(conn, ALWAYS_SECURE_SEARCH_PATH_SQL);
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
		pg_fatal("could not clear \"search_path\": %s",
				 PQresultErrorMessage(res));
	PQclear(res);

	/*
	 * Also check that full_page_writes is enabled.  We can get torn pages if
	 * a page is modified while we read it with pg_read_binary_file(), and we
	 * rely on full page images to fix them.
	 */
	str = run_simple_query(conn, "SHOW full_page_writes");
	if (strcmp(str, "on") != 0)
		pg_fatal("\"full_page_writes\" must be enabled in the source server");
	pg_free(str);

	/* Prepare a statement we'll use to fetch files */
	res = PQprepare(conn, "fetch_chunks_stmt",
					"SELECT path, begin,\n"
					"  pg_read_binary_file(path, begin, len, true) AS chunk\n"
					"FROM unnest ($1::text[], $2::int8[], $3::int4[]) as x(path, begin, len)",
					3, NULL);

	if (PQresultStatus(res) != PGRES_COMMAND_OK)
		pg_fatal("could not prepare statement to fetch file contents: %s",
				 PQresultErrorMessage(res));
	PQclear(res);
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
		filesize = atoll(PQgetvalue(res, i, 1));
		isdir = (strcmp(PQgetvalue(res, i, 2), "t") == 0);
		link_target = PQgetvalue(res, i, 3);

		if (link_target[0])
		{
			/*
			 * In-place tablespaces are directories located in pg_tblspc/ with
			 * relative paths.
			 */
			if (is_absolute_path(link_target))
				type = FILE_TYPE_SYMLINK;
			else
				type = FILE_TYPE_DIRECTORY;
		}
		else if (isdir)
			type = FILE_TYPE_DIRECTORY;
		else
			type = FILE_TYPE_REGULAR;

		callback(path, type, filesize, link_target);
	}
	PQclear(res);
}

/*
 * Queue up a request to fetch a file from remote system.
 */
static void
libpq_queue_fetch_file(rewind_source *source, const char *path, size_t len)
{
	/*
	 * Truncate the target file immediately, and queue a request to fetch it
	 * from the source. If the file is small, smaller than MAX_CHUNK_SIZE,
	 * request fetching a full-sized chunk anyway, so that if the file has
	 * become larger in the source system, after we scanned the source
	 * directory, we still fetch the whole file. This only works for files up
	 * to MAX_CHUNK_SIZE, but that's good enough for small configuration files
	 * and such that are changed every now and then, but not WAL-logged. For
	 * larger files, we fetch up to the original size.
	 *
	 * Even with that mechanism, there is an inherent race condition if the
	 * file is modified at the same instant that we're copying it, so that we
	 * might copy a torn version of the file with one half from the old
	 * version and another half from the new. But pg_basebackup has the same
	 * problem, and it hasn't been a problem in practice.
	 *
	 * It might seem more natural to truncate the file later, when we receive
	 * it from the source server, but then we'd need to track which
	 * fetch-requests are for a whole file.
	 */
	open_target_file(path, true);
	libpq_queue_fetch_range(source, path, 0, Max(len, MAX_CHUNK_SIZE));
}

/*
 * Queue up a request to fetch a piece of a file from remote system.
 */
static void
libpq_queue_fetch_range(rewind_source *source, const char *path, off_t off,
						size_t len)
{
	libpq_source *src = (libpq_source *) source;

	/*
	 * Does this request happen to be a continuation of the previous chunk? If
	 * so, merge it with the previous one.
	 *
	 * XXX: We use pointer equality to compare the path. That's good enough
	 * for our purposes; the caller always passes the same pointer for the
	 * same filename. If it didn't, we would fail to merge requests, but it
	 * wouldn't affect correctness.
	 */
	if (src->num_requests > 0)
	{
		fetch_range_request *prev = &src->request_queue[src->num_requests - 1];

		if (prev->offset + prev->length == off &&
			prev->length < MAX_CHUNK_SIZE &&
			prev->path == path)
		{
			/*
			 * Extend the previous request to cover as much of this new
			 * request as possible, without exceeding MAX_CHUNK_SIZE.
			 */
			size_t		thislen;

			thislen = Min(len, MAX_CHUNK_SIZE - prev->length);
			prev->length += thislen;

			off += thislen;
			len -= thislen;

			/*
			 * Fall through to create new requests for any remaining 'len'
			 * that didn't fit in the previous chunk.
			 */
		}
	}

	/* Divide the request into pieces of MAX_CHUNK_SIZE bytes each */
	while (len > 0)
	{
		int32		thislen;

		/* if the queue is full, perform all the work queued up so far */
		if (src->num_requests == MAX_CHUNKS_PER_QUERY)
			process_queued_fetch_requests(src);

		thislen = Min(len, MAX_CHUNK_SIZE);
		src->request_queue[src->num_requests].path = path;
		src->request_queue[src->num_requests].offset = off;
		src->request_queue[src->num_requests].length = thislen;
		src->num_requests++;

		off += thislen;
		len -= thislen;
	}
}

/*
 * Fetch all the queued chunks and write them to the target data directory.
 */
static void
libpq_finish_fetch(rewind_source *source)
{
	process_queued_fetch_requests((libpq_source *) source);
}

static void
process_queued_fetch_requests(libpq_source *src)
{
	const char *params[3];
	PGresult   *res;
	int			chunkno;

	if (src->num_requests == 0)
		return;

	pg_log_debug("getting %d file chunks", src->num_requests);

	/*
	 * The prepared statement, 'fetch_chunks_stmt', takes three arrays with
	 * the same length as parameters: paths, offsets and lengths. Construct
	 * the string representations of them.
	 */
	resetStringInfo(&src->paths);
	resetStringInfo(&src->offsets);
	resetStringInfo(&src->lengths);

	appendStringInfoChar(&src->paths, '{');
	appendStringInfoChar(&src->offsets, '{');
	appendStringInfoChar(&src->lengths, '{');
	for (int i = 0; i < src->num_requests; i++)
	{
		fetch_range_request *rq = &src->request_queue[i];

		if (i > 0)
		{
			appendStringInfoChar(&src->paths, ',');
			appendStringInfoChar(&src->offsets, ',');
			appendStringInfoChar(&src->lengths, ',');
		}

		appendArrayEscapedString(&src->paths, rq->path);
		appendStringInfo(&src->offsets, INT64_FORMAT, (int64) rq->offset);
		appendStringInfo(&src->lengths, INT64_FORMAT, (int64) rq->length);
	}
	appendStringInfoChar(&src->paths, '}');
	appendStringInfoChar(&src->offsets, '}');
	appendStringInfoChar(&src->lengths, '}');

	/*
	 * Execute the prepared statement.
	 */
	params[0] = src->paths.data;
	params[1] = src->offsets.data;
	params[2] = src->lengths.data;

	if (PQsendQueryPrepared(src->conn, "fetch_chunks_stmt", 3, params, NULL, NULL, 1) != 1)
		pg_fatal("could not send query: %s", PQerrorMessage(src->conn));

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
	chunkno = 0;
	while ((res = PQgetResult(src->conn)) != NULL)
	{
		fetch_range_request *rq = &src->request_queue[chunkno];
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

		if (chunkno > src->num_requests)
			pg_fatal("received more data chunks than requested");

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
		 * unconditionally anything missing.
		 */
		if (PQgetisnull(res, 0, 2))
		{
			pg_log_debug("received null value for chunk for file \"%s\", file has been deleted",
						 filename);
			remove_target_file(filename, true);
		}
		else
		{
			pg_log_debug("received chunk for file \"%s\", offset %lld, size %d",
						 filename, (long long int) chunkoff, chunksize);

			if (strcmp(filename, rq->path) != 0)
			{
				pg_fatal("received data for file \"%s\", when requested for \"%s\"",
						 filename, rq->path);
			}
			if (chunkoff != rq->offset)
				pg_fatal("received data at offset %lld of file \"%s\", when requested for offset %lld",
						 (long long int) chunkoff, rq->path, (long long int) rq->offset);

			/*
			 * We should not receive more data than we requested, or
			 * pg_read_binary_file() messed up.  We could receive less,
			 * though, if the file was truncated in the source after we
			 * checked its size. That's OK, there should be a WAL record of
			 * the truncation, which will get replayed when you start the
			 * target system for the first time after pg_rewind has completed.
			 */
			if (chunksize > rq->length)
				pg_fatal("received more than requested for file \"%s\"", rq->path);

			open_target_file(filename, false);

			write_target_range(chunk, chunkoff, chunksize);
		}

		pg_free(filename);

		PQclear(res);
		chunkno++;
	}
	if (chunkno != src->num_requests)
		pg_fatal("unexpected number of data chunks received");

	src->num_requests = 0;
}

/*
 * Escape a string to be used as element in a text array constant
 */
static void
appendArrayEscapedString(StringInfo buf, const char *str)
{
	appendStringInfoCharMacro(buf, '\"');
	while (*str)
	{
		char		ch = *str;

		if (ch == '"' || ch == '\\')
			appendStringInfoCharMacro(buf, '\\');

		appendStringInfoCharMacro(buf, ch);

		str++;
	}
	appendStringInfoCharMacro(buf, '\"');
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
	libpq_source *src = (libpq_source *) source;

	pfree(src->paths.data);
	pfree(src->offsets.data);
	pfree(src->lengths.data);
	pfree(src);

	/* NOTE: we don't close the connection here, as it was not opened by us. */
}
