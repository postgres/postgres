/*-------------------------------------------------------------------------
 *
 * receivelog.c - receive WAL files using the streaming
 *				  replication protocol.
 *
 * Author: Magnus Hagander <magnus@hagander.net>
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  src/bin/pg_basebackup/receivelog.c
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <sys/stat.h>
#include <unistd.h>
#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif

#include "access/xlog_internal.h"
#include "common/file_utils.h"
#include "common/logging.h"
#include "libpq-fe.h"
#include "receivelog.h"
#include "streamutil.h"

/* fd and filename for currently open WAL file */
static Walfile *walfile = NULL;
static char current_walfile_name[MAXPGPATH] = "";
static bool reportFlushPosition = false;
static XLogRecPtr lastFlushPosition = InvalidXLogRecPtr;

static bool still_sending = true;	/* feedback still needs to be sent? */

static PGresult *HandleCopyStream(PGconn *conn, StreamCtl *stream,
								  XLogRecPtr *stoppos);
static int	CopyStreamPoll(PGconn *conn, long timeout_ms, pgsocket stop_socket);
static int	CopyStreamReceive(PGconn *conn, long timeout, pgsocket stop_socket,
							  char **buffer);
static bool ProcessKeepaliveMsg(PGconn *conn, StreamCtl *stream, char *copybuf,
								int len, XLogRecPtr blockpos, TimestampTz *last_status);
static bool ProcessXLogDataMsg(PGconn *conn, StreamCtl *stream, char *copybuf, int len,
							   XLogRecPtr *blockpos);
static PGresult *HandleEndOfCopyStream(PGconn *conn, StreamCtl *stream, char *copybuf,
									   XLogRecPtr blockpos, XLogRecPtr *stoppos);
static bool CheckCopyStreamStop(PGconn *conn, StreamCtl *stream, XLogRecPtr blockpos,
								XLogRecPtr *stoppos);
static long CalculateCopyStreamSleeptime(TimestampTz now, int standby_message_timeout,
										 TimestampTz last_status);

static bool ReadEndOfStreamingResult(PGresult *res, XLogRecPtr *startpos,
									 uint32 *timeline);

static bool
mark_file_as_archived(StreamCtl *stream, const char *fname)
{
	Walfile    *f;
	static char tmppath[MAXPGPATH];

	snprintf(tmppath, sizeof(tmppath), "archive_status/%s.done",
			 fname);

	f = stream->walmethod->open_for_write(tmppath, NULL, 0);
	if (f == NULL)
	{
		pg_log_error("could not create archive status file \"%s\": %s",
					 tmppath, stream->walmethod->getlasterror());
		return false;
	}

	stream->walmethod->close(f, CLOSE_NORMAL);

	return true;
}

/*
 * Open a new WAL file in the specified directory.
 *
 * Returns true if OK; on failure, returns false after printing an error msg.
 * On success, 'walfile' is set to the FD for the file, and the base filename
 * (without partial_suffix) is stored in 'current_walfile_name'.
 *
 * The file will be padded to 16Mb with zeroes.
 */
static bool
open_walfile(StreamCtl *stream, XLogRecPtr startpoint)
{
	Walfile    *f;
	char		fn[MAXPGPATH];
	ssize_t		size;
	XLogSegNo	segno;

	XLByteToSeg(startpoint, segno, WalSegSz);
	XLogFileName(current_walfile_name, stream->timeline, segno, WalSegSz);

	snprintf(fn, sizeof(fn), "%s%s", current_walfile_name,
			 stream->partial_suffix ? stream->partial_suffix : "");

	/*
	 * When streaming to files, if an existing file exists we verify that it's
	 * either empty (just created), or a complete WalSegSz segment (in which
	 * case it has been created and padded). Anything else indicates a corrupt
	 * file.
	 *
	 * When streaming to tar, no file with this name will exist before, so we
	 * never have to verify a size.
	 */
	if (stream->walmethod->existsfile(fn))
	{
		size = stream->walmethod->get_file_size(fn);
		if (size < 0)
		{
			pg_log_error("could not get size of write-ahead log file \"%s\": %s",
						 fn, stream->walmethod->getlasterror());
			return false;
		}
		if (size == WalSegSz)
		{
			/* Already padded file. Open it for use */
			f = stream->walmethod->open_for_write(current_walfile_name, stream->partial_suffix, 0);
			if (f == NULL)
			{
				pg_log_error("could not open existing write-ahead log file \"%s\": %s",
							 fn, stream->walmethod->getlasterror());
				return false;
			}

			/* fsync file in case of a previous crash */
			if (stream->walmethod->sync(f) != 0)
			{
				pg_log_fatal("could not fsync existing write-ahead log file \"%s\": %s",
							 fn, stream->walmethod->getlasterror());
				stream->walmethod->close(f, CLOSE_UNLINK);
				exit(1);
			}

			walfile = f;
			return true;
		}
		if (size != 0)
		{
			/* if write didn't set errno, assume problem is no disk space */
			if (errno == 0)
				errno = ENOSPC;
			pg_log_error(ngettext("write-ahead log file \"%s\" has %d byte, should be 0 or %d",
								  "write-ahead log file \"%s\" has %d bytes, should be 0 or %d",
								  size),
						 fn, (int) size, WalSegSz);
			return false;
		}
		/* File existed and was empty, so fall through and open */
	}

	/* No file existed, so create one */

	f = stream->walmethod->open_for_write(current_walfile_name,
										  stream->partial_suffix, WalSegSz);
	if (f == NULL)
	{
		pg_log_error("could not open write-ahead log file \"%s\": %s",
					 fn, stream->walmethod->getlasterror());
		return false;
	}

	walfile = f;
	return true;
}

