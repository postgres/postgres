/* $PostgreSQL: pgsql/src/backend/port/dynloader/win32.c,v 1.3 2003/11/29 19:51:54 pgsql Exp $ */

#include <windows.h>

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
