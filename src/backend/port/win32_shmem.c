/*-------------------------------------------------------------------------
 *
 * win32_shmem.c
 *	  Implement shared memory using win32 facilities
 *
 * Portions Copyright (c) 1996-2018, PostgreSQL Global Development Group
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

HANDLE		UsedShmemSegID = INVALID_HANDLE_VALUE;
void	   *UsedShmemSegAddr = NULL;
static Size UsedShmemSegSize = 0;

static bool EnableLockPagesPrivilege(int elevel);
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

	retptr = malloc(bufsize + 18);	/* 18 for Global\PostgreSQL: */
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
 * EnableLockPagesPrivilege
 *
 * Try to acquire SeLockMemoryPrivilege so we can use large pages.
 */
static bool
EnableLockPagesPrivilege(int elevel)
{
	HANDLE		hToken;
	TOKEN_PRIVILEGES tp;
	LUID		luid;

	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
	{
		ereport(elevel,
				(errmsg("could not enable Lock Pages in Memory user right: error code %lu", GetLastError()),
				 errdetail("Failed system call was %s.", "OpenProcessToken")));
		return FALSE;
	}

	if (!LookupPrivilegeValue(NULL, SE_LOCK_MEMORY_NAME, &luid))
	{
		ereport(elevel,
				(errmsg("could not enable Lock Pages in Memory user right: error code %lu", GetLastError()),
				 errdetail("Failed system call was %s.", "LookupPrivilegeValue")));
		CloseHandle(hToken);
		return FALSE;
	}
	tp.PrivilegeCount = 1;
	tp.Privileges[0].Luid = luid;
	tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

	if (!AdjustTokenPrivileges(hToken, FALSE, &tp, 0, NULL, NULL))
	{
		ereport(elevel,
				(errmsg("could not enable Lock Pages in Memory user right: error code %lu", GetLastError()),
				 errdetail("Failed system call was %s.", "AdjustTokenPrivileges")));
		CloseHandle(hToken);
		return FALSE;
	}

	if (GetLastError() != ERROR_SUCCESS)
	{
		if (GetLastError() == ERROR_NOT_ALL_ASSIGNED)
			ereport(elevel,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("could not enable Lock Pages in Memory user right"),
					 errhint("Assign Lock Pages in Memory user right to the Windows user account which runs PostgreSQL.")));
		else
			ereport(elevel,
					(errmsg("could not enable Lock Pages in Memory user right: error code %lu", GetLastError()),
					 errdetail("Failed system call was %s.", "AdjustTokenPrivileges")));
		CloseHandle(hToken);
		return FALSE;
	}

	CloseHandle(hToken);

	return TRUE;
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
	SIZE_T		largePageSize = 0;
	Size		orig_size = size;
	DWORD		flProtect = PAGE_READWRITE;
	MEMORY_BASIC_INFORMATION info;

	/* Room for a header? */
	Assert(size > MAXALIGN(sizeof(PGShmemHeader)));

	szShareMem = GetSharedMemName();

	UsedShmemSegAddr = NULL;

	if (huge_pages == HUGE_PAGES_ON || huge_pages == HUGE_PAGES_TRY)
	{
		/* Does the processor support large pages? */
		largePageSize = GetLargePageMinimum();
		if (largePageSize == 0)
		{
			ereport(huge_pages == HUGE_PAGES_ON ? FATAL : DEBUG1,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("the processor does not support large pages")));
			ereport(DEBUG1,
					(errmsg("disabling huge pages")));
		}
		else if (!EnableLockPagesPrivilege(huge_pages == HUGE_PAGES_ON ? FATAL : DEBUG1))
		{
			ereport(DEBUG1,
					(errmsg("disabling huge pages")));
		}
		else
		{
			/* Huge pages available and privilege enabled, so turn on */
			flProtect = PAGE_READWRITE | SEC_COMMIT | SEC_LARGE_PAGES;

			/* Round size up as appropriate. */
			if (size % largePageSize != 0)
				size += largePageSize - (size % largePageSize);
		}
	}

