/* $PostgreSQL: pgsql/src/backend/port/dynloader/win32.c,v 1.5 2004/12/02 19:38:50 momjian Exp $ */

#include <windows.h>

char *dlerror(void);
int dlclose(void *handle);
void *dlsym(void *handle, const char *symbol);
void *dlopen(const char *path, int mode);

char *
dlerror(void)
{
	return "dynamic load error";
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
