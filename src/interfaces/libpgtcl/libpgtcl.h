/*-------------------------------------------------------------------------
 *
 * libpgtcl.h--
 *	  libpgtcl is a tcl package for front-ends to interface with pglite
 *	 It's the tcl equivalent of the old libpq C interface.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: libpgtcl.h,v 1.5 1998/09/01 04:39:53 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#ifndef LIBPGTCL_H
#define LIBPGTCL_H

#include "tcl.h"

extern int	Pgtcl_Init(Tcl_Interp * interp);
extern int	Pgtcl_SafeInit(Tcl_Interp * interp);

#endif	 /* LIBPGTCL_H */
