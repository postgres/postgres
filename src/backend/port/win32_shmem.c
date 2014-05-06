/*-------------------------------------------------------------------------
 *
 * win32_shmem.c
 *	  Implement shared memory using win32 facilities
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/port/win32_shmem.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "miscadmin.h"
#include "storage/dsm.h"
#include "storage/ipc.h"
#include "storage/pg_shmem.h"

HANDLE		UsedShmemSegID = 0;
void	   *UsedShmemSegAddr = NULL;
static Size UsedShmemSegSize = 0;

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
		elog(FATAL, "could not get size for full pathname of datadir %s: error code %lu",
			 DataDir, GetLastError());

	retptr = malloc(bufsize + 18);		/* 18 for Global\PostgreSQL: */
	if (retptr == NULL)
		elog(FATAL, "could not allocate memory for shared memory name");

	strcpy(retptr, "Global\\PostgreSQL:");
	r = GetFullPathName(DataDir, bufsize, retptr + 18, NULL);
	if (r == 0 || r > bufsize)
		elog(FATAL, "could not generate full pathname for datadir %s: error code %lu",
			 DataDir, GetLastError());

	/*
	 * XXX: Intentionally overwriting the Global\ part here. This was not the
	 * original approach, but putting it in the actual Global\ namespace
	 * causes permission errors in a lot of cases, so we leave it in the
	 * default namespace for now.
	 */
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
 * crashed, but it left child backends that are still running.  Therefore
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
PGSharedMemoryCreate(Size size, bool makePrivate, int port,
					 PGShmemHeader **shim)
{
	void	   *memAddress;
	PGShmemHeader *hdr;
	HANDLE		hmap,
				hmap2;
	char	   *szShareMem;
	int			i;
	DWORD		size_high;
	DWORD		size_low;

	if (huge_pages == HUGE_PAGES_ON)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("huge pages not supported on this platform")));

	/* Room for a header? */
	Assert(size > MAXALIGN(sizeof(PGShmemHeader)));

	szShareMem = GetSharedMemName();

	UsedShmemSegAddr = NULL;

#ifdef _WIN64
	size_high = size >> 32;
#else
	size_high = 0;
#endif
	size_low = (DWORD) size;

	/*
	 * When recycling a shared memory segment, it may take a short while
	 * before it gets dropped from the global namespace. So re-try after
	 * sleeping for a second, and continue retrying 10 times. (both the 1
	 * second time and the 10 retries are completely arbitrary)
	 */
	for (i = 0; i < 10; i++)
	{
		/*
		 * In case CreateFileMapping() doesn't set the error code to 0 on
		 * success
		 */
		SetLastError(0);

		hmap = CreateFileMapping(INVALID_HANDLE_VALUE,	/* Use the pagefile */
								 NULL,	/* Default security attrs */
								 PAGE_READWRITE,		/* Memory is Read/Write */
								 size_high,		/* Size Upper 32 Bits	*/
								 size_low,		/* Size Lower 32 bits */
								 szShareMem);

		if (!hmap)
			ereport(FATAL,
					(errmsg("could not create shared memory segment: error code %lu", GetLastError()),
					 errdetail("Failed system call was CreateFileMapping(size=%zu, name=%s).",
							   size, szShareMem)));

		/*
		 * If the segment already existed, CreateFileMapping() will return a
		 * handle to the existing one and set ERROR_ALREADY_EXISTS.
		 */
		if (GetLastError() == ERROR_ALREADY_EXISTS)
		{
			CloseHandle(hmap);	/* Close the handle, since we got a valid one
								 * to the previous segment. */
			hmap = NULL;
			Sleep(1000);
			continue;
		}
		break;
	}

	/*
	 * If the last call in the loop still returned ERROR_ALREADY_EXISTS, this
	 * shared memory segment exists and we assume it belongs to somebody else.
	 */
	if (!hmap)
		ereport(FATAL,
				(errmsg("pre-existing shared memory block is still in use"),
				 errhint("Check if there are any old server processes still running, and terminate them.")));

	free(szShareMem);

	/*
	 * Make the handle inheritable
	 */
	if (!DuplicateHandle(GetCurrentProcess(), hmap, GetCurrentProcess(), &hmap2, 0, TRUE, DUPLICATE_SAME_ACCESS))
		ereport(FATAL,
				(errmsg("could not create shared memory segment: error code %lu", GetLastError()),
				 errdetail("Failed system call was DuplicateHandle.")));

	/*
	 * Close the old, non-inheritable handle. If this fails we don't really
	 * care.
	 */
	if (!CloseHandle(hmap))
		elog(LOG, "could not close handle to shared memory: error code %lu", GetLastError());


	/* Register on-exit routine to delete the new segment */
	on_shmem_exit(pgwin32_SharedMemoryDelete, PointerGetDatum(hmap2));

	/*
	 * Get a pointer to the new shared memory segment. Map the whole segment
	 * at once, and let the system decide on the initial address.
	 */
	memAddress = MapViewOfFileEx(hmap2, FILE_MAP_WRITE | FILE_MAP_READ, 0, 0, 0, NULL);
	if (!memAddress)
		ereport(FATAL,
				(errmsg("could not create shared memory segment: error code %lu", GetLastError()),
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
	hdr->dsm_control = 0;

	/* Save info for possible future use */
	UsedShmemSegAddr = memAddress;
	UsedShmemSegSize = size;
	UsedShmemSegID = hmap2;

	*shim = hdr;
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

	/*
	 * Release memory region reservation that was made by the postmaster
	 */
	if (VirtualFree(UsedShmemSegAddr, 0, MEM_RELEASE) == 0)
		elog(FATAL, "failed to release reserved memory region (addr=%p): error code %lu",
			 UsedShmemSegAddr, GetLastError());

	hdr = (PGShmemHeader *) MapViewOfFileEx(UsedShmemSegID, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0, UsedShmemSegAddr);
	if (!hdr)
		elog(FATAL, "could not reattach to shared memory (key=%p, addr=%p): error code %lu",
			 UsedShmemSegID, UsedShmemSegAddr, GetLastError());
	if (hdr != origUsedShmemSegAddr)
		elog(FATAL, "reattaching to shared memory returned unexpected address (got %p, expected %p)",
			 hdr, origUsedShmemSegAddr);
	if (hdr->magic != PGShmemMagic)
		elog(FATAL, "reattaching to shared memory returned non-PostgreSQL memory");
	dsm_set_control_handle(hdr->dsm_control);

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
			elog(LOG, "could not unmap view of shared memory: error code %lu", GetLastError());

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
	if (!CloseHandle(DatumGetPointer(shmId)))
		elog(LOG, "could not close handle to shared memory: error code %lu", GetLastError());
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
	void	   *address;

	Assert(UsedShmemSegAddr != NULL);
	Assert(UsedShmemSegSize != 0);

	address = VirtualAllocEx(hChild, UsedShmemSegAddr, UsedShmemSegSize,
							 MEM_RESERVE, PAGE_READWRITE);
	if (address == NULL)
	{
		/* Don't use FATAL since we're running in the postmaster */
		elog(LOG, "could not reserve shared memory region (addr=%p) for child %p: error code %lu",
			 UsedShmemSegAddr, hChild, GetLastError());
		return false;
	}
	if (address != UsedShmemSegAddr)
	{
		/*
		 * Should never happen - in theory if allocation granularity causes
		 * strange effects it could, so check just in case.
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
