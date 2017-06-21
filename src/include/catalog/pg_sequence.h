#ifndef PG_SEQUENCE_H
#define PG_SEQUENCE_H

#include "catalog/genbki.h"

#define SequenceRelationId	2224

CATALOG(pg_sequence,2224) BKI_WITHOUT_OIDS
{
	Oid			seqrelid;
	Oid			seqtypid;
	int64		seqstart;
	int64		seqincrement;
	int64		seqmax;
	int64		seqmin;
	int64		seqcache;
	bool		seqcycle;
} FormData_pg_sequence;

typedef FormData_pg_sequence *Form_pg_sequence;

#define Natts_pg_sequence				8
#define Anum_pg_sequence_seqrelid		1
#define Anum_pg_sequence_seqtypid		2
#define Anum_pg_sequence_seqstart		3
#define Anum_pg_sequence_seqincrement	4
#define Anum_pg_sequence_seqmax			5
#define Anum_pg_sequence_seqmin			6
#define Anum_pg_sequence_seqcache		7
#define Anum_pg_sequence_seqcycle		8

#endif							/* PG_SEQUENCE_H */
