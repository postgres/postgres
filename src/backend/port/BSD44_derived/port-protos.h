/*-------------------------------------------------------------------------
 *
 * port-protos.h--
 *    port-specific prototypes for NetBSD 1.0
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: port-protos.h,v 1.3 1996/10/31 11:09:37 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PORT_PROTOS_H
#define PORT_PROTOS_H

#include <sys/types.h>
#include <nlist.h>
#include <link.h>

#include "postgres.h"

#include "fmgr.h"			/* for func_ptr */
#include "utils/dynamic_loader.h"

/* dynloader.c */
/*
 * Dynamic Loader on NetBSD 1.0.
 *
 * this dynamic loader uses the system dynamic loading interface for shared 
 * libraries (ie. dlopen/dlsym/dlclose). The user must specify a shared
 * library as the file to be dynamically loaded.
 *
 * agc - I know this is all a bit crufty, but it does work, is fairly
 * portable, and works (the stipulation that the d.l. function must
 * begin with an underscore is fairly tricky, and some versions of
 * NetBSD (like 1.0, and 1.0A pre June 1995) have no dlerror.)
 */
#define	pg_dlopen(f)	BSD44_derived_dlopen(f, 1)
#define	pg_dlsym	BSD44_derived_dlsym
#define	pg_dlclose	BSD44_derived_dlclose
#define	pg_dlerror	BSD44_derived_dlerror

char *		BSD44_derived_dlerror(void);
void *		BSD44_derived_dlopen(const char *filename, int num);
void *		BSD44_derived_dlsym(void *handle, const char *name);
void *		BSD44_derived_dlsym(void *handle, const char *name);
void		BSD44_derived_dlclose(void *handle);

#endif /* PORT_PROTOS_H */
