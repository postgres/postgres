/*-------------------------------------------------------------------------
 *
 * sysv_shmem.c
 *	  Implement shared memory using SysV facilities
 *
 * These routines represent a fairly thin layer on top of SysV shared
 * memory functionality.
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/port/sysv_shmem.c,v 1.24.2.2 2004/11/09 20:35:16 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <sys/file.h>
#ifdef HAVE_SYS_IPC_H
#include <sys/ipc.h>
#endif
#ifdef HAVE_SYS_SHM_H
#include <sys/shm.h>
#endif
#ifdef HAVE_KERNEL_OS_H
#include <kernel/OS.h>
#endif

#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/pg_shmem.h"

typedef int IpcMemoryId;		/* shared memory ID returned by shmget(2) */

#define IPCProtection	(0600)	/* access/modify by user only */


IpcMemoryKey UsedShmemSegID = 0;
void	   *UsedShmemSegAddr = NULL;

static void *InternalIpcMemoryCreate(IpcMemoryKey memKey, uint32 size);
static void IpcMemoryDetach(int status, Datum shmaddr);
static void IpcMemoryDelete(int status, Datum shmId);
static PGShmemHeader *PGSharedMemoryAttach(IpcMemoryKey key,
					 IpcMemoryId *shmid);


/*
 *	InternalIpcMemoryCreate(memKey, size)
 *
 * Attempt to create a new shared memory segment with the specified key.
 * Will fail (return NULL) if such a segment already exists.  If successful,
 * attach the segment to the current process and return its attached address.
 * On success, callbacks are registered with on_shmem_exit to detach and
 * delete the segment when on_shmem_exit is called.
 *
 * If we fail with a failure code other than collision-with-existing-segment,
 * print out an error and abort.  Other types of errors are not recoverable.
 */
static void *
InternalIpcMemoryCreate(IpcMemoryKey memKey, uint32 size)
{
	IpcMemoryId shmid;
	void	   *memAddress;

	shmid = shmget(memKey, size, IPC_CREAT | IPC_EXCL | IPCProtection);

	if (shmid < 0)
	{
		/*
		 * Fail quietly if error indicates a collision with existing
		 * segment. One would expect EEXIST, given that we said IPC_EXCL,
		 * but perhaps we could get a permission violation instead?  Also,
		 * EIDRM might occur if an old seg is slated for destruction but
		 * not gone yet.
		 */
		if (errno == EEXIST || errno == EACCES
#ifdef EIDRM
			|| errno == EIDRM
#endif
			)
			return NULL;

		/*
		 * Else complain and abort
		 */
		ereport(FATAL,
				(errmsg("could not create shared memory segment: %m"),
			errdetail("Failed system call was shmget(key=%d, size=%u, 0%o).",
					  (int) memKey, size,
					  IPC_CREAT | IPC_EXCL | IPCProtection),
				 (errno == EINVAL) ?
				 errhint("This error usually means that PostgreSQL's request for a shared memory "
						 "segment exceeded your kernel's SHMMAX parameter.  You can either "
						 "reduce the request size or reconfigure the kernel with larger SHMMAX.  "
						 "To reduce the request size (currently %u bytes), reduce "
						 "PostgreSQL's shared_buffers parameter (currently %d) and/or "
						 "its max_connections parameter (currently %d).\n"
						 "If the request size is already small, it's possible that it is less than "
						 "your kernel's SHMMIN parameter, in which case raising the request size or "
						 "reconfiguring SHMMIN is called for.\n"
						 "The PostgreSQL documentation contains more information about shared "
						 "memory configuration.",
						 size, NBuffers, MaxBackends) : 0,
				 (errno == ENOMEM) ?
				 errhint("This error usually means that PostgreSQL's request for a shared "
			   "memory segment exceeded available memory or swap space. "
			   "To reduce the request size (currently %u bytes), reduce "
		   "PostgreSQL's shared_buffers parameter (currently %d) and/or "
						 "its max_connections parameter (currently %d).\n"
						 "The PostgreSQL documentation contains more information about shared "
						 "memory configuration.",
						 size, NBuffers, MaxBackends) : 0,
				 (errno == ENOSPC) ?
				 errhint("This error does *not* mean that you have run out of disk space. "
						 "It occurs either if all available shared memory IDs have been taken, "
						 "in which case you need to raise the SHMMNI parameter in your kernel, "
						 "or because the system's overall limit for shared memory has been "
			 "reached.  If you cannot increase the shared memory limit, "
		"reduce PostgreSQL's shared memory request (currently %u bytes), "
		"by reducing its shared_buffers parameter (currently %d) and/or "
						 "its max_connections parameter (currently %d).\n"
						 "The PostgreSQL documentation contains more information about shared "
						 "memory configuration.",
						 size, NBuffers, MaxBackends) : 0));
	}

	/* Register on-exit routine to delete the new segment */
	on_shmem_exit(IpcMemoryDelete, Int32GetDatum(shmid));

	/* OK, should be able to attach to the segment */
#ifdef SHM_SHARE_MMU
	/* use intimate shared memory on Solaris */
	memAddress = shmat(shmid, 0, SHM_SHARE_MMU);
#else
	memAddress = shmat(shmid, 0, 0);
#endif

	if (memAddress == (void *) -1)
		elog(FATAL, "shmat(id=%d) failed: %m", shmid);

	/* Register on-exit routine to detach new segment before deleting */
	on_shmem_exit(IpcMemoryDetach, PointerGetDatum(memAddress));

	/* Record key and ID in lockfile for data directory. */
	RecordSharedMemoryInLockFile((unsigned long) memKey,
								 (unsigned long) shmid);

	return memAddress;
}

