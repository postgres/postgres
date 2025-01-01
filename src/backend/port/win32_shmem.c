/*-------------------------------------------------------------------------
 *
 * win32_shmem.c
 *	  Implement shared memory using win32 facilities
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
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
#include "utils/guc_hooks.h"


/*
 * Early in a process's life, Windows asynchronously creates threads for the
 * process's "default thread pool"
 * (https://docs.microsoft.com/en-us/windows/desktop/ProcThread/thread-pools).
 * Occasionally, thread creation allocates a stack after
 * PGSharedMemoryReAttach() has released UsedShmemSegAddr and before it has
 * mapped shared memory at UsedShmemSegAddr.  This would cause mapping to fail
 * if the allocator preferred the just-released region for allocating the new
 * thread stack.  We observed such failures in some Windows Server 2016
 * configurations.  To give the system another region to prefer, reserve and
 * release an additional, protective region immediately before reserving or
 * releasing shared memory.  The idea is that, if the allocator handed out
 * REGION1 pages before REGION2 pages at one occasion, it will do so whenever
 * both regions are free.  Windows Server 2016 exhibits that behavior, and a
 * system behaving differently would have less need to protect
 * UsedShmemSegAddr.  The protective region must be at least large enough for
 * one thread stack.  However, ten times as much is less than 2% of the 32-bit
 * address space and is negligible relative to the 64-bit address space.
 */
