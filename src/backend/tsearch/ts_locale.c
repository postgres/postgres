/*-------------------------------------------------------------------------
 *
 * ts_locale.c
 *		locale compatibility layer for tsearch
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/tsearch/ts_locale.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_collation.h"
#include "storage/fd.h"
#include "tsearch/ts_locale.h"
#include "tsearch/ts_public.h"

static void tsearch_readline_callback(void *arg);


#ifdef USE_WIDE_UPPER_LOWER

int
t_isdigit(const char *ptr)
{
	int			clen = pg_mblen(ptr);
	wchar_t		character[2];
	Oid			collation = DEFAULT_COLLATION_OID;		/* TODO */
	pg_locale_t mylocale = 0;	/* TODO */

	if (clen == 1 || lc_ctype_is_c(collation))
		return isdigit(TOUCHAR(ptr));

	char2wchar(character, 2, ptr, clen, mylocale);

	return iswdigit((wint_t) character[0]);
}

int
t_isspace(const char *ptr)
{
	int			clen = pg_mblen(ptr);
	wchar_t		character[2];
	Oid			collation = DEFAULT_COLLATION_OID;		/* TODO */
	pg_locale_t mylocale = 0;	/* TODO */

	if (clen == 1 || lc_ctype_is_c(collation))
		return isspace(TOUCHAR(ptr));

	char2wchar(character, 2, ptr, clen, mylocale);

	return iswspace((wint_t) character[0]);
}

int
t_isalpha(const char *ptr)
{
	int			clen = pg_mblen(ptr);
	wchar_t		character[2];
	Oid			collation = DEFAULT_COLLATION_OID;		/* TODO */
	pg_locale_t mylocale = 0;	/* TODO */

	if (clen == 1 || lc_ctype_is_c(collation))
		return isalpha(TOUCHAR(ptr));

	char2wchar(character, 2, ptr, clen, mylocale);

	return iswalpha((wint_t) character[0]);
}

int
t_isprint(const char *ptr)
{
	int			clen = pg_mblen(ptr);
	wchar_t		character[2];
	Oid			collation = DEFAULT_COLLATION_OID;		/* TODO */
	pg_locale_t mylocale = 0;	/* TODO */

	if (clen == 1 || lc_ctype_is_c(collation))
		return isprint(TOUCHAR(ptr));

	char2wchar(character, 2, ptr, clen, mylocale);

	return iswprint((wint_t) character[0]);
}
#endif   /* USE_WIDE_UPPER_LOWER */


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
	stp->curline = NULL;
	/* Setup error traceback support for ereport() */
	stp->cb.callback = tsearch_readline_callback;
	stp->cb.arg = (void *) stp;
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
	char	   *result;

	stp->lineno++;
	stp->curline = NULL;
	result = t_readline(stp->fp);
	stp->curline = result;
	return result;
}

/*
 * Close down after reading a file with tsearch_readline()
 */
void
tsearch_readline_end(tsearch_readline_state *stp)
{
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
	 * during t_readline() itself.  This is only partly a consequence of our
	 * arms-length use of that routine: the major cause of such errors is
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
 * Read the next line from a tsearch data file (expected to be in UTF-8), and
 * convert it to database encoding if needed. The returned string is palloc'd.
 * NULL return means EOF.
 *
 * Note: direct use of this function is now deprecated.  Go through
 * tsearch_readline() to provide better error reporting.
 */
char *
t_readline(FILE *fp)
{
	int			len;
	char	   *recoded;
	char		buf[4096];		/* lines must not be longer than this */

	if (fgets(buf, sizeof(buf), fp) == NULL)
		return NULL;

	len = strlen(buf);

	/* Make sure the input is valid UTF-8 */
	(void) pg_verify_mbstr(PG_UTF8, buf, len, false);

	/* And convert */
	recoded = pg_any_to_server(buf, len, PG_UTF8);
	if (recoded == buf)
	{
		/*
		 * conversion didn't pstrdup, so we must. We can use the length of the
		 * original string, because no conversion was done.
		 */
		recoded = pnstrdup(recoded, len);
	}

	return recoded;
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

#ifdef USE_WIDE_UPPER_LOWER
	Oid			collation = DEFAULT_COLLATION_OID;		/* TODO */
	pg_locale_t mylocale = 0;	/* TODO */
#endif

	if (len == 0)
		return pstrdup("");

#ifdef USE_WIDE_UPPER_LOWER

	/*
	 * Use wide char code only when max encoding length > 1 and ctype != C.
	 * Some operating systems fail with multi-byte encodings and a C locale.
	 * Also, for a C locale there is no need to process as multibyte. From
	 * backend/utils/adt/oracle_compat.c Teodor
	 */
	if (pg_database_encoding_max_length() > 1 && !lc_ctype_is_c(collation))
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
#endif   /* USE_WIDE_UPPER_LOWER */
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
