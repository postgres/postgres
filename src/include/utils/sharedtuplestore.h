/*-------------------------------------------------------------------------
 *
 * sharedtuplestore.h
 *	  Simple mechanism for sharing tuples between backends.
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/sharedtuplestore.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SHAREDTUPLESTORE_H
#define SHAREDTUPLESTORE_H

#include "access/htup.h"
#include "storage/fd.h"
#include "storage/sharedfileset.h"

struct SharedTuplestore;
typedef struct SharedTuplestore SharedTuplestore;

struct SharedTuplestoreAccessor;
typedef struct SharedTuplestoreAccessor SharedTuplestoreAccessor;
struct tupleMetadata;
typedef struct tupleMetadata tupleMetadata;
struct tupleMetadata
{
	uint32		hashvalue;
	union
	{
		uint32		tupleid;	/* tuple number or id on the outer side */
		int			stripe;		/* stripe number for inner side */
	};
};

/*
 * A flag indicating that the tuplestore will only be scanned once, so backing
 * files can be unlinked early.
 */
#define SHARED_TUPLESTORE_SINGLE_PASS 0x01

extern size_t sts_estimate(int participants);

extern SharedTuplestoreAccessor *sts_initialize(SharedTuplestore *sts,
												int participants,
												int my_participant_number,
												size_t meta_data_size,
												int flags,
												SharedFileSet *fileset,
												const char *name);

extern SharedTuplestoreAccessor *sts_attach(SharedTuplestore *sts,
											int my_participant_number,
											SharedFileSet *fileset);

extern void sts_end_write(SharedTuplestoreAccessor *accessor);

extern void sts_reinitialize(SharedTuplestoreAccessor *accessor);

extern void sts_begin_parallel_scan(SharedTuplestoreAccessor *accessor);

extern void sts_resume_parallel_scan(SharedTuplestoreAccessor *accessor);

extern void sts_end_parallel_scan(SharedTuplestoreAccessor *accessor);

extern void sts_puttuple(SharedTuplestoreAccessor *accessor,
						 void *meta_data,
						 MinimalTuple tuple);

extern MinimalTuple sts_parallel_scan_next(SharedTuplestoreAccessor *accessor,
										   void *meta_data);

extern void sts_parallel_scan_rewind(SharedTuplestoreAccessor *accessor);

extern void sts_reset_rewound(SharedTuplestoreAccessor *accessor);
extern uint32 sts_increment_ntuples(SharedTuplestoreAccessor *accessor);
extern uint32 sts_get_tuplenum(SharedTuplestoreAccessor *accessor);

#endif							/* SHAREDTUPLESTORE_H */