#define PROTECTIVE_REGION_SIZE (10 * WIN32_STACK_RLIMIT)
void	   *ShmemProtectiveRegion = NULL;

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
				(errmsg("could not enable user right \"%s\": error code %lu",

		/*
		 * translator: This is a term from Windows and should be translated to
		 * match the Windows localization.
		 */
						_("Lock pages in memory"),
						GetLastError()),
				 errdetail("Failed system call was %s.", "OpenProcessToken")));
		return FALSE;
	}

	if (!LookupPrivilegeValue(NULL, SE_LOCK_MEMORY_NAME, &luid))
	{
		ereport(elevel,
				(errmsg("could not enable user right \"%s\": error code %lu", _("Lock pages in memory"), GetLastError()),
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
				(errmsg("could not enable user right \"%s\": error code %lu", _("Lock pages in memory"), GetLastError()),
				 errdetail("Failed system call was %s.", "AdjustTokenPrivileges")));
		CloseHandle(hToken);
		return FALSE;
	}

	if (GetLastError() != ERROR_SUCCESS)
	{
		if (GetLastError() == ERROR_NOT_ALL_ASSIGNED)
			ereport(elevel,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("could not enable user right \"%s\"", _("Lock pages in memory")),
					 errhint("Assign user right \"%s\" to the Windows user account which runs PostgreSQL.",
							 _("Lock pages in memory"))));
		else
			ereport(elevel,
					(errmsg("could not enable user right \"%s\": error code %lu", _("Lock pages in memory"), GetLastError()),
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
 */
PGShmemHeader *
PGSharedMemoryCreate(Size size,
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
	DWORD		desiredAccess;

	ShmemProtectiveRegion = VirtualAlloc(NULL, PROTECTIVE_REGION_SIZE,
										 MEM_RESERVE, PAGE_NOACCESS);
	if (ShmemProtectiveRegion == NULL)
		elog(FATAL, "could not reserve memory region: error code %lu",
			 GetLastError());

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
					(errmsg_internal("disabling huge pages")));
		}
		else if (!EnableLockPagesPrivilege(huge_pages == HUGE_PAGES_ON ? FATAL : DEBUG1))
		{
			ereport(DEBUG1,
					(errmsg_internal("disabling huge pages")));
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

	desiredAccess = FILE_MAP_WRITE | FILE_MAP_READ;

#ifdef FILE_MAP_LARGE_PAGES
	/* Set large pages if wanted. */
	if ((flProtect & SEC_LARGE_PAGES) != 0)
		desiredAccess |= FILE_MAP_LARGE_PAGES;
#endif

	/*
	 * Get a pointer to the new shared memory segment. Map the whole segment
	 * at once, and let the system decide on the initial address.
	 */
	memAddress = MapViewOfFileEx(hmap2, desiredAccess, 0, 0, 0, NULL);
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

	*shim = hdr;

	/* Report whether huge pages are in use */
	SetConfigOption("huge_pages_status", (flProtect & SEC_LARGE_PAGES) ?
					"on" : "off", PGC_INTERNAL, PGC_S_DYNAMIC_DEFAULT);

	return hdr;
}

/*
 * PGSharedMemoryReAttach
 *
 * This is called during startup of a postmaster child process to re-attach to
 * an already existing shared memory segment, using the handle inherited from
 * the postmaster.
 *
 * ShmemProtectiveRegion, UsedShmemSegID and UsedShmemSegAddr are implicit
 * parameters to this routine.  The caller must have already restored them to
 * the postmaster's values.
 */
void
PGSharedMemoryReAttach(void)
{
	PGShmemHeader *hdr;
	void	   *origUsedShmemSegAddr = UsedShmemSegAddr;

	Assert(ShmemProtectiveRegion != NULL);
	Assert(UsedShmemSegAddr != NULL);
	Assert(IsUnderPostmaster);

	/*
	 * Release memory region reservations made by the postmaster
	 */
	if (VirtualFree(ShmemProtectiveRegion, 0, MEM_RELEASE) == 0)
		elog(FATAL, "failed to release reserved memory region (addr=%p): error code %lu",
			 ShmemProtectiveRegion, GetLastError());
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
 * PGSharedMemoryNoReAttach
 *
 * This is called during startup of a postmaster child process when we choose
 * *not* to re-attach to the existing shared memory segment.  We must clean up
 * to leave things in the appropriate state.
 *
 * The child process startup logic might or might not call PGSharedMemoryDetach
 * after this; make sure that it will be a no-op if called.
 *
 * ShmemProtectiveRegion, UsedShmemSegID and UsedShmemSegAddr are implicit
 * parameters to this routine.  The caller must have already restored them to
 * the postmaster's values.
 */
void
PGSharedMemoryNoReAttach(void)
{
	Assert(ShmemProtectiveRegion != NULL);
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
 * ShmemProtectiveRegion, UsedShmemSegID and UsedShmemSegAddr are implicit
 * parameters to this routine.
 */
void
PGSharedMemoryDetach(void)
{
	/*
	 * Releasing the protective region liberates an unimportant quantity of
	 * address space, but be tidy.
	 */
	if (ShmemProtectiveRegion != NULL)
	{
		if (VirtualFree(ShmemProtectiveRegion, 0, MEM_RELEASE) == 0)
			elog(LOG, "failed to release reserved memory region (addr=%p): error code %lu",
				 ShmemProtectiveRegion, GetLastError());

		ShmemProtectiveRegion = NULL;
	}

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

	Assert(ShmemProtectiveRegion != NULL);
	Assert(UsedShmemSegAddr != NULL);
	Assert(UsedShmemSegSize != 0);

	/* ShmemProtectiveRegion */
	address = VirtualAllocEx(hChild, ShmemProtectiveRegion,
							 PROTECTIVE_REGION_SIZE,
							 MEM_RESERVE, PAGE_NOACCESS);
	if (address == NULL)
	{
		/* Don't use FATAL since we're running in the postmaster */
		elog(LOG, "could not reserve shared memory region (addr=%p) for child %p: error code %lu",
			 ShmemProtectiveRegion, hChild, GetLastError());
		return false;
	}
	if (address != ShmemProtectiveRegion)
	{
		/*
		 * Should never happen - in theory if allocation granularity causes
		 * strange effects it could, so check just in case.
		 *
		 * Don't use FATAL since we're running in the postmaster.
		 */
		elog(LOG, "reserved shared memory region got incorrect address %p, expected %p",
			 address, ShmemProtectiveRegion);
		return false;
	}

	/* UsedShmemSegAddr */
	address = VirtualAllocEx(hChild, UsedShmemSegAddr, UsedShmemSegSize,
							 MEM_RESERVE, PAGE_READWRITE);
	if (address == NULL)
	{
		elog(LOG, "could not reserve shared memory region (addr=%p) for child %p: error code %lu",
			 UsedShmemSegAddr, hChild, GetLastError());
		return false;
	}
	if (address != UsedShmemSegAddr)
	{
		elog(LOG, "reserved shared memory region got incorrect address %p, expected %p",
			 address, UsedShmemSegAddr);
		return false;
	}

	return true;
}

/*
 * This function is provided for consistency with sysv_shmem.c and does not
 * provide any useful information for Windows.  To obtain the large page size,
 * use GetLargePageMinimum() instead.
 */
void
GetHugePageSize(Size *hugepagesize, int *mmap_flags)
{
	if (hugepagesize)
		*hugepagesize = 0;
	if (mmap_flags)
		*mmap_flags = 0;
}

/*
 * GUC check_hook for huge_page_size
 */
bool
check_huge_page_size(int *newval, void **extra, GucSource source)
{
	if (*newval != 0)
	{
		GUC_check_errdetail("\"huge_page_size\" must be 0 on this platform.");
		return false;
	}
	return true;
}