/*
 * Close the current WAL file (if open), and rename it to the correct
 * filename if it's complete. On failure, prints an error message to stderr
 * and returns false, otherwise returns true.
 */
static bool
close_walfile(StreamCtl *stream, XLogRecPtr pos)
{
	off_t		currpos;
	int			r;

	if (walfile == NULL)
		return true;

	currpos = stream->walmethod->get_current_pos(walfile);
	if (currpos == -1)
	{
		pg_log_error("could not determine seek position in file \"%s\": %s",
					 current_walfile_name, stream->walmethod->getlasterror());
		stream->walmethod->close(walfile, CLOSE_UNLINK);
		walfile = NULL;

		return false;
	}

	if (stream->partial_suffix)
	{
		if (currpos == WalSegSz)
			r = stream->walmethod->close(walfile, CLOSE_NORMAL);
		else
		{
			pg_log_info("not renaming \"%s%s\", segment is not complete",
						current_walfile_name, stream->partial_suffix);
			r = stream->walmethod->close(walfile, CLOSE_NO_RENAME);
		}
	}
	else
		r = stream->walmethod->close(walfile, CLOSE_NORMAL);

	walfile = NULL;

	if (r != 0)
	{
		pg_log_error("could not close file \"%s\": %s",
					 current_walfile_name, stream->walmethod->getlasterror());
		return false;
	}

	/*
	 * Mark file as archived if requested by the caller - pg_basebackup needs
	 * to do so as files can otherwise get archived again after promotion of a
	 * new node. This is in line with walreceiver.c always doing a
	 * XLogArchiveForceDone() after a complete segment.
	 */
	if (currpos == WalSegSz && stream->mark_done)
	{
		/* writes error message if failed */
		if (!mark_file_as_archived(stream, current_walfile_name))
			return false;
	}

	lastFlushPosition = pos;
	return true;
}


/*
 * Check if a timeline history file exists.
 */
static bool
existsTimeLineHistoryFile(StreamCtl *stream)
{
	char		histfname[MAXFNAMELEN];

	/*
	 * Timeline 1 never has a history file. We treat that as if it existed,
	 * since we never need to stream it.
	 */
	if (stream->timeline == 1)
		return true;

	TLHistoryFileName(histfname, stream->timeline);

	return stream->walmethod->existsfile(histfname);
}

static bool
writeTimeLineHistoryFile(StreamCtl *stream, char *filename, char *content)
{
	int			size = strlen(content);
	char		histfname[MAXFNAMELEN];
	Walfile    *f;

	/*
	 * Check that the server's idea of how timeline history files should be
	 * named matches ours.
	 */
	TLHistoryFileName(histfname, stream->timeline);
	if (strcmp(histfname, filename) != 0)
	{
		pg_log_error("server reported unexpected history file name for timeline %u: %s",
					 stream->timeline, filename);
		return false;
	}

	f = stream->walmethod->open_for_write(histfname, ".tmp", 0);
	if (f == NULL)
	{
		pg_log_error("could not create timeline history file \"%s\": %s",
					 histfname, stream->walmethod->getlasterror());
		return false;
	}

	if ((int) stream->walmethod->write(f, content, size) != size)
	{
		pg_log_error("could not write timeline history file \"%s\": %s",
					 histfname, stream->walmethod->getlasterror());

		/*
		 * If we fail to make the file, delete it to release disk space
		 */
		stream->walmethod->close(f, CLOSE_UNLINK);

		return false;
	}

	if (stream->walmethod->close(f, CLOSE_NORMAL) != 0)
	{
		pg_log_error("could not close file \"%s\": %s",
					 histfname, stream->walmethod->getlasterror());
		return false;
	}

	/* Maintain archive_status, check close_walfile() for details. */
	if (stream->mark_done)
	{
		/* writes error message if failed */
		if (!mark_file_as_archived(stream, histfname))
			return false;
	}

	return true;
}

