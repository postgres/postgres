/*-------------------------------------------------------------------------
 *
 * pgoutput.h
 *		Logical Replication output plugin
 *
 * Copyright (c) 2015-2019, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		pgoutput.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PGOUTPUT_H
#define PGOUTPUT_H

#include "nodes/pg_list.h"

typedef struct PGOutputData
{
	MemoryContext context;		/* private memory context for transient
								 * allocations */

	/* client info */
	uint32		protocol_version;

	List	   *publication_names;
	List	   *publications;
} PGOutputData;

#endif							/* PGOUTPUT_H */
