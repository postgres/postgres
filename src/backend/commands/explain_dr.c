/*-------------------------------------------------------------------------
 *
 * explain_dr.c
 *	  Explain DestReceiver to measure serialization overhead
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994-5, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/commands/explain.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "commands/explain.h"
#include "commands/explain_dr.h"
#include "commands/explain_state.h"
#include "libpq/pqformat.h"
#include "libpq/protocol.h"
#include "utils/lsyscache.h"

/*
 * DestReceiver functions for SERIALIZE option
 *
 * A DestReceiver for query tuples, that serializes passed rows into RowData
 * messages while measuring the resources expended and total serialized size,
 * while never sending the data to the client.  This allows measuring the
 * overhead of deTOASTing and datatype out/sendfuncs, which are not otherwise
 * exercisable without actually hitting the network.
 */
typedef struct SerializeDestReceiver
{
	DestReceiver pub;
	ExplainState *es;			/* this EXPLAIN statement's ExplainState */
	int8		format;			/* text or binary, like pq wire protocol */
	TupleDesc	attrinfo;		/* the output tuple desc */
	int			nattrs;			/* current number of columns */
	FmgrInfo   *finfos;			/* precomputed call info for output fns */
	MemoryContext tmpcontext;	/* per-row temporary memory context */
	StringInfoData buf;			/* buffer to hold the constructed message */
	SerializeMetrics metrics;	/* collected metrics */
} SerializeDestReceiver;

/*
 * Get the function lookup info that we'll need for output.
 *
 * This is a subset of what printtup_prepare_info() does.  We don't need to
 * cope with format choices varying across columns, so it's slightly simpler.
 */
static void
serialize_prepare_info(SerializeDestReceiver *receiver,
					   TupleDesc typeinfo, int nattrs)
{
	/* get rid of any old data */
	if (receiver->finfos)
		pfree(receiver->finfos);
	receiver->finfos = NULL;

	receiver->attrinfo = typeinfo;
	receiver->nattrs = nattrs;
	if (nattrs <= 0)
		return;

	receiver->finfos = (FmgrInfo *) palloc0(nattrs * sizeof(FmgrInfo));

	for (int i = 0; i < nattrs; i++)
	{
		FmgrInfo   *finfo = receiver->finfos + i;
		Form_pg_attribute attr = TupleDescAttr(typeinfo, i);
		Oid			typoutput;
		Oid			typsend;
		bool		typisvarlena;

		if (receiver->format == 0)
		{
			/* wire protocol format text */
			getTypeOutputInfo(attr->atttypid,
							  &typoutput,
							  &typisvarlena);
			fmgr_info(typoutput, finfo);
		}
		else if (receiver->format == 1)
		{
			/* wire protocol format binary */
			getTypeBinaryOutputInfo(attr->atttypid,
									&typsend,
									&typisvarlena);
			fmgr_info(typsend, finfo);
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("unsupported format code: %d", receiver->format)));
	}
}

/*
 * serializeAnalyzeReceive - collect tuples for EXPLAIN (SERIALIZE)
 *
 * This should match printtup() in printtup.c as closely as possible,
 * except for the addition of measurement code.
 */
static bool
serializeAnalyzeReceive(TupleTableSlot *slot, DestReceiver *self)
{
	TupleDesc	typeinfo = slot->tts_tupleDescriptor;
	SerializeDestReceiver *myState = (SerializeDestReceiver *) self;
	MemoryContext oldcontext;
	StringInfo	buf = &myState->buf;
	int			natts = typeinfo->natts;
	instr_time	start,
				end;
	BufferUsage instr_start;

	/* only measure time, buffers if requested */
	if (myState->es->timing)
		INSTR_TIME_SET_CURRENT(start);
	if (myState->es->buffers)
		instr_start = pgBufferUsage;

	/* Set or update my derived attribute info, if needed */
	if (myState->attrinfo != typeinfo || myState->nattrs != natts)
		serialize_prepare_info(myState, typeinfo, natts);

	/* Make sure the tuple is fully deconstructed */
	slot_getallattrs(slot);

	/* Switch into per-row context so we can recover memory below */
	oldcontext = MemoryContextSwitchTo(myState->tmpcontext);

	/*
	 * Prepare a DataRow message (note buffer is in per-query context)
	 *
	 * Note that we fill a StringInfo buffer the same as printtup() does, so
	 * as to capture the costs of manipulating the strings accurately.
	 */
	pq_beginmessage_reuse(buf, PqMsg_DataRow);

	pq_sendint16(buf, natts);

	/*
	 * send the attributes of this tuple
	 */
	for (int i = 0; i < natts; i++)
	{
		FmgrInfo   *finfo = myState->finfos + i;
		Datum		attr = slot->tts_values[i];

		if (slot->tts_isnull[i])
		{
			pq_sendint32(buf, -1);
			continue;
		}

		if (myState->format == 0)
		{
			/* Text output */
			char	   *outputstr;

			outputstr = OutputFunctionCall(finfo, attr);
			pq_sendcountedtext(buf, outputstr, strlen(outputstr));
		}
		else
		{
			/* Binary output */
			bytea	   *outputbytes;

			outputbytes = SendFunctionCall(finfo, attr);
			pq_sendint32(buf, VARSIZE(outputbytes) - VARHDRSZ);
			pq_sendbytes(buf, VARDATA(outputbytes),
						 VARSIZE(outputbytes) - VARHDRSZ);
		}
	}

	/*
	 * We mustn't call pq_endmessage_reuse(), since that would actually send
	 * the data to the client.  Just count the data, instead.  We can leave
	 * the buffer alone; it'll be reset on the next iteration (as would also
	 * happen in printtup()).
	 */
	myState->metrics.bytesSent += buf->len;

	/* Return to caller's context, and flush row's temporary memory */
	MemoryContextSwitchTo(oldcontext);
	MemoryContextReset(myState->tmpcontext);

	/* Update timing data */
	if (myState->es->timing)
	{
		INSTR_TIME_SET_CURRENT(end);
		INSTR_TIME_ACCUM_DIFF(myState->metrics.timeSpent, end, start);
	}

	/* Update buffer metrics */
	if (myState->es->buffers)
		BufferUsageAccumDiff(&myState->metrics.bufferUsage,
							 &pgBufferUsage,
							 &instr_start);

	return true;
}

