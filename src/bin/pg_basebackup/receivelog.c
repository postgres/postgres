/*-------------------------------------------------------------------------
 *
 * receivelog.c - receive transaction log files using the streaming
 *				  replication protocol.
 *
 * Author: Magnus Hagander <magnus@hagander.net>
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  src/bin/pg_basebackup/receivelog.c
 *-------------------------------------------------------------------------
 */

/*
 * We have to use postgres.h not postgres_fe.h here, because there's so much
 * backend-only stuff in the XLOG include files we need.  But we need a
 * frontend-ish environment otherwise.  Hence this ugly hack.
 */
#define FRONTEND 1
#include "postgres.h"
#include "libpq-fe.h"
#include "access/xlog_internal.h"
#include "replication/walprotocol.h"
#include "utils/datetime.h"
#include "utils/timestamp.h"

#include "receivelog.h"
#include "streamutil.h"

#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>


/* Size of the streaming replication protocol headers */
#define STREAMING_HEADER_SIZE (1+sizeof(WalDataMessageHeader))
#define STREAMING_KEEPALIVE_SIZE (1+sizeof(PrimaryKeepaliveMessage))

const XLogRecPtr InvalidXLogRecPtr = {0, 0};

/* fd for currently open WAL file */
static int	walfile = -1;


/*
 * Open a new WAL file in the specified directory. Store the name
 * (not including the full directory) in namebuf. Assumes there is
 * enough room in this buffer...
 *
 * The file will be padded to 16Mb with zeroes.
 */
static int
open_walfile(XLogRecPtr startpoint, uint32 timeline, char *basedir,
			 char *namebuf)
{
	int			f;
	char		fn[MAXPGPATH];
	struct stat statbuf;
	char	   *zerobuf;
	int			bytes;

	XLogFileName(namebuf, timeline, startpoint.xlogid,
				 startpoint.xrecoff / XLOG_SEG_SIZE);

	snprintf(fn, sizeof(fn), "%s/%s.partial", basedir, namebuf);
	f = open(fn, O_WRONLY | O_CREAT | PG_BINARY, S_IRUSR | S_IWUSR);
	if (f == -1)
	{
		fprintf(stderr,
				_("%s: could not open transaction log file \"%s\": %s\n"),
				progname, fn, strerror(errno));
		return -1;
	}

	/*
	 * Verify that the file is either empty (just created), or a complete
	 * XLogSegSize segment. Anything in between indicates a corrupt file.
	 */
	if (fstat(f, &statbuf) != 0)
	{
		fprintf(stderr,
				_("%s: could not stat transaction log file \"%s\": %s\n"),
				progname, fn, strerror(errno));
		close(f);
		return -1;
	}
	if (statbuf.st_size == XLogSegSize)
		return f;				/* File is open and ready to use */
	if (statbuf.st_size != 0)
	{
		fprintf(stderr,
				_("%s: transaction log file \"%s\" has %d bytes, should be 0 or %d\n"),
				progname, fn, (int) statbuf.st_size, XLogSegSize);
		close(f);
		return -1;
	}

	/* New, empty, file. So pad it to 16Mb with zeroes */
	zerobuf = xmalloc0(XLOG_BLCKSZ);
	for (bytes = 0; bytes < XLogSegSize; bytes += XLOG_BLCKSZ)
	{
		if (write(f, zerobuf, XLOG_BLCKSZ) != XLOG_BLCKSZ)
		{
			fprintf(stderr,
					_("%s: could not pad transaction log file \"%s\": %s\n"),
					progname, fn, strerror(errno));
			free(zerobuf);
			close(f);
			unlink(fn);
			return -1;
		}
	}
	free(zerobuf);

	if (lseek(f, SEEK_SET, 0) != 0)
	{
		fprintf(stderr,
				_("%s: could not seek to beginning of transaction log file \"%s\": %s\n"),
				progname, fn, strerror(errno));
		close(f);
		return -1;
	}
	return f;
}

/*
 * Close the current WAL file, and rename it to the correct filename if it's
 * complete.
 *
 * If segment_complete is true, rename the current WAL file even if we've not
 * completed writing the whole segment.
 */
