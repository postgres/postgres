/*-------------------------------------------------------------------------
 *
 * alpha.h
 *	  prototypes for OSF/1-specific routines
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: osf.h,v 1.1 2000/10/10 21:08:04 petere Exp $
 *
 *-------------------------------------------------------------------------
 */

#ifndef PORT_PROTOS_H
#define PORT_PROTOS_H

#include <dlfcn.h>
#include "utils/dynamic_loader.h"

/* dynloader.c */

/*
 * Dynamic Loader on Alpha OSF/1.x
 *
 * this dynamic loader uses the system dynamic loading interface for shared
 * libraries (ie. dlopen/dlsym/dlclose). The user must specify a shared
 * library as the file to be dynamically loaded.
 *
 */
#define  pg_dlopen(f)	dlopen(f, RTLD_LAZY)
#define  pg_dlsym(h, f) ((PGFunction) dlsym(h, f))
#define  pg_dlclose(h)	dlclose(h)
#define  pg_dlerror()	dlerror()

#endif	 /* PORT_PROTOS_H */
