/*-------------------------------------------------------------------------
 *
 * sem.c
 *	  System V Semaphore Emulation
 *
 * Copyright (c) 1999, repas AEG Automation GmbH
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/port/qnx4/Attic/sem.c,v 1.11 2002/11/08 20:23:56 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <errno.h>
#include <semaphore.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/sem.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/proc.h"


#define SEMMAX	(PROC_NSEMS_PER_SET+1)
#define OPMAX	8

#define MODE	0700
#define SHM_INFO_NAME	"PgSysV_Sem_Info"


struct pending_ops
{
	int			op[OPMAX];		/* array of pending operations */
	int			idx;			/* index of first free array member */
};

struct sem_set_info
{
	key_t		key;
	int			nsems;
	sem_t		sem[SEMMAX];	/* array of POSIX semaphores */
	struct sem	semV[SEMMAX];	/* array of System V semaphore structures */
	struct pending_ops pendingOps[SEMMAX];		/* array of pending
												 * operations */
};

struct sem_info
{
	sem_t		sem;
	int			nsets;
	/* there are actually nsets of these: */
	struct sem_set_info set[1]; /* VARIABLE LENGTH ARRAY */
};

static struct sem_info *SemInfo = (struct sem_info *) - 1;

/* ----------------------------------------------------------------
 * semclean - remove the shared memory file on exit
 *			  only called by the process which created the shm file
 * ----------------------------------------------------------------
 */

static void
semclean(void)
{
	remove("/dev/shmem/" SHM_INFO_NAME);
}

int
semctl(int semid, int semnum, int cmd, /* ... */ union semun arg)
{
	int			r = 0;

	sem_wait(&SemInfo->sem);

	if (semid < 0 || semid >= SemInfo->nsets ||
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
				semnum,
				nsets;
	int			exist = 0;
	Size		sem_info_size;
	struct stat statbuf;

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
		nsets = PROC_SEM_MAP_ENTRIES(MaxBackends);
		sem_info_size = sizeof(struct sem_info) + (nsets - 1) * sizeof(struct sem_set_info);
		ltrunc(fd, sem_info_size, SEEK_SET);
		if (fstat(fd, &statbuf))	/* would be strange : the only doc'ed */
		{						/* error is EBADF */
			close(fd);
			return -1;
		}

		/*
		 * size is rounded by proc to the next __PAGESIZE
		 */
		if (statbuf.st_size !=
			(((sem_info_size / __PAGESIZE) + 1) * __PAGESIZE))
		{
			fprintf(stderr,
					"Found a pre-existing shared memory block for the semaphore memory\n"
					"of a different size (%ld instead %ld). Make sure that all executables\n"
					"are from the same release or remove the file \"/dev/shmem/%s\"\n"
					"left by a previous version.\n",
					(long) statbuf.st_size,
					(long) sem_info_size,
					SHM_INFO_NAME);
			errno = EACCES;
			return -1;
		}
		SemInfo = mmap(NULL, sem_info_size,
					   PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (SemInfo == MAP_FAILED)
			return -1;
		if (!exist)
		{
			/* initialize shared memory */
			memset(SemInfo, 0, sem_info_size);
			SemInfo->nsets = nsets;
			for (semid = 0; semid < nsets; semid++)
				SemInfo->set[semid].key = -1;
			/* create semaphore for locking */
			sem_init(&SemInfo->sem, 1, 1);
			on_proc_exit(semclean, 0);
		}
	}

	sem_wait(&SemInfo->sem);
	nsets = SemInfo->nsets;

	if (key != IPC_PRIVATE)
	{
		/* search existing element */
		semid = 0;
		while (semid < nsets && SemInfo->set[semid].key != key)
			semid++;
		if (!(semflg & IPC_CREAT) && semid >= nsets)
		{
			sem_post(&SemInfo->sem);
			errno = ENOENT;
			return -1;
		}
		else if (semid < nsets)
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
	while (semid < nsets && SemInfo->set[semid].key != -1)
		semid++;
	if (semid >= nsets)
	{
		sem_post(&SemInfo->sem);
		errno = ENOSPC;
		return -1;
	}

	for (semnum = 0; semnum < nsems; semnum++)
	{
		sem_init(&SemInfo->set[semid].sem[semnum], 1, 0);
/* Currently sem_init always returns -1. */
#ifdef NOT_USED
		if (sem_init(&SemInfo->set[semid].sem[semnum], 1, 0) == -1)
		{
			int			semnum1;

			for (semnum1 = 0; semnum1 < semnum; semnum1++)
				sem_destroy(&SemInfo->set[semid].sem[semnum1]);
			sem_post(&SemInfo->sem);
			return -1;
		}
#endif
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

	if (semid < 0 || semid >= SemInfo->nsets)
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
