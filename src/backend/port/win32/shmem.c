/*-------------------------------------------------------------------------
 *
 * shmem.c
 *	  Microsoft Windows Win32 Shared Memory Emulation
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/port/win32/shmem.c,v 1.13.2.2 2010/07/23 13:53:30 mha Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "miscadmin.h"

static DWORD s_segsize = 0;
extern void *UsedShmemSegAddr;
extern Size UsedShmemSegSize;

/* Detach from a shared mem area based on its address */
int
shmdt(const void *shmaddr)
{
	if (UnmapViewOfFile((LPCVOID *) shmaddr))
		return 0;
	else
		return -1;
}

/* Attach to an existing area */
void *
shmat(int memId, void *shmaddr, int flag)
{
	/* Release the memory region reserved in the postmaster */
	if (IsUnderPostmaster)
	{
		if (VirtualFree(shmaddr, 0, MEM_RELEASE) == 0)
			elog(FATAL, "failed to release reserved memory region (addr=%p): %lu",
				 shmaddr, GetLastError());
	}
	/* TODO -- shmat needs to count # attached to shared mem */
	void	   *lpmem = MapViewOfFileEx((HANDLE) memId,
										FILE_MAP_WRITE | FILE_MAP_READ,
			0, 0, /* (DWORD)pshmdsc->segsize */ 0 /* s_segsize */ , shmaddr);

	if (lpmem == NULL)
	{
		lpmem = (void *) -1;
		_dosmaperr(GetLastError());
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
	sprintf(szShareMem, "PostgreSQL.%d", memKey);

	if (flag & IPC_CREAT)
	{
		SetLastError(0);
		hmap = CreateFileMapping((HANDLE) 0xFFFFFFFF,	/* Use the swap file	*/
								 NULL,
								 PAGE_READWRITE,		/* Memory is Read/Write */
								 0L,	/* Size Upper 32 Bits	*/
								 (DWORD) s_segsize,		/* Size Lower 32 bits */
								 szShareMem);
	}
	else
	{
		SetLastError(0);
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
		_dosmaperr(GetLastError());
		return -1;
	}

	return (int) hmap;
}

/*
 * pgwin32_ReserveSharedMemoryRegion(hChild)
 *
 * Reserve the memory region that will be used for shared memory in a child
 * process. It is called before the child process starts, to make sure the
 * memory is available.
 *
 * Once the child starts, DLLs loading in different order or threads getting
 * scheduled differently may allocate memory which can conflict with the
 * address space we need for our shared memory. By reserving the shared
 * memory region before the child starts, and freeing it only just before we
 * attempt to get access to the shared memory forces these allocations to
 * be given different address ranges that don't conflict.
 *
 * NOTE! This function executes in the postmaster, and should for this
 * reason not use elog(FATAL) since that would take down the postmaster.
 */
int
pgwin32_ReserveSharedMemoryRegion(HANDLE hChild)
{
	void *address;

	Assert(UsedShmemSegAddr != NULL);
	Assert(UsedShmemSegSize != 0);

	address = VirtualAllocEx(hChild, UsedShmemSegAddr, UsedShmemSegSize,
								MEM_RESERVE, PAGE_READWRITE);
	if (address == NULL) {
		/* Don't use FATAL since we're running in the postmaster */
		elog(LOG, "could not reserve shared memory region (addr=%p) for child %lu: %lu",
			 UsedShmemSegAddr, hChild, GetLastError());
		return false;
	}
	if (address != UsedShmemSegAddr)
	{
		/*
		 * Should never happen - in theory if allocation granularity causes strange
		 * effects it could, so check just in case.
		 *
		 * Don't use FATAL since we're running in the postmaster.
		 */
	    elog(LOG, "reserved shared memory region got incorrect address %p, expected %p",
			 address, UsedShmemSegAddr);
		VirtualFreeEx(hChild, address, 0, MEM_RELEASE);
		return false;
	}

	return true;
}
