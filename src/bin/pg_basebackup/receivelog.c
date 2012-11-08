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
 * frontend-ish environment otherwise.	Hence this ugly hack.
 */
#define FRONTEND 1
#include "postgres.h"

#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
/* for ntohl/htonl */
#include <netinet/in.h>
#include <arpa/inet.h>

#include "libpq-fe.h"
#include "access/xlog_internal.h"
#include "utils/datetime.h"
#include "utils/timestamp.h"

#include "receivelog.h"
#include "streamutil.h"


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
	XLogSegNo	segno;

	XLByteToSeg(startpoint, segno);
	XLogFileName(namebuf, timeline, segno);

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
	zerobuf = pg_malloc0(XLOG_BLCKSZ);
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
 * backend code. The protocol always uses integer timestamps, regardless of
 * server setting.
 */
static int64
localGetCurrentTimestamp(void)
{
	int64 result;
	struct timeval tp;

	gettimeofday(&tp, NULL);

	result = (int64) tp.tv_sec -
		((POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * SECS_PER_DAY);

	result = (result * USECS_PER_SEC) + tp.tv_usec;

	return result;
}

/*
 * Local version of TimestampDifference(), since we are not linked with
 * backend code.
 */
static void
localTimestampDifference(int64 start_time, int64 stop_time,
						 long *secs, int *microsecs)
{
	int64 diff = stop_time - start_time;

	if (diff <= 0)
	{
		*secs = 0;
		*microsecs = 0;
	}
	else
	{
		*secs = (long) (diff / USECS_PER_SEC);
		*microsecs = (int) (diff % USECS_PER_SEC);
	}
}

/*
 * Local version of TimestampDifferenceExceeds(), since we are not
 * linked with backend code.
 */
static bool
localTimestampDifferenceExceeds(int64 start_time,
								int64 stop_time,
								int msec)
{
	int64 diff = stop_time - start_time;

	return (diff >= msec * INT64CONST(1000));
}

/*
 * Converts an int64 to network byte order.
 */
static void
sendint64(int64 i, char *buf)
{
	uint32		n32;

	/* High order half first, since we're doing MSB-first */
	n32 = (uint32) (i >> 32);
	n32 = htonl(n32);
	memcpy(&buf[0], &n32, 4);

	/* Now the low order half */
	n32 = (uint32) i;
	n32 = htonl(n32);
	memcpy(&buf[4], &n32, 4);
}

/*
 * Converts an int64 from network byte order to native format.
 */
static int64
recvint64(char *buf)
{
	int64		result;
	uint32		h32;
	uint32		l32;

	memcpy(&h32, buf, 4);
	memcpy(&l32, buf + 4, 4);
	h32 = ntohl(h32);
	l32 = ntohl(l32);

	result = h32;
	result <<= 32;
	result |= l32;

	return result;
}

/*
 * Send a Standby Status Update message to server.
 */
static bool
sendFeedback(PGconn *conn, XLogRecPtr blockpos, int64 now, bool replyRequested)
{
	char		replybuf[1 + 8 + 8 + 8 + 8 + 1];
	int 		len = 0;

	replybuf[len] = 'r';
	len += 1;
	sendint64(blockpos, &replybuf[len]);			/* write */
	len += 8;
	sendint64(InvalidXLogRecPtr, &replybuf[len]);	/* flush */
	len += 8;
	sendint64(InvalidXLogRecPtr, &replybuf[len]);	/* apply */
	len += 8;
	sendint64(now, &replybuf[len]);					/* sendTime */
	len += 8;
	replybuf[len] = replyRequested ? 1 : 0;			/* replyRequested */
	len += 1;

	if (PQputCopyData(conn, replybuf, len) <= 0 || PQflush(conn))
	{
		fprintf(stderr, _("%s: could not send feedback packet: %s"),
				progname, PQerrorMessage(conn));
		return false;
	}

	return true;
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
	snprintf(query, sizeof(query), "START_REPLICATION %X/%X",
			 (uint32) (startpos >> 32), (uint32) startpos);
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
		int			hdr_len;

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
			if (!sendFeedback(conn, blockpos, now, false))
				goto error;
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
				int64		targettime;
				long		secs;
				int			usecs;

				targettime = last_status + (standby_message_timeout - 1) * ((int64) 1000);
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

		/* Check the message type. */
		if (copybuf[0] == 'k')
		{
			int		pos;
			bool	replyRequested;

			/*
			 * Parse the keepalive message, enclosed in the CopyData message.
			 * We just check if the server requested a reply, and ignore the
			 * rest.
			 */
			pos = 1;	/* skip msgtype 'k' */
			pos += 8;	/* skip walEnd */
			pos += 8;	/* skip sendTime */

			if (r < pos + 1)
			{
				fprintf(stderr, _("%s: streaming header too small: %d\n"),
						progname, r);
				goto error;
			}
			replyRequested = copybuf[pos];

			/* If the server requested an immediate reply, send one. */
			if (replyRequested)
			{
				now = localGetCurrentTimestamp();
				if (!sendFeedback(conn, blockpos, now, false))
					goto error;
				last_status = now;
			}
			continue;
		}
		else if (copybuf[0] != 'w')
		{
			fprintf(stderr, _("%s: unrecognized streaming header: \"%c\"\n"),
					progname, copybuf[0]);
			goto error;
		}

		/*
		 * Read the header of the XLogData message, enclosed in the CopyData
		 * message. We only need the WAL location field (dataStart), the rest
		 * of the header is ignored.
		 */
		hdr_len = 1;	/* msgtype 'w' */
		hdr_len += 8;	/* dataStart */
		hdr_len += 8;	/* walEnd */
		hdr_len += 8;	/* sendTime */
		if (r < hdr_len + 1)
		{
			fprintf(stderr, _("%s: streaming header too small: %d\n"),
					progname, r);
			goto error;
		}
		blockpos = recvint64(&copybuf[1]);

		/* Extract WAL location for this block */
		xlogoff = blockpos % XLOG_SEG_SIZE;

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

		bytes_left = r - hdr_len;
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
					  copybuf + hdr_len + bytes_written,
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
			if (blockpos % XLOG_SEG_SIZE == 0)
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