retry:
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
								 flProtect,
								 size_high, /* Size Upper 32 Bits	*/
								 size_low,	/* Size Lower 32 bits */
								 szShareMem);

		if (!hmap)
		{
			if (GetLastError() == ERROR_NO_SYSTEM_RESOURCES &&
				huge_pages == HUGE_PAGES_TRY &&
				(flProtect & SEC_LARGE_PAGES) != 0)
			{
				elog(DEBUG1, "CreateFileMapping(%zu) with SEC_LARGE_PAGES failed, "
					 "huge pages disabled",
					 size);

				/*
				 * Use the original size, not the rounded-up value, when
				 * falling back to non-huge pages.
				 */
				size = orig_size;
				flProtect = PAGE_READWRITE;
				goto retry;
			}
			else
				ereport(FATAL,
						(errmsg("could not create shared memory segment: error code %lu", GetLastError()),
						 errdetail("Failed system call was CreateFileMapping(size=%zu, name=%s).",
								   size, szShareMem)));
		}

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

	/* Register on-exit routine to delete the new segment */
	on_shmem_exit(pgwin32_SharedMemoryDelete, PointerGetDatum(hmap2));

	/* Log information about the segment's virtual memory use */
	if (VirtualQuery(memAddress, &info, sizeof(info)) != 0)
		elog(LOG, "mapped shared memory segment at %p, requested size %zu, mapped size %zu",
			 memAddress, size, info.RegionSize);
	else
		elog(LOG, "VirtualQuery(%p) failed: error code %lu",
			 memAddress, GetLastError());

	*shim = hdr;
	return hdr;
}

/*
 * PGSharedMemoryReAttach
 *
 * This is called during startup of a postmaster child process to re-attach to
 * an already existing shared memory segment, using the handle inherited from
 * the postmaster.
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
	MEMORY_BASIC_INFORMATION previnfo;
	MEMORY_BASIC_INFORMATION afterinfo;
	DWORD		preverr;
	DWORD		aftererr;

	Assert(UsedShmemSegAddr != NULL);
	Assert(IsUnderPostmaster);

	/* Preliminary probe of region we intend to release */
	if (VirtualQuery(UsedShmemSegAddr, &previnfo, sizeof(previnfo)) != 0)
		preverr = 0;
	else
		preverr = GetLastError();

	/*
	 * Release memory region reservation that was made by the postmaster
	 */
	if (VirtualFree(UsedShmemSegAddr, 0, MEM_RELEASE) == 0)
		elog(FATAL, "failed to release reserved memory region (addr=%p): error code %lu",
			 UsedShmemSegAddr, GetLastError());

	/* Verify post-release state */
	if (VirtualQuery(UsedShmemSegAddr, &afterinfo, sizeof(afterinfo)) != 0)
		aftererr = 0;
	else
		aftererr = GetLastError();

	hdr = (PGShmemHeader *) MapViewOfFileEx(UsedShmemSegID, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0, UsedShmemSegAddr);
	if (!hdr)
	{
		DWORD		maperr = GetLastError();
		MEMORY_BASIC_INFORMATION postinfo;
		DWORD		posterr;

		/* Capture post-failure state */
		if (VirtualQuery(UsedShmemSegAddr, &postinfo, sizeof(postinfo)) != 0)
			posterr = 0;
		else
			posterr = GetLastError();

		if (preverr == 0)
			elog(LOG, "VirtualQuery(%p) before free reports region of size %zu, base %p, has state 0x%lx",
				 UsedShmemSegAddr, previnfo.RegionSize,
				 previnfo.AllocationBase, previnfo.State);
		else
			elog(LOG, "VirtualQuery(%p) before free failed: error code %lu",
				 UsedShmemSegAddr, preverr);

		if (aftererr == 0)
			elog(LOG, "VirtualQuery(%p) after free reports region of size %zu, base %p, has state 0x%lx",
				 UsedShmemSegAddr, afterinfo.RegionSize,
				 afterinfo.AllocationBase, afterinfo.State);
		else
			elog(LOG, "VirtualQuery(%p) after free failed: error code %lu",
				 UsedShmemSegAddr, aftererr);

		if (posterr == 0)
			elog(LOG, "VirtualQuery(%p) after map reports region of size %zu, base %p, has state 0x%lx",
				 UsedShmemSegAddr, postinfo.RegionSize,
				 postinfo.AllocationBase, postinfo.State);
		else
			elog(LOG, "VirtualQuery(%p) after map failed: error code %lu",
				 UsedShmemSegAddr, posterr);

		elog(FATAL, "could not reattach to shared memory (key=%p, addr=%p): error code %lu",
			 UsedShmemSegID, UsedShmemSegAddr, maperr);
	}
	if (hdr != origUsedShmemSegAddr)
		elog(FATAL, "reattaching to shared memory returned unexpected address (got %p, expected %p)",
			 hdr, origUsedShmemSegAddr);
	if (hdr->magic != PGShmemMagic)
		elog(FATAL, "reattaching to shared memory returned non-PostgreSQL memory");
	dsm_set_control_handle(hdr->dsm_control);

	UsedShmemSegAddr = hdr;		/* probably redundant */
}

