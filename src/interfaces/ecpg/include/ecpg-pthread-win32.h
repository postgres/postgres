/* src/interfaces/ecpg/include/ecpg-pthread-win32.h */
/*
 * pthread mapping macros for win32 native thread implementation
 */
#ifndef _ECPG_PTHREAD_WIN32_H
#define _ECPG_PTHREAD_WIN32_H

#ifdef ENABLE_THREAD_SAFETY

#ifndef WIN32

#include <pthread.h>
#else

typedef struct pthread_mutex_t
{
	/* initstate = 0: not initialized; 1: init done; 2: init in progress */
	LONG		initstate;
	CRITICAL_SECTION csection;
} pthread_mutex_t;

typedef DWORD pthread_key_t;
typedef bool pthread_once_t;

#define PTHREAD_MUTEX_INITIALIZER	{ 0 }
#define PTHREAD_ONCE_INIT			false

int			pthread_mutex_init(pthread_mutex_t *, void *attr);
int			pthread_mutex_lock(pthread_mutex_t *);
int			pthread_mutex_unlock(pthread_mutex_t *);

void		win32_pthread_once(volatile pthread_once_t *once, void (*fn) (void));

#define pthread_getspecific(key) \
	TlsGetValue((key))

#define pthread_setspecific(key, value) \
	TlsSetValue((key), (value))

/* FIXME: destructor is never called in Win32. */
#define pthread_key_create(key, destructor) \
	do { *(key) = TlsAlloc(); ((void)(destructor)); } while(0)

#define pthread_once(once, fn) \
	do { \
		if (!*(once)) \
			win32_pthread_once((once), (fn)); \
	} while(0)
#endif							/* WIN32 */
#endif							/* ENABLE_THREAD_SAFETY */

#endif							/* _ECPG_PTHREAD_WIN32_H */
