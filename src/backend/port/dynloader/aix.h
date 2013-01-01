/*-------------------------------------------------------------------------
 *
 * aix.h
 *	  prototypes for AIX-specific routines
 *
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/port/dynloader/aix.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef PORT_PROTOS_H
#define PORT_PROTOS_H

#include <dlfcn.h>
#include "utils/dynamic_loader.h"		/* pgrminclude ignore */

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

#define  pg_dlopen(f)	dlopen((f), RTLD_NOW | RTLD_GLOBAL)
#define  pg_dlsym(h, f) ((PGFunction) dlsym(h, f))
#define  pg_dlclose(h)	dlclose(h)
#define  pg_dlerror()	dlerror()

#endif   /* PORT_PROTOS_H */