/*
 * Send a Standby Status Update message to server.
 */
static bool
sendFeedback(PGconn *conn, XLogRecPtr blockpos, TimestampTz now, bool replyRequested)
{
	char		replybuf[1 + 8 + 8 + 8 + 8 + 1];
	int			len = 0;

	replybuf[len] = 'r';
	len += 1;
	fe_sendint64(blockpos, &replybuf[len]); /* write */
	len += 8;
	if (reportFlushPosition)
		fe_sendint64(lastFlushPosition, &replybuf[len]);	/* flush */
	else
		fe_sendint64(InvalidXLogRecPtr, &replybuf[len]);	/* flush */
	len += 8;
	fe_sendint64(InvalidXLogRecPtr, &replybuf[len]);	/* apply */
	len += 8;
	fe_sendint64(now, &replybuf[len]);	/* sendTime */
	len += 8;
	replybuf[len] = replyRequested ? 1 : 0; /* replyRequested */
	len += 1;

	if (PQputCopyData(conn, replybuf, len) <= 0 || PQflush(conn))
	{
		pg_log_error("could not send feedback packet: %s",
					 PQerrorMessage(conn));
		return false;
	}

	return true;
}

/*
 * Check that the server version we're connected to is supported by
 * ReceiveXlogStream().
 *
 * If it's not, an error message is printed to stderr, and false is returned.
 */
bool
CheckServerVersionForStreaming(PGconn *conn)
{
	int			minServerMajor,
				maxServerMajor;
	int			serverMajor;

	/*
	 * The message format used in streaming replication changed in 9.3, so we
	 * cannot stream from older servers. And we don't support servers newer
	 * than the client; it might work, but we don't know, so err on the safe
	 * side.
	 */
	minServerMajor = 903;
	maxServerMajor = PG_VERSION_NUM / 100;
	serverMajor = PQserverVersion(conn) / 100;
	if (serverMajor < minServerMajor)
	{
		const char *serverver = PQparameterStatus(conn, "server_version");

		pg_log_error("incompatible server version %s; client does not support streaming from server versions older than %s",
					 serverver ? serverver : "'unknown'",
					 "9.3");
		return false;
	}
	else if (serverMajor > maxServerMajor)
	{
		const char *serverver = PQparameterStatus(conn, "server_version");

		pg_log_error("incompatible server version %s; client does not support streaming from server versions newer than %s",
					 serverver ? serverver : "'unknown'",
					 PG_VERSION);
		return false;
	}
	return true;
}

/*
 * Receive a log stream starting at the specified position.
 *
 * Individual parameters are passed through the StreamCtl structure.
 *
 * If sysidentifier is specified, validate that both the system
 * identifier and the timeline matches the specified ones
 * (by sending an extra IDENTIFY_SYSTEM command)
 *
 * All received segments will be written to the directory
 * specified by basedir. This will also fetch any missing timeline history
 * files.
 *
 * The stream_stop callback will be called every time data
 * is received, and whenever a segment is completed. If it returns
 * true, the streaming will stop and the function
 * return. As long as it returns false, streaming will continue
 * indefinitely.
 *
 * If stream_stop() checks for external input, stop_socket should be set to
 * the FD it checks.  This will allow such input to be detected promptly
 * rather than after standby_message_timeout (which might be indefinite).
 * Note that signals will interrupt waits for input as well, but that is
 * race-y since a signal received while busy won't interrupt the wait.
 *
 * standby_message_timeout controls how often we send a message
 * back to the master letting it know our progress, in milliseconds.
 * Zero means no messages are sent.
 * This message will only contain the write location, and never
 * flush or replay.
 *
 * If 'partial_suffix' is not NULL, files are initially created with the
 * given suffix, and the suffix is removed once the file is finished. That
 * allows you to tell the difference between partial and completed files,
 * so that you can continue later where you left.
 *
 * If 'synchronous' is true, the received WAL is flushed as soon as written,
 * otherwise only when the WAL file is closed.
 *
 * Note: The WAL location *must* be at a log segment start!
 */
