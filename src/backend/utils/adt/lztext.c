/* ----------
 * lztext.c -
 *
 * $Header: /cvsroot/pgsql/src/backend/utils/adt/Attic/lztext.c,v 1.8 2000/07/03 23:09:52 wieck Exp $
 *
 *	Text type with internal LZ compressed representation. Uses the
 *	standard PostgreSQL compression method.
 *
 *	This code requires that the LZ compressor found in pg_lzcompress
 *	codes a usable VARSIZE word at the beginning of the output buffer.
 * ----------
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>

#include "postgres.h"
#include "utils/builtins.h"
#ifdef MULTIBYTE
#include "mb/pg_wchar.h"
#endif

/* ----------
 * lztextin -
 *
 *		Input function for datatype lztext
 * ----------
 */
lztext *
lztextin(char *str)
{
	lztext	   *result;
	int32		rawsize;

	/* ----------
	 * Handle NULL
	 * ----------
	 */
	if (str == NULL)
		return NULL;

	rawsize = strlen(str);
	result  = (lztext *)palloc(VARHDRSZ + rawsize);

	VARATT_SIZEP(result) = VARHDRSZ + rawsize;
	memcpy(VARATT_DATA(result), str, rawsize);

	return result;
}


/* ----------
 * lztextout -
 *
 *		Output function for data type lztext
 * ----------
 */
char *
lztextout(lztext *lz)
{
	char	   *result;
	void	   *tmp;
	int32		rawsize;

	/* ----------
	 * Handle NULL
	 * ----------
	 */
	if (lz == NULL)
	{
		result = (char *) palloc(2);
		result[0] = '-';
		result[1] = '\0';
		return result;
	}

	VARATT_GETPLAIN(lz, tmp);

	rawsize = VARATT_SIZE(tmp) - VARHDRSZ;
	result  = (char *)palloc(rawsize + 1);
	memcpy(result, VARATT_DATA(tmp), rawsize);
	result[rawsize] = '\0';

	VARATT_FREE(lz, tmp);

	return result;
}


/* ----------
 * lztextlen -
 *
 *	Logical length of lztext field (it's the uncompressed size
 *	of the original data).
 * ----------
 */
int32
lztextlen(lztext *lz)
{
#ifdef MULTIBYTE
	unsigned char *s1,
			   *s2;
	int			len;
	int			l;
	int			wl;

#endif
	/* ----------
	 * Handle NULL
	 * ----------
	 */
	if (lz == NULL)
		return 0;

#ifdef MULTIBYTE
	len = 0;
	s1 = s2 = (unsigned char *) lztextout(lz);
	l = PGLZ_RAW_SIZE(lz);
	while (l > 0)
	{
		wl = pg_mblen(s1);
		l -= wl;
		s1 += wl;
		len++;
	}
	pfree((char *) s2);
	return (len);
#else
	/* ----------
	 * without multibyte support, it's the remembered rawsize
	 * ----------
	 */
	if (!VARATT_IS_EXTENDED(lz))
	    return VARATT_SIZE(lz) - VARHDRSZ;

    if (VARATT_IS_EXTERNAL(lz))
	    return lz->va_content.va_external.va_rawsize;

	return lz->va_content.va_compressed.va_rawsize;
#endif
}


/* ----------
 * lztextoctetlen -
 *
 *	Physical length of lztext field (it's the compressed size
 *	plus the rawsize field).
 * ----------
 */
int32
lztextoctetlen(lztext *lz)
{
	/* ----------
	 * Handle NULL
	 * ----------
	 */
	if (lz == NULL)
		return 0;

	if (!VARATT_IS_EXTERNAL(lz))
	    return VARATT_SIZE(lz) - VARHDRSZ;

	return lz->va_content.va_external.va_extsize;
}


/* ----------
 * text_lztext -
 *
 *	Convert text to lztext
 * ----------
 */
