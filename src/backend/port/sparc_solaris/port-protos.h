/*-------------------------------------------------------------------------
 *
 * port-protos.h--
 *    port-specific prototypes for SunOS 4
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: port-protos.h,v 1.2 1996/11/27 08:15:23 bryanh Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PORT_PROTOS_H
#define PORT_PROTOS_H

#include <dlfcn.h>

#include "config.h"   /* For MAXPATHLEN */
#include "fmgr.h"			/* for func_ptr */
#include "utils/dynamic_loader.h"

/* dynloader.c */
/*
 * Dynamic Loader on SunOS 4.
 *
 * this dynamic loader uses the system dynamic loading interface for shared 
 * libraries (ie. dlopen/dlsym/dlclose). The user must specify a shared
 * library as the file to be dynamically loaded.
 *
 */
#define pg_dlopen(f)	dlopen(f,1)
#define	pg_dlsym	dlsym
#define	pg_dlclose	dlclose
#define	pg_dlerror	dlerror

/* port.c */
extern long random(void);
extern void srandom(int seed);

/* inet_aton.c in backend/port directory */
extern int inet_aton(const char *cp, struct in_addr *addr);

/* In system library, but can't find prototype in system library .h files */
extern int gethostname(char *name, int namelen);


#endif /* PORT_PROTOS_H */
