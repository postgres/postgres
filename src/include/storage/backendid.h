/*-------------------------------------------------------------------------
 *
 * backendid.h
 *	  POSTGRES backend id communication definitions
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/backendid.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef BACKENDID_H
#define BACKENDID_H

/*
 * BackendId uniquely identifies an active backend or auxiliary process.  It's
 * assigned at backend startup after authentication.  Note that a backend ID
 * can be reused for a different backend immediately after a backend exits.
 *
 * Backend IDs are assigned starting from 1. For historical reasons, BackendId
 * 0 is unused, but InvalidBackendId is defined as -1.
 */
typedef int BackendId;

#define InvalidBackendId		(-1)

extern PGDLLIMPORT BackendId MyBackendId;	/* backend id of this backend */

/* backend id of our parallel session leader, or InvalidBackendId if none */
extern PGDLLIMPORT BackendId ParallelLeaderBackendId;

/*
 * The BackendId to use for our session's temp relations is normally our own,
 * but parallel workers should use their leader's ID.
 */
#define BackendIdForTempRelations() \
	(ParallelLeaderBackendId == InvalidBackendId ? MyBackendId : ParallelLeaderBackendId)

#endif							/* BACKENDID_H */
