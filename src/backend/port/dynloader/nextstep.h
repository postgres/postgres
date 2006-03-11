/*-------------------------------------------------------------------------
 *
 * port_protos.h
 *	  port-specific prototypes for NeXT
 *
 * $PostgreSQL: pgsql/src/backend/port/dynloader/nextstep.h,v 1.9 2006/03/11 04:38:31 momjian Exp $
 */

#ifndef PORT_PROTOS_H
#define PORT_PROTOS_H

#include "utils/dynamic_loader.h"

void	   *next_dlopen(char *name);
int			next_dlclose(void *handle);
void	   *next_dlsym(void *handle, char *symbol);
char	   *next_dlerror(void);

#define pg_dlopen(f)	next_dlopen
#define pg_dlsym		next_dlsym
#define pg_dlclose		next_dlclose
#define pg_dlerror		next_dlerror

/* port.c */

#endif   /* PORT_PROTOS_H */
