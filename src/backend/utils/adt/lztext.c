/* ----------
 * lztext.c -
 *
 * $Header: /cvsroot/pgsql/src/backend/utils/adt/Attic/lztext.c,v 1.3 1999/11/24 03:45:12 ishii Exp $
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
#include "utils/palloc.h"
#include "utils/pg_lzcompress.h"
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
	lztext		   *result;
	int32			rawsize;
	lztext		   *tmp;
	int				tmp_size;

	/* ----------
	 * Handle NULL
	 * ----------
	 */
	if (str == NULL)
		return NULL;

	/* ----------
	 * Determine input size and maximum output Datum size
	 * ----------
	 */
	rawsize = strlen(str);
	tmp_size = PGLZ_MAX_OUTPUT(rawsize);

	/* ----------
	 * Allocate a temporary result and compress into it
	 * ----------
	 */
	tmp = (lztext *) palloc(tmp_size);
	pglz_compress(str, rawsize, tmp, NULL);

	/* ----------
	 * If we miss less than 25% bytes at the end of the temp value,
	 * so be it. Therefore we save a palloc()/memcpy()/pfree()
	 * sequence.
	 * ----------
	 */
	if (tmp_size - tmp->varsize < 256 || 
					tmp_size - tmp->varsize < tmp_size / 4)
	{
		result = tmp;
	} else {
		result = (lztext *) palloc(tmp->varsize);
		memcpy(result, tmp, tmp->varsize);
		pfree(tmp);
	}

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
	char			*result;

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

	/* ----------
	 * Allocate the result string - the required size is remembered
	 * in the lztext header so we don't need a temporary buffer or
	 * have to diddle with realloc's.
	 * ----------
	 */
	result = (char *) palloc(PGLZ_RAW_SIZE(lz) + 1);

	/* ----------
	 * Decompress and add terminating ZERO
	 * ----------
	 */
	pglz_decompress(lz, result);
	result[lz->rawsize] = '\0';

	/* ----------
	 * Return the result
	 * ----------
	 */
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
	unsigned char	*s1,*s2;
	int	len;
	int	l;
	int	wl;
#endif
	/* ----------
	 * Handle NULL
	 * ----------
	 */
	if (lz == NULL)
		return 0;

#ifdef MULTIBYTE
	len = 0;
	s1 = s2 = (unsigned char *)lztextout(lz);
	l = PGLZ_RAW_SIZE(lz);
	while (l > 0)
	{
		wl = pg_mblen(s1);
		l -= wl;
		s1 += wl;
		len++;
	}
	pfree((char *)s2);
	return (len);
#else
	/* ----------
	 * without multibyte support, it's the remembered rawsize
	 * ----------
	 */
	return PGLZ_RAW_SIZE(lz);
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

	/* ----------
	 * Return the varsize minus the VARSIZE field itself.
	 * ----------
	 */
	return VARSIZE(lz) - VARHDRSZ;
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
	lztext		   *result;
	int32			rawsize;
	lztext		   *tmp;
	int				tmp_size;
	char		   *str;

	/* ----------
	 * Handle NULL
	 * ----------
	 */
	if (txt == NULL)
		return NULL;

	/* ----------
	 * Determine input size and eventually tuple size
	 * ----------
	 */
	rawsize  = VARSIZE(txt) - VARHDRSZ;
	str      = VARDATA(txt);
	tmp_size = PGLZ_MAX_OUTPUT(rawsize);

	/* ----------
	 * Allocate a temporary result and compress into it
	 * ----------
	 */
	tmp = (lztext *) palloc(tmp_size);
	pglz_compress(str, rawsize, tmp, NULL);

	/* ----------
	 * If we miss less than 25% bytes at the end of the temp value,
	 * so be it. Therefore we save a palloc()/memcpy()/pfree()
	 * sequence.
	 * ----------
	 */
	if (tmp_size - tmp->varsize < 256 || 
					tmp_size - tmp->varsize < tmp_size / 4)
	{
		result = tmp;
	} else {
		result = (lztext *) palloc(tmp->varsize);
		memcpy(result, tmp, tmp->varsize);
		pfree(tmp);
	}

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

	/* ----------
	 * Handle NULL
	 * ----------
	 */
	if (lz == NULL)
		return NULL;

	/* ----------
	 * Allocate and initialize the text result
	 * ----------
	 */
	result = (text *) palloc(PGLZ_RAW_SIZE(lz) + VARHDRSZ + 1);
	VARSIZE(result) = lz->rawsize + VARHDRSZ;

	/* ----------
	 * Decompress directly into the text data area.
	 * ----------
	 */
	VARDATA(result)[lz->rawsize] = 0;
	pglz_decompress(lz, VARDATA(result));

	return result;
}


