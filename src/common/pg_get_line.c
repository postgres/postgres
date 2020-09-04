/*-------------------------------------------------------------------------
 *
 * pg_get_line.c
 *	  fgets() with an expansible result buffer
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/common/pg_get_line.c
 *
 *-------------------------------------------------------------------------
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "common/string.h"
#include "lib/stringinfo.h"


/*
 * pg_get_line()
 *
 * This is meant to be equivalent to fgets(), except that instead of
 * reading into a caller-supplied, fixed-size buffer, it reads into
 * a palloc'd (in frontend, really malloc'd) string, which is resized
 * as needed to handle indefinitely long input lines.  The caller is
 * responsible for pfree'ing the result string when appropriate.
 *
 * As with fgets(), returns NULL if there is a read error or if no
 * characters are available before EOF.  The caller can distinguish
 * these cases by checking ferror(stream).
 *
 * Since this is meant to be equivalent to fgets(), the trailing newline
 * (if any) is not stripped.  Callers may wish to apply pg_strip_crlf().
 *
 * Note that while I/O errors are reflected back to the caller to be
 * dealt with, an OOM condition for the palloc'd buffer will not be;
 * there'll be an ereport(ERROR) or exit(1) inside stringinfo.c.
 */
char *
pg_get_line(FILE *stream)
{
	StringInfoData buf;

	initStringInfo(&buf);

	/* Read some data, appending it to whatever we already have */
	while (fgets(buf.data + buf.len, buf.maxlen - buf.len, stream) != NULL)
	{
		buf.len += strlen(buf.data + buf.len);

		/* Done if we have collected a newline */
		if (buf.len > 0 && buf.data[buf.len - 1] == '\n')
			return buf.data;

		/* Make some more room in the buffer, and loop to read more data */
		enlargeStringInfo(&buf, 128);
	}

	/* Did fgets() fail because of an I/O error? */
	if (ferror(stream))
	{
		/* ensure that free() doesn't mess up errno */
		int			save_errno = errno;

		pfree(buf.data);
		errno = save_errno;
		return NULL;
	}

	/* If we read no data before reaching EOF, we should return NULL */
	if (buf.len == 0)
	{
		pfree(buf.data);
		return NULL;
	}

	/* No newline at EOF ... so return what we have */
	return buf.data;
}
