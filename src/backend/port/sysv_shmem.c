/*-------------------------------------------------------------------------
 *
 * sysv_shmem.c
 *	  Implement shared memory using SysV facilities
 *
 * These routines represent a fairly thin layer on top of SysV shared
 * memory functionality.
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/port/sysv_shmem.c,v 1.8 2003/05/06 23:34:55 momjian Exp $
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


#ifdef EXEC_BACKEND
IpcMemoryKey UsedShmemSegID = 0;
#endif

static void *InternalIpcMemoryCreate(IpcMemoryKey memKey, uint32 size);
static void IpcMemoryDetach(int status, Datum shmaddr);
static void IpcMemoryDelete(int status, Datum shmId);
static void *PrivateMemoryCreate(uint32 size);
static void PrivateMemoryDelete(int status, Datum memaddr);


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
		fprintf(stderr, "IpcMemoryCreate: shmget(key=%d, size=%u, 0%o) failed: %s\n",
			  (int) memKey, size, (IPC_CREAT | IPC_EXCL | IPCProtection),
				strerror(errno));

		if (errno == EINVAL)
			fprintf(stderr,
					"\nThis error usually means that PostgreSQL's request for a shared memory\n"
					"segment exceeded your kernel's SHMMAX parameter.  You can either\n"
					"reduce the request size or reconfigure the kernel with larger SHMMAX.\n"
					"To reduce the request size (currently %u bytes), reduce\n"
					"PostgreSQL's shared_buffers parameter (currently %d) and/or\n"
					"its max_connections parameter (currently %d).\n"
					"\n"
					"If the request size is already small, it's possible that it is less than\n"
					"your kernel's SHMMIN parameter, in which case raising the request size or\n"
					"reconfiguring SHMMIN is called for.\n"
					"\n"
					"The PostgreSQL documentation contains more information about shared\n"
					"memory configuration.\n\n",
					size, NBuffers, MaxBackends);

		else if (errno == ENOMEM)
			fprintf(stderr,
					"\nThis error usually means that PostgreSQL's request for a shared\n"
					"memory segment exceeded available memory or swap space.\n"
					"To reduce the request size (currently %u bytes), reduce\n"
					"PostgreSQL's shared_buffers parameter (currently %d) and/or\n"
					"its max_connections parameter (currently %d).\n"
					"\n"
					"The PostgreSQL documentation contains more information about shared\n"
					"memory configuration.\n\n",
					size, NBuffers, MaxBackends);

		else if (errno == ENOSPC)
			fprintf(stderr,
					"\nThis error does *not* mean that you have run out of disk space.\n"
					"\n"
					"It occurs either if all available shared memory IDs have been taken,\n"
					"in which case you need to raise the SHMMNI parameter in your kernel,\n"
					"or because the system's overall limit for shared memory has been\n"
			"reached.  If you cannot increase the shared memory limit,\n"
					"reduce PostgreSQL's shared memory request (currently %u bytes),\n"
					"by reducing its shared_buffers parameter (currently %d) and/or\n"
					"its max_connections parameter (currently %d).\n"
					"\n"
					"The PostgreSQL documentation contains more information about shared\n"
					"memory configuration.\n\n",
					size, NBuffers, MaxBackends);

		proc_exit(1);
	}

	/* Register on-exit routine to delete the new segment */
	on_shmem_exit(IpcMemoryDelete, Int32GetDatum(shmid));

	/* OK, should be able to attach to the segment */
#if defined(solaris) && defined(__sparc__)
	/* use intimate shared memory on SPARC Solaris */
	memAddress = shmat(shmid, 0, SHM_SHARE_MMU);
#else
	memAddress = shmat(shmid, 0, 0);
#endif

	if (memAddress == (void *) -1)
	{
		fprintf(stderr, "IpcMemoryCreate: shmat(id=%d) failed: %s\n",
				shmid, strerror(errno));
		proc_exit(1);
	}

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
		fprintf(stderr, "IpcMemoryDetach: shmdt(%p) failed: %s\n",
				DatumGetPointer(shmaddr), strerror(errno));

	/*
	 * We used to report a failure via elog(WARNING), but that's pretty
	 * pointless considering any client has long since disconnected ...
	 */
}

