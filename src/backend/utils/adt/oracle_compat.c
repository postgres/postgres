/*-------------------------------------------------------------------------
 * oracle_compat.c
 *	Oracle compatible functions.
 *
 * Copyright (c) 1997-2001, PostgreSQL Global Development Group
 *
 *	Author: Edmund Mergl <E.Mergl@bawue.de>
 *	Multibyte enhancement: Tatsuo Ishii <ishii@postgresql.org>
 *
 *
 * IDENTIFICATION
 *	$Header: /cvsroot/pgsql/src/backend/utils/adt/oracle_compat.c,v 1.36 2001/10/25 05:49:45 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <ctype.h>

#include "utils/builtins.h"

#ifdef MULTIBYTE
#include "mb/pg_wchar.h"
#endif

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
	text	   *string = PG_GETARG_TEXT_P_COPY(0);
	char	   *ptr;
	int			m;

	/* Since we copied the string, we can scribble directly on the value */
	ptr = VARDATA(string);
	m = VARSIZE(string) - VARHDRSZ;

	while (m-- > 0)
	{
		*ptr = tolower((unsigned char) *ptr);
		ptr++;
	}

	PG_RETURN_TEXT_P(string);
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
	text	   *string = PG_GETARG_TEXT_P_COPY(0);
	char	   *ptr;
	int			m;

	/* Since we copied the string, we can scribble directly on the value */
	ptr = VARDATA(string);
	m = VARSIZE(string) - VARHDRSZ;

	while (m-- > 0)
	{
		*ptr = toupper((unsigned char) *ptr);
		ptr++;
	}

	PG_RETURN_TEXT_P(string);
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
 *	 Returns string, with first letter of each word in uppercase,
 *	 all other letters in lowercase. A word is delimited by white
 *	 space.
 *
 ********************************************************************/

