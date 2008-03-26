/*-------------------------------------------------------------------------
 *
 * snapmgmt.h
 *	  POSTGRES snapshot management definitions
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/utils/snapmgmt.h,v 1.1 2008/03/26 16:20:48 alvherre Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef SNAPMGMT_H
#define SNAPMGMT_H

#include "utils/snapshot.h"


extern PGDLLIMPORT Snapshot SerializableSnapshot;
extern PGDLLIMPORT Snapshot LatestSnapshot;
extern PGDLLIMPORT Snapshot ActiveSnapshot;

extern TransactionId TransactionXmin;
extern TransactionId RecentXmin;
extern TransactionId RecentGlobalXmin;

extern Snapshot GetTransactionSnapshot(void);
extern Snapshot GetLatestSnapshot(void);
extern Snapshot CopySnapshot(Snapshot snapshot);
extern void FreeSnapshot(Snapshot snapshot);
extern void FreeXactSnapshot(void);

#endif /* SNAPMGMT_H */
