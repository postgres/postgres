/*-------------------------------------------------------------------------
 * oracle_compat.c
 *	Oracle compatible functions.
 *
 * Copyright (c) 1996-2006, PostgreSQL Global Development Group
 *
 *	Author: Edmund Mergl <E.Mergl@bawue.de>
 *	Multibyte enhancement: Tatsuo Ishii <ishii@postgresql.org>
 *
 *
 * IDENTIFICATION
 *	$PostgreSQL: pgsql/src/backend/utils/adt/oracle_compat.c,v 1.67.2.2 2007/09/22 05:35:52 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>
#include <limits.h>
/*
 * towlower() and friends should be in <wctype.h>, but some pre-C99 systems
 * declare them in <wchar.h>.
 */
#ifdef HAVE_WCHAR_H
#include <wchar.h>
#endif
#ifdef HAVE_WCTYPE_H
#include <wctype.h>
#endif

#include "utils/builtins.h"
#include "utils/pg_locale.h"
#include "mb/pg_wchar.h"


/*
 * If the system provides the needed functions for wide-character manipulation
 * (which are all standardized by C99), then we implement upper/lower/initcap
 * using wide-character functions.	Otherwise we use the traditional <ctype.h>
 * functions, which of course will not work as desired in multibyte character
 * sets.  Note that in either case we are effectively assuming that the
 * database character encoding matches the encoding implied by LC_CTYPE.
 *
 * We assume if we have these two functions, we have their friends too, and
 * can use the wide-character method.
 */
#if defined(HAVE_WCSTOMBS) && defined(HAVE_TOWLOWER)
#define USE_WIDE_UPPER_LOWER
char *wstring_lower (char *str);
char *wstring_upper(char *str);
#endif

static text *dotrim(const char *string, int stringlen,
	   const char *set, int setlen,
	   bool doltrim, bool dortrim);


#ifdef USE_WIDE_UPPER_LOWER

/*
 * Convert a TEXT value into a palloc'd wchar string.
 */
static wchar_t *
texttowcs(const text *txt)
{
	int			nbytes = VARSIZE(txt) - VARHDRSZ;
	char	   *workstr;
	wchar_t    *result;
	size_t		ncodes;

	/* Overflow paranoia */
	if (nbytes < 0 ||
		nbytes > (int) (INT_MAX / sizeof(wchar_t)) - 1)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));

	/* Need a null-terminated version of the input */
	workstr = (char *) palloc(nbytes + 1);
	memcpy(workstr, VARDATA(txt), nbytes);
	workstr[nbytes] = '\0';

	/* Output workspace cannot have more codes than input bytes */
	result = (wchar_t *) palloc((nbytes + 1) * sizeof(wchar_t));

	/* Do the conversion */
	ncodes = mbstowcs(result, workstr, nbytes + 1);

	if (ncodes == (size_t) -1)
	{
		/*
		 * Invalid multibyte character encountered.  We try to give a useful
		 * error message by letting pg_verifymbstr check the string.  But it's
		 * possible that the string is OK to us, and not OK to mbstowcs ---
		 * this suggests that the LC_CTYPE locale is different from the
		 * database encoding.  Give a generic error message if verifymbstr
		 * can't find anything wrong.
		 */
		pg_verifymbstr(workstr, nbytes, false);
		ereport(ERROR,
				(errcode(ERRCODE_CHARACTER_NOT_IN_REPERTOIRE),
				 errmsg("invalid multibyte character for locale"),
				 errhint("The server's LC_CTYPE locale is probably incompatible with the database encoding.")));
	}

	Assert(ncodes <= (size_t) nbytes);

	return result;
}


/*
 * Convert a wchar string into a palloc'd TEXT value.  The wchar string
 * must be zero-terminated, but we also require the caller to pass the string
 * length, since it will know it anyway in current uses.
 */
static text *
wcstotext(const wchar_t *str, int ncodes)
{
	text	   *result;
	size_t		nbytes;

	/* Overflow paranoia */
	if (ncodes < 0 ||
		ncodes > (int) ((INT_MAX - VARHDRSZ) / MB_CUR_MAX) - 1)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));

	/* Make workspace certainly large enough for result */
	result = (text *) palloc((ncodes + 1) * MB_CUR_MAX + VARHDRSZ);

	/* Do the conversion */
	nbytes = wcstombs((char *) VARDATA(result), str,
					  (ncodes + 1) * MB_CUR_MAX);

	if (nbytes == (size_t) -1)
	{
		/* Invalid multibyte character encountered ... shouldn't happen */
		ereport(ERROR,
				(errcode(ERRCODE_CHARACTER_NOT_IN_REPERTOIRE),
				 errmsg("invalid multibyte character for locale")));
	}

	Assert(nbytes <= (size_t) (ncodes * MB_CUR_MAX));

	VARATT_SIZEP(result) = nbytes + VARHDRSZ;

	return result;
}
#endif   /* USE_WIDE_UPPER_LOWER */


