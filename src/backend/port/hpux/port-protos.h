/*-------------------------------------------------------------------------
 *
 * port-protos.h--
 *	  port-specific prototypes for HP-UX
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: port-protos.h,v 1.6 1998/09/01 04:31:17 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PORT_PROTOS_H
#define PORT_PROTOS_H

#include <sys/resource.h>		/* for struct rusage */
#include <dl.h>					/* for shl_t */

#include "utils/dynamic_loader.h"

/* dynloader.c */

/* pg_dl{open,close,sym} prototypes are in utils/dynamic_loader.h */

/* port.c */

extern int	init_address_fixup(void);
extern double rint(double x);
extern double cbrt(double x);

#endif	 /* PORT_PROTOS_H */