static bool
close_walfile(char *basedir, char *walname, bool segment_complete)
{
	off_t		currpos = lseek(walfile, 0, SEEK_CUR);

	if (currpos == -1)
	{
		fprintf(stderr,
			 _("%s: could not determine seek position in file \"%s\": %s\n"),
				progname, walname, strerror(errno));
		return false;
	}

	if (fsync(walfile) != 0)
	{
		fprintf(stderr, _("%s: could not fsync file \"%s\": %s\n"),
				progname, walname, strerror(errno));
		return false;
	}

	if (close(walfile) != 0)
	{
		fprintf(stderr, _("%s: could not close file \"%s\": %s\n"),
				progname, walname, strerror(errno));
		walfile = -1;
		return false;
	}
	walfile = -1;

	/*
	 * Rename the .partial file only if we've completed writing the whole
	 * segment or segment_complete is true.
	 */
	if (currpos == XLOG_SEG_SIZE || segment_complete)
	{
		char		oldfn[MAXPGPATH];
		char		newfn[MAXPGPATH];

		snprintf(oldfn, sizeof(oldfn), "%s/%s.partial", basedir, walname);
		snprintf(newfn, sizeof(newfn), "%s/%s", basedir, walname);
		if (rename(oldfn, newfn) != 0)
		{
			fprintf(stderr, _("%s: could not rename file \"%s\": %s\n"),
					progname, walname, strerror(errno));
			return false;
		}
	}
	else
		fprintf(stderr,
				_("%s: not renaming \"%s\", segment is not complete\n"),
				progname, walname);

	return true;
}


/*
 * Local version of GetCurrentTimestamp(), since we are not linked with
 * backend code.
 */
