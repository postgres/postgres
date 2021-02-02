/* -------------------------------------------------------------------------
 *
 * pg_sequence.h
 *	  definition of the "sequence" system catalog (pg_sequence)
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_sequence.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 * -------------------------------------------------------------------------
 */
#ifndef PG_SEQUENCE_H
#define PG_SEQUENCE_H

#include "catalog/genbki.h"
#include "catalog/pg_sequence_d.h"

CATALOG(pg_sequence,2224,SequenceRelationId)
{
	Oid			seqrelid BKI_LOOKUP(pg_class);
	Oid			seqtypid BKI_LOOKUP(pg_type);
	int64		seqstart;
	int64		seqincrement;
	int64		seqmax;
	int64		seqmin;
	int64		seqcache;
	bool		seqcycle;
} FormData_pg_sequence;

/* ----------------
 *		Form_pg_sequence corresponds to a pointer to a tuple with
 *		the format of pg_sequence relation.
 * ----------------
 */
typedef FormData_pg_sequence *Form_pg_sequence;

DECLARE_UNIQUE_INDEX_PKEY(pg_sequence_seqrelid_index, 5002, on pg_sequence using btree(seqrelid oid_ops));
#define SequenceRelidIndexId	5002

#endif							/* PG_SEQUENCE_H */
