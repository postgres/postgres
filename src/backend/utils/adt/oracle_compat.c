/*
 *	Edmund Mergl <E.Mergl@bawue.de>
 *
 *	$Id: oracle_compat.c,v 1.26 2000/07/03 23:09:52 wieck Exp $
 *
 */


#include <ctype.h>
#include "postgres.h"
#include "utils/builtins.h"


/********************************************************************
 *
 * lower
 *
 * Syntax:
 *
 *	 text *lower(text *string)
 *
 * Purpose:
 *
 *	 Returns string, with all letters forced to lowercase.
 *
 ********************************************************************/

text *
lower(text *string)
{
	text	   *ret;
	char	   *ptr,
			   *ptr_ret;
	int			m;

	if ((string == (text *) NULL) || ((m = VARSIZE(string) - VARHDRSZ) <= 0))
		return string;

	ret = (text *) palloc(VARSIZE(string));
	VARATT_SIZEP(ret) = VARSIZE(string);

	ptr = VARDATA(string);
	ptr_ret = VARDATA(ret);

	while (m--)
		*ptr_ret++ = tolower((unsigned char) *ptr++);

	return ret;
}


/********************************************************************
 *
 * upper
 *
 * Syntax:
 *
 *	 text *upper(text *string)
 *
 * Purpose:
 *
 *	 Returns string, with all letters forced to uppercase.
 *
 ********************************************************************/

text *
upper(text *string)
{
	text	   *ret;
	char	   *ptr,
			   *ptr_ret;
	int			m;

	if ((string == (text *) NULL) || ((m = VARSIZE(string) - VARHDRSZ) <= 0))
		return string;

	ret = (text *) palloc(VARSIZE(string));
	VARATT_SIZEP(ret) = VARSIZE(string);

	ptr = VARDATA(string);
	ptr_ret = VARDATA(ret);

	while (m--)
		*ptr_ret++ = toupper((unsigned char) *ptr++);

	return ret;
}


/********************************************************************
 *
 * initcap
 *
 * Syntax:
 *
 *	 text *initcap(text *string)
 *
 * Purpose:
 *
 *	 Returns string, with first letter of each word in uppercase,
 *	 all other letters in lowercase. A word is delimited by white
 *	 space.
 *
 ********************************************************************/

text *
initcap(text *string)
{
	text	   *ret;
	char	   *ptr,
			   *ptr_ret;
	int			m;

	if ((string == (text *) NULL) || ((m = VARSIZE(string) - VARHDRSZ) <= 0))
		return string;

	ret = (text *) palloc(VARSIZE(string));
	VARATT_SIZEP(ret) = VARSIZE(string);

	ptr = VARDATA(string);
	ptr_ret = VARDATA(ret);

	*ptr_ret++ = toupper((unsigned char) *ptr++);
	--m;

	while (m--)
	{
		if (*(ptr_ret - 1) == ' ' || *(ptr_ret - 1) == '	')
			*ptr_ret++ = toupper((unsigned char) *ptr++);
		else
			*ptr_ret++ = tolower((unsigned char) *ptr++);
	}

	return ret;
}


