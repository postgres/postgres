/*-------------------------------------------------------------------------
 *
 * win32_shmem.c
 *	  Implement shared memory using win32 facilities
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/port/win32_shmem.c,v 1.4 2008/01/01 19:45:51 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/pg_shmem.h"

unsigned long UsedShmemSegID = 0;
void	   *UsedShmemSegAddr = NULL;

static void pgwin32_SharedMemoryDelete(int status, Datum shmId);

/*
 * Generate shared memory segment name. Expand the data directory, to generate
 * an identifier unique for this data directory. Then replace all backslashes
 * with forward slashes, since backslashes aren't permitted in global object names.
 *
 * Store the shared memory segment in the Global\ namespace (requires NT2 TSE or
 * 2000, but that's all we support for other reasons as well), to make sure you can't
 * open two postmasters in different sessions against the same data directory.
 *
 * XXX: What happens with junctions? It's only someone breaking things on purpose,
 *		and this is still better than before, but we might want to do something about
 *		that sometime in the future.
 */
static char *
GetSharedMemName(void)
{
	char	   *retptr;
	DWORD		bufsize;
	DWORD		r;
	char	   *cp;

	bufsize = GetFullPathName(DataDir, 0, NULL, NULL);
	if (bufsize == 0)
		elog(FATAL, "could not get size for full pathname of datadir %s: %lu",
			 DataDir, GetLastError());

	retptr = malloc(bufsize + 1 + 18);	/* 1 NULL and 18 for
										 * Global\PostgreSQL: */
	if (retptr == NULL)
		elog(FATAL, "could not allocate memory for shared memory name");

	strcpy(retptr, "Global\\PostgreSQL:");
	r = GetFullPathName(DataDir, bufsize, retptr + 11, NULL);
	if (r == 0 || r > bufsize)
		elog(FATAL, "could not generate full pathname for datadir %s: %lu",
			 DataDir, GetLastError());

	for (cp = retptr; *cp; cp++)
		if (*cp == '\\')
			*cp = '/';

	return retptr;
}


/*
 * PGSharedMemoryIsInUse
 *
 * Is a previously-existing shmem segment still existing and in use?
 *
 * The point of this exercise is to detect the case where a prior postmaster
 * crashed, but it left child backends that are still running.	Therefore
 * we only care about shmem segments that are associated with the intended
 * DataDir.  This is an important consideration since accidental matches of
 * shmem segment IDs are reasonably common.
 *
 */
bool
PGSharedMemoryIsInUse(unsigned long id1, unsigned long id2)
{
	char	   *szShareMem;
	HANDLE		hmap;

	szShareMem = GetSharedMemName();

	hmap = OpenFileMapping(FILE_MAP_READ, FALSE, szShareMem);

	free(szShareMem);

	if (hmap == NULL)
		return false;

	CloseHandle(hmap);
	return true;
}


/*
 * PGSharedMemoryCreate
 *
 * Create a shared memory segment of the given size and initialize its
 * standard header.
 *
 * makePrivate means to always create a new segment, rather than attach to
 * or recycle any existing segment. On win32, we always create a new segment,
 * since there is no need for recycling (segments go away automatically
 * when the last backend exits)
 *
 */
