/*-------------------------------------------------------------------------
 *
 * port-protos.h--
 *	  prototypes for OSF/1-specific routines
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: port-protos.h,v 1.2 1997/09/07 04:45:20 momjian Exp $
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
#define  pg_dlsym(h, f) ((func_ptr)dlsym(h, f))
#define  pg_dlclose(h)	dlclose(h)
#define  pg_dlerror()	dlerror()

/* port.c */

extern void		init_address_fixup(void);

#endif							/* PORT_PROTOS_H */
