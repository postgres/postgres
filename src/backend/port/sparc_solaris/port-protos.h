/*-------------------------------------------------------------------------
 *
 * port-protos.h--
 *	  port-specific prototypes for SunOS 4
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: port-protos.h,v 1.7 1997/09/18 16:09:14 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PORT_PROTOS_H
#define PORT_PROTOS_H

#include <netinet/in.h>			/* For struct in_addr */
#include <arpa/inet.h>

#include <dlfcn.h>

#include "fmgr.h"				/* for func_ptr */
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
#define pg_dlsym		dlsym
#define pg_dlclose		dlclose
#define pg_dlerror		dlerror

/* port.c */
#ifndef HAVE_RANDOM
extern long random(void);
#endif
#ifndef HAVE_SRANDOM
extern void srandom(int seed);
#endif

/* inet_aton.c in backend/port directory */
extern int	inet_aton(const char *cp, struct in_addr * addr);

/* In system library, but can't find prototype in system library .h files */
extern int	gethostname(char *name, int namelen);

/* In system library, but can't find prototype in system library .h files */
#include <sys/resource.h>
extern int	getrusage(int who, struct rusage * rusage);

#endif							/* PORT_PROTOS_H */