/*
 * On Windows, the "Unicode" locales assume UTF16 not UTF8 encoding.
 * To make use of the upper/lower functionality, we need to map UTF8 to
 * UTF16, which for some reason mbstowcs and wcstombs won't do for us.
 * This conversion layer takes care of it.
 */

#ifdef WIN32

/* texttowcs for the case of UTF8 to UTF16 */
static wchar_t *
win32_utf8_texttowcs(const text *txt)
{
	int			nbytes = VARSIZE(txt) - VARHDRSZ;
	wchar_t    *result;
	int			r;

	/* Overflow paranoia */
	if (nbytes < 0 ||
		nbytes > (int) (INT_MAX / sizeof(wchar_t)) - 1)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));

	/* Output workspace cannot have more codes than input bytes */
	result = (wchar_t *) palloc((nbytes + 1) * sizeof(wchar_t));

	/* stupid Microsloth API does not work for zero-length input */
	if (nbytes == 0)
		r = 0;
	else
	{
		/* Do the conversion */
		r = MultiByteToWideChar(CP_UTF8, 0, VARDATA(txt), nbytes,
								result, nbytes);

		if (!r)					/* assume it's NO_UNICODE_TRANSLATION */
		{
			/* see notes above about error reporting */
			pg_verifymbstr(VARDATA(txt), nbytes, false);
			ereport(ERROR,
					(errcode(ERRCODE_CHARACTER_NOT_IN_REPERTOIRE),
					 errmsg("invalid multibyte character for locale"),
					 errhint("The server's LC_CTYPE locale is probably incompatible with the database encoding.")));
		}
	}

	Assert(r <= nbytes);
	result[r] = 0;

	return result;
}

/* wcstotext for the case of UTF16 to UTF8 */
static text *
win32_utf8_wcstotext(const wchar_t *str)
{
	text	   *result;
	int			nbytes;
	int			r;

	nbytes = WideCharToMultiByte(CP_UTF8, 0, str, -1, NULL, 0, NULL, NULL);
	if (nbytes == 0)			/* shouldn't happen */
		ereport(ERROR,
				(errcode(ERRCODE_CHARACTER_NOT_IN_REPERTOIRE),
				 errmsg("UTF-16 to UTF-8 translation failed: %lu",
						GetLastError())));

	result = palloc(nbytes + VARHDRSZ);

	r = WideCharToMultiByte(CP_UTF8, 0, str, -1, VARDATA(result), nbytes,
							NULL, NULL);
	if (r == 0)					/* shouldn't happen */
		ereport(ERROR,
				(errcode(ERRCODE_CHARACTER_NOT_IN_REPERTOIRE),
				 errmsg("UTF-16 to UTF-8 translation failed: %lu",
						GetLastError())));

	VARATT_SIZEP(result) = nbytes + VARHDRSZ - 1;		/* -1 to ignore null */

	return result;
}

/* interface layer to check which encoding is in use */

static wchar_t *
win32_texttowcs(const text *txt)
{
	if (GetDatabaseEncoding() == PG_UTF8)
		return win32_utf8_texttowcs(txt);
	else
		return texttowcs(txt);
}

static text *
win32_wcstotext(const wchar_t *str, int ncodes)
{
	if (GetDatabaseEncoding() == PG_UTF8)
		return win32_utf8_wcstotext(str);
	else
		return wcstotext(str, ncodes);
}

/* use macros to cause routines below to call interface layer */

#define texttowcs	win32_texttowcs
#define wcstotext	win32_wcstotext
#endif   /* WIN32 */

#ifdef USE_WIDE_UPPER_LOWER
/* 
 * string_upper and string_lower are used for correct multibyte upper/lower 
 * transformations localized strings. Returns pointers to transformated
 * string.
 */
