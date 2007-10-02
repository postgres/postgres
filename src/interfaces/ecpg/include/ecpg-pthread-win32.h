/* $PostgreSQL: pgsql/src/interfaces/ecpg/include/ecpg-pthread-win32.h,v 1.3 2007/10/02 09:49:59 meskes Exp $ */
/*
 * pthread mapping macros for win32 native thread implementation
 */
#ifndef _ECPG_PTHREAD_WIN32_H
#define _ECPG_PTHREAD_WIN32_H

#ifdef ENABLE_THREAD_SAFETY

#ifndef WIN32

#include <pthread.h>
#define NON_EXEC_STATIC		static

#else

#define NON_EXEC_STATIC

typedef HANDLE		pthread_mutex_t;
typedef DWORD		pthread_key_t;

#define PTHREAD_MUTEX_INITIALIZER	INVALID_HANDLE_VALUE

#define pthread_mutex_lock(mutex) \
	WaitForSingleObject(*(mutex), INFINITE);

#define pthread_mutex_unlock(mutex) \
	ReleaseMutex(*(mutex))

#define pthread_getspecific(key) \
	TlsGetValue((key))

#define pthread_setspecific(key, value) \
	TlsSetValue((key), (value))

/* FIXME: destructor is never called in Win32. */
#define pthread_key_create(key, destructor) \
	do { *(key) = TlsAlloc(); ((void)(destructor)); } while(0)

/* init-once functions are always called when libecpg is loaded */
#define pthread_once(key, fn) \
	((void)0)

extern pthread_mutex_t	connections_mutex;
extern pthread_mutex_t	debug_mutex;
extern pthread_mutex_t	debug_init_mutex;
extern void auto_mem_key_init(void);
extern void ecpg_actual_connection_init(void);
extern void ecpg_sqlca_key_init(void);
extern void descriptor_key_init(void);
extern BOOL WINAPI DllMain(HANDLE module, DWORD reason, LPVOID reserved);

#endif	/* WIN32 */

#endif	/* ENABLE_THREAD_SAFETY */

#endif  /* _ECPG_PTHREAD_WIN32_H */
