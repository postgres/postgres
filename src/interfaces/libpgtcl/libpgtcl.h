/*-------------------------------------------------------------------------
 *
 * libpgtcl.h--
 *    libpgtcl is a tcl package for front-ends to interface with pglite
 *   It's the tcl equivalent of the old libpq C interface.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: libpgtcl.h,v 1.1.1.1 1996/07/09 06:22:16 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */

#ifndef LIBPGTCL_H
#define LIBPGTCL_H

#include "tcl.h"

extern int Pg_Init (Tcl_Interp *interp);

#endif /* LIBPGTCL_H */