char *
wstring_upper(char *str)
{
	wchar_t		*workspace;
	text		*in_text;
	text		*out_text;
	char		*result;    
	int 	nbytes = strlen(str);
	int	i;
	
	in_text = palloc(nbytes + VARHDRSZ);
	memcpy(VARDATA(in_text), str, nbytes);
	VARATT_SIZEP(in_text) = nbytes + VARHDRSZ;

	workspace = texttowcs(in_text);

	for (i = 0; workspace[i] != 0; i++)
		workspace[i] = towupper(workspace[i]);

	out_text = wcstotext(workspace, i);
	
    	nbytes = VARSIZE(out_text) - VARHDRSZ;
	result = palloc(nbytes + 1);
	memcpy(result, VARDATA(out_text), nbytes);

	result[nbytes] = '\0';

	pfree(workspace);
	pfree(in_text);
	pfree(out_text);
	
	return result;
}

char *
wstring_lower(char *str)
{
	wchar_t		*workspace;
	text		*in_text;
	text		*out_text;
	char		*result;    
	int 	nbytes = strlen(str);
	int	i;
	
	in_text = palloc(nbytes + VARHDRSZ);
	memcpy(VARDATA(in_text), str, nbytes);
	VARATT_SIZEP(in_text) = nbytes + VARHDRSZ;

	workspace = texttowcs(in_text);

	for (i = 0; workspace[i] != 0; i++)
		workspace[i] = towlower(workspace[i]);

	out_text = wcstotext(workspace, i);
	
    	nbytes = VARSIZE(out_text) - VARHDRSZ;
	result = palloc(nbytes + 1);
	memcpy(result, VARDATA(out_text), nbytes);

	result[nbytes] = '\0';

	pfree(workspace);
	pfree(in_text);
	pfree(out_text);
	
	return result;
}
#endif	/* USE_WIDE_UPPER_LOWER */

/********************************************************************
 *
 * lower
 *
 * Syntax:
 *
 *	 text lower(text string)
 *
 * Purpose:
 *
 *	 Returns string, with all letters forced to lowercase.
 *
 ********************************************************************/

Datum
lower(PG_FUNCTION_ARGS)
{
#ifdef USE_WIDE_UPPER_LOWER

	/*
	 * Use wide char code only when max encoding length > 1 and ctype != C.
	 * Some operating systems fail with multi-byte encodings and a C locale.
	 * Also, for a C locale there is no need to process as multibyte.
	 */
	if (pg_database_encoding_max_length() > 1 && !lc_ctype_is_c())
	{
		text	   *string = PG_GETARG_TEXT_P(0);
		text	   *result;
		wchar_t    *workspace;
		int			i;

		workspace = texttowcs(string);

		for (i = 0; workspace[i] != 0; i++)
			workspace[i] = towlower(workspace[i]);

		result = wcstotext(workspace, i);

		pfree(workspace);

		PG_RETURN_TEXT_P(result);
	}
	else
#endif   /* USE_WIDE_UPPER_LOWER */
	{
		text	   *string = PG_GETARG_TEXT_P_COPY(0);
		char	   *ptr;
		int			m;

		/*
		 * Since we copied the string, we can scribble directly on the value
		 */
		ptr = VARDATA(string);
		m = VARSIZE(string) - VARHDRSZ;

		while (m-- > 0)
		{
			*ptr = tolower((unsigned char) *ptr);
			ptr++;
		}

		PG_RETURN_TEXT_P(string);
	}
}


/********************************************************************
 *
 * upper
 *
 * Syntax:
 *
 *	 text upper(text string)
 *
 * Purpose:
 *
 *	 Returns string, with all letters forced to uppercase.
 *
 ********************************************************************/

Datum
upper(PG_FUNCTION_ARGS)
{
#ifdef USE_WIDE_UPPER_LOWER

	/*
	 * Use wide char code only when max encoding length > 1 and ctype != C.
	 * Some operating systems fail with multi-byte encodings and a C locale.
	 * Also, for a C locale there is no need to process as multibyte.
	 */
	if (pg_database_encoding_max_length() > 1 && !lc_ctype_is_c())
	{
		text	   *string = PG_GETARG_TEXT_P(0);
		text	   *result;
		wchar_t    *workspace;
		int			i;

		workspace = texttowcs(string);

		for (i = 0; workspace[i] != 0; i++)
			workspace[i] = towupper(workspace[i]);

		result = wcstotext(workspace, i);

		pfree(workspace);

		PG_RETURN_TEXT_P(result);
	}
	else
#endif   /* USE_WIDE_UPPER_LOWER */
	{
		text	   *string = PG_GETARG_TEXT_P_COPY(0);
		char	   *ptr;
		int			m;

		/*
		 * Since we copied the string, we can scribble directly on the value
		 */
		ptr = VARDATA(string);
		m = VARSIZE(string) - VARHDRSZ;

		while (m-- > 0)
		{
			*ptr = toupper((unsigned char) *ptr);
			ptr++;
		}

		PG_RETURN_TEXT_P(string);
	}
}