/*
 * serializeAnalyzeStartup - start up the serializeAnalyze receiver
 */
static void
serializeAnalyzeStartup(DestReceiver *self, int operation, TupleDesc typeinfo)
{
	SerializeDestReceiver *receiver = (SerializeDestReceiver *) self;

	Assert(receiver->es != NULL);

	switch (receiver->es->serialize)
	{
		case EXPLAIN_SERIALIZE_NONE:
			Assert(false);
			break;
		case EXPLAIN_SERIALIZE_TEXT:
			receiver->format = 0;	/* wire protocol format text */
			break;
		case EXPLAIN_SERIALIZE_BINARY:
			receiver->format = 1;	/* wire protocol format binary */
			break;
	}

	/* Create per-row temporary memory context */
	receiver->tmpcontext = AllocSetContextCreate(CurrentMemoryContext,
												 "SerializeTupleReceive",
												 ALLOCSET_DEFAULT_SIZES);

	/* The output buffer is re-used across rows, as in printtup.c */
	initStringInfo(&receiver->buf);

	/* Initialize results counters */
	memset(&receiver->metrics, 0, sizeof(SerializeMetrics));
	INSTR_TIME_SET_ZERO(receiver->metrics.timeSpent);
}

/*
 * serializeAnalyzeShutdown - shut down the serializeAnalyze receiver
 */
static void
serializeAnalyzeShutdown(DestReceiver *self)
{
	SerializeDestReceiver *receiver = (SerializeDestReceiver *) self;

	if (receiver->finfos)
		pfree(receiver->finfos);
	receiver->finfos = NULL;

	if (receiver->buf.data)
		pfree(receiver->buf.data);
	receiver->buf.data = NULL;

	if (receiver->tmpcontext)
		MemoryContextDelete(receiver->tmpcontext);
	receiver->tmpcontext = NULL;
}

/*
 * serializeAnalyzeDestroy - destroy the serializeAnalyze receiver
 */
static void
serializeAnalyzeDestroy(DestReceiver *self)
{
	pfree(self);
}

/*
 * Build a DestReceiver for EXPLAIN (SERIALIZE) instrumentation.
 */
DestReceiver *
CreateExplainSerializeDestReceiver(ExplainState *es)
{
	SerializeDestReceiver *self;

	self = (SerializeDestReceiver *) palloc0(sizeof(SerializeDestReceiver));

	self->pub.receiveSlot = serializeAnalyzeReceive;
	self->pub.rStartup = serializeAnalyzeStartup;
	self->pub.rShutdown = serializeAnalyzeShutdown;
	self->pub.rDestroy = serializeAnalyzeDestroy;
	self->pub.mydest = DestExplainSerialize;

	self->es = es;

	return (DestReceiver *) self;
}

/*
 * GetSerializationMetrics - collect metrics
 *
 * We have to be careful here since the receiver could be an IntoRel
 * receiver if the subject statement is CREATE TABLE AS.  In that
 * case, return all-zeroes stats.
 */
SerializeMetrics
GetSerializationMetrics(DestReceiver *dest)
{
	SerializeMetrics empty;

	if (dest->mydest == DestExplainSerialize)
		return ((SerializeDestReceiver *) dest)->metrics;

	memset(&empty, 0, sizeof(SerializeMetrics));
	INSTR_TIME_SET_ZERO(empty.timeSpent);

	return empty;
}
