/* $Header: /cvsroot/pgsql/src/backend/port/dynloader/darwin.h,v 1.2 2000/11/09 19:00:50 petere Exp $ */

#include "fmgr.h"

void	   *pg_dlopen(const char *filename);
PGFunction	pg_dlsym(void *handle, const char *funcname);
void		pg_dlclose(void *handle);
const char *pg_dlerror(void);
