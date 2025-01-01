/*-------------------------------------------------------------------------
 *
 * sequence.h
 *	  Generic routines for sequence-related code.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/sequence.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef ACCESS_SEQUENCE_H
#define ACCESS_SEQUENCE_H

#include "storage/lockdefs.h"
#include "utils/relcache.h"

extern Relation sequence_open(Oid relationId, LOCKMODE lockmode);
extern void sequence_close(Relation relation, LOCKMODE lockmode);

#endif							/* ACCESS_SEQUENCE_H */