/*
 * PGSharedMemoryNoReAttach
 *
 * This is called during startup of a postmaster child process when we choose
 * *not* to re-attach to the existing shared memory segment.  We must clean up
 * to leave things in the appropriate state.
 *
 * The child process startup logic might or might not call PGSharedMemoryDetach
 * after this; make sure that it will be a no-op if called.
 *
 * UsedShmemSegID and UsedShmemSegAddr are implicit parameters to this
 * routine.  The caller must have already restored them to the postmaster's
 * values.
 */
void
PGSharedMemoryNoReAttach(void)
{
	Assert(UsedShmemSegAddr != NULL);
	Assert(IsUnderPostmaster);

	/*
	 * Under Windows we will not have mapped the segment, so we don't need to
	 * un-map it.  Just reset UsedShmemSegAddr to show we're not attached.
	 */
	UsedShmemSegAddr = NULL;

	/*
	 * We *must* close the inherited shmem segment handle, else Windows will
	 * consider the existence of this process to mean it can't release the
	 * shmem segment yet.  We can now use PGSharedMemoryDetach to do that.
	 */
	PGSharedMemoryDetach();
}

/*
 * PGSharedMemoryDetach
 *
 * Detach from the shared memory segment, if still attached.  This is not
 * intended to be called explicitly by the process that originally created the
 * segment (it will have an on_shmem_exit callback registered to do that).
 * Rather, this is for subprocesses that have inherited an attachment and want
 * to get rid of it.
 *
 * UsedShmemSegID and UsedShmemSegAddr are implicit parameters to this
 * routine.
 */
void
PGSharedMemoryDetach(void)
{
	/* Unmap the view, if it's mapped */
	if (UsedShmemSegAddr != NULL)
	{
		if (!UnmapViewOfFile(UsedShmemSegAddr))
			elog(LOG, "could not unmap view of shared memory: error code %lu",
				 GetLastError());

		UsedShmemSegAddr = NULL;
	}

	/* And close the shmem handle, if we have one */
	if (UsedShmemSegID != INVALID_HANDLE_VALUE)
	{
		if (!CloseHandle(UsedShmemSegID))
			elog(LOG, "could not close handle to shared memory: error code %lu",
				 GetLastError());

		UsedShmemSegID = INVALID_HANDLE_VALUE;
	}
}


/*
 * pgwin32_SharedMemoryDelete
 *
 * Detach from and delete the shared memory segment
 * (called as an on_shmem_exit callback, hence funny argument list)
 */
static void
pgwin32_SharedMemoryDelete(int status, Datum shmId)
{
	Assert(DatumGetPointer(shmId) == UsedShmemSegID);
	PGSharedMemoryDetach();
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
