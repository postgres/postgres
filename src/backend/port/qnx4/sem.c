/*-------------------------------------------------------------------------
 *
 * sem.c
 *	  System V Semaphore Emulation
 *
 * Copyright (c) 1999, repas AEG Automation GmbH
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/port/qnx4/Attic/sem.c,v 1.4 2001/02/02 18:21:58 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include <errno.h>
#include <semaphore.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include "postgres.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include <sys/sem.h>


#define SETMAX	((MAXBACKENDS + PROC_NSEMS_PER_SET - 1) / PROC_NSEMS_PER_SET)
#define SEMMAX	(PROC_NSEMS_PER_SET+1)
#define OPMAX	8

#define MODE	0700
#define SHM_INFO_NAME	"SysV_Sem_Info"


struct pending_ops
{
	int			op[OPMAX];		/* array of pending operations */
	int			idx;			/* index of first free array member */
};

struct sem_info
{
	sem_t		sem;
	struct
	{
		key_t		key;
		int			nsems;
		sem_t		sem[SEMMAX];/* array of POSIX semaphores */
		struct sem	semV[SEMMAX];		/* array of System V semaphore
										 * structures */
		struct pending_ops pendingOps[SEMMAX];	/* array of pending
												 * operations */
	}			set[SETMAX];
};

static struct sem_info *SemInfo = (struct sem_info *) - 1;


int
semctl(int semid, int semnum, int cmd, /* ... */ union semun arg)
{
	int			r = 0;

	sem_wait(&SemInfo->sem);

	if (semid < 0 || semid >= SETMAX ||
		semnum < 0 || semnum >= SemInfo->set[semid].nsems)
	{
		sem_post(&SemInfo->sem);
		errno = EINVAL;
		return -1;
	}

	switch (cmd)
	{
		case GETNCNT:
			r = SemInfo->set[semid].semV[semnum].semncnt;
			break;

		case GETPID:
			r = SemInfo->set[semid].semV[semnum].sempid;
			break;

		case GETVAL:
			r = SemInfo->set[semid].semV[semnum].semval;
			break;

		case GETALL:
			for (semnum = 0; semnum < SemInfo->set[semid].nsems; semnum++)
				arg.array[semnum] = SemInfo->set[semid].semV[semnum].semval;
			break;

		case SETVAL:
			SemInfo->set[semid].semV[semnum].semval = arg.val;
			break;

		case SETALL:
			for (semnum = 0; semnum < SemInfo->set[semid].nsems; semnum++)
				SemInfo->set[semid].semV[semnum].semval = arg.array[semnum];
			break;

		case GETZCNT:
			r = SemInfo->set[semid].semV[semnum].semzcnt;
			break;

		case IPC_RMID:
			for (semnum = 0; semnum < SemInfo->set[semid].nsems; semnum++)
			{
				if (sem_destroy(&SemInfo->set[semid].sem[semnum]) == -1)
					r = -1;
			}
			SemInfo->set[semid].key = -1;
			SemInfo->set[semid].nsems = 0;
			break;

		default:
			sem_post(&SemInfo->sem);
			errno = EINVAL;
			return -1;
	}

	sem_post(&SemInfo->sem);

	return r;
}

