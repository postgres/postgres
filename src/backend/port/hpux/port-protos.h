/*-------------------------------------------------------------------------
 *
 * port_protos.h
 *	  port-specific prototypes for HP-UX
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: port-protos.h,v 1.9 2000/01/26 05:56:46 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PORT_PROTOS_H
#define PORT_PROTOS_H

#include <sys/resource.h>
#include "dl.h"

#include "utils/dynamic_loader.h"

/* dynloader.c */

/* pg_dl{open,close,sym} prototypes are in utils/dynamic_loader.h */

/* port.c */

extern int	init_address_fixup(void);
extern double rint(double x);
extern double cbrt(double x);

#endif	 /* PORT_PROTOS_H */
