/*
 * Dynamic loading support for macOS (Darwin)
 *
 * src/backend/port/dynloader/darwin.c
 */
#include "postgres.h"

#include <dlfcn.h>

#include "dynloader.h"


void *
pg_dlopen(const char *filename)
{
	return dlopen(filename, RTLD_NOW | RTLD_GLOBAL);
}

void
pg_dlclose(void *handle)
{
	dlclose(handle);
}

PGFunction
pg_dlsym(void *handle, const char *funcname)
{
	return dlsym(handle, funcname);
}

char *
pg_dlerror(void)
{
	return dlerror();
}
