/* $Header: /cvsroot/pgsql/src/backend/port/dynloader/win32.c,v 1.2 2003/08/04 00:43:21 momjian Exp $ */

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