/********************************************************************
 *
 * lpad
 *
 * Syntax:
 *
 *	 text *lpad(text *string1, int4 len, text *string2)
 *
 * Purpose:
 *
 *	 Returns string1, left-padded to length len with the sequence of
 *	 characters in string2.
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
			   *ptr_ret;
	int			m,
				n;

	if (((VARSIZE(string1) - VARHDRSZ) < 0) ||
		((m = len - (VARSIZE(string1) - VARHDRSZ)) <= 0) ||
		((VARSIZE(string2) - VARHDRSZ) <= 0))
		PG_RETURN_TEXT_P(string1);

	ret = (text *) palloc(VARHDRSZ + len);
	VARATT_SIZEP(ret) = VARHDRSZ + len;

	ptr2 = VARDATA(string2);
	ptr_ret = VARDATA(ret);

	while (m--)
	{
		*ptr_ret++ = *ptr2;
		ptr2 = (ptr2 == VARDATA(string2) + VARSIZE(string2) - VARHDRSZ - 1) ? VARDATA(string2) : ++ptr2;
	}

	n = VARSIZE(string1) - VARHDRSZ;
	ptr1 = VARDATA(string1);

	while (n--)
		*ptr_ret++ = *ptr1++;

	PG_RETURN_TEXT_P(ret);
}


/********************************************************************
 *
 * rpad
 *
 * Syntax:
 *
 *	 text *rpad(text *string1, int4 len, text *string2)
 *
 * Purpose:
 *
 *	 Returns string1, right-padded to length len with the sequence of
 *	 characters in string2.
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
			   *ptr_ret;
	int			m,
				n;

	if (((VARSIZE(string1) - VARHDRSZ) < 0) ||
		((m = len - (VARSIZE(string1) - VARHDRSZ)) <= 0) ||
		((VARSIZE(string2) - VARHDRSZ) <= 0))
		PG_RETURN_TEXT_P(string1);

	ret = (text *) palloc(VARHDRSZ + len);
	VARATT_SIZEP(ret) = VARHDRSZ + len;

	n = VARSIZE(string1) - VARHDRSZ;
	ptr1 = VARDATA(string1);
	ptr_ret = VARDATA(ret);

	while (n--)
		*ptr_ret++ = *ptr1++;

	ptr2 = VARDATA(string2);

	while (m--)
	{
		*ptr_ret++ = *ptr2;
		ptr2 = (ptr2 == VARDATA(string2) + VARSIZE(string2) - VARHDRSZ - 1) ? VARDATA(string2) : ++ptr2;
	}

	PG_RETURN_TEXT_P(ret);
}


/********************************************************************
 *
 * btrim
 *
 * Syntax:
 *
 *	 text *btrim(text *string, text *set)
 *
 * Purpose:
 *
 *	 Returns string with characters removed from the front and back
 *	 up to the first character not in set.
 *
 ********************************************************************/

text *
btrim(text *string, text *set)
{
	text	   *ret;
	char	   *ptr,
			   *end,
			   *ptr2,
			   *end2;
	int			m;

	if ((string == (text *) NULL) ||
		((m = VARSIZE(string) - VARHDRSZ) <= 0) ||
		(set == (text *) NULL) ||
		((VARSIZE(set) - VARHDRSZ) <= 0))
		return string;

	ptr = VARDATA(string);
	ptr2 = VARDATA(set);
	end2 = VARDATA(set) + VARSIZE(set) - VARHDRSZ - 1;

	while (m--)
	{
		while (ptr2 <= end2)
		{
			if (*ptr == *ptr2)
				break;
			++ptr2;
		}
		if (ptr2 > end2)
			break;
		ptr++;
		ptr2 = VARDATA(set);
	}

	++m;

	end = VARDATA(string) + VARSIZE(string) - VARHDRSZ - 1;
	ptr2 = VARDATA(set);

	while (m--)
	{
		while (ptr2 <= end2)
		{
			if (*end == *ptr2)
				break;
			++ptr2;
		}
		if (ptr2 > end2)
			break;
		--end;
		ptr2 = VARDATA(set);
	}

	++m;

	ret = (text *) palloc(VARHDRSZ + m);
	VARATT_SIZEP(ret) = VARHDRSZ + m;
	memcpy(VARDATA(ret), ptr, m);

	return ret;
}	/* btrim() */


/********************************************************************
 *
 * ltrim
 *
 * Syntax:
 *
 *	 text *ltrim(text *string, text *set)
 *
 * Purpose:
 *
 *	 Returns string with initial characters removed up to the first
 *	 character not in set.
 *
 ********************************************************************/