/********************************************************************
 *
 * initcap
 *
 * Syntax:
 *
 *	 text initcap(text string)
 *
 * Purpose:
 *
 *	 Returns string, with first letter of each word in uppercase, all
 *	 other letters in lowercase. A word is defined as a sequence of
 *	 alphanumeric characters, delimited by non-alphanumeric
 *	 characters.
 *
 ********************************************************************/

Datum
initcap(PG_FUNCTION_ARGS)
{
#ifdef USE_WIDE_UPPER_LOWER

	/*
	 * Use wide char code only when max encoding length > 1 and ctype != C.
	 * Some operating systems fail with multi-byte encodings and a C locale.
	 * Also, for a C locale there is no need to process as multibyte.
	 */
	if (pg_database_encoding_max_length() > 1 && !lc_ctype_is_c())
	{
		text	   *string = PG_GETARG_TEXT_P(0);
		text	   *result;
		wchar_t    *workspace;
		int			wasalnum = 0;
		int			i;

		workspace = texttowcs(string);

		for (i = 0; workspace[i] != 0; i++)
		{
			if (wasalnum)
				workspace[i] = towlower(workspace[i]);
			else
				workspace[i] = towupper(workspace[i]);
			wasalnum = iswalnum(workspace[i]);
		}

		result = wcstotext(workspace, i);

		pfree(workspace);

		PG_RETURN_TEXT_P(result);
	}
	else
#endif   /* USE_WIDE_UPPER_LOWER */
	{
		text	   *string = PG_GETARG_TEXT_P_COPY(0);
		int			wasalnum = 0;
		char	   *ptr;
		int			m;

		/*
		 * Since we copied the string, we can scribble directly on the value
		 */
		ptr = VARDATA(string);
		m = VARSIZE(string) - VARHDRSZ;

		while (m-- > 0)
		{
			if (wasalnum)
				*ptr = tolower((unsigned char) *ptr);
			else
				*ptr = toupper((unsigned char) *ptr);
			wasalnum = isalnum((unsigned char) *ptr);
			ptr++;
		}

		PG_RETURN_TEXT_P(string);
	}
}


/********************************************************************
 *
 * lpad
 *
 * Syntax:
 *
 *	 text lpad(text string1, int4 len, text string2)
 *
 * Purpose:
 *
 *	 Returns string1, left-padded to length len with the sequence of
 *	 characters in string2.  If len is less than the length of string1,
 *	 instead truncate (on the right) to len.
 *
 ********************************************************************/

Datum
lpad(PG_FUNCTION_ARGS)
{
	text	   *string1 = PG_GETARG_TEXT_P(0);
	int32		len = PG_GETARG_INT32(1);
	text	   *string2 = PG_GETARG_TEXT_P(2);
	text	   *ret;
	char	   *ptr1,
			   *ptr2,
			   *ptr2end,
			   *ptr_ret;
	int			m,
				s1len,
				s2len;

	int			bytelen;

	/* Negative len is silently taken as zero */
	if (len < 0)
		len = 0;

	s1len = VARSIZE(string1) - VARHDRSZ;
	if (s1len < 0)
		s1len = 0;				/* shouldn't happen */

	s2len = VARSIZE(string2) - VARHDRSZ;
	if (s2len < 0)
		s2len = 0;				/* shouldn't happen */

	s1len = pg_mbstrlen_with_len(VARDATA(string1), s1len);

	if (s1len > len)
		s1len = len;			/* truncate string1 to len chars */

	if (s2len <= 0)
		len = s1len;			/* nothing to pad with, so don't pad */

	bytelen = pg_database_encoding_max_length() * len;

	/* check for integer overflow */
	if (len != 0 && bytelen / pg_database_encoding_max_length() != len)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("requested length too large")));

	ret = (text *) palloc(VARHDRSZ + bytelen);

	m = len - s1len;

	ptr2 = VARDATA(string2);
	ptr2end = ptr2 + s2len;
	ptr_ret = VARDATA(ret);

	while (m--)
	{
		int			mlen = pg_mblen(ptr2);

		memcpy(ptr_ret, ptr2, mlen);
		ptr_ret += mlen;
		ptr2 += mlen;
		if (ptr2 == ptr2end)	/* wrap around at end of s2 */
			ptr2 = VARDATA(string2);
	}

	ptr1 = VARDATA(string1);

	while (s1len--)
	{
		int			mlen = pg_mblen(ptr1);

		memcpy(ptr_ret, ptr1, mlen);
		ptr_ret += mlen;
		ptr1 += mlen;
	}

	VARATT_SIZEP(ret) = ptr_ret - (char *) ret;

	PG_RETURN_TEXT_P(ret);
}


