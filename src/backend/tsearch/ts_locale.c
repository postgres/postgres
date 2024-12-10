/*-------------------------------------------------------------------------
 *
 * ts_locale.c
 *		locale compatibility layer for tsearch
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/tsearch/ts_locale.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "common/string.h"
#include "storage/fd.h"
#include "tsearch/ts_locale.h"

static void tsearch_readline_callback(void *arg);


/*
 * The reason these functions use a 3-wchar_t output buffer, not 2 as you
 * might expect, is that on Windows "wchar_t" is 16 bits and what we'll be
 * getting from char2wchar() is UTF16 not UTF32.  A single input character
 * may therefore produce a surrogate pair rather than just one wchar_t;
 * we also need room for a trailing null.  When we do get a surrogate pair,
 * we pass just the first code to iswdigit() etc, so that these functions will
 * always return false for characters outside the Basic Multilingual Plane.
 */
#define WC_BUF_LEN  3

int
t_isdigit(const char *ptr)
{
	int			clen = pg_mblen(ptr);
	wchar_t		character[WC_BUF_LEN];
	pg_locale_t mylocale = 0;	/* TODO */

	if (clen == 1 || database_ctype_is_c)
		return isdigit(TOUCHAR(ptr));

	char2wchar(character, WC_BUF_LEN, ptr, clen, mylocale);

	return iswdigit((wint_t) character[0]);
}

int
t_isspace(const char *ptr)
{
	int			clen = pg_mblen(ptr);
	wchar_t		character[WC_BUF_LEN];
	pg_locale_t mylocale = 0;	/* TODO */

	if (clen == 1 || database_ctype_is_c)
		return isspace(TOUCHAR(ptr));

	char2wchar(character, WC_BUF_LEN, ptr, clen, mylocale);

	return iswspace((wint_t) character[0]);
}

int
t_isalpha(const char *ptr)
{
	int			clen = pg_mblen(ptr);
	wchar_t		character[WC_BUF_LEN];
	pg_locale_t mylocale = 0;	/* TODO */

	if (clen == 1 || database_ctype_is_c)
		return isalpha(TOUCHAR(ptr));

	char2wchar(character, WC_BUF_LEN, ptr, clen, mylocale);

	return iswalpha((wint_t) character[0]);
}

int
t_isalnum(const char *ptr)
{
	int			clen = pg_mblen(ptr);
	wchar_t		character[WC_BUF_LEN];
	pg_locale_t mylocale = 0;	/* TODO */

	if (clen == 1 || database_ctype_is_c)
		return isalnum(TOUCHAR(ptr));

	char2wchar(character, WC_BUF_LEN, ptr, clen, mylocale);

	return iswalnum((wint_t) character[0]);
}

int
t_isprint(const char *ptr)
{
	int			clen = pg_mblen(ptr);
	wchar_t		character[WC_BUF_LEN];
	pg_locale_t mylocale = 0;	/* TODO */

	if (clen == 1 || database_ctype_is_c)
		return isprint(TOUCHAR(ptr));

	char2wchar(character, WC_BUF_LEN, ptr, clen, mylocale);

	return iswprint((wint_t) character[0]);
}


/*
 * Set up to read a file using tsearch_readline().  This facility is
 * better than just reading the file directly because it provides error
 * context pointing to the specific line where a problem is detected.
 *
 * Expected usage is:
 *
 *		tsearch_readline_state trst;
 *
 *		if (!tsearch_readline_begin(&trst, filename))
 *			ereport(ERROR,
 *					(errcode(ERRCODE_CONFIG_FILE_ERROR),
 *					 errmsg("could not open stop-word file \"%s\": %m",
 *							filename)));
 *		while ((line = tsearch_readline(&trst)) != NULL)
 *			process line;
 *		tsearch_readline_end(&trst);
 *
 * Note that the caller supplies the ereport() for file open failure;
 * this is so that a custom message can be provided.  The filename string
 * passed to tsearch_readline_begin() must remain valid through
 * tsearch_readline_end().
 */
bool
tsearch_readline_begin(tsearch_readline_state *stp,
					   const char *filename)
{
	if ((stp->fp = AllocateFile(filename, "r")) == NULL)
		return false;
	stp->filename = filename;
	stp->lineno = 0;
	initStringInfo(&stp->buf);
	stp->curline = NULL;
	/* Setup error traceback support for ereport() */
	stp->cb.callback = tsearch_readline_callback;
	stp->cb.arg = stp;
	stp->cb.previous = error_context_stack;
	error_context_stack = &stp->cb;
	return true;
}

/*
 * Read the next line from a tsearch data file (expected to be in UTF-8), and
 * convert it to database encoding if needed. The returned string is palloc'd.
 * NULL return means EOF.
 */
