/* $Header: /cvsroot/pgsql/src/backend/port/dynloader/solaris.h,v 1.1 2000/10/10 21:22:23 petere Exp $ */

#ifndef DYNLOADER_SOLARIS_H
#define DYNLOADER_SOLARIS_H

#include "config.h"
#include <dlfcn.h>
#include "fmgr.h"
#include "utils/dynamic_loader.h"

#define pg_dlopen(f)	dlopen(f,1)
#define pg_dlsym		dlsym
#define pg_dlclose		dlclose
#define pg_dlerror		dlerror

#endif /* DYNLOADER_SOLARIS_H */
