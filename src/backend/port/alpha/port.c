/*-------------------------------------------------------------------------
 *
 * port.c--
 *	  OSF/1-specific routines
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/port/alpha/Attic/port.c,v 1.3 1997/09/08 02:25:50 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <sys/types.h>
#include <sys/sysinfo.h>
#include <sys/proc.h>
#include "c.h"
#include "utils/elog.h"

void
init_address_fixup()
{
#ifdef NOFIXADE
	int			buffer[] = {SSIN_UACPROC, UAC_SIGBUS};

#endif							/* NOFIXADE */
#ifdef NOPRINTADE
	int			buffer[] = {SSIN_UACPROC, UAC_NOPRINT};

#endif							/* NOPRINTADE */

	if (setsysinfo(SSI_NVPAIRS, buffer, 1, (caddr_t) NULL,
				   (unsigned long) NULL) < 0)
	{
		elog(NOTICE, "setsysinfo failed: %d\n", errno);
	}
}