/********************************************************************
 *
 * rpad
 *
 * Syntax:
 *
 *	 text rpad(text string1, int4 len, text string2)
 *
 * Purpose:
 *
 *	 Returns string1, right-padded to length len with the sequence of
 *	 characters in string2.  If len is less than the length of string1,
 *	 instead truncate (on the right) to len.
 *
 ********************************************************************/

Datum
rpad(PG_FUNCTION_ARGS)
{
	text	   *string1 = PG_GETARG_TEXT_P(0);
	int32		len = PG_GETARG_INT32(1);
	text	   *string2 = PG_GETARG_TEXT_P(2);
	text	   *ret;
	char	   *ptr1,
			   *ptr2,
			   *ptr2end,
			   *ptr_ret;
	int			m,
				s1len,
				s2len;

	int			bytelen;

	/* Negative len is silently taken as zero */
	if (len < 0)
		len = 0;

	s1len = VARSIZE(string1) - VARHDRSZ;
	if (s1len < 0)
		s1len = 0;				/* shouldn't happen */

	s2len = VARSIZE(string2) - VARHDRSZ;
	if (s2len < 0)
		s2len = 0;				/* shouldn't happen */

	s1len = pg_mbstrlen_with_len(VARDATA(string1), s1len);

	if (s1len > len)
		s1len = len;			/* truncate string1 to len chars */

	if (s2len <= 0)
		len = s1len;			/* nothing to pad with, so don't pad */

	bytelen = pg_database_encoding_max_length() * len;

	/* Check for integer overflow */
	if (len != 0 && bytelen / pg_database_encoding_max_length() != len)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("requested length too large")));

	ret = (text *) palloc(VARHDRSZ + bytelen);
	m = len - s1len;

	ptr1 = VARDATA(string1);
	ptr_ret = VARDATA(ret);

	while (s1len--)
	{
		int			mlen = pg_mblen(ptr1);

		memcpy(ptr_ret, ptr1, mlen);
		ptr_ret += mlen;
		ptr1 += mlen;
	}

	ptr2 = VARDATA(string2);
	ptr2end = ptr2 + s2len;

	while (m--)
	{
		int			mlen = pg_mblen(ptr2);

		memcpy(ptr_ret, ptr2, mlen);
		ptr_ret += mlen;
		ptr2 += mlen;
		if (ptr2 == ptr2end)	/* wrap around at end of s2 */
			ptr2 = VARDATA(string2);
	}

	VARATT_SIZEP(ret) = ptr_ret - (char *) ret;

	PG_RETURN_TEXT_P(ret);
}


/********************************************************************
 *
 * btrim
 *
 * Syntax:
 *
 *	 text btrim(text string, text set)
 *
 * Purpose:
 *
 *	 Returns string with characters removed from the front and back
 *	 up to the first character not in set.
 *
 ********************************************************************/

Datum
btrim(PG_FUNCTION_ARGS)
{
	text	   *string = PG_GETARG_TEXT_P(0);
	text	   *set = PG_GETARG_TEXT_P(1);
	text	   *ret;

	ret = dotrim(VARDATA(string), VARSIZE(string) - VARHDRSZ,
				 VARDATA(set), VARSIZE(set) - VARHDRSZ,
				 true, true);

	PG_RETURN_TEXT_P(ret);
}

/********************************************************************
 *
 * btrim1 --- btrim with set fixed as ' '
 *
 ********************************************************************/

Datum
btrim1(PG_FUNCTION_ARGS)
{
	text	   *string = PG_GETARG_TEXT_P(0);
	text	   *ret;

	ret = dotrim(VARDATA(string), VARSIZE(string) - VARHDRSZ,
				 " ", 1,
				 true, true);

	PG_RETURN_TEXT_P(ret);
}

/*
 * Common implementation for btrim, ltrim, rtrim
 */
