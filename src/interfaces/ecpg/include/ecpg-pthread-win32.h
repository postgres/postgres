/* $PostgreSQL: pgsql/src/interfaces/ecpg/include/ecpg-pthread-win32.h,v 1.1 2007/03/29 12:02:24 meskes Exp $ */
/*
 * pthread mapping macros for win32 native thread implementation
 */
#ifndef _ECPG_PTHREAD_WIN32_H
#define _ECPG_PTHREAD_WIN32_H
#define pthread_mutex_lock(x) do { \
	if (*x == INVALID_HANDLE_VALUE) \
	   *x = CreateMutex(NULL, FALSE, NULL); \
    WaitForSingleObject(*x, INFINITE); \
} while (0);
#define pthread_mutex_unlock(x) ReleaseMutex(*x)
#define pthread_getspecific(x) TlsGetValue(x)
#define pthread_setspecific(x,y) TlsSetValue(x,y)
#define pthread_key_create(x,y) *x = TlsAlloc();
#endif
