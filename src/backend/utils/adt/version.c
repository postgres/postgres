/*-------------------------------------------------------------------------
 *
 * version.c
 *	 Returns the version string
 *
 * IDENTIFICATION
 *
 * $Header: /cvsroot/pgsql/src/backend/utils/adt/version.c,v 1.6.2.1 1999/08/02 05:24:58 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */


#include "postgres.h"
#include "version.h"


text	   *version(void);

text *
version(void)
{
	int			n = strlen(PG_VERSION_STR) + VARHDRSZ;
	text	   *ret = (text *) palloc(n);

	VARSIZE(ret) = n;
	memcpy(VARDATA(ret), PG_VERSION_STR, strlen(PG_VERSION_STR));

	return ret;
}
