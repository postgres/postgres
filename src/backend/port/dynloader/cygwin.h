/*-------------------------------------------------------------------------
 *
 * Dynamic loader declarations for Cygwin
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/backend/port/dynloader/cygwin.h,v 1.6 2006/03/05 15:58:35 momjian Exp $
 *
 *-------------------------------------------------------------------------
  */
#ifndef PORT_PROTOS_H
#define PORT_PROTOS_H

#include <dlfcn.h>
#include "utils/dynamic_loader.h"

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

#define pg_dlopen(f)	dlopen((f), RTLD_NOW | RTLD_GLOBAL)
#define pg_dlsym		dlsym
#define pg_dlclose		dlclose
#define pg_dlerror		dlerror

#endif   /* PORT_PROTOS_H */