char *
tsearch_readline(tsearch_readline_state *stp)
{
	char	   *recoded;

	/* Advance line number to use in error reports */
	stp->lineno++;

	/* Clear curline, it's no longer relevant */
	if (stp->curline)
	{
		if (stp->curline != stp->buf.data)
			pfree(stp->curline);
		stp->curline = NULL;
	}

	/* Collect next line, if there is one */
	if (!pg_get_line_buf(stp->fp, &stp->buf))
		return NULL;

	/* Validate the input as UTF-8, then convert to DB encoding if needed */
	recoded = pg_any_to_server(stp->buf.data, stp->buf.len, PG_UTF8);

	/* Save the correctly-encoded string for possible error reports */
	stp->curline = recoded;		/* might be equal to buf.data */

	/*
	 * We always return a freshly pstrdup'd string.  This is clearly necessary
	 * if pg_any_to_server() returned buf.data, and we need a second copy even
	 * if encoding conversion did occur.  The caller is entitled to pfree the
	 * returned string at any time, which would leave curline pointing to
	 * recycled storage, causing problems if an error occurs after that point.
	 * (It's preferable to return the result of pstrdup instead of the output
	 * of pg_any_to_server, because the conversion result tends to be
	 * over-allocated.  Since callers might save the result string directly
	 * into a long-lived dictionary structure, we don't want it to be a larger
	 * palloc chunk than necessary.  We'll reclaim the conversion result on
	 * the next call.)
	 */
	return pstrdup(recoded);
}

/*
 * Close down after reading a file with tsearch_readline()
 */
void
tsearch_readline_end(tsearch_readline_state *stp)
{
	/* Suppress use of curline in any error reported below */
	if (stp->curline)
	{
		if (stp->curline != stp->buf.data)
			pfree(stp->curline);
		stp->curline = NULL;
	}

	/* Release other resources */
	pfree(stp->buf.data);
	FreeFile(stp->fp);

	/* Pop the error context stack */
	error_context_stack = stp->cb.previous;
}

/*
 * Error context callback for errors occurring while reading a tsearch
 * configuration file.
 */
static void
tsearch_readline_callback(void *arg)
{
	tsearch_readline_state *stp = (tsearch_readline_state *) arg;

	/*
	 * We can't include the text of the config line for errors that occur
	 * during tsearch_readline() itself.  The major cause of such errors is
	 * encoding violations, and we daren't try to print error messages
	 * containing badly-encoded data.
	 */
	if (stp->curline)
		errcontext("line %d of configuration file \"%s\": \"%s\"",
				   stp->lineno,
				   stp->filename,
				   stp->curline);
	else
		errcontext("line %d of configuration file \"%s\"",
				   stp->lineno,
				   stp->filename);
}


/*
 * lowerstr --- fold null-terminated string to lower case
 *
 * Returned string is palloc'd
 */
char *
lowerstr(const char *str)
{
	return lowerstr_with_len(str, strlen(str));
}

/*
 * lowerstr_with_len --- fold string to lower case
 *
 * Input string need not be null-terminated.
 *
 * Returned string is palloc'd
 */
char *
lowerstr_with_len(const char *str, int len)
{
	char	   *out;
	pg_locale_t mylocale = 0;	/* TODO */

	if (len == 0)
		return pstrdup("");

	/*
	 * Use wide char code only when max encoding length > 1 and ctype != C.
	 * Some operating systems fail with multi-byte encodings and a C locale.
	 * Also, for a C locale there is no need to process as multibyte. From
	 * backend/utils/adt/oracle_compat.c Teodor
	 */
	if (pg_database_encoding_max_length() > 1 && !database_ctype_is_c)
	{
		wchar_t    *wstr,
				   *wptr;
		int			wlen;

		/*
		 * alloc number of wchar_t for worst case, len contains number of
		 * bytes >= number of characters and alloc 1 wchar_t for 0, because
		 * wchar2char wants zero-terminated string
		 */
		wptr = wstr = (wchar_t *) palloc(sizeof(wchar_t) * (len + 1));

		wlen = char2wchar(wstr, len + 1, str, len, mylocale);
		Assert(wlen <= len);

		while (*wptr)
		{
			*wptr = towlower((wint_t) *wptr);
			wptr++;
		}

		/*
		 * Alloc result string for worst case + '\0'
		 */
		len = pg_database_encoding_max_length() * wlen + 1;
		out = (char *) palloc(len);

		wlen = wchar2char(out, wstr, len, mylocale);

		pfree(wstr);

		if (wlen < 0)
			ereport(ERROR,
					(errcode(ERRCODE_CHARACTER_NOT_IN_REPERTOIRE),
					 errmsg("conversion from wchar_t to server encoding failed: %m")));
		Assert(wlen < len);
	}
	else
	{
		const char *ptr = str;
		char	   *outptr;

		outptr = out = (char *) palloc(sizeof(char) * (len + 1));
		while ((ptr - str) < len && *ptr)
		{
			*outptr++ = tolower(TOUCHAR(ptr));
			ptr++;
		}
		*outptr = '\0';
	}

	return out;
}