int
semget(key_t key, int nsems, int semflg)
{
	int			fd,
				semid,
				semnum /* , semnum1 */ ;
	int			exist = 0;

	if (nsems < 0 || nsems > SEMMAX)
	{
		errno = EINVAL;
		return -1;
	}

	/* open and map shared memory */
	if (SemInfo == (struct sem_info *) - 1)
	{
		/* test if the shared memory already exists */
		fd = shm_open(SHM_INFO_NAME, O_RDWR | O_CREAT | O_EXCL, MODE);
		if (fd == -1 && errno == EEXIST)
		{
			exist = 1;
			fd = shm_open(SHM_INFO_NAME, O_RDWR | O_CREAT, MODE);
		}
		if (fd == -1)
			return fd;
		/* The size may only be set once. Ignore errors. */
		ltrunc(fd, sizeof(struct sem_info), SEEK_SET);
		SemInfo = mmap(NULL, sizeof(struct sem_info),
					   PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (SemInfo == MAP_FAILED)
			return -1;
		if (!exist)
		{
			/* create semaphore for locking */
			sem_init(&SemInfo->sem, 1, 1);
			sem_wait(&SemInfo->sem);
			/* initilize shared memory */
			memset(SemInfo->set, 0, sizeof(SemInfo->set));
			for (semid = 0; semid < SETMAX; semid++)
				SemInfo->set[semid].key = -1;
			sem_post(&SemInfo->sem);
		}
	}

	sem_wait(&SemInfo->sem);

	if (key != IPC_PRIVATE)
	{
		/* search existing element */
		semid = 0;
		while (semid < SETMAX && SemInfo->set[semid].key != key)
			semid++;
		if (!(semflg & IPC_CREAT) && semid >= SETMAX)
		{
			sem_post(&SemInfo->sem);
			errno = ENOENT;
			return -1;
		}
		else if (semid < SETMAX)
		{
			if (semflg & IPC_CREAT && semflg & IPC_EXCL)
			{
				sem_post(&SemInfo->sem);
				errno = EEXIST;
				return -1;
			}
			else
			{
				if (nsems != 0 && SemInfo->set[semid].nsems < nsems)
				{
					sem_post(&SemInfo->sem);
					errno = EINVAL;
					return -1;
				}
				sem_post(&SemInfo->sem);
				return semid;
			}
		}
	}

	/* search first free element */
	semid = 0;
	while (semid < SETMAX && SemInfo->set[semid].key != -1)
		semid++;
	if (semid >= SETMAX)
	{
		sem_post(&SemInfo->sem);
		errno = ENOSPC;
		return -1;
	}

	for (semnum = 0; semnum < nsems; semnum++)
	{
		sem_init(&SemInfo->set[semid].sem[semnum], 1, 0);
/* Currently sem_init always returns -1.
	if( sem_init( &SemInfo->set[semid].sem[semnum], 1, 0 ) == -1 )	{
	  for( semnum1 = 0; semnum1 < semnum; semnum1++ )  {
		sem_destroy( &SemInfo->set[semid].sem[semnum1] );
	  }
	  sem_post( &SemInfo->sem );
	  return -1;
	}
*/
	}

	SemInfo->set[semid].key = key;
	SemInfo->set[semid].nsems = nsems;

	sem_post(&SemInfo->sem);

	return semid;
}

int
semop(int semid, struct sembuf * sops, size_t nsops)
{
	int			i,
				r = 0,
				r1,
				errno1 = 0,
				op;

	sem_wait(&SemInfo->sem);

	if (semid < 0 || semid >= SETMAX)
	{
		sem_post(&SemInfo->sem);
		errno = EINVAL;
		return -1;
	}
	for (i = 0; i < nsops; i++)
	{
		if ( /* sops[i].sem_num < 0 || */ sops[i].sem_num >= SemInfo->set[semid].nsems)
		{
			sem_post(&SemInfo->sem);
			errno = EFBIG;
			return -1;
		}
	}

	for (i = 0; i < nsops; i++)
	{
		if (sops[i].sem_op < 0)
		{
			if (SemInfo->set[semid].semV[sops[i].sem_num].semval < -sops[i].sem_op)
			{
				if (sops[i].sem_flg & IPC_NOWAIT)
				{
					sem_post(&SemInfo->sem);
					errno = EAGAIN;
					return -1;
				}
				SemInfo->set[semid].semV[sops[i].sem_num].semncnt++;
				if (SemInfo->set[semid].pendingOps[sops[i].sem_num].idx >= OPMAX)
				{
					/* pending operations array overflow */
					sem_post(&SemInfo->sem);
					errno = ERANGE;
					return -1;
				}
				SemInfo->set[semid].pendingOps[sops[i].sem_num].op[SemInfo->set[semid].pendingOps[sops[i].sem_num].idx++] = sops[i].sem_op;
				/* suspend */
				sem_post(&SemInfo->sem);		/* avoid deadlock */
				r1 = sem_wait(&SemInfo->set[semid].sem[sops[i].sem_num]);
				sem_wait(&SemInfo->sem);
				if (r1)
				{
					errno1 = errno;
					r = r1;
					/* remove pending operation */
					SemInfo->set[semid].pendingOps[sops[i].sem_num].op[--SemInfo->set[semid].pendingOps[sops[i].sem_num].idx] = 0;
				}
				else
					SemInfo->set[semid].semV[sops[i].sem_num].semval -= -sops[i].sem_op;
				SemInfo->set[semid].semV[sops[i].sem_num].semncnt--;
			}
			else
				SemInfo->set[semid].semV[sops[i].sem_num].semval -= -sops[i].sem_op;
		}
		else if (sops[i].sem_op > 0)
		{
			SemInfo->set[semid].semV[sops[i].sem_num].semval += sops[i].sem_op;
			op = sops[i].sem_op;
			while (op > 0 && SemInfo->set[semid].pendingOps[sops[i].sem_num].idx > 0)
			{					/* operations pending */
				if (SemInfo->set[semid].pendingOps[sops[i].sem_num].op[SemInfo->set[semid].pendingOps[sops[i].sem_num].idx - 1] + op >= 0)
				{
					/* unsuspend processes */
					if (sem_post(&SemInfo->set[semid].sem[sops[i].sem_num]))
					{
						errno1 = errno;
						r = -1;
					}
					/* adjust pending operations */
					op += SemInfo->set[semid].pendingOps[sops[i].sem_num].op[--SemInfo->set[semid].pendingOps[sops[i].sem_num].idx];
					SemInfo->set[semid].pendingOps[sops[i].sem_num].op[SemInfo->set[semid].pendingOps[sops[i].sem_num].idx] = 0;
				}
				else
				{
					/* adjust pending operations */
					SemInfo->set[semid].pendingOps[sops[i].sem_num].op[SemInfo->set[semid].pendingOps[sops[i].sem_num].idx - 1] += op;
					op = 0;
				}
			}
		}
		else
			/* sops[i].sem_op == 0 */
		{
			/* not supported */
			sem_post(&SemInfo->sem);
			errno = ENOSYS;
			return -1;
		}
		SemInfo->set[semid].semV[sops[i].sem_num].sempid = getpid();
	}

	sem_post(&SemInfo->sem);

	errno = errno1;
	return r;
}