/****************************************************************************/
/*	IpcMemoryDetach(status, shmaddr)	removes a shared memory segment		*/
/*										from process' address spaceq		*/
/*	(called as an on_shmem_exit callback, hence funny argument list)		*/
/****************************************************************************/
static void
IpcMemoryDetach(int status, Datum shmaddr)
{
	if (shmdt(DatumGetPointer(shmaddr)) < 0)
		elog(LOG, "shmdt(%p) failed: %m", DatumGetPointer(shmaddr));
}

/****************************************************************************/
/*	IpcMemoryDelete(status, shmId)		deletes a shared memory segment		*/
/*	(called as an on_shmem_exit callback, hence funny argument list)		*/
/****************************************************************************/
static void
IpcMemoryDelete(int status, Datum shmId)
{
	if (shmctl(DatumGetInt32(shmId), IPC_RMID, (struct shmid_ds *) NULL) < 0)
		elog(LOG, "shmctl(%d, %d, 0) failed: %m",
			 DatumGetInt32(shmId), IPC_RMID);
}

/*
 * PGSharedMemoryIsInUse
 *
 * Is a previously-existing shmem segment still existing and in use?
 */
bool
PGSharedMemoryIsInUse(unsigned long id1, unsigned long id2)
{
	IpcMemoryId shmId = (IpcMemoryId) id2;
	struct shmid_ds shmStat;

	/*
	 * We detect whether a shared memory segment is in use by seeing
	 * whether it (a) exists and (b) has any processes are attached to it.
	 */
	if (shmctl(shmId, IPC_STAT, &shmStat) < 0)
	{
		/*
		 * EINVAL actually has multiple possible causes documented in the
		 * shmctl man page, but we assume it must mean the segment no
		 * longer exists.
		 */
		if (errno == EINVAL)
			return false;
		/*
		 * EACCES implies that the segment belongs to some other userid,
		 * which means it is not a Postgres shmem segment (or at least,
		 * not one that is relevant to our data directory).
		 */
		if (errno == EACCES)
			return false;
		/*
		 * Otherwise, we had better assume that the segment is in use.
		 * The only likely case is EIDRM, which implies that the segment
		 * has been IPC_RMID'd but there are still processes attached to it.
		 */
		return true;
	}
	/* If it has attached processes, it's in use */
	if (shmStat.shm_nattch != 0)
		return true;
	return false;
}


/*
 * PGSharedMemoryCreate
 *
 * Create a shared memory segment of the given size and initialize its
 * standard header.  Also, register an on_shmem_exit callback to release
 * the storage.  For an exec'ed backend, it just attaches.
 *
 * Dead Postgres segments are recycled if found, but we do not fail upon
 * collision with non-Postgres shmem segments.	The idea here is to detect and
 * re-use keys that may have been assigned by a crashed postmaster or backend.
 *
 * makePrivate means to always create a new segment, rather than attach to
 * or recycle any existing segment.
 *
 * The port number is passed for possible use as a key (for SysV, we use
 * it to generate the starting shmem key).	In a standalone backend,
 * zero will be passed.
 */