/****************************************************************************/
/*	IpcMemoryDelete(status, shmId)		deletes a shared memory segment		*/
/*	(called as an on_shmem_exit callback, hence funny argument list)		*/
/****************************************************************************/
static void
IpcMemoryDelete(int status, Datum shmId)
{
	if (shmctl(DatumGetInt32(shmId), IPC_RMID, (struct shmid_ds *) NULL) < 0)
		fprintf(stderr, "IpcMemoryDelete: shmctl(%d, %d, 0) failed: %s\n",
				DatumGetInt32(shmId), IPC_RMID, strerror(errno));

	/*
	 * We used to report a failure via elog(WARNING), but that's pretty
	 * pointless considering any client has long since disconnected ...
	 */
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
	 *
	 * If we are unable to perform the stat operation for a reason other than
	 * nonexistence of the segment (most likely, because it doesn't belong
	 * to our userid), assume it is in use.
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
		/* Else assume segment is in use */
		return true;
	}
	/* If it has attached processes, it's in use */
	if (shmStat.shm_nattch != 0)
		return true;
	return false;
}


/* ----------------------------------------------------------------
 *						private memory support
 *
 * Rather than allocating shmem segments with IPC_PRIVATE key, we
 * just malloc() the requested amount of space.  This code emulates
 * the needed shmem functions.
 * ----------------------------------------------------------------
 */

static void *
PrivateMemoryCreate(uint32 size)
{
	void	   *memAddress;

	memAddress = malloc(size);
	if (!memAddress)
	{
		fprintf(stderr, "PrivateMemoryCreate: malloc(%u) failed\n", size);
		proc_exit(1);
	}
	MemSet(memAddress, 0, size);	/* keep Purify quiet */

	/* Register on-exit routine to release storage */
	on_shmem_exit(PrivateMemoryDelete, PointerGetDatum(memAddress));

	return memAddress;
}

static void
PrivateMemoryDelete(int status, Datum memaddr)
{
	free(DatumGetPointer(memaddr));
}


/*
 * PGSharedMemoryCreate
 *
 * Create a shared memory segment of the given size and initialize its
 * standard header.  Also, register an on_shmem_exit callback to release
 * the storage.
 *
 * Dead Postgres segments are recycled if found, but we do not fail upon
 * collision with non-Postgres shmem segments.	The idea here is to detect and
 * re-use keys that may have been assigned by a crashed postmaster or backend.
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

	/* Room for a header? */
	Assert(size > MAXALIGN(sizeof(PGShmemHeader)));

#ifdef EXEC_BACKEND
	if (UsedShmemSegID != 0)
		NextShmemSegID = UsedShmemSegID;
	else
#endif
		NextShmemSegID = port * 1000 + 1;

	for (;;NextShmemSegID++)
	{
		IpcMemoryId shmid;

		/* Special case if creating a private segment --- just malloc() it */
		if (makePrivate)
		{
			memAddress = PrivateMemoryCreate(size);
			break;
		}

		/* Try to create new segment */
		memAddress = InternalIpcMemoryCreate(NextShmemSegID, size);
		if (memAddress)
			break;				/* successful create and attach */

		/* See if it looks to be leftover from a dead Postgres process */
		shmid = shmget(NextShmemSegID, sizeof(PGShmemHeader), 0);
		if (shmid < 0)
			continue;			/* failed: must be some other app's */

#if defined(solaris) && defined(__sparc__)
		/* use intimate shared memory on SPARC Solaris */
		memAddress = shmat(shmid, 0, SHM_SHARE_MMU);
#else
		memAddress = shmat(shmid, 0, 0);
#endif

		if (memAddress == (void *) -1)
			continue;			/* failed: must be some other app's */
		hdr = (PGShmemHeader *) memAddress;
		if (hdr->magic != PGShmemMagic)
		{
			shmdt(memAddress);
			continue;			/* segment belongs to a non-Postgres app */
		}

		/*
		 * If the creator PID is my own PID or does not belong to any
		 * extant process, it's safe to zap it.
		 */
		if (hdr->creatorPID != getpid())
		{
			if (kill(hdr->creatorPID, 0) == 0 ||
				errno != ESRCH)
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

#ifdef EXEC_BACKEND
	if (!makePrivate && UsedShmemSegID == 0)
		UsedShmemSegID = NextShmemSegID;
#endif

	return hdr;
}
