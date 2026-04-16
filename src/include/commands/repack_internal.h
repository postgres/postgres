/*-------------------------------------------------------------------------
 *
 * repack_internal.h
 *	  header for REPACK internals
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/include/commands/repack_internal.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef REPACK_INTERNAL_H
#define REPACK_INTERNAL_H

#include "nodes/execnodes.h"
#include "replication/decode.h"
#include "postmaster/bgworker.h"
#include "replication/logical.h"
#include "storage/buffile.h"
#include "storage/sharedfileset.h"
#include "storage/shm_mq.h"
#include "utils/resowner.h"

/*
 * The type of a change stored in the output files.
 */
typedef char ConcurrentChangeKind;

#define		CHANGE_INSERT		'i'
#define		CHANGE_UPDATE_OLD	'u'
#define		CHANGE_UPDATE_NEW	'U'
#define		CHANGE_DELETE		'd'

/*
 * Logical decoding state.
 *
 * The output plugin uses it to store the data changes that it decodes from
 * WAL while the table contents is being copied to a new storage.
 */
typedef struct RepackDecodingState
{
#ifdef	USE_ASSERT_CHECKING
	/* The relation whose changes we're decoding. */
	Oid			relid;
#endif

	/* Per-change memory context. */
	MemoryContext change_cxt;

	/* A tuple slot used to pass tuples back and forth */
	TupleTableSlot *slot;

	/*
	 * Memory context and resource owner of the decoding worker's transaction.
	 */
	MemoryContext worker_cxt;
	ResourceOwner worker_resowner;

	/* The current output file. */
	BufFile    *file;
} RepackDecodingState;

/*
 * Shared memory used for communication between the backend running REPACK and
 * the worker that performs logical decoding of data changes.
 */
typedef struct DecodingWorkerShared
{
	/* Is the decoding initialized? */
	bool		initialized;

	/*
	 * Once the worker has reached this LSN, it should close the current
	 * output file and either create a new one or exit, according to the field
	 * 'done'. If the value is InvalidXLogRecPtr, the worker should decode all
	 * the WAL available and keep checking this field. It is ok if the worker
	 * had already decoded records whose LSN is >= lsn_upto before this field
	 * has been set.
	 */
	XLogRecPtr	lsn_upto;

	/* Exit after closing the current file? */
	bool		done;

	/* The output is stored here. */
	SharedFileSet sfs;

	/* Number of the last file exported by the worker. */
	int			last_exported;

	/* Synchronize access to the fields above. */
	slock_t		mutex;

	/* Database to connect to. */
	Oid			dbid;

	/* Role to connect as. */
	Oid			roleid;

	/* Relation from which data changes to decode. */
	Oid			relid;

	/* CV the backend waits on */
	ConditionVariable cv;

	/* Info to signal the backend. */
	PGPROC	   *backend_proc;
	pid_t		backend_pid;
	ProcNumber	backend_proc_number;

	/*
	 * Memory the queue is located in.
	 *
	 * For considerations on the value see the comments of
	 * PARALLEL_ERROR_QUEUE_SIZE.
	 */
#define REPACK_ERROR_QUEUE_SIZE			16384
	char		error_queue[FLEXIBLE_ARRAY_MEMBER];
} DecodingWorkerShared;

extern void DecodingWorkerFileName(char *fname, Oid relid, uint32 seq);


#endif							/* REPACK_INTERNAL_H */
