/*-------------------------------------------------------------------------
 *
 * port_protos.h
 *	  port-specific prototypes for SunOS 4
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * port_protos.h,v 1.2 1995/05/25 22:51:03 andrew Exp
 *
 *-------------------------------------------------------------------------
 */
#ifndef PORT_PROTOS_H
#define PORT_PROTOS_H

#include "fmgr.h"
#include "utils/dynamic_loader.h"

/* dynloader.c */

#ifndef  PRE_BSDI_2_1
#include <dlfcn.h>
#define		  pg_dlopen(f)	  dlopen(f, RTLD_LAZY)
#define		  pg_dlsym		  dlsym
#define		  pg_dlclose	  dlclose
#define		  pg_dlerror	  dlerror
#else
#define pg_dlsym(handle, funcname)	  ((func_ptr) dld_get_func((funcname)))
#define pg_dlclose(handle)			  ({ dld_unlink_by_file(handle, 1); free(handle); })
#endif

/* port.c */

#endif	 /* PORT_PROTOS_H */
