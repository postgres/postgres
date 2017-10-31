/* src/backend/port/dynloader/darwin.h */

#include "fmgr.h"

void	   *pg_dlopen(const char *filename);
PGFunction	pg_dlsym(void *handle, const char *funcname);
void		pg_dlclose(void *handle);
char	   *pg_dlerror(void);
