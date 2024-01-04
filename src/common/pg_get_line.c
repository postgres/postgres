/*-------------------------------------------------------------------------
 *
 * pg_get_line.c
 *	  fgets() with an expansible result buffer
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
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

#include <setjmp.h>

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
 *
 * Also note that the palloc'd buffer is usually a lot longer than
 * strictly necessary, so it may be inadvisable to use this function
 * to collect lots of long-lived data.  A less memory-hungry option
 * is to use pg_get_line_buf() or pg_get_line_append() in a loop,
 * then pstrdup() each line.
 *
 * prompt_ctx can optionally be provided to allow this function to be
 * canceled via an existing SIGINT signal handler that will longjmp to the
 * specified place only when *(prompt_ctx->enabled) is true.  If canceled,
 * this function returns NULL, and prompt_ctx->canceled is set to true.
 */
char *
pg_get_line(FILE *stream, PromptInterruptContext *prompt_ctx)
{
	StringInfoData buf;

	initStringInfo(&buf);

	if (!pg_get_line_append(stream, &buf, prompt_ctx))
	{
		/* ensure that free() doesn't mess up errno */
		int			save_errno = errno;

		pfree(buf.data);
		errno = save_errno;
		return NULL;
	}

	return buf.data;
}

/*
 * pg_get_line_buf()
 *
 * This has similar behavior to pg_get_line(), and thence to fgets(),
 * except that the collected data is returned in a caller-supplied
 * StringInfo buffer.  This is a convenient API for code that just
 * wants to read and process one line at a time, without any artificial
 * limit on line length.
 *
 * Returns true if a line was successfully collected (including the
 * case of a non-newline-terminated line at EOF).  Returns false if
 * there was an I/O error or no data was available before EOF.
 * (Check ferror(stream) to distinguish these cases.)
 *
 * In the false-result case, buf is reset to empty.
 */
bool
pg_get_line_buf(FILE *stream, StringInfo buf)
{
	/* We just need to drop any data from the previous call */
	resetStringInfo(buf);
	return pg_get_line_append(stream, buf, NULL);
}

/*
 * pg_get_line_append()
 *
 * This has similar behavior to pg_get_line(), and thence to fgets(),
 * except that the collected data is appended to whatever is in *buf.
 * This is useful in preference to pg_get_line_buf() if the caller wants
 * to merge some lines together, e.g. to implement backslash continuation.
 *
 * Returns true if a line was successfully collected (including the
 * case of a non-newline-terminated line at EOF).  Returns false if
 * there was an I/O error or no data was available before EOF.
 * (Check ferror(stream) to distinguish these cases.)
 *
 * In the false-result case, the contents of *buf are logically unmodified,
 * though it's possible that the buffer has been resized.
 *
 * prompt_ctx can optionally be provided to allow this function to be
 * canceled via an existing SIGINT signal handler that will longjmp to the
 * specified place only when *(prompt_ctx->enabled) is true.  If canceled,
 * this function returns false, and prompt_ctx->canceled is set to true.
 */
bool
pg_get_line_append(FILE *stream, StringInfo buf,
				   PromptInterruptContext *prompt_ctx)
{
	int			orig_len = buf->len;

	if (prompt_ctx && sigsetjmp(*((sigjmp_buf *) prompt_ctx->jmpbuf), 1) != 0)
	{
		/* Got here with longjmp */
		prompt_ctx->canceled = true;
		/* Discard any data we collected before detecting error */
		buf->len = orig_len;
		buf->data[orig_len] = '\0';
		return false;
	}

	/* Loop until newline or EOF/error */
	for (;;)
	{
		char	   *res;

		/* Enable longjmp while waiting for input */
		if (prompt_ctx)
			*(prompt_ctx->enabled) = true;

		/* Read some data, appending it to whatever we already have */
		res = fgets(buf->data + buf->len, buf->maxlen - buf->len, stream);

		/* Disable longjmp again, then break if fgets failed */
		if (prompt_ctx)
			*(prompt_ctx->enabled) = false;

		if (res == NULL)
			break;

		/* Got data, so update buf->len */
		buf->len += strlen(buf->data + buf->len);

		/* Done if we have collected a newline */
		if (buf->len > orig_len && buf->data[buf->len - 1] == '\n')
			return true;

		/* Make some more room in the buffer, and loop to read more data */
		enlargeStringInfo(buf, 128);
	}

	/* Check for I/O errors and EOF */
	if (ferror(stream) || buf->len == orig_len)
	{
		/* Discard any data we collected before detecting error */
		buf->len = orig_len;
		buf->data[orig_len] = '\0';
		return false;
	}

	/* No newline at EOF, but we did collect some data */
	return true;
}