PGShmemHeader *
PGSharedMemoryCreate(uint32 size, bool makePrivate, int port)
{
	IpcMemoryKey NextShmemSegID;
	void	   *memAddress;
	PGShmemHeader *hdr;
	IpcMemoryId shmid;

	/* Room for a header? */
	Assert(size > MAXALIGN(sizeof(PGShmemHeader)));

	/* If Exec case, just attach and return the pointer */
	if (ExecBackend && UsedShmemSegAddr != NULL && !makePrivate)
	{
		if ((hdr = PGSharedMemoryAttach(UsedShmemSegID, &shmid)) == NULL)
			elog(FATAL, "could not attach to proper memory at fixed address: shmget(key=%d, addr=%p) failed: %m",
				 (int) UsedShmemSegID, UsedShmemSegAddr);
		return hdr;
	}

	/* Make sure PGSharedMemoryAttach doesn't fail without need */
	UsedShmemSegAddr = NULL;

	/* Loop till we find a free IPC key */
	NextShmemSegID = port * 1000;

	for (NextShmemSegID++;; NextShmemSegID++)
	{
		/* Try to create new segment */
		memAddress = InternalIpcMemoryCreate(NextShmemSegID, size);
		if (memAddress)
			break;				/* successful create and attach */

		/* Check shared memory and possibly remove and recreate */

		if (makePrivate)		/* a standalone backend shouldn't do this */
			continue;

		if ((memAddress = PGSharedMemoryAttach(NextShmemSegID, &shmid)) == NULL)
			continue;			/* can't attach, not one of mine */

		/*
		 * If I am not the creator and it belongs to an extant process,
		 * continue.
		 */
		hdr = (PGShmemHeader *) memAddress;
		if (hdr->creatorPID != getpid())
		{
			if (kill(hdr->creatorPID, 0) == 0 || errno != ESRCH)
			{
				shmdt(memAddress);
				continue;		/* segment belongs to a live process */
			}
		}

		/*
		 * The segment appears to be from a dead Postgres process, or from
		 * a previous cycle of life in this same process.  Zap it, if
		 * possible.  This probably shouldn't fail, but if it does, assume
		 * the segment belongs to someone else after all, and continue
		 * quietly.
		 */
		shmdt(memAddress);
		if (shmctl(shmid, IPC_RMID, (struct shmid_ds *) NULL) < 0)
			continue;

		/*
		 * Now try again to create the segment.
		 */
		memAddress = InternalIpcMemoryCreate(NextShmemSegID, size);
		if (memAddress)
			break;				/* successful create and attach */

		/*
		 * Can only get here if some other process managed to create the
		 * same shmem key before we did.  Let him have that one, loop
		 * around to try next key.
		 */
	}

	/*
	 * OK, we created a new segment.  Mark it as created by this process.
	 * The order of assignments here is critical so that another Postgres
	 * process can't see the header as valid but belonging to an invalid
	 * PID!
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
	UsedShmemSegID = NextShmemSegID;

	return hdr;
}

/*
 * PGSharedMemoryDetach
 *
 * Detach from the shared memory segment, if still attached.  This is not
 * intended for use by the process that originally created the segment
 * (it will have an on_shmem_exit callback registered to do that).  Rather,
 * this is for subprocesses that have inherited an attachment and want to
 * get rid of it.
 */
void
PGSharedMemoryDetach(void)
{
	if (UsedShmemSegAddr != NULL)
	{
		if (shmdt(UsedShmemSegAddr) < 0)
			elog(LOG, "shmdt(%p) failed: %m", UsedShmemSegAddr);
		UsedShmemSegAddr = NULL;
	}
}

/*
 * Attach to shared memory and make sure it has a Postgres header
 *
 * Returns attach address if OK, else NULL
 */
static PGShmemHeader *
PGSharedMemoryAttach(IpcMemoryKey key, IpcMemoryId *shmid)
{
	PGShmemHeader *hdr;

	if ((*shmid = shmget(key, sizeof(PGShmemHeader), 0)) < 0)
		return NULL;

	hdr = (PGShmemHeader *) shmat(*shmid,
								  UsedShmemSegAddr,
#ifdef SHM_SHARE_MMU
	/* use intimate shared memory on Solaris */
								  SHM_SHARE_MMU
#else
								  0
#endif
		);

	if (hdr == (PGShmemHeader *) -1)
		return NULL;			/* failed: must be some other app's */

	if (hdr->magic != PGShmemMagic)
	{
		shmdt(hdr);
		return NULL;			/* segment belongs to a non-Postgres app */
	}

	return hdr;
}
