/* $Header: /cvsroot/pgsql/src/backend/port/dynloader/solaris.h,v 1.2 2001/02/10 02:31:26 tgl Exp $ */

#ifndef DYNLOADER_SOLARIS_H
#define DYNLOADER_SOLARIS_H

#include <dlfcn.h>
#include "utils/dynamic_loader.h"

#define pg_dlopen(f)	dlopen(f,1)
#define pg_dlsym		dlsym
#define pg_dlclose		dlclose
#define pg_dlerror		dlerror

#endif /* DYNLOADER_SOLARIS_H */
