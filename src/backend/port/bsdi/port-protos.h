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

#include "fmgr.h"			/* for func_ptr */
#include "utils/dynamic_loader.h"

/* dynloader.c */

#define SAVE_MAXPATHLEN MAXPATHLEN
#undef MAXPATHLEN	/* prevent compiler warning */
#include <sys/param.h>
#undef MAXPATHLEN
#define MAXPATHLEN SAVE_MAXPATHLEN 
#undef SAVE_MAXPATHLEN

#if _BSDI_VERSION >= 199510
#  include <dlfcn.h>
#  define	pg_dlopen(f)	dlopen(f, 1)
#  define	pg_dlsym	dlsym
#  define	pg_dlclose	dlclose
#  define	pg_dlerror	dlerror
#else
#  define pg_dlsym(handle, funcname)	((func_ptr) dld_get_func((funcname)))
#  define pg_dlclose(handle)		({ dld_unlink_by_file(handle, 1); free(handle); })
#endif

/* port.c */

#endif /* PORT_PROTOS_H */
