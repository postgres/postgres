/*-------------------------------------------------------------------------
 *
 * version.c--
 *	 Returns the version string
 *
 * IDENTIFICATION
 *
 * $Header: /cvsroot/pgsql/src/backend/utils/adt/version.c,v 1.4 1998/10/12 05:09:55 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include <string.h>

#include "postgres.h"
#include "version.h"


text	   *version(void);

text *
version(void)
{
	int			n = strlen(PG_VERSION_STR) + VARHDRSZ;
	text	   *ret = (text *) palloc(n);

	VARSIZE(ret) = n;
	StrNCpy(VARDATA(ret), PG_VERSION_STR, strlen(PG_VERSION_STR));

	return ret;
}
