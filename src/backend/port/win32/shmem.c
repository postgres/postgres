/*-------------------------------------------------------------------------
 *
 * shmem.c
 *	  Microsoft Windows Win32 Shared Memory Emulation
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <stdio.h>
#include <errno.h>

static DWORD s_segsize = 0;

/* Detach from a shared mem area based on its address */
int
shmdt(const void *shmaddr)
{
	if (UnmapViewOfFile(shmaddr))
		return 0;
	else
		return -1;
}

/* Attach to an existing area */
void *
shmat(int memId, void *shmaddr, int flag)
{
	/* TODO -- shmat needs to count # attached to shared mem */
	void	   *lpmem = MapViewOfFileEx((HANDLE) memId,
										FILE_MAP_WRITE | FILE_MAP_READ,
				 0, 0, /* (DWORD)pshmdsc->segsize */ s_segsize, shmaddr);

	if (lpmem == NULL)
	{
		lpmem = (void *) -1;
		errno = GetLastError();
	}

	return lpmem;
}

/* Control a shared mem area */
int
shmctl(int shmid, int flag, struct shmid_ds * dummy)
{
	if (flag == IPC_RMID)
	{
		/* Delete the area */
		CloseHandle((HANDLE) shmid);
		return 0;
	}
	if (flag == IPC_STAT)
	{
		/* Can only test for if exists */
		int			hmap = shmget(shmid, 0, 0);

		if (hmap < 0)
		{
			/* Shared memory does not exist */
			errno = EINVAL;
			return -1;
		}
		else
		{
			/* Shared memory does exist and must be in use */
			shmctl(hmap, IPC_RMID, NULL);		/* Release our hold on it */
			errno = 0;
			return 0;
		}
	}

	errno = EINVAL;
	return -1;
}

/* Get an area based on the IPC key */
int
shmget(int memKey, int size, int flag)
{
	HANDLE		hmap;
	char		szShareMem[32];
	DWORD		dwRet;

	s_segsize = size;
	sprintf(szShareMem, "sharemem.%d", memKey);

	if (flag & IPC_CREAT)
	{
		hmap = CreateFileMapping((HANDLE) 0xFFFFFFFF,	/* Use the swap file	*/
								 NULL,
								 PAGE_READWRITE,		/* Memory is Read/Write */
								 0L,	/* Size Upper 32 Bits	*/
								 (DWORD) s_segsize,		/* Size Lower 32 bits */
								 szShareMem);
	}
	else
	{
		hmap = OpenFileMapping(FILE_MAP_ALL_ACCESS,
							   FALSE,
							   szShareMem);
		if (!hmap)
		{
			errno = ENOENT;
			return -1;
		}
	}

	dwRet = GetLastError();
	if (dwRet == ERROR_ALREADY_EXISTS && hmap && (flag & (IPC_CREAT | IPC_EXCL)))
	{
		/* Caller wanted to create the segment -- error if already exists */
		CloseHandle(hmap);
		errno = EEXIST;
		return -1;
	}
	else if (!hmap)
	{
		/* Unable to get shared memory */
		return -1;
	}

	return (int) hmap;
}
