/* $Header: /cvsroot/pgsql/src/backend/port/dynloader/darwin.h,v 1.3 2000/12/11 00:49:54 tgl Exp $ */

#include "fmgr.h"

void*		pg_dlopen(char *filename);
PGFunction	pg_dlsym(void *handle, char *funcname);
void		pg_dlclose(void *handle);
char*		pg_dlerror(void);