text *
ltrim(text *string, text *set)
{
	text	   *ret;
	char	   *ptr,
			   *ptr2,
			   *end2;
	int			m;

	if ((string == (text *) NULL) ||
		((m = VARSIZE(string) - VARHDRSZ) <= 0) ||
		(set == (text *) NULL) ||
		((VARSIZE(set) - VARHDRSZ) <= 0))
		return string;

	ptr = VARDATA(string);
	ptr2 = VARDATA(set);
	end2 = VARDATA(set) + VARSIZE(set) - VARHDRSZ - 1;

	while (m--)
	{
		while (ptr2 <= end2)
		{
			if (*ptr == *ptr2)
				break;
			++ptr2;
		}
		if (ptr2 > end2)
			break;
		ptr++;
		ptr2 = VARDATA(set);
	}

	++m;

	ret = (text *) palloc(VARHDRSZ + m);
	VARATT_SIZEP(ret) = VARHDRSZ + m;

	memcpy(VARDATA(ret), ptr, m);

	return ret;
}


/********************************************************************
 *
 * rtrim
 *
 * Syntax:
 *
 *	 text *rtrim(text *string, text *set)
 *
 * Purpose:
 *
 *	 Returns string with final characters removed after the last
 *	 character not in set.
 *
 ********************************************************************/

text *
rtrim(text *string, text *set)
{
	text	   *ret;
	char	   *ptr,
			   *ptr2,
			   *end2,
			   *ptr_ret;
	int			m;

	if ((string == (text *) NULL) ||
		((m = VARSIZE(string) - VARHDRSZ) <= 0) ||
		(set == (text *) NULL) ||
		((VARSIZE(set) - VARHDRSZ) <= 0))
		return string;

	ptr = VARDATA(string) + VARSIZE(string) - VARHDRSZ - 1;
	ptr2 = VARDATA(set);
	end2 = VARDATA(set) + VARSIZE(set) - VARHDRSZ - 1;

	while (m--)
	{
		while (ptr2 <= end2)
		{
			if (*ptr == *ptr2)
				break;
			++ptr2;
		}
		if (ptr2 > end2)
			break;
		--ptr;
		ptr2 = VARDATA(set);
	}

	++m;

	ret = (text *) palloc(VARHDRSZ + m);
	VARATT_SIZEP(ret) = VARHDRSZ + m;
#ifdef NOT_USED
	memcpy(VARDATA(ret), ptr - VARSIZE(ret) + m, m);
#endif

	ptr_ret = VARDATA(ret) + m - 1;

	while (m--)
		*ptr_ret-- = *ptr--;

	return ret;
}


/********************************************************************
 *
 * translate
 *
 * Syntax:
 *
 *	 text *translate(text *string, text *from, text *to)
 *
 * Purpose:
 *
 *	 Returns string after replacing all occurrences of characters in from
 *	 with the corresponding character in to.  If from is longer than to,
 *	 occurrences of the extra characters in from are deleted.
 *	 Improved by Edwin Ramirez <ramirez@doc.mssm.edu>.
 *
 ********************************************************************/

text *
translate(text *string, text *from, text *to)
{
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

	if (string == (text *) NULL ||
		from == (text *) NULL ||
		to == (text *) NULL)
		return (text *) NULL;

	if ((m = VARSIZE(string) - VARHDRSZ) <= 0)
		return string;

	fromlen = VARSIZE(from) - VARHDRSZ;
	from_ptr = VARDATA(from);
	tolen = VARSIZE(to) - VARHDRSZ;
	to_ptr = VARDATA(to);

	result = (text *) palloc(VARSIZE(string));

	source = VARDATA(string);
	target = VARDATA(result);
	retlen = 0;

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

	VARATT_SIZEP(result) = retlen + VARHDRSZ;

	/*
	 * There may be some wasted space in the result if deletions occurred,
	 * but it's not worth reallocating it; the function result probably
	 * won't live long anyway.
	 */

	return result;
}


int4
ascii(text *string)
{
	if (!PointerIsValid(string))
		return 0;

	if (VARSIZE(string) <= VARHDRSZ)
		return 0;

	return ((int) *(VARDATA(string)));
}	/* ascii() */


Datum
ichar(PG_FUNCTION_ARGS)
{
	int32		cvalue = PG_GETARG_INT32(0);
	text	   *result;

	result = (text *) palloc(VARHDRSZ + 1);
	VARATT_SIZEP(result) = VARHDRSZ + 1;
	*VARDATA(result) = (char) cvalue;

	PG_RETURN_TEXT_P(result);
}


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