bool
ReceiveXlogStream(PGconn *conn, StreamCtl *stream)
{
	char		query[128];
	char		slotcmd[128];
	PGresult   *res;
	XLogRecPtr	stoppos;

	/*
	 * The caller should've checked the server version already, but doesn't do
	 * any harm to check it here too.
	 */
	if (!CheckServerVersionForStreaming(conn))
		return false;

	/*
	 * Decide whether we want to report the flush position. If we report the
	 * flush position, the primary will know what WAL we'll possibly
	 * re-request, and it can then remove older WAL safely. We must always do
	 * that when we are using slots.
	 *
	 * Reporting the flush position makes one eligible as a synchronous
	 * replica. People shouldn't include generic names in
	 * synchronous_standby_names, but we've protected them against it so far,
	 * so let's continue to do so unless specifically requested.
	 */
	if (stream->replication_slot != NULL)
	{
		reportFlushPosition = true;
		sprintf(slotcmd, "SLOT \"%s\" ", stream->replication_slot);
	}
	else
	{
		if (stream->synchronous)
			reportFlushPosition = true;
		else
			reportFlushPosition = false;
		slotcmd[0] = 0;
	}

	if (stream->sysidentifier != NULL)
	{
		/* Validate system identifier hasn't changed */
		res = PQexec(conn, "IDENTIFY_SYSTEM");
		if (PQresultStatus(res) != PGRES_TUPLES_OK)
		{
			pg_log_error("could not send replication command \"%s\": %s",
						 "IDENTIFY_SYSTEM", PQerrorMessage(conn));
			PQclear(res);
			return false;
		}
		if (PQntuples(res) != 1 || PQnfields(res) < 3)
		{
			pg_log_error("could not identify system: got %d rows and %d fields, expected %d rows and %d or more fields",
						 PQntuples(res), PQnfields(res), 1, 3);
			PQclear(res);
			return false;
		}
		if (strcmp(stream->sysidentifier, PQgetvalue(res, 0, 0)) != 0)
		{
			pg_log_error("system identifier does not match between base backup and streaming connection");
			PQclear(res);
			return false;
		}
		if (stream->timeline > atoi(PQgetvalue(res, 0, 1)))
		{
			pg_log_error("starting timeline %u is not present in the server",
						 stream->timeline);
			PQclear(res);
			return false;
		}
		PQclear(res);
	}

	/*
	 * initialize flush position to starting point, it's the caller's
	 * responsibility that that's sane.
	 */
	lastFlushPosition = stream->startpos;

	while (1)
	{
		/*
		 * Fetch the timeline history file for this timeline, if we don't have
		 * it already. When streaming log to tar, this will always return
		 * false, as we are never streaming into an existing file and
		 * therefore there can be no pre-existing timeline history file.
		 */
		if (!existsTimeLineHistoryFile(stream))
		{
			snprintf(query, sizeof(query), "TIMELINE_HISTORY %u", stream->timeline);
			res = PQexec(conn, query);
			if (PQresultStatus(res) != PGRES_TUPLES_OK)
			{
				/* FIXME: we might send it ok, but get an error */
				pg_log_error("could not send replication command \"%s\": %s",
							 "TIMELINE_HISTORY", PQresultErrorMessage(res));
				PQclear(res);
				return false;
			}

			/*
			 * The response to TIMELINE_HISTORY is a single row result set
			 * with two fields: filename and content
			 */
			if (PQnfields(res) != 2 || PQntuples(res) != 1)
			{
				pg_log_warning("unexpected response to TIMELINE_HISTORY command: got %d rows and %d fields, expected %d rows and %d fields",
							   PQntuples(res), PQnfields(res), 1, 2);
			}

			/* Write the history file to disk */
			writeTimeLineHistoryFile(stream,
									 PQgetvalue(res, 0, 0),
									 PQgetvalue(res, 0, 1));

			PQclear(res);
		}

		/*
		 * Before we start streaming from the requested location, check if the
		 * callback tells us to stop here.
		 */
		if (stream->stream_stop(stream->startpos, stream->timeline, false))
			return true;

		/* Initiate the replication stream at specified location */
		snprintf(query, sizeof(query), "START_REPLICATION %s%X/%X TIMELINE %u",
				 slotcmd,
				 (uint32) (stream->startpos >> 32), (uint32) stream->startpos,
				 stream->timeline);
		res = PQexec(conn, query);
		if (PQresultStatus(res) != PGRES_COPY_BOTH)
		{
			pg_log_error("could not send replication command \"%s\": %s",
						 "START_REPLICATION", PQresultErrorMessage(res));
			PQclear(res);
			return false;
		}
		PQclear(res);

		/* Stream the WAL */
		res = HandleCopyStream(conn, stream, &stoppos);
		if (res == NULL)
			goto error;

		/*
		 * Streaming finished.
		 *
		 * There are two possible reasons for that: a controlled shutdown, or
		 * we reached the end of the current timeline. In case of
		 * end-of-timeline, the server sends a result set after Copy has
		 * finished, containing information about the next timeline. Read
		 * that, and restart streaming from the next timeline. In case of
		 * controlled shutdown, stop here.
		 */
		if (PQresultStatus(res) == PGRES_TUPLES_OK)
		{
			/*
			 * End-of-timeline. Read the next timeline's ID and starting
			 * position. Usually, the starting position will match the end of
			 * the previous timeline, but there are corner cases like if the
			 * server had sent us half of a WAL record, when it was promoted.
			 * The new timeline will begin at the end of the last complete
			 * record in that case, overlapping the partial WAL record on the
			 * old timeline.
			 */
			uint32		newtimeline;
			bool		parsed;

			parsed = ReadEndOfStreamingResult(res, &stream->startpos, &newtimeline);
			PQclear(res);
			if (!parsed)
				goto error;

			/* Sanity check the values the server gave us */
			if (newtimeline <= stream->timeline)
			{
				pg_log_error("server reported unexpected next timeline %u, following timeline %u",
							 newtimeline, stream->timeline);
				goto error;
			}
			if (stream->startpos > stoppos)
			{
				pg_log_error("server stopped streaming timeline %u at %X/%X, but reported next timeline %u to begin at %X/%X",
							 stream->timeline, (uint32) (stoppos >> 32), (uint32) stoppos,
							 newtimeline, (uint32) (stream->startpos >> 32), (uint32) stream->startpos);
				goto error;
			}

			/* Read the final result, which should be CommandComplete. */
			res = PQgetResult(conn);
			if (PQresultStatus(res) != PGRES_COMMAND_OK)
			{
				pg_log_error("unexpected termination of replication stream: %s",
							 PQresultErrorMessage(res));
				PQclear(res);
				goto error;
			}
			PQclear(res);

			/*
			 * Loop back to start streaming from the new timeline. Always
			 * start streaming at the beginning of a segment.
			 */
			stream->timeline = newtimeline;
			stream->startpos = stream->startpos -
				XLogSegmentOffset(stream->startpos, WalSegSz);
			continue;
		}
		else if (PQresultStatus(res) == PGRES_COMMAND_OK)
		{
			PQclear(res);

			/*
			 * End of replication (ie. controlled shut down of the server).
			 *
			 * Check if the callback thinks it's OK to stop here. If not,
			 * complain.
			 */
			if (stream->stream_stop(stoppos, stream->timeline, false))
				return true;
			else
			{
				pg_log_error("replication stream was terminated before stop point");
				goto error;
			}
		}
		else
		{
			/* Server returned an error. */
			pg_log_error("unexpected termination of replication stream: %s",
						 PQresultErrorMessage(res));
			PQclear(res);
			goto error;
		}
	}

error:
	if (walfile != NULL && stream->walmethod->close(walfile, CLOSE_NO_RENAME) != 0)
		pg_log_error("could not close file \"%s\": %s",
					 current_walfile_name, stream->walmethod->getlasterror());
	walfile = NULL;
	return false;
}