Datum
initcap(PG_FUNCTION_ARGS)
{
	text	   *string = PG_GETARG_TEXT_P_COPY(0);
	char	   *ptr;
	int			m;

	/* Since we copied the string, we can scribble directly on the value */
	ptr = VARDATA(string);
	m = VARSIZE(string) - VARHDRSZ;

	if (m > 0)
	{
		*ptr = toupper((unsigned char) *ptr);
		ptr++;
		m--;
	}

	while (m-- > 0)
	{
		if (isspace((unsigned char) ptr[-1]))
			*ptr = toupper((unsigned char) *ptr);
		else
			*ptr = tolower((unsigned char) *ptr);
		ptr++;
	}

	PG_RETURN_TEXT_P(string);
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

#ifdef MULTIBYTE
	int			bytelen;
#endif

	/* Negative len is silently taken as zero */
	if (len < 0)
		len = 0;

	s1len = VARSIZE(string1) - VARHDRSZ;
	if (s1len < 0)
		s1len = 0;				/* shouldn't happen */

	s2len = VARSIZE(string2) - VARHDRSZ;
	if (s2len < 0)
		s2len = 0;				/* shouldn't happen */

#ifdef MULTIBYTE
	s1len = pg_mbstrlen_with_len(VARDATA(string1), s1len);
#endif
	if (s1len > len)
		s1len = len;			/* truncate string1 to len chars */

	if (s2len <= 0)
		len = s1len;			/* nothing to pad with, so don't pad */

#ifdef MULTIBYTE
	bytelen = pg_database_encoding_max_length() * len;
	ret = (text *) palloc(VARHDRSZ + bytelen);
	VARATT_SIZEP(ret) = VARHDRSZ + bytelen;
#else
	ret = (text *) palloc(VARHDRSZ + len);
	VARATT_SIZEP(ret) = VARHDRSZ + len;
#endif
	m = len - s1len;

	ptr2 = VARDATA(string2);
	ptr2end = ptr2 + s2len;
	ptr_ret = VARDATA(ret);

#ifdef MULTIBYTE
	while (m--)
	{
		int			mlen = pg_mblen(ptr2);

		memcpy(ptr_ret, ptr2, mlen);
		ptr_ret += mlen;
		ptr2 += mlen;
		if (ptr2 == ptr2end)	/* wrap around at end of s2 */
			ptr2 = VARDATA(string2);
	}
#else
	while (m--)
	{
		*ptr_ret++ = *ptr2++;
		if (ptr2 == ptr2end)	/* wrap around at end of s2 */
			ptr2 = VARDATA(string2);
	}
#endif

	ptr1 = VARDATA(string1);

#ifdef MULTIBYTE
	while (s1len--)
	{
		int			mlen = pg_mblen(ptr1);

		memcpy(ptr_ret, ptr1, mlen);
		ptr_ret += mlen;
		ptr1 += mlen;
	}
#else
	while (s1len--)
		*ptr_ret++ = *ptr1++;
#endif

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

#ifdef MULTIBYTE
	int			bytelen;
#endif

	/* Negative len is silently taken as zero */
	if (len < 0)
		len = 0;

	s1len = VARSIZE(string1) - VARHDRSZ;
	if (s1len < 0)
		s1len = 0;				/* shouldn't happen */

	s2len = VARSIZE(string2) - VARHDRSZ;
	if (s2len < 0)
		s2len = 0;				/* shouldn't happen */

#ifdef MULTIBYTE
	s1len = pg_mbstrlen_with_len(VARDATA(string1), s1len);
#endif

	if (s1len > len)
		s1len = len;			/* truncate string1 to len chars */

	if (s2len <= 0)
		len = s1len;			/* nothing to pad with, so don't pad */

#ifdef MULTIBYTE
	bytelen = pg_database_encoding_max_length() * len;
	ret = (text *) palloc(VARHDRSZ + bytelen);
	VARATT_SIZEP(ret) = VARHDRSZ + bytelen;
#else
	ret = (text *) palloc(VARHDRSZ + len);
	VARATT_SIZEP(ret) = VARHDRSZ + len;
#endif
	m = len - s1len;

	ptr1 = VARDATA(string1);
	ptr_ret = VARDATA(ret);

#ifdef MULTIBYTE
	while (s1len--)
	{
		int			mlen = pg_mblen(ptr1);

		memcpy(ptr_ret, ptr1, mlen);
		ptr_ret += mlen;
		ptr1 += mlen;
	}
#else
	while (s1len--)
		*ptr_ret++ = *ptr1++;
#endif

	ptr2 = VARDATA(string2);
	ptr2end = ptr2 + s2len;

#ifdef MULTIBYTE
	while (m--)
	{
		int			mlen = pg_mblen(ptr2);

		memcpy(ptr_ret, ptr2, mlen);
		ptr_ret += mlen;
		ptr2 += mlen;
		if (ptr2 == ptr2end)	/* wrap around at end of s2 */
			ptr2 = VARDATA(string2);
	}
#else
	while (m--)
	{
		*ptr_ret++ = *ptr2++;
		if (ptr2 == ptr2end)	/* wrap around at end of s2 */
			ptr2 = VARDATA(string2);
	}
#endif

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
	char	   *ptr,
			   *end,
			   *ptr2,
			   *end2;
	int			m;

#ifdef MULTIBYTE
	char	  **mp;
	int			mplen;
	char	   *p;
	int			mblen;
	int			len;
#endif

	if ((m = VARSIZE(string) - VARHDRSZ) <= 0 ||
		(VARSIZE(set) - VARHDRSZ) <= 0)
		PG_RETURN_TEXT_P(string);

	ptr = VARDATA(string);

#ifdef MULTIBYTE
	len = m;
	mp = (char **) palloc(len * sizeof(char *));
	p = ptr;
	mplen = 0;

	/* build the mb pointer array */
	while (len > 0)
	{
		mp[mplen++] = p;
		mblen = pg_mblen(p);
		p += mblen;
		len -= mblen;
	}
	mplen--;
#else
	end = VARDATA(string) + VARSIZE(string) - VARHDRSZ - 1;
#endif
	end2 = VARDATA(set) + VARSIZE(set) - VARHDRSZ - 1;

#ifdef MULTIBYTE
	while (m > 0)
	{
		int			str_len = pg_mblen(ptr);

		ptr2 = VARDATA(set);
		while (ptr2 <= end2)
		{
			int			set_len = pg_mblen(ptr2);

			if (str_len == set_len &&
				memcmp(ptr, ptr2, str_len) == 0)
				break;
			ptr2 += set_len;
		}
		if (ptr2 > end2)
			break;
		ptr += str_len;
		m -= str_len;
	}

	while (m > 0)
	{
		int			str_len;

		end = mp[mplen--];
		str_len = pg_mblen(end);
		ptr2 = VARDATA(set);
		while (ptr2 <= end2)
		{
			int			set_len = pg_mblen(ptr2);

			if (str_len == set_len &&
				memcmp(end, ptr2, str_len) == 0)
				break;
			ptr2 += set_len;
		}
		if (ptr2 > end2)
			break;
		m -= str_len;
	}
	pfree(mp);
#else
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
#endif
	ret = (text *) palloc(VARHDRSZ + m);
	VARATT_SIZEP(ret) = VARHDRSZ + m;
	memcpy(VARDATA(ret), ptr, m);

	PG_RETURN_TEXT_P(ret);
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
	char	   *ptr,
			   *ptr2,
			   *end2;
	int			m;

	if ((m = VARSIZE(string) - VARHDRSZ) <= 0 ||
		(VARSIZE(set) - VARHDRSZ) <= 0)
		PG_RETURN_TEXT_P(string);

	ptr = VARDATA(string);
	end2 = VARDATA(set) + VARSIZE(set) - VARHDRSZ - 1;

#ifdef MULTIBYTE
	while (m > 0)
	{
		int			str_len = pg_mblen(ptr);

		ptr2 = VARDATA(set);
		while (ptr2 <= end2)
		{
			int			set_len = pg_mblen(ptr2);

			if (str_len == set_len &&
				memcmp(ptr, ptr2, str_len) == 0)
				break;
			ptr2 += set_len;
		}
		if (ptr2 > end2)
			break;
		ptr += str_len;
		m -= str_len;
	}
#else
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
#endif
	ret = (text *) palloc(VARHDRSZ + m);
	VARATT_SIZEP(ret) = VARHDRSZ + m;
	memcpy(VARDATA(ret), ptr, m);

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

	char	   *ptr,
			   *end,
			   *ptr2,
			   *end2;
	int			m;

#ifdef MULTIBYTE
	char	  **mp;
	int			mplen;
	char	   *p;
	int			mblen;
	int			len;
#endif

	if ((m = VARSIZE(string) - VARHDRSZ) <= 0 ||
		(VARSIZE(set) - VARHDRSZ) <= 0)
		PG_RETURN_TEXT_P(string);

	ptr = VARDATA(string);

#ifdef MULTIBYTE
	len = m;
	mp = (char **) palloc(len * sizeof(char *));
	p = ptr;
	mplen = 0;

	/* build the mb pointer array */
	while (len > 0)
	{
		mp[mplen++] = p;
		mblen = pg_mblen(p);
		p += mblen;
		len -= mblen;
	}
	mplen--;
#else
	end = VARDATA(string) + VARSIZE(string) - VARHDRSZ - 1;
#endif
	end2 = VARDATA(set) + VARSIZE(set) - VARHDRSZ - 1;

#ifdef MULTIBYTE
	while (m > 0)
	{
		int			str_len;

		end = mp[mplen--];
		str_len = pg_mblen(end);
		ptr2 = VARDATA(set);
		while (ptr2 <= end2)
		{
			int			set_len = pg_mblen(ptr2);

			if (str_len == set_len &&
				memcmp(end, ptr2, str_len) == 0)
				break;
			ptr2 += set_len;
		}
		if (ptr2 > end2)
			break;
		m -= str_len;
	}
	pfree(mp);
#else
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
#endif
	ret = (text *) palloc(VARHDRSZ + m);
	VARATT_SIZEP(ret) = VARHDRSZ + m;
	memcpy(VARDATA(ret), ptr, m);

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

#ifdef MULTIBYTE
	int			str_len;
	int			estimate_len;
	int			len;
	int			source_len;
	int			from_index;
#endif

	if ((m = VARSIZE(string) - VARHDRSZ) <= 0)
		PG_RETURN_TEXT_P(string);

	fromlen = VARSIZE(from) - VARHDRSZ;
	from_ptr = VARDATA(from);
	tolen = VARSIZE(to) - VARHDRSZ;
	to_ptr = VARDATA(to);

#ifdef MULTIBYTE
	str_len = VARSIZE(string);
	estimate_len = (tolen * 1.0 / fromlen + 0.5) * str_len;
	estimate_len = estimate_len > str_len ? estimate_len : str_len;
	result = (text *) palloc(estimate_len);
#else
	result = (text *) palloc(VARSIZE(string));
#endif

	source = VARDATA(string);
	target = VARDATA(result);
	retlen = 0;

#ifdef MULTIBYTE
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
#else
	while (m-- > 0)
	{
		char		rep = *source++;

		for (i = 0; i < fromlen; i++)
		{
			if (from_ptr[i] == rep)
				break;
		}
		if (i < fromlen)
		{
			if (i < tolen)
			{
				/* substitute */
				*target++ = to_ptr[i];
				retlen++;
			}
			else
			{
				/* discard */
			}
		}
		else
		{
			/* no match, so copy */
			*target++ = rep;
			retlen++;
		}
	}
#endif

	VARATT_SIZEP(result) = retlen + VARHDRSZ;

	/*
	 * There may be some wasted space in the result if deletions occurred,
	 * but it's not worth reallocating it; the function result probably
	 * won't live long anyway.
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
chr(PG_FUNCTION_ARGS)
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
