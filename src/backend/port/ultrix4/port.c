/*-------------------------------------------------------------------------
 *
 * port.c--
 *	  Ultrix-specific routines
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/port/ultrix4/Attic/port.c,v 1.3 1997/09/07 04:47:27 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <sys/syscall.h>
#include <sys/sysmips.h>

#include "c.h"
#include "port-protos.h"

void
init_address_fixup()
{
#ifdef NOFIXADE
	syscall(SYS_sysmips, MIPS_FIXADE, 0, NULL, NULL, NULL);
#endif							/* NOFIXADE */
}
