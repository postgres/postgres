/*-------------------------------------------------------------------------
 *
 * sequence.c
 *	  Generic routines for sequence-related code.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/sequence/sequence.c
 *
 *
 * NOTES
 *	  This file contains sequence_ routines that implement access to sequences
 *	  (in contrast to other relation types like indexes).
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/relation.h"
#include "access/sequence.h"
#include "utils/rel.h"

static inline void validate_relation_kind(Relation r);

/* ----------------
 *		sequence_open - open a sequence relation by relation OID
 *
 *		This is essentially relation_open plus check that the relation
 *		is a sequence.
 * ----------------
 */
Relation
sequence_open(Oid relationId, LOCKMODE lockmode)
{
	Relation	r;

	r = relation_open(relationId, lockmode);

	validate_relation_kind(r);

	return r;
}

/* ----------------
 *		sequence_close - close a sequence
 *
 *		If lockmode is not "NoLock", we then release the specified lock.
 *
 *		Note that it is often sensible to hold a lock beyond relation_close;
 *		in that case, the lock is released automatically at xact end.
 * ----------------
 */
void
sequence_close(Relation relation, LOCKMODE lockmode)
{
	relation_close(relation, lockmode);
}

/* ----------------
 *		validate_relation_kind - check the relation's kind
 *
 *		Make sure relkind is from a sequence.
 * ----------------
 */
static inline void
validate_relation_kind(Relation r)
{
	if (r->rd_rel->relkind != RELKIND_SEQUENCE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot open relation \"%s\"",
						RelationGetRelationName(r)),
				 errdetail_relkind_not_supported(r->rd_rel->relkind)));
}