PGShmemHeader *
PGSharedMemoryCreate(Size size, bool makePrivate, int port)
{
	void	   *memAddress;
	PGShmemHeader *hdr;
	HANDLE		hmap,
				hmap2;
	char	   *szShareMem;

	/* Room for a header? */
	Assert(size > MAXALIGN(sizeof(PGShmemHeader)));

	szShareMem = GetSharedMemName();

	UsedShmemSegAddr = NULL;

	hmap = CreateFileMapping((HANDLE) 0xFFFFFFFF,		/* Use the pagefile */
							 NULL,		/* Default security attrs */
							 PAGE_READWRITE,	/* Memory is Read/Write */
							 0L,	/* Size Upper 32 Bits	*/
							 (DWORD) size,		/* Size Lower 32 bits */
							 szShareMem);

	if (!hmap)
		ereport(FATAL,
				(errmsg("could not create shared memory segment: %lu", GetLastError()),
				 errdetail("Failed system call was CreateFileMapping(size=%lu, name=%s).",
						   (unsigned long) size, szShareMem)));

	/*
	 * If the segment already existed, CreateFileMapping() will return a
	 * handle to the existing one.
	 */
	if (GetLastError() == ERROR_ALREADY_EXISTS)
	{
		/*
		 * When recycling a shared memory segment, it may take a short while
		 * before it gets dropped from the global namespace. So re-try after
		 * sleeping for a second.
		 */
		CloseHandle(hmap);		/* Close the old handle, since we got a valid
								 * one to the previous segment. */

		Sleep(1000);

		hmap = CreateFileMapping((HANDLE) 0xFFFFFFFF, NULL, PAGE_READWRITE, 0L, (DWORD) size, szShareMem);
		if (!hmap)
			ereport(FATAL,
					(errmsg("could not create shared memory segment: %lu", GetLastError()),
					 errdetail("Failed system call was CreateFileMapping(size=%lu, name=%s).",
							   (unsigned long) size, szShareMem)));

		if (GetLastError() == ERROR_ALREADY_EXISTS)
			ereport(FATAL,
				 (errmsg("pre-existing shared memory block is still in use"),
				  errhint("Check if there are any old server processes still running, and terminate them.")));
	}

	free(szShareMem);

	/*
	 * Make the handle inheritable
	 */
	if (!DuplicateHandle(GetCurrentProcess(), hmap, GetCurrentProcess(), &hmap2, 0, TRUE, DUPLICATE_SAME_ACCESS))
		ereport(FATAL,
				(errmsg("could not create shared memory segment: %lu", GetLastError()),
				 errdetail("Failed system call was DuplicateHandle.")));

	/*
	 * Close the old, non-inheritable handle. If this fails we don't really
	 * care.
	 */
	if (!CloseHandle(hmap))
		elog(LOG, "could not close handle to shared memory: %lu", GetLastError());


	/* Register on-exit routine to delete the new segment */
	on_shmem_exit(pgwin32_SharedMemoryDelete, Int32GetDatum((unsigned long) hmap2));

	/*
	 * Get a pointer to the new shared memory segment. Map the whole segment
	 * at once, and let the system decide on the initial address.
	 */
	memAddress = MapViewOfFileEx(hmap2, FILE_MAP_WRITE | FILE_MAP_READ, 0, 0, 0, NULL);
	if (!memAddress)
		ereport(FATAL,
				(errmsg("could not create shared memory segment: %lu", GetLastError()),
				 errdetail("Failed system call was MapViewOfFileEx.")));



	/*
	 * OK, we created a new segment.  Mark it as created by this process. The
	 * order of assignments here is critical so that another Postgres process
	 * can't see the header as valid but belonging to an invalid PID!
	 */
	hdr = (PGShmemHeader *) memAddress;
	hdr->creatorPID = getpid();
	hdr->magic = PGShmemMagic;

	/*
	 * Initialize space allocation status for segment.
	 */
	hdr->totalsize = size;
	hdr->freeoffset = MAXALIGN(sizeof(PGShmemHeader));

	/* Save info for possible future use */
	UsedShmemSegAddr = memAddress;
	UsedShmemSegID = (unsigned long) hmap2;

	return hdr;
}

/*
 * PGSharedMemoryReAttach
 *
 * Re-attach to an already existing shared memory segment. Use the
 * handle inherited from the postmaster.
 *
 * UsedShmemSegID and UsedShmemSegAddr are implicit parameters to this
 * routine.  The caller must have already restored them to the postmaster's
 * values.
 */
void
PGSharedMemoryReAttach(void)
{
	PGShmemHeader *hdr;
	void	   *origUsedShmemSegAddr = UsedShmemSegAddr;

	Assert(UsedShmemSegAddr != NULL);
	Assert(IsUnderPostmaster);

	hdr = (PGShmemHeader *) MapViewOfFileEx((HANDLE) UsedShmemSegID, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0, UsedShmemSegAddr);
	if (!hdr)
		elog(FATAL, "could not reattach to shared memory (key=%d, addr=%p): %lu",
			 (int) UsedShmemSegID, UsedShmemSegAddr, GetLastError());
	if (hdr != origUsedShmemSegAddr)
		elog(FATAL, "reattaching to shared memory returned unexpected address (got %p, expected %p)",
			 hdr, origUsedShmemSegAddr);
	if (hdr->magic != PGShmemMagic)
		elog(FATAL, "reattaching to shared memory returned non-PostgreSQL memory");

	UsedShmemSegAddr = hdr;		/* probably redundant */
}

/*
 * PGSharedMemoryDetach
 *
 * Detach from the shared memory segment, if still attached.  This is not
 * intended for use by the process that originally created the segment. Rather,
 * this is for subprocesses that have inherited an attachment and want to
 * get rid of it.
 */
void
PGSharedMemoryDetach(void)
{
	if (UsedShmemSegAddr != NULL)
	{
		if (!UnmapViewOfFile(UsedShmemSegAddr))
			elog(LOG, "could not unmap view of shared memory: %lu", GetLastError());

		UsedShmemSegAddr = NULL;
	}
}


/*
 *	pgwin32_SharedMemoryDelete(status, shmId)		deletes a shared memory segment
 *	(called as an on_shmem_exit callback, hence funny argument list)
 */
static void
pgwin32_SharedMemoryDelete(int status, Datum shmId)
{
	PGSharedMemoryDetach();
	if (!CloseHandle((HANDLE) DatumGetInt32(shmId)))
		elog(LOG, "could not close handle to shared memory: %lu", GetLastError());
}
