/* $Header: /cvsroot/pgsql/src/backend/port/dynloader/solaris.h,v 1.6 2001/10/28 06:25:47 momjian Exp $ */

#ifndef DYNLOADER_SOLARIS_H
#define DYNLOADER_SOLARIS_H

#include <dlfcn.h>
#include "utils/dynamic_loader.h"

#define pg_dlopen(f)	dlopen((f), RTLD_LAZY | RTLD_GLOBAL)
#define pg_dlsym		dlsym
#define pg_dlclose		dlclose
#define pg_dlerror		dlerror

#endif	 /* DYNLOADER_SOLARIS_H */
