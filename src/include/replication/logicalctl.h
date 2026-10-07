/*-------------------------------------------------------------------------
 *
 * logicalctl.h
 *		Definitions for logical decoding status control facility.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/include/replication/logicalctl.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef LOGICALCTL_H
#define LOGICALCTL_H

#include "access/xlogdefs.h"

extern void StartupLogicalDecodingStatus(bool last_status);
extern void InitializeProcessXLogLogicalInfo(void);
extern bool ProcessBarrierUpdateXLogLogicalInfo(void);
extern bool IsLogicalDecodingEnabled(void);
extern bool StandbyLogicalDecodingEnabledSince(XLogRecPtr lsn);
extern bool IsXLogLogicalInfoEnabled(void);
extern void AtEOXact_LogicalCtl(void);
extern void EnsureLogicalDecodingEnabled(void);
extern void EnableLogicalDecoding(XLogRecPtr lsn);
extern void RequestDisableLogicalDecoding(void);
extern void DisableLogicalDecodingIfNecessary(void);
extern void DisableLogicalDecoding(void);
extern void UpdateLogicalDecodingStatusEndOfRecovery(void);

#endif
