/*-------------------------------------------------------------------------
 *
 * libpgtcl.h--
 *
 *	libpgtcl is a tcl package for front-ends to interface with PostgreSQL.
 *	It's a Tcl wrapper for libpq.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: libpgtcl.h,v 1.6 1998/09/21 01:01:58 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#ifndef LIBPGTCL_H
#define LIBPGTCL_H

#include "tcl.h"

extern int	Pgtcl_Init(Tcl_Interp * interp);
extern int	Pgtcl_SafeInit(Tcl_Interp * interp);

#endif	 /* LIBPGTCL_H */