static text *
dotrim(const char *string, int stringlen,
	   const char *set, int setlen,
	   bool doltrim, bool dortrim)
{
	text	   *result;
	int			i;

	/* Nothing to do if either string or set is empty */
	if (stringlen > 0 && setlen > 0)
	{
		if (pg_database_encoding_max_length() > 1)
		{
			/*
			 * In the multibyte-encoding case, build arrays of pointers to
			 * character starts, so that we can avoid inefficient checks in
			 * the inner loops.
			 */
			const char **stringchars;
			const char **setchars;
			int		   *stringmblen;
			int		   *setmblen;
			int			stringnchars;
			int			setnchars;
			int			resultndx;
			int			resultnchars;
			const char *p;
			int			len;
			int			mblen;
			const char *str_pos;
			int			str_len;

			stringchars = (const char **) palloc(stringlen * sizeof(char *));
			stringmblen = (int *) palloc(stringlen * sizeof(int));
			stringnchars = 0;
			p = string;
			len = stringlen;
			while (len > 0)
			{
				stringchars[stringnchars] = p;
				stringmblen[stringnchars] = mblen = pg_mblen(p);
				stringnchars++;
				p += mblen;
				len -= mblen;
			}

			setchars = (const char **) palloc(setlen * sizeof(char *));
			setmblen = (int *) palloc(setlen * sizeof(int));
			setnchars = 0;
			p = set;
			len = setlen;
			while (len > 0)
			{
				setchars[setnchars] = p;
				setmblen[setnchars] = mblen = pg_mblen(p);
				setnchars++;
				p += mblen;
				len -= mblen;
			}

			resultndx = 0;		/* index in stringchars[] */
			resultnchars = stringnchars;

			if (doltrim)
			{
				while (resultnchars > 0)
				{
					str_pos = stringchars[resultndx];
					str_len = stringmblen[resultndx];
					for (i = 0; i < setnchars; i++)
					{
						if (str_len == setmblen[i] &&
							memcmp(str_pos, setchars[i], str_len) == 0)
							break;
					}
					if (i >= setnchars)
						break;	/* no match here */
					string += str_len;
					stringlen -= str_len;
					resultndx++;
					resultnchars--;
				}
			}

			if (dortrim)
			{
				while (resultnchars > 0)
				{
					str_pos = stringchars[resultndx + resultnchars - 1];
					str_len = stringmblen[resultndx + resultnchars - 1];
					for (i = 0; i < setnchars; i++)
					{
						if (str_len == setmblen[i] &&
							memcmp(str_pos, setchars[i], str_len) == 0)
							break;
					}
					if (i >= setnchars)
						break;	/* no match here */
					stringlen -= str_len;
					resultnchars--;
				}
			}

			pfree(stringchars);
			pfree(stringmblen);
			pfree(setchars);
			pfree(setmblen);
		}
		else
		{
			/*
			 * In the single-byte-encoding case, we don't need such overhead.
			 */
			if (doltrim)
			{
				while (stringlen > 0)
				{
					char		str_ch = *string;

					for (i = 0; i < setlen; i++)
					{
						if (str_ch == set[i])
							break;
					}
					if (i >= setlen)
						break;	/* no match here */
					string++;
					stringlen--;
				}
			}

			if (dortrim)
			{
				while (stringlen > 0)
				{
					char		str_ch = string[stringlen - 1];

					for (i = 0; i < setlen; i++)
					{
						if (str_ch == set[i])
							break;
					}
					if (i >= setlen)
						break;	/* no match here */
					stringlen--;
				}
			}
		}
	}

	/* Return selected portion of string */
	result = (text *) palloc(VARHDRSZ + stringlen);
	VARATT_SIZEP(result) = VARHDRSZ + stringlen;
	memcpy(VARDATA(result), string, stringlen);

	return result;
}

/********************************************************************
 *
 * byteatrim
 *
 * Syntax:
 *
 *	 bytea byteatrim(byta string, bytea set)
 *
 * Purpose:
 *
 *	 Returns string with characters removed from the front and back
 *	 up to the first character not in set.
 *
 * Cloned from btrim and modified as required.
 ********************************************************************/