/*
 * Helper function to parse the result set returned by server after streaming
 * has finished. On failure, prints an error to stderr and returns false.
 */
static bool
ReadEndOfStreamingResult(PGresult *res, XLogRecPtr *startpos, uint32 *timeline)
{
	uint32		startpos_xlogid,
				startpos_xrecoff;

	/*----------
	 * The result set consists of one row and two columns, e.g:
	 *
	 *	next_tli | next_tli_startpos
	 * ----------+-------------------
	 *		   4 | 0/9949AE0
	 *
	 * next_tli is the timeline ID of the next timeline after the one that
	 * just finished streaming. next_tli_startpos is the WAL location where
	 * the server switched to it.
	 *----------
	 */
	if (PQnfields(res) < 2 || PQntuples(res) != 1)
	{
		pg_log_error("unexpected result set after end-of-timeline: got %d rows and %d fields, expected %d rows and %d fields",
					 PQntuples(res), PQnfields(res), 1, 2);
		return false;
	}

	*timeline = atoi(PQgetvalue(res, 0, 0));
	if (sscanf(PQgetvalue(res, 0, 1), "%X/%X", &startpos_xlogid,
			   &startpos_xrecoff) != 2)
	{
		pg_log_error("could not parse next timeline's starting point \"%s\"",
					 PQgetvalue(res, 0, 1));
		return false;
	}
	*startpos = ((uint64) startpos_xlogid << 32) | startpos_xrecoff;

	return true;
}

/*
 * The main loop of ReceiveXlogStream. Handles the COPY stream after
 * initiating streaming with the START_REPLICATION command.
 *
 * If the COPY ends (not necessarily successfully) due a message from the
 * server, returns a PGresult and sets *stoppos to the last byte written.
 * On any other sort of error, returns NULL.
 */
