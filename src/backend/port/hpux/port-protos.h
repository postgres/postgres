/*-------------------------------------------------------------------------
 *
 * port_protos.h
 *	  port-specific prototypes for HP-UX
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: port-protos.h,v 1.10 2001/01/24 19:43:04 momjian Exp $
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