Datum
byteatrim(PG_FUNCTION_ARGS)
{
	bytea	   *string = PG_GETARG_BYTEA_P(0);
	bytea	   *set = PG_GETARG_BYTEA_P(1);
	bytea	   *ret;
	char	   *ptr,
			   *end,
			   *ptr2,
			   *end2;
	int			m;

	if ((m = VARSIZE(string) - VARHDRSZ) <= 0 ||
		(VARSIZE(set) - VARHDRSZ) <= 0)
		PG_RETURN_BYTEA_P(string);

	ptr = VARDATA(string);
	end = VARDATA(string) + VARSIZE(string) - VARHDRSZ - 1;
	end2 = VARDATA(set) + VARSIZE(set) - VARHDRSZ - 1;

	while (m > 0)
	{
		ptr2 = VARDATA(set);
		while (ptr2 <= end2)
		{
			if (*ptr == *ptr2)
				break;
			++ptr2;
		}
		if (ptr2 > end2)
			break;
		ptr++;
		m--;
	}

	while (m > 0)
	{
		ptr2 = VARDATA(set);
		while (ptr2 <= end2)
		{
			if (*end == *ptr2)
				break;
			++ptr2;
		}
		if (ptr2 > end2)
			break;
		end--;
		m--;
	}

	ret = (bytea *) palloc(VARHDRSZ + m);
	VARATT_SIZEP(ret) = VARHDRSZ + m;
	memcpy(VARDATA(ret), ptr, m);

	PG_RETURN_BYTEA_P(ret);
}

/********************************************************************
 *
 * ltrim
 *
 * Syntax:
 *
 *	 text ltrim(text string, text set)
 *
 * Purpose:
 *
 *	 Returns string with initial characters removed up to the first
 *	 character not in set.
 *
 ********************************************************************/

Datum
ltrim(PG_FUNCTION_ARGS)
{
	text	   *string = PG_GETARG_TEXT_P(0);
	text	   *set = PG_GETARG_TEXT_P(1);
	text	   *ret;

	ret = dotrim(VARDATA(string), VARSIZE(string) - VARHDRSZ,
				 VARDATA(set), VARSIZE(set) - VARHDRSZ,
				 true, false);

	PG_RETURN_TEXT_P(ret);
}

/********************************************************************
 *
 * ltrim1 --- ltrim with set fixed as ' '
 *
 ********************************************************************/

Datum
ltrim1(PG_FUNCTION_ARGS)
{
	text	   *string = PG_GETARG_TEXT_P(0);
	text	   *ret;

	ret = dotrim(VARDATA(string), VARSIZE(string) - VARHDRSZ,
				 " ", 1,
				 true, false);

	PG_RETURN_TEXT_P(ret);
}

/********************************************************************
 *
 * rtrim
 *
 * Syntax:
 *
 *	 text rtrim(text string, text set)
 *
 * Purpose:
 *
 *	 Returns string with final characters removed after the last
 *	 character not in set.
 *
 ********************************************************************/

Datum
rtrim(PG_FUNCTION_ARGS)
{
	text	   *string = PG_GETARG_TEXT_P(0);
	text	   *set = PG_GETARG_TEXT_P(1);
	text	   *ret;

	ret = dotrim(VARDATA(string), VARSIZE(string) - VARHDRSZ,
				 VARDATA(set), VARSIZE(set) - VARHDRSZ,
				 false, true);

	PG_RETURN_TEXT_P(ret);
}

/********************************************************************
 *
 * rtrim1 --- rtrim with set fixed as ' '
 *
 ********************************************************************/

Datum
rtrim1(PG_FUNCTION_ARGS)
{
	text	   *string = PG_GETARG_TEXT_P(0);
	text	   *ret;

	ret = dotrim(VARDATA(string), VARSIZE(string) - VARHDRSZ,
				 " ", 1,
				 false, true);

	PG_RETURN_TEXT_P(ret);
}


/********************************************************************
 *
 * translate
 *
 * Syntax:
 *
 *	 text translate(text string, text from, text to)
 *
 * Purpose:
 *
 *	 Returns string after replacing all occurrences of characters in from
 *	 with the corresponding character in to.  If from is longer than to,
 *	 occurrences of the extra characters in from are deleted.
 *	 Improved by Edwin Ramirez <ramirez@doc.mssm.edu>.
 *
 ********************************************************************/

