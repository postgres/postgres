/*-------------------------------------------------------------------------
*
* pthread-win32.c
*	 partial pthread implementation for win32
*
* Copyright (c) 2004-2008, PostgreSQL Global Development Group
* IDENTIFICATION
*	$PostgreSQL: pgsql/src/interfaces/libpq/pthread-win32.c,v 1.16 2008/05/16 18:30:53 mha Exp $
*
*-------------------------------------------------------------------------
*/

#include "postgres_fe.h"

#include <windows.h>
#include "pthread-win32.h"

DWORD
pthread_self(void)
{
	return GetCurrentThreadId();
}

void
pthread_setspecific(pthread_key_t key, void *val)
{
}

void *
pthread_getspecific(pthread_key_t key)
{
	return NULL;
}

int
pthread_mutex_init(pthread_mutex_t *mp, void *attr)
{
	*mp = CreateMutex(0, 0, 0);
	if (*mp == NULL)
		return 1;
	return 0;
}

int
pthread_mutex_lock(pthread_mutex_t *mp)
{
	if (WaitForSingleObject(*mp, INFINITE) != WAIT_OBJECT_0)
		return 1;
	return 0;
}

int
pthread_mutex_unlock(pthread_mutex_t *mp)
{
	if (!ReleaseMutex(*mp))
		return 1;
	return 0;
}