lztext *
text_lztext(text *txt)
{
	lztext	   *result;
	int32		rawsize;

	/* ----------
	 * Handle NULL
	 * ----------
	 */
	if (txt == NULL)
		return NULL;

	/* ----------
	 * Copy the entire attribute
	 * ----------
	 */
	rawsize = VARSIZE(txt) - VARHDRSZ;
	result  = (lztext *)palloc(rawsize + VARHDRSZ);
	VARATT_SIZEP(result) = rawsize + VARHDRSZ;
	memcpy(VARATT_DATA(result), VARATT_DATA(txt), rawsize);

	return result;
}


/* ----------
 * lztext_text -
 *
 *	Convert lztext to text
 * ----------
 */
text *
lztext_text(lztext *lz)
{
	text	   *result;
	lztext	   *tmp;
	int32		rawsize;

	/* ----------
	 * Handle NULL
	 * ----------
	 */
	if (lz == NULL)
		return NULL;

	VARATT_GETPLAIN(lz, tmp);
	
	rawsize = VARATT_SIZE(tmp) - VARHDRSZ;
	result  = (text *)palloc(rawsize + VARHDRSZ);
	VARATT_SIZEP(result) = rawsize + VARHDRSZ;
	memcpy(VARATT_DATA(result), VARATT_DATA(tmp), rawsize);

	VARATT_FREE(lz, tmp);

	return result;
}


/* ----------
 * lztext_cmp -
 *
 *		Comparision function for two lztext datum's.
 *
 *		Returns -1, 0 or 1.
 * ----------
 */
int32
lztext_cmp(lztext *lz1, lztext *lz2)
{
#ifdef USE_LOCALE

	char	   *cp1;
	char	   *cp2;
	int			result;

	if (lz1 == NULL || lz2 == NULL)
		return (int32) 0;

	cp1 = lztextout(lz1);
	cp2 = lztextout(lz2);

	result = strcoll(cp1, cp2);

	pfree(cp1);
	pfree(cp2);

	return result;

#else							/* !USE_LOCALE */

	int		result;
	char   *p1 = NULL;
	char   *p2 = NULL;
	int		size1;
	int		size2;

	if (lz1 == NULL || lz2 == NULL)
		return 0;

	VARATT_GETPLAIN(lz1, p1);
	VARATT_GETPLAIN(lz2, p2);

    size1 = VARATT_SIZE(p1) - VARHDRSZ;
    size2 = VARATT_SIZE(p2) - VARHDRSZ;
    result = memcmp(VARATT_DATA(p1), VARATT_DATA(p2),
                (size1 < size2) ? size1 : size2);
    if (result == 0)
    {
        if (size1 > size2)
            result = 1;
        else if (size1 < size2)
            result = -1;
    }

    VARATT_FREE(lz2, p2);
    VARATT_FREE(lz1, p1);

	return result;

#endif	 /* USE_LOCALE */
}


/* ----------
 * lztext_eq ... -
 *
 *		=, !=, >, >=, < and <= operator functions for two
 *		lztext datums.
 * ----------
 */
bool
lztext_eq(lztext *lz1, lztext *lz2)
{
	if (lz1 == NULL || lz2 == NULL)
		return false;

	return (bool) (lztext_cmp(lz1, lz2) == 0);
}


bool
lztext_ne(lztext *lz1, lztext *lz2)
{
	if (lz1 == NULL || lz2 == NULL)
		return false;

	return (bool) (lztext_cmp(lz1, lz2) != 0);
}


bool
lztext_gt(lztext *lz1, lztext *lz2)
{
	if (lz1 == NULL || lz2 == NULL)
		return false;

	return (bool) (lztext_cmp(lz1, lz2) > 0);
}


bool
lztext_ge(lztext *lz1, lztext *lz2)
{
	if (lz1 == NULL || lz2 == NULL)
		return false;

	return (bool) (lztext_cmp(lz1, lz2) >= 0);
}


bool
lztext_lt(lztext *lz1, lztext *lz2)
{
	if (lz1 == NULL || lz2 == NULL)
		return false;

	return (bool) (lztext_cmp(lz1, lz2) < 0);
}


bool
lztext_le(lztext *lz1, lztext *lz2)
{
	if (lz1 == NULL || lz2 == NULL)
		return false;

	return (bool) (lztext_cmp(lz1, lz2) <= 0);
}
