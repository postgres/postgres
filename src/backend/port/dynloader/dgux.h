/*-------------------------------------------------------------------------
 *
 * dgux.h--
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: dgux.h,v 1.4 1998/09/01 04:30:58 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PORT_PROTOS_H
#define PORT_PROTOS_H

#include <dlfcn.h>
#include "fmgr.h"				/* for func_ptr */
#include "utils/dynamic_loader.h"

/*
 * Dynamic Loader on DG/UX.
 *
 * this dynamic loader uses the system dynamic loading interface for shared
 * libraries (ie. dlopen/dlsym/dlclose). The user must specify a shared
 * library as the file to be dynamically loaded.
 *
 */
#define pg_dlopen(f)  dlopen(f,1)
#define pg_dlsym		dlsym
#define pg_dlclose		dlclose
#define pg_dlerror		dlerror

#endif	 /* PORT_PROTOS_H */
