/* $PostgreSQL: pgsql/src/backend/port/dynloader/win32.c,v 1.4 2004/11/17 08:30:08 neilc Exp $ */

#include <windows.h>

char *dlerror(void);
int dlclose(void *handle);
void *dlsym(void *handle, const char *symbol);
void *dlopen(const char *path, int mode);

char *
dlerror(void)
{
	return "error";
}

int
dlclose(void *handle)
{
	return FreeLibrary((HMODULE) handle) ? 0 : 1;
}

void *
dlsym(void *handle, const char *symbol)
{
	return (void *) GetProcAddress((HMODULE) handle, symbol);
}

void *
dlopen(const char *path, int mode)
{
	return (void *) LoadLibrary(path);
}
