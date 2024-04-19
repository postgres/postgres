/*-------------------------------------------------------------------------
 *
 * slotsync.h
 *	  Exports for slot synchronization.
 *
 * Portions Copyright (c) 2016-2024, PostgreSQL Global Development Group
 *
 * src/include/replication/slotsync.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SLOTSYNC_H
#define SLOTSYNC_H

#include "replication/walreceiver.h"

extern PGDLLIMPORT bool sync_replication_slots;

/*
 * GUCs needed by slot sync worker to connect to the primary
 * server and carry on with slots synchronization.
 */
extern PGDLLIMPORT char *PrimaryConnInfo;
extern PGDLLIMPORT char *PrimarySlotName;

extern char *CheckAndGetDbnameFromConninfo(void);
extern bool ValidateSlotSyncParams(int elevel);

extern void ReplSlotSyncWorkerMain(char *startup_data, size_t startup_data_len) pg_attribute_noreturn();

extern void ShutDownSlotSync(void);
extern bool SlotSyncWorkerCanRestart(void);
extern bool IsSyncingReplicationSlots(void);
extern Size SlotSyncShmemSize(void);
extern void SlotSyncShmemInit(void);
extern void SyncReplicationSlots(WalReceiverConn *wrconn);

#endif							/* SLOTSYNC_H */
