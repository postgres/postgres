/* $Header: /cvsroot/pgsql/src/backend/port/dynloader/darwin.h,v 1.1 2000/10/31 19:55:19 petere Exp $ */
void	   *pg_dlopen(char *filename);
PGFunction	pg_dlsym(void *handle, char *funcname);
void		pg_dlclose(void *handle);
char	   *pg_dlerror();
