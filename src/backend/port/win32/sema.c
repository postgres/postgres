/*-------------------------------------------------------------------------
 *
 * sema.c
 *	  Microsoft Windows Win32 Semaphores Emulation
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "storage/shmem.h"

#include <errno.h>

typedef struct
{
	int			m_numSems;
	off_t		m_semaphoreHandles;
	/* offset from beginning of header */
	off_t		m_semaphoreCounts;
	/* offset from beginning of header */
}	win32_sem_set_hdr;

/* Control of a semaphore pool. The pool is an area in which we stored all
** the semIds of the pool. The first long is the number of semaphore
** allocated in the pool followed by semaphore handles
*/

int
semctl(int semId, int semNum, int flag, union semun semun)
{
	win32_sem_set_hdr *the_set = (win32_sem_set_hdr *) MAKE_PTR(semId);

	/* semNum might be 0 */
	/* semun.array contains the sem initial values */
	int		   *sem_counts = (int *) ((off_t) the_set + the_set->m_semaphoreCounts);

	/* Fix the count of all sem of the pool to semun.array */
	if (flag == SETALL)
	{
		int			i;
		struct sembuf sops;

		sops.sem_flg = IPC_NOWAIT;

		for (i = 0; i < the_set->m_numSems; ++i)
		{
			if (semun.array[i] == sem_counts[i])
				continue;		/* Nothing to do */

			if (semun.array[i] < sem_counts[i])
				sops.sem_op = -1;
			else
				sops.sem_op = 1;

			sops.sem_num = i;

			/* Quickly lock/unlock the semaphore (if we can) */
			if (semop(semId, &sops, 1) < 0)
				return -1;
		}
		return 1;
	}

	/* Fix the count of one semaphore to semun.val */
	else if (flag == SETVAL)
	{
		if (semun.val != sem_counts[semNum])
		{
			struct sembuf sops;

			sops.sem_flg = IPC_NOWAIT;
			sops.sem_num = semNum;

			if (semun.val < sem_counts[semNum])
				sops.sem_op = -1;
			else
				sops.sem_op = 1;

			/* Quickly lock/unlock the semaphore (if we can) */
			if (semop(semId, &sops, 1) < 0)
				return -1;
		}

		return 1;
	}

	/* Delete the pool */
	else if (flag == IPC_RMID)
	{
		int			i;
		HANDLE	   *sem_handles = (HANDLE *) ((off_t) the_set + the_set->m_semaphoreHandles);

		/* Loop over all semaphore to delete them */
		for (i = 0; i < the_set->m_numSems; ++i)
			CloseHandle(sem_handles[i]);

		return 1;
	}

	/* Get the current semaphore count */
	else if (flag == GETNCNT)
		return the_set->m_numSems;

	/* Get the current semaphore count of the first semaphore in the pool */
	else if (flag == GETVAL)
		return sem_counts[semNum];

	/* Other commands not yet supported */
	else
	{
		errno = EINVAL;
		return -1;
	}
}

/* Find a pool id based on IPC key */
int
semget(int semKey, int semNum, int flags)
{
	char		semname[32];
	char		cur_num[20];
	DWORD		last_error;
	char	   *num_part;
	bool		ans = true;
	SECURITY_ATTRIBUTES sec_attrs;
	HANDLE		cur_handle;
	bool		found = false;
	Size		sem_set_size = sizeof(win32_sem_set_hdr) + semNum * (sizeof(HANDLE) + sizeof(int));
	HANDLE	   *sem_handles = NULL;
	int		   *sem_counts = NULL;
	int			i;

	sec_attrs.nLength = sizeof(sec_attrs);
	sec_attrs.lpSecurityDescriptor = NULL;
	sec_attrs.bInheritHandle = TRUE;

	sprintf(semname, "PG_SEMSET.%d.", semKey);
	num_part = semname + strlen(semname);

	strcpy(num_part, _itoa(_getpid() * -1, cur_num, 10));		/* For shared memory,
																 * include the pid */
	win32_sem_set_hdr *new_set = (win32_sem_set_hdr *) ShmemInitStruct(semname, sem_set_size, &found);

	if (found)
	{
		/* This should *never* happen */
		errno = EEXIST;
		return -1;
	}

	new_set->m_numSems = semNum;
	new_set->m_semaphoreHandles = sizeof(win32_sem_set_hdr);
	/* array starts after header */
	new_set->m_semaphoreCounts = new_set->m_semaphoreHandles + (sizeof(HANDLE) * semNum);

	sem_handles = (HANDLE *) ((off_t) new_set + new_set->m_semaphoreHandles);
	sem_counts = (int *) ((off_t) new_set + new_set->m_semaphoreCounts);

	for (i = 0; i < semNum && ans; ++i)
	{
		strcpy(num_part, _itoa(i, cur_num, 10));

		if (flags & IPC_CREAT)
			cur_handle = CreateSemaphore(&sec_attrs, 0, 1, semname);
		else
			cur_handle = OpenSemaphore(SEMAPHORE_ALL_ACCESS, TRUE, semname);

		sem_handles[i] = cur_handle;

		last_error = GetLastError();
		if (!cur_handle)
		{
			errno = EACCES;
			ans = false;
		}
		else if (last_error == ERROR_ALREADY_EXISTS && (flags & (IPC_CREAT | IPC_EXCL)))
		{
			errno = EEXIST;
			ans = false;
		}
	}

	if (ans)
		return MAKE_OFFSET(new_set);
	else
	{
		int			i;

		/* Blow away what we've got right now... */
		for (i = 0; i < semNum; ++i)
		{
			if (sem_handles[i])
				CloseHandle(sem_handles[i]);
			else
				break;
		}

		return -1;
	}
}

/* Acquire or release in the semaphore pool */
int
semop(int semId, struct sembuf * sops, int nsops)
{
	win32_sem_set_hdr *the_set = (win32_sem_set_hdr *) MAKE_PTR(semId);
	HANDLE	   *sem_handles = (HANDLE *) ((off_t) the_set + the_set->m_semaphoreHandles);
	int		   *sem_counts = (int *) ((off_t) the_set + the_set->m_semaphoreCounts);
	HANDLE		cur_handle;

	if (nsops != 1)
	{
		/*
		 * Not supported (we return on 1st success, and don't cancel
		 * earlier ops)
		 */
		errno = E2BIG;
		return -1;
	}

	cur_handle = sem_handles[sops[0].sem_num];

	if (sops[0].sem_op == -1)
	{
		DWORD		ret;

		if (sops[0].sem_flg & IPC_NOWAIT)
			ret = WaitForSingleObject(cur_handle, 0);
		else
			ret = WaitForSingleObject(cur_handle, INFINITE);

		if (ret == WAIT_OBJECT_0)
		{
			/* We got it! */
			sem_counts[sops[0].sem_num]--;
			return 0;
		}
		else if (ret == WAIT_TIMEOUT)
			/* Couldn't get it */
			errno = EAGAIN;
		else
			errno = EIDRM;
	}
	else if (sops[0].sem_op > 0)
	{
		/* Don't want the lock anymore */
		sem_counts[sops[0].sem_num]++;
		ReleaseSemaphore(cur_handle, sops[0].sem_op, NULL);
		return 0;
	}
	else
		/* Not supported */
		errno = ERANGE;

	/* If we get down here, then something is wrong */
	return -1;
}
