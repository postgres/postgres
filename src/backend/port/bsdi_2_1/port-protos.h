/*-------------------------------------------------------------------------
 *
 * port-protos.h--
 *    port-specific prototypes for SunOS 4
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * port-protos.h,v 1.2 1995/05/25 22:51:03 andrew Exp
 *
 *-------------------------------------------------------------------------
 */
#ifndef PORT_PROTOS_H
#define PORT_PROTOS_H

#include <dlfcn.h>
#include "fmgr.h"			/* for func_ptr */
#include "utils/dynamic_loader.h"

/* dynloader.c */

#define	pg_dlopen(f)	dlopen(f, 1)
#define	pg_dlsym	dlsym
#define	pg_dlclose	dlclose
#define	pg_dlerror	dlerror

/* port.c */

#endif /* PORT_PROTOS_H */
