/*-------------------------------------------------------------------------
 *
 * excabort.c--
 *    Default exception abort code.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/utils/error/Attic/excabort.c,v 1.2 1996/11/03 06:53:32 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "utils/exc.h"		/* where function declarations go */

void
ExcAbort(const Exception *excP, 
	 ExcDetail detail, 
	 ExcData data,
	 ExcMessage message)
{
#ifdef	__SABER__
    saber_stop();
#else
    /* dump core */
    abort();
#endif
}
