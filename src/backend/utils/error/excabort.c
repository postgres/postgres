/*-------------------------------------------------------------------------
 *
 * excabort.c--
 *	  Default exception abort code.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/error/Attic/excabort.c,v 1.5 1998/05/29 17:00:16 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "utils/exc.h"			/* where function declarations go */

void
ExcAbort(const Exception *excP,
		 ExcDetail detail,
		 ExcData data,
		 ExcMessage message)
{
	/* dump core */
	abort();
}
