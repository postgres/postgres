/* ----------
 * lztext.c -
 *
 * $Header: /cvsroot/pgsql/src/backend/utils/adt/Attic/lztext.c,v 1.1 1999/11/17 21:21:50 wieck Exp $
 *
 *	Text type with internal LZ compressed representation. Uses the
 *	standard PostgreSQL compression method.
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
	 * Determine input size and eventually tuple size
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
	 * If we miss less than x% bytes at the end of the temp value,
	 * so be it. Therefore we save a memcpy().
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
	/* ----------
	 * Handle NULL
	 * ----------
	 */
	if (lz == NULL)
		return 0;

	/* ----------
	 * without multibyte support, it's the remembered rawsize
	 * ----------
	 */
	return lz->rawsize;
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
	return lz->varsize - sizeof(int32);
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
	 * If we miss less than x% bytes at the end of the temp value,
	 * so be it. Therefore we save a memcpy().
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
	result = (text *) palloc(lz->rawsize + VARHDRSZ + 1);
	VARSIZE(result) = lz->rawsize + VARHDRSZ;

	/* ----------
	 * Decompress directly into the text data area.
	 * ----------
	 */
	pglz_decompress(lz, VARDATA(result));
	VARDATA(result)[lz->rawsize] = 0;

	return result;
}