static PGresult *
HandleCopyStream(PGconn *conn, StreamCtl *stream,
				 XLogRecPtr *stoppos)
{
	char	   *copybuf = NULL;
	TimestampTz last_status = -1;
	XLogRecPtr	blockpos = stream->startpos;

	still_sending = true;

	while (1)
	{
		int			r;
		TimestampTz now;
		long		sleeptime;

		/*
		 * Check if we should continue streaming, or abort at this point.
		 */
		if (!CheckCopyStreamStop(conn, stream, blockpos, stoppos))
			goto error;

		now = feGetCurrentTimestamp();

		/*
		 * If synchronous option is true, issue sync command as soon as there
		 * are WAL data which has not been flushed yet.
		 */
		if (stream->synchronous && lastFlushPosition < blockpos && walfile != NULL)
		{
			if (stream->walmethod->sync(walfile) != 0)
			{
				pg_log_fatal("could not fsync file \"%s\": %s",
							 current_walfile_name, stream->walmethod->getlasterror());
				exit(1);
			}
			lastFlushPosition = blockpos;

			/*
			 * Send feedback so that the server sees the latest WAL locations
			 * immediately.
			 */
			if (!sendFeedback(conn, blockpos, now, false))
				goto error;
			last_status = now;
		}

		/*
		 * Potentially send a status message to the master
		 */
		if (still_sending && stream->standby_message_timeout > 0 &&
			feTimestampDifferenceExceeds(last_status, now,
										 stream->standby_message_timeout))
		{
			/* Time to send feedback! */
			if (!sendFeedback(conn, blockpos, now, false))
				goto error;
			last_status = now;
		}

		/*
		 * Calculate how long send/receive loops should sleep
		 */
		sleeptime = CalculateCopyStreamSleeptime(now, stream->standby_message_timeout,
												 last_status);

		r = CopyStreamReceive(conn, sleeptime, stream->stop_socket, &copybuf);
		while (r != 0)
		{
			if (r == -1)
				goto error;
			if (r == -2)
			{
				PGresult   *res = HandleEndOfCopyStream(conn, stream, copybuf, blockpos, stoppos);

				if (res == NULL)
					goto error;
				else
					return res;
			}

			/* Check the message type. */
			if (copybuf[0] == 'k')
			{
				if (!ProcessKeepaliveMsg(conn, stream, copybuf, r, blockpos,
										 &last_status))
					goto error;
			}
			else if (copybuf[0] == 'w')
			{
				if (!ProcessXLogDataMsg(conn, stream, copybuf, r, &blockpos))
					goto error;

				/*
				 * Check if we should continue streaming, or abort at this
				 * point.
				 */
				if (!CheckCopyStreamStop(conn, stream, blockpos, stoppos))
					goto error;
			}
			else
			{
				pg_log_error("unrecognized streaming header: \"%c\"",
							 copybuf[0]);
				goto error;
			}

			/*
			 * Process the received data, and any subsequent data we can read
			 * without blocking.
			 */
			r = CopyStreamReceive(conn, 0, stream->stop_socket, &copybuf);
		}
	}

error:
	if (copybuf != NULL)
		PQfreemem(copybuf);
	return NULL;
}

/*
 * Wait until we can read a CopyData message,
 * or timeout, or occurrence of a signal or input on the stop_socket.
 * (timeout_ms < 0 means wait indefinitely; 0 means don't wait.)
 *
 * Returns 1 if data has become available for reading, 0 if timed out
 * or interrupted by signal or stop_socket input, and -1 on an error.
 */
static int
CopyStreamPoll(PGconn *conn, long timeout_ms, pgsocket stop_socket)
{
	int			ret;
	fd_set		input_mask;
	int			connsocket;
	int			maxfd;
	struct timeval timeout;
	struct timeval *timeoutptr;

	connsocket = PQsocket(conn);
	if (connsocket < 0)
	{
		pg_log_error("invalid socket: %s", PQerrorMessage(conn));
		return -1;
	}

	FD_ZERO(&input_mask);
	FD_SET(connsocket, &input_mask);
	maxfd = connsocket;
	if (stop_socket != PGINVALID_SOCKET)
	{
		FD_SET(stop_socket, &input_mask);
		maxfd = Max(maxfd, stop_socket);
	}

	if (timeout_ms < 0)
		timeoutptr = NULL;
	else
	{
		timeout.tv_sec = timeout_ms / 1000L;
		timeout.tv_usec = (timeout_ms % 1000L) * 1000L;
		timeoutptr = &timeout;
	}

	ret = select(maxfd + 1, &input_mask, NULL, NULL, timeoutptr);

	if (ret < 0)
	{
		if (errno == EINTR)
			return 0;			/* Got a signal, so not an error */
		pg_log_error("select() failed: %m");
		return -1;
	}
	if (ret > 0 && FD_ISSET(connsocket, &input_mask))
		return 1;				/* Got input on connection socket */

	return 0;					/* Got timeout or input on stop_socket */
}

/*
 * Receive CopyData message available from XLOG stream, blocking for
 * maximum of 'timeout' ms.
 *
 * If data was received, returns the length of the data. *buffer is set to
 * point to a buffer holding the received message. The buffer is only valid
 * until the next CopyStreamReceive call.
 *
 * Returns 0 if no data was available within timeout, or if wait was
 * interrupted by signal or stop_socket input.
 * -1 on error. -2 if the server ended the COPY.
 */
