/*-------------------------------------------------------------------------
 *
 * version.c--
 *	 Returns the version string
 *
 * IDENTIFICATION
 *
 * $Header: /cvsroot/pgsql/src/backend/utils/adt/version.c,v 1.2 1998/09/01 04:32:55 momjian Exp $
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
	strcpy(VARDATA(ret), PG_VERSION_STR);

	return ret;
}
