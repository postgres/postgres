/* $PostgreSQL: pgsql/src/backend/port/dynloader/darwin.h,v 1.5 2003/11/29 19:51:54 pgsql Exp $ */

#include "fmgr.h"

void	   *pg_dlopen(char *filename);
PGFunction	pg_dlsym(void *handle, char *funcname);
void		pg_dlclose(void *handle);
char	   *pg_dlerror(void);
