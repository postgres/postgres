/*-------------------------------------------------------------------------
 *
 * pgoutput.h
 *		Logical Replication output plugin
 *
 * Copyright (c) 2015-2022, PostgreSQL Global Development Group
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

	/* client-supplied info: */
	uint32		protocol_version;
	List	   *publication_names;
	List	   *publications;
	bool		binary;
	bool		streaming;
	bool		messages;
	bool		two_phase;
} PGOutputData;

#endif							/* PGOUTPUT_H */
