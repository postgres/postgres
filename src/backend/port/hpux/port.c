/*-------------------------------------------------------------------------
 *
 * port.c--
 *	  port-specific routines for HP-UX
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/port/hpux/Attic/port.c,v 1.4 1997/12/19 02:45:44 scrappy Exp $
 *
 * NOTES
 *	  For the most part, this file gets around some non-POSIX calls
 *	  in POSTGRES.
 *
 *-------------------------------------------------------------------------
 */
#include <unistd.h>				/* for rand()/srand() prototypes */
#include <math.h>				/* for pow() prototype */
#include <sys/syscall.h>		/* for syscall #defines */

#include "c.h"

void
init_address_fixup()
{

	/*
	 * On PA-RISC, unaligned access fixup is handled by the compiler, not
	 * by the kernel.
	 */
}
