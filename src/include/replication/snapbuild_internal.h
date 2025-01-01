/*-------------------------------------------------------------------------
 *
 * snapbuild_internal.h
 *    This file contains declarations for logical decoding utility
 *    functions for internal use.
 *
 * Copyright (c) 2024-2025, PostgreSQL Global Development Group
 *
 * src/include/replication/snapbuild_internal.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef SNAPBUILD_INTERNAL_H
#define SNAPBUILD_INTERNAL_H

#include "port/pg_crc32c.h"
#include "replication/reorderbuffer.h"
#include "replication/snapbuild.h"

/*
 * This struct contains the current state of the snapshot building
 * machinery. It is exposed to the public, so pay attention when changing its
 * contents.
 */
struct SnapBuild
{
	/* how far are we along building our first full snapshot */
	SnapBuildState state;

	/* private memory context used to allocate memory for this module. */
	MemoryContext context;

	/* all transactions < than this have committed/aborted */
	TransactionId xmin;

	/* all transactions >= than this are uncommitted */
	TransactionId xmax;

	/*
	 * Don't replay commits from an LSN < this LSN. This can be set externally
	 * but it will also be advanced (never retreat) from within snapbuild.c.
	 */
	XLogRecPtr	start_decoding_at;

	/*
	 * LSN at which two-phase decoding was enabled or LSN at which we found a
	 * consistent point at the time of slot creation.
	 *
	 * The prepared transactions, that were skipped because previously
	 * two-phase was not enabled or are not covered by initial snapshot, need
	 * to be sent later along with commit prepared and they must be before
	 * this point.
	 */
	XLogRecPtr	two_phase_at;

	/*
	 * Don't start decoding WAL until the "xl_running_xacts" information
	 * indicates there are no running xids with an xid smaller than this.
	 */
	TransactionId initial_xmin_horizon;

	/* Indicates if we are building full snapshot or just catalog one. */
	bool		building_full_snapshot;

	/*
	 * Indicates if we are using the snapshot builder for the creation of a
	 * logical replication slot. If it's true, the start point for decoding
	 * changes is not determined yet. So we skip snapshot restores to properly
	 * find the start point. See SnapBuildFindSnapshot() for details.
	 */
	bool		in_slot_creation;

	/*
	 * Snapshot that's valid to see the catalog state seen at this moment.
	 */
	Snapshot	snapshot;

	/*
	 * LSN of the last location we are sure a snapshot has been serialized to.
	 */
	XLogRecPtr	last_serialized_snapshot;

	/*
	 * The reorderbuffer we need to update with usable snapshots et al.
	 */
	ReorderBuffer *reorder;

	/*
	 * TransactionId at which the next phase of initial snapshot building will
	 * happen. InvalidTransactionId if not known (i.e. SNAPBUILD_START), or
	 * when no next phase necessary (SNAPBUILD_CONSISTENT).
	 */
	TransactionId next_phase_at;

	/*
	 * Array of transactions which could have catalog changes that committed
	 * between xmin and xmax.
	 */
	struct
	{
		/* number of committed transactions */
		size_t		xcnt;

		/* available space for committed transactions */
		size_t		xcnt_space;

		/*
		 * Until we reach a CONSISTENT state, we record commits of all
		 * transactions, not just the catalog changing ones. Record when that
		 * changes so we know we cannot export a snapshot safely anymore.
		 */
		bool		includes_all_transactions;

		/*
		 * Array of committed transactions that have modified the catalog.
		 *
		 * As this array is frequently modified we do *not* keep it in
		 * xidComparator order. Instead we sort the array when building &
		 * distributing a snapshot.
		 *
		 * TODO: It's unclear whether that reasoning has much merit. Every
		 * time we add something here after becoming consistent will also
		 * require distributing a snapshot. Storing them sorted would
		 * potentially also make it easier to purge (but more complicated wrt
		 * wraparound?). Should be improved if sorting while building the
		 * snapshot shows up in profiles.
		 */
		TransactionId *xip;
	}			committed;

	/*
	 * Array of transactions and subtransactions that had modified catalogs
	 * and were running when the snapshot was serialized.
	 *
	 * We normally rely on some WAL record types such as HEAP2_NEW_CID to know
	 * if the transaction has changed the catalog. But it could happen that
	 * the logical decoding decodes only the commit record of the transaction
	 * after restoring the previously serialized snapshot in which case we
	 * will miss adding the xid to the snapshot and end up looking at the
	 * catalogs with the wrong snapshot.
	 *
	 * Now to avoid the above problem, we serialize the transactions that had
	 * modified the catalogs and are still running at the time of snapshot
	 * serialization. We fill this array while restoring the snapshot and then
	 * refer it while decoding commit to ensure if the xact has modified the
	 * catalog. We discard this array when all the xids in the list become old
	 * enough to matter. See SnapBuildPurgeOlderTxn for details.
	 */
	struct
	{
		/* number of transactions */
		size_t		xcnt;

		/* This array must be sorted in xidComparator order */
		TransactionId *xip;
	}			catchange;
};

/* -----------------------------------
 * Snapshot serialization support
 * -----------------------------------
 */

/*
 * We store current state of struct SnapBuild on disk in the following manner:
 *
 * struct SnapBuildOnDisk;
 * TransactionId * committed.xcnt; (*not xcnt_space*)
 * TransactionId * catchange.xcnt;
 *
 * Check if the SnapBuildOnDiskConstantSize and SnapBuildOnDiskNotChecksummedSize
 * macros need to be updated when modifying the SnapBuildOnDisk struct.
 */
typedef struct SnapBuildOnDisk
{
	/* first part of this struct needs to be version independent */

	/* data not covered by checksum */
	uint32		magic;
	pg_crc32c	checksum;

	/* data covered by checksum */

	/* version, in case we want to support pg_upgrade */
	uint32		version;
	/* how large is the on disk data, excluding the constant sized part */
	uint32		length;

	/* version dependent part */
	SnapBuild	builder;

	/* variable amount of TransactionIds follows */
} SnapBuildOnDisk;

extern bool SnapBuildRestoreSnapshot(SnapBuildOnDisk *ondisk, const char *path,
									 MemoryContext context, bool missing_ok);

#endif							/* SNAPBUILD_INTERNAL_H */