Datum
translate(PG_FUNCTION_ARGS)
{
	text	   *string = PG_GETARG_TEXT_P(0);
	text	   *from = PG_GETARG_TEXT_P(1);
	text	   *to = PG_GETARG_TEXT_P(2);
	text	   *result;
	char	   *from_ptr,
			   *to_ptr;
	char	   *source,
			   *target;
	int			m,
				fromlen,
				tolen,
				retlen,
				i;
	int			worst_len;
	int			len;
	int			source_len;
	int			from_index;

	m = VARSIZE(string) - VARHDRSZ;
	if (m <= 0)
		PG_RETURN_TEXT_P(string);
	source = VARDATA(string);

	fromlen = VARSIZE(from) - VARHDRSZ;
	from_ptr = VARDATA(from);
	tolen = VARSIZE(to) - VARHDRSZ;
	to_ptr = VARDATA(to);

	/*
	 * The worst-case expansion is to substitute a max-length character for
	 * a single-byte character at each position of the string.
	 */
	worst_len = pg_database_encoding_max_length() * m;

	/* check for integer overflow */
	if (worst_len / pg_database_encoding_max_length() != m)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("requested length too large")));

	result = (text *) palloc(worst_len + VARHDRSZ);
	target = VARDATA(result);
	retlen = 0;

	while (m > 0)
	{
		source_len = pg_mblen(source);
		from_index = 0;

		for (i = 0; i < fromlen; i += len)
		{
			len = pg_mblen(&from_ptr[i]);
			if (len == source_len &&
				memcmp(source, &from_ptr[i], len) == 0)
				break;

			from_index++;
		}
		if (i < fromlen)
		{
			/* substitute */
			char	   *p = to_ptr;

			for (i = 0; i < from_index; i++)
			{
				p += pg_mblen(p);
				if (p >= (to_ptr + tolen))
					break;
			}
			if (p < (to_ptr + tolen))
			{
				len = pg_mblen(p);
				memcpy(target, p, len);
				target += len;
				retlen += len;
			}

		}
		else
		{
			/* no match, so copy */
			memcpy(target, source, source_len);
			target += source_len;
			retlen += source_len;
		}

		source += source_len;
		m -= source_len;
	}

	VARATT_SIZEP(result) = retlen + VARHDRSZ;

	/*
	 * The function result is probably much bigger than needed, if we're
	 * using a multibyte encoding, but it's not worth reallocating it;
	 * the result probably won't live long anyway.
	 */

	PG_RETURN_TEXT_P(result);
}

/********************************************************************
 *
 * ascii
 *
 * Syntax:
 *
 *	 int ascii(text string)
 *
 * Purpose:
 *
 *	 Returns the decimal representation of the first character from
 *	 string.
 *
 ********************************************************************/

Datum
ascii(PG_FUNCTION_ARGS)
{
	text	   *string = PG_GETARG_TEXT_P(0);

	if (VARSIZE(string) <= VARHDRSZ)
		PG_RETURN_INT32(0);

	PG_RETURN_INT32((int32) *((unsigned char *) VARDATA(string)));
}

/********************************************************************
 *
 * chr
 *
 * Syntax:
 *
 *	 text chr(int val)
 *
 * Purpose:
 *
 *	Returns the character having the binary equivalent to val
 *
 ********************************************************************/

Datum
chr			(PG_FUNCTION_ARGS)
{
	int32		cvalue = PG_GETARG_INT32(0);
	text	   *result;

	result = (text *) palloc(VARHDRSZ + 1);
	VARATT_SIZEP(result) = VARHDRSZ + 1;
	*VARDATA(result) = (char) cvalue;

	PG_RETURN_TEXT_P(result);
}

/********************************************************************
 *
 * repeat
 *
 * Syntax:
 *
 *	 text repeat(text string, int val)
 *
 * Purpose:
 *
 *	Repeat string by val.
 *
 ********************************************************************/

Datum
repeat(PG_FUNCTION_ARGS)
{
	text	   *string = PG_GETARG_TEXT_P(0);
	int32		count = PG_GETARG_INT32(1);
	text	   *result;
	int			slen,
				tlen;
	int			i;
	char	   *cp;

	if (count < 0)
		count = 0;

	slen = (VARSIZE(string) - VARHDRSZ);
	tlen = (VARHDRSZ + (count * slen));

	/* Check for integer overflow */
	if (slen != 0 && count != 0)
	{
		int			check = count * slen;
		int			check2 = check + VARHDRSZ;

		if ((check / slen) != count || check2 <= check)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("requested length too large")));
	}

	result = (text *) palloc(tlen);

	VARATT_SIZEP(result) = tlen;
	cp = VARDATA(result);
	for (i = 0; i < count; i++)
	{
		memcpy(cp, VARDATA(string), slen);
		cp += slen;
	}

	PG_RETURN_TEXT_P(result);
}
