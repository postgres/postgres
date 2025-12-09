/*
 * multixact_read_v18.h
 *
 * Copyright (c) 2025, PostgreSQL Global Development Group
 * src/bin/pg_upgrade/multixact_read_v18.h
 */
#ifndef MULTIXACT_READ_V18_H
#define MULTIXACT_READ_V18_H

#include "access/multixact.h"
#include "slru_io.h"

/*
 * MultiXactOffset changed from uint32 to uint64 between versions 18 and 19.
 * MultiXactOffset32 is used to represent a 32-bit offset from the old
 * cluster.
 */
typedef uint32 MultiXactOffset32;

typedef struct OldMultiXactReader
{
	MultiXactId nextMXact;
	MultiXactOffset32 nextOffset;

	SlruSegState *offset;
	SlruSegState *members;
} OldMultiXactReader;

extern OldMultiXactReader *AllocOldMultiXactRead(char *pgdata,
												 MultiXactId nextMulti,
												 MultiXactOffset32 nextOffset);
extern bool GetOldMultiXactIdSingleMember(OldMultiXactReader *state,
										  MultiXactId multi,
										  MultiXactMember *member);
extern void FreeOldMultiXactReader(OldMultiXactReader *reader);

#endif							/* MULTIXACT_READ_V18_H */
