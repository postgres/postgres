/*-------------------------------------------------------------------------
 *
 * Dynamic loader interface for BSD/OS
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * port_protos.h,v 1.2 1995/05/25 22:51:03 andrew Exp
 *
 *-------------------------------------------------------------------------
 */
#ifndef PORT_PROTOS_H
#define PORT_PROTOS_H

#include "utils/dynamic_loader.h"


#ifdef HAVE_DLOPEN

#include <dlfcn.h>
#define		  pg_dlopen(f)	  dlopen((f), RTLD_LAZY | RTLD_GLOBAL)
#define		  pg_dlsym		  dlsym
#define		  pg_dlclose	  dlclose
#define		  pg_dlerror	  dlerror

#else /* not HAVE_DLOPEN */

#define pg_dlsym(handle, funcname)	  ((PGFunction) dld_get_func((funcname)))
#define pg_dlclose(handle)			  ({ dld_unlink_by_file(handle, 1); free(handle); })

#endif /* not HAVE_DLOPEN */

#endif	 /* PORT_PROTOS_H */