static int
CopyStreamReceive(PGconn *conn, long timeout, pgsocket stop_socket,
				  char **buffer)
{
	char	   *copybuf = NULL;
	int			rawlen;

	if (*buffer != NULL)
		PQfreemem(*buffer);
	*buffer = NULL;

	/* Try to receive a CopyData message */
	rawlen = PQgetCopyData(conn, &copybuf, 1);
	if (rawlen == 0)
	{
		int			ret;

		/*
		 * No data available.  Wait for some to appear, but not longer than
		 * the specified timeout, so that we can ping the server.  Also stop
		 * waiting if input appears on stop_socket.
		 */
		ret = CopyStreamPoll(conn, timeout, stop_socket);
		if (ret <= 0)
			return ret;

		/* Now there is actually data on the socket */
		if (PQconsumeInput(conn) == 0)
		{
			pg_log_error("could not receive data from WAL stream: %s",
						 PQerrorMessage(conn));
			return -1;
		}

		/* Now that we've consumed some input, try again */
		rawlen = PQgetCopyData(conn, &copybuf, 1);
		if (rawlen == 0)
			return 0;
	}
	if (rawlen == -1)			/* end-of-streaming or error */
		return -2;
	if (rawlen == -2)
	{
		pg_log_error("could not read COPY data: %s", PQerrorMessage(conn));
		return -1;
	}

	/* Return received messages to caller */
	*buffer = copybuf;
	return rawlen;
}

/*
 * Process the keepalive message.
 */
static bool
ProcessKeepaliveMsg(PGconn *conn, StreamCtl *stream, char *copybuf, int len,
					XLogRecPtr blockpos, TimestampTz *last_status)
{
	int			pos;
	bool		replyRequested;
	TimestampTz now;

	/*
	 * Parse the keepalive message, enclosed in the CopyData message. We just
	 * check if the server requested a reply, and ignore the rest.
	 */
	pos = 1;					/* skip msgtype 'k' */
	pos += 8;					/* skip walEnd */
	pos += 8;					/* skip sendTime */

	if (len < pos + 1)
	{
		pg_log_error("streaming header too small: %d", len);
		return false;
	}
	replyRequested = copybuf[pos];

	/* If the server requested an immediate reply, send one. */
	if (replyRequested && still_sending)
	{
		if (reportFlushPosition && lastFlushPosition < blockpos &&
			walfile != NULL)
		{
			/*
			 * If a valid flush location needs to be reported, flush the
			 * current WAL file so that the latest flush location is sent back
			 * to the server. This is necessary to see whether the last WAL
			 * data has been successfully replicated or not, at the normal
			 * shutdown of the server.
			 */
			if (stream->walmethod->sync(walfile) != 0)
			{
				pg_log_fatal("could not fsync file \"%s\": %s",
							 current_walfile_name, stream->walmethod->getlasterror());
				exit(1);
			}
			lastFlushPosition = blockpos;
		}

		now = feGetCurrentTimestamp();
		if (!sendFeedback(conn, blockpos, now, false))
			return false;
		*last_status = now;
	}

	return true;
}

/*
 * Process XLogData message.
 */
