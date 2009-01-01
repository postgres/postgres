/*-------------------------------------------------------------------------
 *
 * bsdi.h
 *		Dynamic loader interface for BSD/OS
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/backend/port/dynloader/bsdi.h,v 1.26 2009/01/01 17:23:46 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PORT_PROTOS_H
#define PORT_PROTOS_H

#include "utils/dynamic_loader.h"


#ifdef HAVE_DLOPEN

#include <dlfcn.h>

/*
 * In some older systems, the RTLD_NOW flag isn't defined and the mode
 * argument to dlopen must always be 1.  The RTLD_GLOBAL flag is wanted
 * if available, but it doesn't exist everywhere.
 * If it doesn't exist, set it to 0 so it has no effect.
 */
#ifndef RTLD_NOW
#define RTLD_NOW 1
#endif
#ifndef RTLD_GLOBAL
#define RTLD_GLOBAL 0
#endif

#define		  pg_dlopen(f)	  dlopen((f), RTLD_NOW | RTLD_GLOBAL)
#define		  pg_dlsym		  dlsym
#define		  pg_dlclose	  dlclose
#define		  pg_dlerror	  dlerror
#else							/* not HAVE_DLOPEN */

#define pg_dlsym(handle, funcname)	  ((PGFunction) dld_get_func((funcname)))
#define pg_dlclose(handle) \
do { \
	dld_unlink_by_file(handle, 1); \
	free(handle); \
} while (0)
#endif   /* not HAVE_DLOPEN */

#endif   /* PORT_PROTOS_H */
