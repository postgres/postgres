/*
 * $PostgreSQL: pgsql/src/backend/port/dynloader/win32.h,v 1.5 2009/06/11 14:49:00 momjian Exp $
 */
#ifndef PORT_PROTOS_H
#define PORT_PROTOS_H

#include "utils/dynamic_loader.h"

#define pg_dlopen(f)	dlopen((f), 1)
#define pg_dlsym		dlsym
#define pg_dlclose		dlclose
#define pg_dlerror		dlerror

char	   *dlerror(void);
int			dlclose(void *handle);
void	   *dlsym(void *handle, const char *symbol);
void	   *dlopen(const char *path, int mode);

#endif   /* PORT_PROTOS_H */