static bool
ProcessXLogDataMsg(PGconn *conn, StreamCtl *stream, char *copybuf, int len,
				   XLogRecPtr *blockpos)
{
	int			xlogoff;
	int			bytes_left;
	int			bytes_written;
	int			hdr_len;

	/*
	 * Once we've decided we don't want to receive any more, just ignore any
	 * subsequent XLogData messages.
	 */
	if (!(still_sending))
		return true;

	/*
	 * Read the header of the XLogData message, enclosed in the CopyData
	 * message. We only need the WAL location field (dataStart), the rest of
	 * the header is ignored.
	 */
	hdr_len = 1;				/* msgtype 'w' */
	hdr_len += 8;				/* dataStart */
	hdr_len += 8;				/* walEnd */
	hdr_len += 8;				/* sendTime */
	if (len < hdr_len)
	{
		pg_log_error("streaming header too small: %d", len);
		return false;
	}
	*blockpos = fe_recvint64(&copybuf[1]);

	/* Extract WAL location for this block */
	xlogoff = XLogSegmentOffset(*blockpos, WalSegSz);

	/*
	 * Verify that the initial location in the stream matches where we think
	 * we are.
	 */
	if (walfile == NULL)
	{
		/* No file open yet */
		if (xlogoff != 0)
		{
			pg_log_error("received write-ahead log record for offset %u with no file open",
						 xlogoff);
			return false;
		}
	}
	else
	{
		/* More data in existing segment */
		if (stream->walmethod->get_current_pos(walfile) != xlogoff)
		{
			pg_log_error("got WAL data offset %08x, expected %08x",
						 xlogoff, (int) stream->walmethod->get_current_pos(walfile));
			return false;
		}
	}

	bytes_left = len - hdr_len;
	bytes_written = 0;

	while (bytes_left)
	{
		int			bytes_to_write;

		/*
		 * If crossing a WAL boundary, only write up until we reach wal
		 * segment size.
		 */
		if (xlogoff + bytes_left > WalSegSz)
			bytes_to_write = WalSegSz - xlogoff;
		else
			bytes_to_write = bytes_left;

		if (walfile == NULL)
		{
			if (!open_walfile(stream, *blockpos))
			{
				/* Error logged by open_walfile */
				return false;
			}
		}

		if (stream->walmethod->write(walfile, copybuf + hdr_len + bytes_written,
									 bytes_to_write) != bytes_to_write)
		{
			pg_log_error("could not write %u bytes to WAL file \"%s\": %s",
						 bytes_to_write, current_walfile_name,
						 stream->walmethod->getlasterror());
			return false;
		}

		/* Write was successful, advance our position */
		bytes_written += bytes_to_write;
		bytes_left -= bytes_to_write;
		*blockpos += bytes_to_write;
		xlogoff += bytes_to_write;

		/* Did we reach the end of a WAL segment? */
		if (XLogSegmentOffset(*blockpos, WalSegSz) == 0)
		{
			if (!close_walfile(stream, *blockpos))
				/* Error message written in close_walfile() */
				return false;

			xlogoff = 0;

			if (still_sending && stream->stream_stop(*blockpos, stream->timeline, true))
			{
				if (PQputCopyEnd(conn, NULL) <= 0 || PQflush(conn))
				{
					pg_log_error("could not send copy-end packet: %s",
								 PQerrorMessage(conn));
					return false;
				}
				still_sending = false;
				return true;	/* ignore the rest of this XLogData packet */
			}
		}
	}
	/* No more data left to write, receive next copy packet */

	return true;
}

/*
 * Handle end of the copy stream.
 */
static PGresult *
HandleEndOfCopyStream(PGconn *conn, StreamCtl *stream, char *copybuf,
					  XLogRecPtr blockpos, XLogRecPtr *stoppos)
{
	PGresult   *res = PQgetResult(conn);

	/*
	 * The server closed its end of the copy stream.  If we haven't closed
	 * ours already, we need to do so now, unless the server threw an error,
	 * in which case we don't.
	 */
	if (still_sending)
	{
		if (!close_walfile(stream, blockpos))
		{
			/* Error message written in close_walfile() */
			PQclear(res);
			return NULL;
		}
		if (PQresultStatus(res) == PGRES_COPY_IN)
		{
			if (PQputCopyEnd(conn, NULL) <= 0 || PQflush(conn))
			{
				pg_log_error("could not send copy-end packet: %s",
							 PQerrorMessage(conn));
				PQclear(res);
				return NULL;
			}
			res = PQgetResult(conn);
		}
		still_sending = false;
	}
	if (copybuf != NULL)
		PQfreemem(copybuf);
	*stoppos = blockpos;
	return res;
}

/*
 * Check if we should continue streaming, or abort at this point.
 */
static bool
CheckCopyStreamStop(PGconn *conn, StreamCtl *stream, XLogRecPtr blockpos,
					XLogRecPtr *stoppos)
{
	if (still_sending && stream->stream_stop(blockpos, stream->timeline, false))
	{
		if (!close_walfile(stream, blockpos))
		{
			/* Potential error message is written by close_walfile */
			return false;
		}
		if (PQputCopyEnd(conn, NULL) <= 0 || PQflush(conn))
		{
			pg_log_error("could not send copy-end packet: %s",
						 PQerrorMessage(conn));
			return false;
		}
		still_sending = false;
	}

	return true;
}

/*
 * Calculate how long send/receive loops should sleep
 */
static long
CalculateCopyStreamSleeptime(TimestampTz now, int standby_message_timeout,
							 TimestampTz last_status)
{
	TimestampTz status_targettime = 0;
	long		sleeptime;

	if (standby_message_timeout && still_sending)
		status_targettime = last_status +
			(standby_message_timeout - 1) * ((int64) 1000);

	if (status_targettime > 0)
	{
		long		secs;
		int			usecs;

		feTimestampDifference(now,
							  status_targettime,
							  &secs,
							  &usecs);
		/* Always sleep at least 1 sec */
		if (secs <= 0)
		{
			secs = 1;
			usecs = 0;
		}

		sleeptime = secs * 1000 + usecs / 1000;
	}
	else
		sleeptime = -1;

	return sleeptime;
}