static TimestampTz
localGetCurrentTimestamp(void)
{
	TimestampTz result;
	struct timeval tp;

	gettimeofday(&tp, NULL);

	result = (TimestampTz) tp.tv_sec -
		((POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * SECS_PER_DAY);

#ifdef HAVE_INT64_TIMESTAMP
	result = (result * USECS_PER_SEC) + tp.tv_usec;
#else
	result = result + (tp.tv_usec / 1000000.0);
#endif

	return result;
}

/*
 * Local version of TimestampDifference(), since we are not
 * linked with backend code.
 */
static void
localTimestampDifference(TimestampTz start_time, TimestampTz stop_time,
						 long *secs, int *microsecs)
{
	TimestampTz diff = stop_time - start_time;

	if (diff <= 0)
	{
		*secs = 0;
		*microsecs = 0;
	}
	else
	{
#ifdef HAVE_INT64_TIMESTAMP
		*secs = (long) (diff / USECS_PER_SEC);
		*microsecs = (int) (diff % USECS_PER_SEC);
#else
		*secs = (long) diff;
		*microsecs = (int) ((diff - *secs) * 1000000.0);
#endif
	}
}

/*
 * Local version of TimestampDifferenceExceeds(), since we are not
 * linked with backend code.
 */
static bool
localTimestampDifferenceExceeds(TimestampTz start_time,
								TimestampTz stop_time,
								int msec)
{
	TimestampTz diff = stop_time - start_time;

#ifdef HAVE_INT64_TIMESTAMP
	return (diff >= msec * INT64CONST(1000));
#else
	return (diff * 1000.0 >= msec);
#endif
}

/*
 * Receive a log stream starting at the specified position.
 *
 * If sysidentifier is specified, validate that both the system
 * identifier and the timeline matches the specified ones
 * (by sending an extra IDENTIFY_SYSTEM command)
 *
 * All received segments will be written to the directory
 * specified by basedir.
 *
 * The stream_stop callback will be called every time data
 * is received, and whenever a segment is completed. If it returns
 * true, the streaming will stop and the function
 * return. As long as it returns false, streaming will continue
 * indefinitely.
 *
 * standby_message_timeout controls how often we send a message
 * back to the master letting it know our progress, in seconds.
 * This message will only contain the write location, and never
 * flush or replay.
 *
 * Note: The log position *must* be at a log segment start!
 */
bool
ReceiveXlogStream(PGconn *conn, XLogRecPtr startpos, uint32 timeline,
				  char *sysidentifier, char *basedir,
				  stream_stop_callback stream_stop,
				  int standby_message_timeout, bool rename_partial)
{
	char		query[128];
	char		current_walfile_name[MAXPGPATH];
	PGresult   *res;
	char	   *copybuf = NULL;
	int64		last_status = -1;
	XLogRecPtr	blockpos = InvalidXLogRecPtr;

	if (sysidentifier != NULL)
	{
		/* Validate system identifier and timeline hasn't changed */
		res = PQexec(conn, "IDENTIFY_SYSTEM");
		if (PQresultStatus(res) != PGRES_TUPLES_OK)
		{
			fprintf(stderr,
					_("%s: could not send replication command \"%s\": %s"),
					progname, "IDENTIFY_SYSTEM", PQerrorMessage(conn));
			PQclear(res);
			return false;
		}
		if (PQnfields(res) != 3 || PQntuples(res) != 1)
		{
			fprintf(stderr,
					_("%s: could not identify system: got %d rows and %d fields, expected %d rows and %d fields\n"),
					progname, PQntuples(res), PQnfields(res), 1, 3);
			PQclear(res);
			return false;
		}
		if (strcmp(sysidentifier, PQgetvalue(res, 0, 0)) != 0)
		{
			fprintf(stderr,
					_("%s: system identifier does not match between base backup and streaming connection\n"),
					progname);
			PQclear(res);
			return false;
		}
		if (timeline != atoi(PQgetvalue(res, 0, 1)))
		{
			fprintf(stderr,
					_("%s: timeline does not match between base backup and streaming connection\n"),
					progname);
			PQclear(res);
			return false;
		}
		PQclear(res);
	}

	/* Initiate the replication stream at specified location */
	snprintf(query, sizeof(query), "START_REPLICATION %X/%X", startpos.xlogid, startpos.xrecoff);
	res = PQexec(conn, query);
	if (PQresultStatus(res) != PGRES_COPY_BOTH)
	{
		fprintf(stderr, _("%s: could not send replication command \"%s\": %s"),
				progname, "START_REPLICATION", PQresultErrorMessage(res));
		PQclear(res);
		return false;
	}
	PQclear(res);

	/*
	 * Receive the actual xlog data
	 */
	while (1)
	{
		int			r;
		int			xlogoff;
		int			bytes_left;
		int			bytes_written;
		int64		now;

		if (copybuf != NULL)
		{
			PQfreemem(copybuf);
			copybuf = NULL;
		}

		/*
		 * Check if we should continue streaming, or abort at this point.
		 */
		if (stream_stop && stream_stop(blockpos, timeline, false))
		{
			if (walfile != -1 && !close_walfile(basedir, current_walfile_name,
												rename_partial))
				/* Potential error message is written by close_walfile */
				goto error;
			return true;
		}

		/*
		 * Potentially send a status message to the master
		 */
		now = localGetCurrentTimestamp();
		if (standby_message_timeout > 0 &&
			localTimestampDifferenceExceeds(last_status, now,
											standby_message_timeout))
		{
			/* Time to send feedback! */
			char		replybuf[sizeof(StandbyReplyMessage) + 1];
			StandbyReplyMessage *replymsg;

			replymsg = (StandbyReplyMessage *) (replybuf + 1);
			replymsg->write = blockpos;
			replymsg->flush = InvalidXLogRecPtr;
			replymsg->apply = InvalidXLogRecPtr;
			replymsg->sendTime = now;
			replybuf[0] = 'r';

			if (PQputCopyData(conn, replybuf, sizeof(replybuf)) <= 0 ||
				PQflush(conn))
			{
				fprintf(stderr, _("%s: could not send feedback packet: %s"),
						progname, PQerrorMessage(conn));
				goto error;
			}

			last_status = now;
		}

		r = PQgetCopyData(conn, &copybuf, 1);
		if (r == 0)
		{
			/*
			 * In async mode, and no data available. We block on reading but
			 * not more than the specified timeout, so that we can send a
			 * response back to the client.
			 */
			fd_set		input_mask;
			struct timeval timeout;
			struct timeval *timeoutptr;

			FD_ZERO(&input_mask);
			FD_SET(PQsocket(conn), &input_mask);
			if (standby_message_timeout)
			{
				TimestampTz targettime;
				long		secs;
				int			usecs;

				targettime = TimestampTzPlusMilliseconds(last_status,
												standby_message_timeout - 1);
				localTimestampDifference(now,
										 targettime,
										 &secs,
										 &usecs);
				if (secs <= 0)
					timeout.tv_sec = 1; /* Always sleep at least 1 sec */
				else
					timeout.tv_sec = secs;
				timeout.tv_usec = usecs;
				timeoutptr = &timeout;
			}
			else
				timeoutptr = NULL;

			r = select(PQsocket(conn) + 1, &input_mask, NULL, NULL, timeoutptr);
			if (r == 0 || (r < 0 && errno == EINTR))
			{
				/*
				 * Got a timeout or signal. Continue the loop and either
				 * deliver a status packet to the server or just go back into
				 * blocking.
				 */
				continue;
			}
			else if (r < 0)
			{
				fprintf(stderr, _("%s: select() failed: %s\n"),
						progname, strerror(errno));
				goto error;
			}
			/* Else there is actually data on the socket */
			if (PQconsumeInput(conn) == 0)
			{
				fprintf(stderr,
						_("%s: could not receive data from WAL stream: %s"),
						progname, PQerrorMessage(conn));
				goto error;
			}
			continue;
		}
		if (r == -1)
			/* End of copy stream */
			break;
		if (r == -2)
		{
			fprintf(stderr, _("%s: could not read COPY data: %s"),
					progname, PQerrorMessage(conn));
			goto error;
		}
		if (copybuf[0] == 'k')
		{
			/*
			 * keepalive message, sent in 9.2 and newer. We just ignore this
			 * message completely, but need to skip past it in the stream.
			 */
			if (r != STREAMING_KEEPALIVE_SIZE)
			{
				fprintf(stderr,
						_("%s: keepalive message has incorrect size %d\n"),
						progname, r);
				goto error;
			}
			continue;
		}
		else if (copybuf[0] != 'w')
		{
			fprintf(stderr, _("%s: unrecognized streaming header: \"%c\"\n"),
					progname, copybuf[0]);
			goto error;
		}
		if (r < STREAMING_HEADER_SIZE)
		{
			fprintf(stderr, _("%s: streaming header too small: %d\n"),
					progname, r);
			goto error;
		}

		/* Extract WAL location for this block */
		memcpy(&blockpos, copybuf + 1, 8);
		xlogoff = blockpos.xrecoff % XLOG_SEG_SIZE;

		/*
		 * Verify that the initial location in the stream matches where we
		 * think we are.
		 */
		if (walfile == -1)
		{
			/* No file open yet */
			if (xlogoff != 0)
			{
				fprintf(stderr,
						_("%s: received transaction log record for offset %u with no file open\n"),
						progname, xlogoff);
				goto error;
			}
		}
		else
		{
			/* More data in existing segment */
			/* XXX: store seek value don't reseek all the time */
			if (lseek(walfile, 0, SEEK_CUR) != xlogoff)
			{
				fprintf(stderr,
						_("%s: got WAL data offset %08x, expected %08x\n"),
						progname, xlogoff, (int) lseek(walfile, 0, SEEK_CUR));
				goto error;
			}
		}

		bytes_left = r - STREAMING_HEADER_SIZE;
		bytes_written = 0;

		while (bytes_left)
		{
			int			bytes_to_write;

			/*
			 * If crossing a WAL boundary, only write up until we reach
			 * XLOG_SEG_SIZE.
			 */
			if (xlogoff + bytes_left > XLOG_SEG_SIZE)
				bytes_to_write = XLOG_SEG_SIZE - xlogoff;
			else
				bytes_to_write = bytes_left;

			if (walfile == -1)
			{
				walfile = open_walfile(blockpos, timeline,
									   basedir, current_walfile_name);
				if (walfile == -1)
					/* Error logged by open_walfile */
					goto error;
			}

			if (write(walfile,
					  copybuf + STREAMING_HEADER_SIZE + bytes_written,
					  bytes_to_write) != bytes_to_write)
			{
				fprintf(stderr,
				  _("%s: could not write %u bytes to WAL file \"%s\": %s\n"),
						progname, bytes_to_write, current_walfile_name,
						strerror(errno));
				goto error;
			}

			/* Write was successful, advance our position */
			bytes_written += bytes_to_write;
			bytes_left -= bytes_to_write;
			XLByteAdvance(blockpos, bytes_to_write);
			xlogoff += bytes_to_write;

			/* Did we reach the end of a WAL segment? */
			if (blockpos.xrecoff % XLOG_SEG_SIZE == 0)
			{
				if (!close_walfile(basedir, current_walfile_name, false))
					/* Error message written in close_walfile() */
					goto error;

				xlogoff = 0;

				if (stream_stop != NULL)
				{
					/*
					 * Callback when the segment finished, and return if it
					 * told us to.
					 */
					if (stream_stop(blockpos, timeline, true))
						return true;
				}
			}
		}
		/* No more data left to write, start receiving next copy packet */
	}

	/*
	 * The only way to get out of the loop is if the server shut down the
	 * replication stream. If it's a controlled shutdown, the server will send
	 * a shutdown message, and we'll return the latest xlog location that has
	 * been streamed.
	 */

	res = PQgetResult(conn);
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		fprintf(stderr,
				_("%s: unexpected termination of replication stream: %s"),
				progname, PQresultErrorMessage(res));
		goto error;
	}
	PQclear(res);

	/* Complain if we've not reached stop point yet */
	if (stream_stop != NULL && !stream_stop(blockpos, timeline, false))
	{
		fprintf(stderr, _("%s: replication stream was terminated before stop point\n"),
				progname);
		goto error;
	}

	if (copybuf != NULL)
		PQfreemem(copybuf);
	if (walfile != -1 && close(walfile) != 0)
		fprintf(stderr, _("%s: could not close file \"%s\": %s\n"),
				progname, current_walfile_name, strerror(errno));
	walfile = -1;
	return true;

error:
	if (copybuf != NULL)
		PQfreemem(copybuf);
	if (walfile != -1 && close(walfile) != 0)
		fprintf(stderr, _("%s: could not close file \"%s\": %s\n"),
				progname, current_walfile_name, strerror(errno));
	walfile = -1;
	return false;
}
