/*-------------------------------------------------------------------------
 *
 * explain_dr.h
 *	  prototypes for explain_dr.c
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994-5, Regents of the University of California
 *
 * src/include/commands/explain_dr.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef EXPLAIN_DR_H
#define EXPLAIN_DR_H

#include "executor/instrument.h"
#include "tcop/dest.h"

struct ExplainState;			/* avoid including explain.h here */

/* Instrumentation data for EXPLAIN's SERIALIZE option */
typedef struct SerializeMetrics
{
	uint64		bytesSent;		/* # of bytes serialized */
	instr_time	timeSpent;		/* time spent serializing */
	BufferUsage bufferUsage;	/* buffers accessed during serialization */
} SerializeMetrics;

extern DestReceiver *CreateExplainSerializeDestReceiver(struct ExplainState *es);
extern SerializeMetrics GetSerializationMetrics(DestReceiver *dest);

#endif
