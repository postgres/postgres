/* $Header: /cvsroot/pgsql/src/backend/port/dynloader/darwin.h,v 1.4 2001/03/22 03:59:42 momjian Exp $ */

#include "fmgr.h"

void	   *pg_dlopen(char *filename);
PGFunction	pg_dlsym(void *handle, char *funcname);
void		pg_dlclose(void *handle);
char	   *pg_dlerror(void);
