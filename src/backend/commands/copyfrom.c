/*-------------------------------------------------------------------------
 *
 * copyfrom.c
 *		COPY <table> FROM file/program/client
 *
 * This file contains routines needed to efficiently load tuples into a
 * table.  That includes looking up the correct partition, firing triggers,
 * calling the table AM function to insert the data, and updating indexes.
 * Reading data from the input file or client and parsing it into Datums
 * is handled in copyfromparse.c.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/copyfrom.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>
#include <unistd.h>
#include <sys/stat.h>

#include "access/heapam.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "commands/copyapi.h"
#include "commands/copyfrom_internal.h"
#include "commands/progress.h"
#include "commands/trigger.h"
#include "executor/execPartition.h"
#include "executor/executor.h"
#include "executor/nodeModifyTable.h"
#include "executor/tuptable.h"
#include "foreign/fdwapi.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/miscnodes.h"
#include "optimizer/optimizer.h"
#include "pgstat.h"
#include "rewrite/rewriteHandler.h"
#include "storage/fd.h"
#include "tcop/tcopprot.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/portal.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

/*
 * No more than this many tuples per CopyMultiInsertBuffer
 *
 * Caution: Don't make this too big, as we could end up with this many
 * CopyMultiInsertBuffer items stored in CopyMultiInsertInfo's
 * multiInsertBuffers list.  Increasing this can cause quadratic growth in
 * memory requirements during copies into partitioned tables with a large
 * number of partitions.
 */
#define MAX_BUFFERED_TUPLES		1000

/*
 * Flush buffers if there are >= this many bytes, as counted by the input
 * size, of tuples stored.
 */
#define MAX_BUFFERED_BYTES		65535

/*
 * Trim the list of buffers back down to this number after flushing.  This
 * must be >= 2.
 */
#define MAX_PARTITION_BUFFERS	32

/* Stores multi-insert data related to a single relation in CopyFrom. */
typedef struct CopyMultiInsertBuffer
{
	TupleTableSlot *slots[MAX_BUFFERED_TUPLES]; /* Array to store tuples */
	ResultRelInfo *resultRelInfo;	/* ResultRelInfo for 'relid' */
	BulkInsertState bistate;	/* BulkInsertState for this rel if plain
								 * table; NULL if foreign table */
	int			nused;			/* number of 'slots' containing tuples */
	uint64		linenos[MAX_BUFFERED_TUPLES];	/* Line # of tuple in copy
												 * stream */
} CopyMultiInsertBuffer;

/*
 * Stores one or many CopyMultiInsertBuffers and details about the size and
 * number of tuples which are stored in them.  This allows multiple buffers to
 * exist at once when COPYing into a partitioned table.
 */
typedef struct CopyMultiInsertInfo
{
	List	   *multiInsertBuffers; /* List of tracked CopyMultiInsertBuffers */
	int			bufferedTuples; /* number of tuples buffered over all buffers */
	int			bufferedBytes;	/* number of bytes from all buffered tuples */
	CopyFromState cstate;		/* Copy state for this CopyMultiInsertInfo */
	EState	   *estate;			/* Executor state used for COPY */
	CommandId	mycid;			/* Command Id used for COPY */
	int			ti_options;		/* table insert options */
} CopyMultiInsertInfo;


/* non-export function prototypes */
static void ClosePipeFromProgram(CopyFromState cstate);

/*
 * Built-in format-specific routines. One-row callbacks are defined in
 * copyfromparse.c.
 */
static void CopyFromTextLikeInFunc(CopyFromState cstate, Oid atttypid, FmgrInfo *finfo,
								   Oid *typioparam);
static void CopyFromTextLikeStart(CopyFromState cstate, TupleDesc tupDesc);
static void CopyFromTextLikeEnd(CopyFromState cstate);
static void CopyFromBinaryInFunc(CopyFromState cstate, Oid atttypid,
								 FmgrInfo *finfo, Oid *typioparam);
static void CopyFromBinaryStart(CopyFromState cstate, TupleDesc tupDesc);
static void CopyFromBinaryEnd(CopyFromState cstate);


/*
 * COPY FROM routines for built-in formats.
 *
 * CSV and text formats share the same TextLike routines except for the
 * one-row callback.
 */

/* text format */
static const CopyFromRoutine CopyFromRoutineText = {
	.CopyFromInFunc = CopyFromTextLikeInFunc,
	.CopyFromStart = CopyFromTextLikeStart,
	.CopyFromOneRow = CopyFromTextOneRow,
	.CopyFromEnd = CopyFromTextLikeEnd,
};

/* CSV format */
static const CopyFromRoutine CopyFromRoutineCSV = {
	.CopyFromInFunc = CopyFromTextLikeInFunc,
	.CopyFromStart = CopyFromTextLikeStart,
	.CopyFromOneRow = CopyFromCSVOneRow,
	.CopyFromEnd = CopyFromTextLikeEnd,
};

/* binary format */
static const CopyFromRoutine CopyFromRoutineBinary = {
	.CopyFromInFunc = CopyFromBinaryInFunc,
	.CopyFromStart = CopyFromBinaryStart,
	.CopyFromOneRow = CopyFromBinaryOneRow,
	.CopyFromEnd = CopyFromBinaryEnd,
};

/* Return a COPY FROM routine for the given options */
static const CopyFromRoutine *
CopyFromGetRoutine(const CopyFormatOptions *opts)
{
	if (opts->csv_mode)
		return &CopyFromRoutineCSV;
	else if (opts->binary)
		return &CopyFromRoutineBinary;

	/* default is text */
	return &CopyFromRoutineText;
}

/* Implementation of the start callback for text and CSV formats */
static void
CopyFromTextLikeStart(CopyFromState cstate, TupleDesc tupDesc)
{
	AttrNumber	attr_count;

	/*
	 * If encoding conversion is needed, we need another buffer to hold the
	 * converted input data.  Otherwise, we can just point input_buf to the
	 * same buffer as raw_buf.
	 */
	if (cstate->need_transcoding)
	{
		cstate->input_buf = (char *) palloc(INPUT_BUF_SIZE + 1);
		cstate->input_buf_index = cstate->input_buf_len = 0;
	}
	else
		cstate->input_buf = cstate->raw_buf;
	cstate->input_reached_eof = false;

	initStringInfo(&cstate->line_buf);

	/*
	 * Create workspace for CopyReadAttributes results; used by CSV and text
	 * format.
	 */
	attr_count = list_length(cstate->attnumlist);
	cstate->max_fields = attr_count;
	cstate->raw_fields = (char **) palloc(attr_count * sizeof(char *));
}

/*
 * Implementation of the infunc callback for text and CSV formats. Assign
 * the input function data to the given *finfo.
 */
static void
CopyFromTextLikeInFunc(CopyFromState cstate, Oid atttypid, FmgrInfo *finfo,
					   Oid *typioparam)
{
	Oid			func_oid;

	getTypeInputInfo(atttypid, &func_oid, typioparam);
	fmgr_info(func_oid, finfo);
}

/* Implementation of the end callback for text and CSV formats */
static void
CopyFromTextLikeEnd(CopyFromState cstate)
{
	/* nothing to do */
}

/* Implementation of the start callback for binary format */
static void
CopyFromBinaryStart(CopyFromState cstate, TupleDesc tupDesc)
{
	/* Read and verify binary header */
	ReceiveCopyBinaryHeader(cstate);
}

/*
 * Implementation of the infunc callback for binary format. Assign
 * the binary input function to the given *finfo.
 */
static void
CopyFromBinaryInFunc(CopyFromState cstate, Oid atttypid,
					 FmgrInfo *finfo, Oid *typioparam)
{
	Oid			func_oid;

	getTypeBinaryInputInfo(atttypid, &func_oid, typioparam);
	fmgr_info(func_oid, finfo);
}

/* Implementation of the end callback for binary format */
static void
CopyFromBinaryEnd(CopyFromState cstate)
{
	/* nothing to do */
}

/*
 * error context callback for COPY FROM
 *
 * The argument for the error context must be CopyFromState.
 */
void
CopyFromErrorCallback(void *arg)
{
	CopyFromState cstate = (CopyFromState) arg;

	if (cstate->relname_only)
	{
		errcontext("COPY %s",
				   cstate->cur_relname);
		return;
	}
	if (cstate->opts.binary)
	{
		/* can't usefully display the data */
		if (cstate->cur_attname)
			errcontext("COPY %s, line %llu, column %s",
					   cstate->cur_relname,
					   (unsigned long long) cstate->cur_lineno,
					   cstate->cur_attname);
		else
			errcontext("COPY %s, line %llu",
					   cstate->cur_relname,
					   (unsigned long long) cstate->cur_lineno);
	}
	else
	{
		if (cstate->cur_attname && cstate->cur_attval)
		{
			/* error is relevant to a particular column */
			char	   *attval;

			attval = CopyLimitPrintoutLength(cstate->cur_attval);
			errcontext("COPY %s, line %llu, column %s: \"%s\"",
					   cstate->cur_relname,
					   (unsigned long long) cstate->cur_lineno,
					   cstate->cur_attname,
					   attval);
			pfree(attval);
		}
		else if (cstate->cur_attname)
		{
			/* error is relevant to a particular column, value is NULL */
			errcontext("COPY %s, line %llu, column %s: null input",
					   cstate->cur_relname,
					   (unsigned long long) cstate->cur_lineno,
					   cstate->cur_attname);
		}
		else
		{
			/*
			 * Error is relevant to a particular line.
			 *
			 * If line_buf still contains the correct line, print it.
			 */
			if (cstate->line_buf_valid)
			{
				char	   *lineval;

				lineval = CopyLimitPrintoutLength(cstate->line_buf.data);
				errcontext("COPY %s, line %llu: \"%s\"",
						   cstate->cur_relname,
						   (unsigned long long) cstate->cur_lineno, lineval);
				pfree(lineval);
			}
			else
			{
				errcontext("COPY %s, line %llu",
						   cstate->cur_relname,
						   (unsigned long long) cstate->cur_lineno);
			}
		}
	}
}

/*
 * Make sure we don't print an unreasonable amount of COPY data in a message.
 *
 * Returns a pstrdup'd copy of the input.
 */
char *
CopyLimitPrintoutLength(const char *str)
{
#define MAX_COPY_DATA_DISPLAY 100

	int			slen = strlen(str);
	int			len;
	char	   *res;

	/* Fast path if definitely okay */
	if (slen <= MAX_COPY_DATA_DISPLAY)
		return pstrdup(str);

	/* Apply encoding-dependent truncation */
	len = pg_mbcliplen(str, slen, MAX_COPY_DATA_DISPLAY);

	/*
	 * Truncate, and add "..." to show we truncated the input.
	 */
	res = (char *) palloc(len + 4);
	memcpy(res, str, len);
	strcpy(res + len, "...");

	return res;
}

/*
 * Allocate memory and initialize a new CopyMultiInsertBuffer for this
 * ResultRelInfo.
 */
static CopyMultiInsertBuffer *
CopyMultiInsertBufferInit(ResultRelInfo *rri)
{
	CopyMultiInsertBuffer *buffer;

	buffer = (CopyMultiInsertBuffer *) palloc(sizeof(CopyMultiInsertBuffer));
	memset(buffer->slots, 0, sizeof(TupleTableSlot *) * MAX_BUFFERED_TUPLES);
	buffer->resultRelInfo = rri;
	buffer->bistate = (rri->ri_FdwRoutine == NULL) ? GetBulkInsertState() : NULL;
	buffer->nused = 0;

	return buffer;
}

/*
 * Make a new buffer for this ResultRelInfo.
 */
static inline void
CopyMultiInsertInfoSetupBuffer(CopyMultiInsertInfo *miinfo,
							   ResultRelInfo *rri)
{
	CopyMultiInsertBuffer *buffer;

	buffer = CopyMultiInsertBufferInit(rri);

	/* Setup back-link so we can easily find this buffer again */
	rri->ri_CopyMultiInsertBuffer = buffer;
	/* Record that we're tracking this buffer */
	miinfo->multiInsertBuffers = lappend(miinfo->multiInsertBuffers, buffer);
}

/*
 * Initialize an already allocated CopyMultiInsertInfo.
 *
 * If rri is a non-partitioned table then a CopyMultiInsertBuffer is set up
 * for that table.
 */
static void
CopyMultiInsertInfoInit(CopyMultiInsertInfo *miinfo, ResultRelInfo *rri,
						CopyFromState cstate, EState *estate, CommandId mycid,
						int ti_options)
{
	miinfo->multiInsertBuffers = NIL;
	miinfo->bufferedTuples = 0;
	miinfo->bufferedBytes = 0;
	miinfo->cstate = cstate;
	miinfo->estate = estate;
	miinfo->mycid = mycid;
	miinfo->ti_options = ti_options;

	/*
	 * Only setup the buffer when not dealing with a partitioned table.
	 * Buffers for partitioned tables will just be setup when we need to send
	 * tuples their way for the first time.
	 */
	if (rri->ri_RelationDesc->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
		CopyMultiInsertInfoSetupBuffer(miinfo, rri);
}

/*
 * Returns true if the buffers are full
 */
static inline bool
CopyMultiInsertInfoIsFull(CopyMultiInsertInfo *miinfo)
{
	if (miinfo->bufferedTuples >= MAX_BUFFERED_TUPLES ||
		miinfo->bufferedBytes >= MAX_BUFFERED_BYTES)
		return true;
	return false;
}

/*
 * Returns true if we have no buffered tuples
 */
static inline bool
CopyMultiInsertInfoIsEmpty(CopyMultiInsertInfo *miinfo)
{
	return miinfo->bufferedTuples == 0;
}

/*
 * Write the tuples stored in 'buffer' out to the table.
 */
static inline void
CopyMultiInsertBufferFlush(CopyMultiInsertInfo *miinfo,
						   CopyMultiInsertBuffer *buffer,
						   int64 *processed)
{
	CopyFromState cstate = miinfo->cstate;
	EState	   *estate = miinfo->estate;
	int			nused = buffer->nused;
	ResultRelInfo *resultRelInfo = buffer->resultRelInfo;
	TupleTableSlot **slots = buffer->slots;
	int			i;

	if (resultRelInfo->ri_FdwRoutine)
	{
		int			batch_size = resultRelInfo->ri_BatchSize;
		int			sent = 0;

		Assert(buffer->bistate == NULL);

		/* Ensure that the FDW supports batching and it's enabled */
		Assert(resultRelInfo->ri_FdwRoutine->ExecForeignBatchInsert);
		Assert(batch_size > 1);

		/*
		 * We suppress error context information other than the relation name,
		 * if one of the operations below fails.
		 */
		Assert(!cstate->relname_only);
		cstate->relname_only = true;

		while (sent < nused)
		{
			int			size = (batch_size < nused - sent) ? batch_size : (nused - sent);
			int			inserted = size;
			TupleTableSlot **rslots;

			/* insert into foreign table: let the FDW do it */
			rslots =
				resultRelInfo->ri_FdwRoutine->ExecForeignBatchInsert(estate,
																	 resultRelInfo,
																	 &slots[sent],
																	 NULL,
																	 &inserted);

			sent += size;

			/* No need to do anything if there are no inserted rows */
			if (inserted <= 0)
				continue;

			/* Triggers on foreign tables should not have transition tables */
			Assert(resultRelInfo->ri_TrigDesc == NULL ||
				   resultRelInfo->ri_TrigDesc->trig_insert_new_table == false);

			/* Run AFTER ROW INSERT triggers */
			if (resultRelInfo->ri_TrigDesc != NULL &&
				resultRelInfo->ri_TrigDesc->trig_insert_after_row)
			{
				Oid			relid = RelationGetRelid(resultRelInfo->ri_RelationDesc);

				for (i = 0; i < inserted; i++)
				{
					TupleTableSlot *slot = rslots[i];

					/*
					 * AFTER ROW Triggers might reference the tableoid column,
					 * so (re-)initialize tts_tableOid before evaluating them.
					 */
					slot->tts_tableOid = relid;

					ExecARInsertTriggers(estate, resultRelInfo,
										 slot, NIL,
										 cstate->transition_capture);
				}
			}

			/* Update the row counter and progress of the COPY command */
			*processed += inserted;
			pgstat_progress_update_param(PROGRESS_COPY_TUPLES_PROCESSED,
										 *processed);
		}

		for (i = 0; i < nused; i++)
			ExecClearTuple(slots[i]);

		/* reset relname_only */
		cstate->relname_only = false;
	}
	else
	{
		CommandId	mycid = miinfo->mycid;
		int			ti_options = miinfo->ti_options;
		bool		line_buf_valid = cstate->line_buf_valid;
		uint64		save_cur_lineno = cstate->cur_lineno;
		MemoryContext oldcontext;

		Assert(buffer->bistate != NULL);

		/*
		 * Print error context information correctly, if one of the operations
		 * below fails.
		 */
		cstate->line_buf_valid = false;

		/*
		 * table_multi_insert may leak memory, so switch to short-lived memory
		 * context before calling it.
		 */
		oldcontext = MemoryContextSwitchTo(GetPerTupleMemoryContext(estate));
		table_multi_insert(resultRelInfo->ri_RelationDesc,
						   slots,
						   nused,
						   mycid,
						   ti_options,
						   buffer->bistate);
		MemoryContextSwitchTo(oldcontext);

		for (i = 0; i < nused; i++)
		{
			/*
			 * If there are any indexes, update them for all the inserted
			 * tuples, and run AFTER ROW INSERT triggers.
			 */
			if (resultRelInfo->ri_NumIndices > 0)
			{
				List	   *recheckIndexes;

				cstate->cur_lineno = buffer->linenos[i];
				recheckIndexes =
					ExecInsertIndexTuples(resultRelInfo,
										  buffer->slots[i], estate, false,
										  false, NULL, NIL, false);
				ExecARInsertTriggers(estate, resultRelInfo,
									 slots[i], recheckIndexes,
									 cstate->transition_capture);
				list_free(recheckIndexes);
			}

			/*
			 * There's no indexes, but see if we need to run AFTER ROW INSERT
			 * triggers anyway.
			 */
			else if (resultRelInfo->ri_TrigDesc != NULL &&
					 (resultRelInfo->ri_TrigDesc->trig_insert_after_row ||
					  resultRelInfo->ri_TrigDesc->trig_insert_new_table))
			{
				cstate->cur_lineno = buffer->linenos[i];
				ExecARInsertTriggers(estate, resultRelInfo,
									 slots[i], NIL,
									 cstate->transition_capture);
			}

			ExecClearTuple(slots[i]);
		}

		/* Update the row counter and progress of the COPY command */
		*processed += nused;
		pgstat_progress_update_param(PROGRESS_COPY_TUPLES_PROCESSED,
									 *processed);

		/* reset cur_lineno and line_buf_valid to what they were */
		cstate->line_buf_valid = line_buf_valid;
		cstate->cur_lineno = save_cur_lineno;
	}

	/* Mark that all slots are free */
	buffer->nused = 0;
}

/*
 * Drop used slots and free member for this buffer.
 *
 * The buffer must be flushed before cleanup.
 */
static inline void
CopyMultiInsertBufferCleanup(CopyMultiInsertInfo *miinfo,
							 CopyMultiInsertBuffer *buffer)
{
	ResultRelInfo *resultRelInfo = buffer->resultRelInfo;
	int			i;

	/* Ensure buffer was flushed */
	Assert(buffer->nused == 0);

	/* Remove back-link to ourself */
	resultRelInfo->ri_CopyMultiInsertBuffer = NULL;

	if (resultRelInfo->ri_FdwRoutine == NULL)
	{
		Assert(buffer->bistate != NULL);
		FreeBulkInsertState(buffer->bistate);
	}
	else
		Assert(buffer->bistate == NULL);

	/* Since we only create slots on demand, just drop the non-null ones. */
	for (i = 0; i < MAX_BUFFERED_TUPLES && buffer->slots[i] != NULL; i++)
		ExecDropSingleTupleTableSlot(buffer->slots[i]);

	if (resultRelInfo->ri_FdwRoutine == NULL)
		table_finish_bulk_insert(resultRelInfo->ri_RelationDesc,
								 miinfo->ti_options);

	pfree(buffer);
}

/*
 * Write out all stored tuples in all buffers out to the tables.
 *
 * Once flushed we also trim the tracked buffers list down to size by removing
 * the buffers created earliest first.
 *
 * Callers should pass 'curr_rri' as the ResultRelInfo that's currently being
 * used.  When cleaning up old buffers we'll never remove the one for
 * 'curr_rri'.
 */
static inline void
CopyMultiInsertInfoFlush(CopyMultiInsertInfo *miinfo, ResultRelInfo *curr_rri,
						 int64 *processed)
{
	ListCell   *lc;

	foreach(lc, miinfo->multiInsertBuffers)
	{
		CopyMultiInsertBuffer *buffer = (CopyMultiInsertBuffer *) lfirst(lc);

		CopyMultiInsertBufferFlush(miinfo, buffer, processed);
	}

	miinfo->bufferedTuples = 0;
	miinfo->bufferedBytes = 0;

	/*
	 * Trim the list of tracked buffers down if it exceeds the limit.  Here we
	 * remove buffers starting with the ones we created first.  It seems less
	 * likely that these older ones will be needed than the ones that were
	 * just created.
	 */
	while (list_length(miinfo->multiInsertBuffers) > MAX_PARTITION_BUFFERS)
	{
		CopyMultiInsertBuffer *buffer;

		buffer = (CopyMultiInsertBuffer *) linitial(miinfo->multiInsertBuffers);

		/*
		 * We never want to remove the buffer that's currently being used, so
		 * if we happen to find that then move it to the end of the list.
		 */
		if (buffer->resultRelInfo == curr_rri)
		{
			/*
			 * The code below would misbehave if we were trying to reduce the
			 * list to less than two items.
			 */
			StaticAssertDecl(MAX_PARTITION_BUFFERS >= 2,
							 "MAX_PARTITION_BUFFERS must be >= 2");

			miinfo->multiInsertBuffers = list_delete_first(miinfo->multiInsertBuffers);
			miinfo->multiInsertBuffers = lappend(miinfo->multiInsertBuffers, buffer);
			buffer = (CopyMultiInsertBuffer *) linitial(miinfo->multiInsertBuffers);
		}

		CopyMultiInsertBufferCleanup(miinfo, buffer);
		miinfo->multiInsertBuffers = list_delete_first(miinfo->multiInsertBuffers);
	}
}

/*
 * Cleanup allocated buffers and free memory
 */
static inline void
CopyMultiInsertInfoCleanup(CopyMultiInsertInfo *miinfo)
{
	ListCell   *lc;

	foreach(lc, miinfo->multiInsertBuffers)
		CopyMultiInsertBufferCleanup(miinfo, lfirst(lc));

	list_free(miinfo->multiInsertBuffers);
}

/*
 * Get the next TupleTableSlot that the next tuple should be stored in.
 *
 * Callers must ensure that the buffer is not full.
 *
 * Note: 'miinfo' is unused but has been included for consistency with the
 * other functions in this area.
 */
static inline TupleTableSlot *
CopyMultiInsertInfoNextFreeSlot(CopyMultiInsertInfo *miinfo,
								ResultRelInfo *rri)
{
	CopyMultiInsertBuffer *buffer = rri->ri_CopyMultiInsertBuffer;
	int			nused;

	Assert(buffer != NULL);
	Assert(buffer->nused < MAX_BUFFERED_TUPLES);

	nused = buffer->nused;

	if (buffer->slots[nused] == NULL)
		buffer->slots[nused] = table_slot_create(rri->ri_RelationDesc, NULL);
	return buffer->slots[nused];
}

/*
 * Record the previously reserved TupleTableSlot that was reserved by
 * CopyMultiInsertInfoNextFreeSlot as being consumed.
 */
static inline void
CopyMultiInsertInfoStore(CopyMultiInsertInfo *miinfo, ResultRelInfo *rri,
						 TupleTableSlot *slot, int tuplen, uint64 lineno)
{
	CopyMultiInsertBuffer *buffer = rri->ri_CopyMultiInsertBuffer;

	Assert(buffer != NULL);
	Assert(slot == buffer->slots[buffer->nused]);

	/* Store the line number so we can properly report any errors later */
	buffer->linenos[buffer->nused] = lineno;

	/* Record this slot as being used */
	buffer->nused++;

	/* Update how many tuples are stored and their size */
	miinfo->bufferedTuples++;
	miinfo->bufferedBytes += tuplen;
}

/*
 * Copy FROM file to relation.
 */
uint64
CopyFrom(CopyFromState cstate)
{
	ResultRelInfo *resultRelInfo;
	ResultRelInfo *target_resultRelInfo;
	ResultRelInfo *prevResultRelInfo = NULL;
	EState	   *estate = CreateExecutorState(); /* for ExecConstraints() */
	ModifyTableState *mtstate;
	ExprContext *econtext;
	TupleTableSlot *singleslot = NULL;
	MemoryContext oldcontext = CurrentMemoryContext;

	PartitionTupleRouting *proute = NULL;
	ErrorContextCallback errcallback;
	CommandId	mycid = GetCurrentCommandId(true);
	int			ti_options = 0; /* start with default options for insert */
	BulkInsertState bistate = NULL;
	CopyInsertMethod insertMethod;
	CopyMultiInsertInfo multiInsertInfo = {0};	/* pacify compiler */
	int64		processed = 0;
	int64		excluded = 0;
	bool		has_before_insert_row_trig;
	bool		has_instead_insert_row_trig;
	bool		leafpart_use_multi_insert = false;

	Assert(cstate->rel);
	Assert(list_length(cstate->range_table) == 1);

	if (cstate->opts.on_error != COPY_ON_ERROR_STOP)
		Assert(cstate->escontext);

	/*
	 * The target must be a plain, foreign, or partitioned relation, or have
	 * an INSTEAD OF INSERT row trigger.  (Currently, such triggers are only
	 * allowed on views, so we only hint about them in the view case.)
	 */
	if (cstate->rel->rd_rel->relkind != RELKIND_RELATION &&
		cstate->rel->rd_rel->relkind != RELKIND_FOREIGN_TABLE &&
		cstate->rel->rd_rel->relkind != RELKIND_PARTITIONED_TABLE &&
		!(cstate->rel->trigdesc &&
		  cstate->rel->trigdesc->trig_insert_instead_row))
	{
		if (cstate->rel->rd_rel->relkind == RELKIND_VIEW)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot copy to view \"%s\"",
							RelationGetRelationName(cstate->rel)),
					 errhint("To enable copying to a view, provide an INSTEAD OF INSERT trigger.")));
		else if (cstate->rel->rd_rel->relkind == RELKIND_MATVIEW)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot copy to materialized view \"%s\"",
							RelationGetRelationName(cstate->rel))));
		else if (cstate->rel->rd_rel->relkind == RELKIND_SEQUENCE)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot copy to sequence \"%s\"",
							RelationGetRelationName(cstate->rel))));
		else
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot copy to non-table relation \"%s\"",
							RelationGetRelationName(cstate->rel))));
	}

	/*
	 * If the target file is new-in-transaction, we assume that checking FSM
	 * for free space is a waste of time.  This could possibly be wrong, but
	 * it's unlikely.
	 */
	if (RELKIND_HAS_STORAGE(cstate->rel->rd_rel->relkind) &&
		(cstate->rel->rd_createSubid != InvalidSubTransactionId ||
		 cstate->rel->rd_firstRelfilelocatorSubid != InvalidSubTransactionId))
		ti_options |= TABLE_INSERT_SKIP_FSM;

	/*
	 * Optimize if new relation storage was created in this subxact or one of
	 * its committed children and we won't see those rows later as part of an
	 * earlier scan or command. The subxact test ensures that if this subxact
	 * aborts then the frozen rows won't be visible after xact cleanup.  Note
	 * that the stronger test of exactly which subtransaction created it is
	 * crucial for correctness of this optimization. The test for an earlier
	 * scan or command tolerates false negatives. FREEZE causes other sessions
	 * to see rows they would not see under MVCC, and a false negative merely
	 * spreads that anomaly to the current session.
	 */
	if (cstate->opts.freeze)
	{
		/*
		 * We currently disallow COPY FREEZE on partitioned tables.  The
		 * reason for this is that we've simply not yet opened the partitions
		 * to determine if the optimization can be applied to them.  We could
		 * go and open them all here, but doing so may be quite a costly
		 * overhead for small copies.  In any case, we may just end up routing
		 * tuples to a small number of partitions.  It seems better just to
		 * raise an ERROR for partitioned tables.
		 */
		if (cstate->rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		{
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot perform COPY FREEZE on a partitioned table")));
		}

		/* There's currently no support for COPY FREEZE on foreign tables. */
		if (cstate->rel->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot perform COPY FREEZE on a foreign table")));

		/*
		 * Tolerate one registration for the benefit of FirstXactSnapshot.
		 * Scan-bearing queries generally create at least two registrations,
		 * though relying on that is fragile, as is ignoring ActiveSnapshot.
		 * Clear CatalogSnapshot to avoid counting its registration.  We'll
		 * still detect ongoing catalog scans, each of which separately
		 * registers the snapshot it uses.
		 */
		InvalidateCatalogSnapshot();
		if (!ThereAreNoPriorRegisteredSnapshots() || !ThereAreNoReadyPortals())
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TRANSACTION_STATE),
					 errmsg("cannot perform COPY FREEZE because of prior transaction activity")));

		if (cstate->rel->rd_createSubid != GetCurrentSubTransactionId() &&
			cstate->rel->rd_newRelfilelocatorSubid != GetCurrentSubTransactionId())
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("cannot perform COPY FREEZE because the table was not created or truncated in the current subtransaction")));

		ti_options |= TABLE_INSERT_FROZEN;
	}

	/*
	 * We need a ResultRelInfo so we can use the regular executor's
	 * index-entry-making machinery.  (There used to be a huge amount of code
	 * here that basically duplicated execUtils.c ...)
	 */
	ExecInitRangeTable(estate, cstate->range_table, cstate->rteperminfos,
					   bms_make_singleton(1));
	resultRelInfo = target_resultRelInfo = makeNode(ResultRelInfo);
	ExecInitResultRelation(estate, resultRelInfo, 1);

	/* Verify the named relation is a valid target for INSERT */
	CheckValidResultRel(resultRelInfo, CMD_INSERT, NIL);

	ExecOpenIndices(resultRelInfo, false);

	/*
	 * Set up a ModifyTableState so we can let FDW(s) init themselves for
	 * foreign-table result relation(s).
	 */
	mtstate = makeNode(ModifyTableState);
	mtstate->ps.plan = NULL;
	mtstate->ps.state = estate;
	mtstate->operation = CMD_INSERT;
	mtstate->mt_nrels = 1;
	mtstate->resultRelInfo = resultRelInfo;
	mtstate->rootResultRelInfo = resultRelInfo;

	if (resultRelInfo->ri_FdwRoutine != NULL &&
		resultRelInfo->ri_FdwRoutine->BeginForeignInsert != NULL)
		resultRelInfo->ri_FdwRoutine->BeginForeignInsert(mtstate,
														 resultRelInfo);

	/*
	 * Also, if the named relation is a foreign table, determine if the FDW
	 * supports batch insert and determine the batch size (a FDW may support
	 * batching, but it may be disabled for the server/table).
	 *
	 * If the FDW does not support batching, we set the batch size to 1.
	 */
	if (resultRelInfo->ri_FdwRoutine != NULL &&
		resultRelInfo->ri_FdwRoutine->GetForeignModifyBatchSize &&
		resultRelInfo->ri_FdwRoutine->ExecForeignBatchInsert)
		resultRelInfo->ri_BatchSize =
			resultRelInfo->ri_FdwRoutine->GetForeignModifyBatchSize(resultRelInfo);
	else
		resultRelInfo->ri_BatchSize = 1;

	Assert(resultRelInfo->ri_BatchSize >= 1);

	/* Prepare to catch AFTER triggers. */
	AfterTriggerBeginQuery();

	/*
	 * If there are any triggers with transition tables on the named relation,
	 * we need to be prepared to capture transition tuples.
	 *
	 * Because partition tuple routing would like to know about whether
	 * transition capture is active, we also set it in mtstate, which is
	 * passed to ExecFindPartition() below.
	 */
	cstate->transition_capture = mtstate->mt_transition_capture =
		MakeTransitionCaptureState(cstate->rel->trigdesc,
								   RelationGetRelid(cstate->rel),
								   CMD_INSERT);

	/*
	 * If the named relation is a partitioned table, initialize state for
	 * CopyFrom tuple routing.
	 */
	if (cstate->rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		proute = ExecSetupPartitionTupleRouting(estate, cstate->rel);

	if (cstate->whereClause)
		cstate->qualexpr = ExecInitQual(castNode(List, cstate->whereClause),
										&mtstate->ps);

	/*
	 * It's generally more efficient to prepare a bunch of tuples for
	 * insertion, and insert them in one
	 * table_multi_insert()/ExecForeignBatchInsert() call, than call
	 * table_tuple_insert()/ExecForeignInsert() separately for every tuple.
	 * However, there are a number of reasons why we might not be able to do
	 * this.  These are explained below.
	 */
	if (resultRelInfo->ri_TrigDesc != NULL &&
		(resultRelInfo->ri_TrigDesc->trig_insert_before_row ||
		 resultRelInfo->ri_TrigDesc->trig_insert_instead_row))
	{
		/*
		 * Can't support multi-inserts when there are any BEFORE/INSTEAD OF
		 * triggers on the table. Such triggers might query the table we're
		 * inserting into and act differently if the tuples that have already
		 * been processed and prepared for insertion are not there.
		 */
		insertMethod = CIM_SINGLE;
	}
	else if (resultRelInfo->ri_FdwRoutine != NULL &&
			 resultRelInfo->ri_BatchSize == 1)
	{
		/*
		 * Can't support multi-inserts to a foreign table if the FDW does not
		 * support batching, or it's disabled for the server or foreign table.
		 */
		insertMethod = CIM_SINGLE;
	}
	else if (proute != NULL && resultRelInfo->ri_TrigDesc != NULL &&
			 resultRelInfo->ri_TrigDesc->trig_insert_new_table)
	{
		/*
		 * For partitioned tables we can't support multi-inserts when there
		 * are any statement level insert triggers. It might be possible to
		 * allow partitioned tables with such triggers in the future, but for
		 * now, CopyMultiInsertInfoFlush expects that any after row insert and
		 * statement level insert triggers are on the same relation.
		 */
		insertMethod = CIM_SINGLE;
	}
	else if (cstate->volatile_defexprs)
	{
		/*
		 * Can't support multi-inserts if there are any volatile default
		 * expressions in the table.  Similarly to the trigger case above,
		 * such expressions may query the table we're inserting into.
		 *
		 * Note: It does not matter if any partitions have any volatile
		 * default expressions as we use the defaults from the target of the
		 * COPY command.
		 */
		insertMethod = CIM_SINGLE;
	}
	else if (contain_volatile_functions(cstate->whereClause))
	{
		/*
		 * Can't support multi-inserts if there are any volatile function
		 * expressions in WHERE clause.  Similarly to the trigger case above,
		 * such expressions may query the table we're inserting into.
		 *
		 * Note: the whereClause was already preprocessed in DoCopy(), so it's
		 * okay to use contain_volatile_functions() directly.
		 */
		insertMethod = CIM_SINGLE;
	}
	else
	{
		/*
		 * For partitioned tables, we may still be able to perform bulk
		 * inserts.  However, the possibility of this depends on which types
		 * of triggers exist on the partition.  We must disable bulk inserts
		 * if the partition is a foreign table that can't use batching or it
		 * has any before row insert or insert instead triggers (same as we
		 * checked above for the parent table).  Since the partition's
		 * resultRelInfos are initialized only when we actually need to insert
		 * the first tuple into them, we must have the intermediate insert
		 * method of CIM_MULTI_CONDITIONAL to flag that we must later
		 * determine if we can use bulk-inserts for the partition being
		 * inserted into.
		 */
		if (proute)
			insertMethod = CIM_MULTI_CONDITIONAL;
		else
			insertMethod = CIM_MULTI;

		CopyMultiInsertInfoInit(&multiInsertInfo, resultRelInfo, cstate,
								estate, mycid, ti_options);
	}

	/*
	 * If not using batch mode (which allocates slots as needed) set up a
	 * tuple slot too. When inserting into a partitioned table, we also need
	 * one, even if we might batch insert, to read the tuple in the root
	 * partition's form.
	 */
	if (insertMethod == CIM_SINGLE || insertMethod == CIM_MULTI_CONDITIONAL)
	{
		singleslot = table_slot_create(resultRelInfo->ri_RelationDesc,
									   &estate->es_tupleTable);
		bistate = GetBulkInsertState();
	}

	has_before_insert_row_trig = (resultRelInfo->ri_TrigDesc &&
								  resultRelInfo->ri_TrigDesc->trig_insert_before_row);

	has_instead_insert_row_trig = (resultRelInfo->ri_TrigDesc &&
								   resultRelInfo->ri_TrigDesc->trig_insert_instead_row);

	/*
	 * Check BEFORE STATEMENT insertion triggers. It's debatable whether we
	 * should do this for COPY, since it's not really an "INSERT" statement as
	 * such. However, executing these triggers maintains consistency with the
	 * EACH ROW triggers that we already fire on COPY.
	 */
	ExecBSInsertTriggers(estate, resultRelInfo);

	econtext = GetPerTupleExprContext(estate);

	/* Set up callback to identify error line number */
	errcallback.callback = CopyFromErrorCallback;
	errcallback.arg = cstate;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	for (;;)
	{
		TupleTableSlot *myslot;
		bool		skip_tuple;

		CHECK_FOR_INTERRUPTS();

		/*
		 * Reset the per-tuple exprcontext. We do this after every tuple, to
		 * clean-up after expression evaluations etc.
		 */
		ResetPerTupleExprContext(estate);

		/* select slot to (initially) load row into */
		if (insertMethod == CIM_SINGLE || proute)
		{
			myslot = singleslot;
			Assert(myslot != NULL);
		}
		else
		{
			Assert(resultRelInfo == target_resultRelInfo);
			Assert(insertMethod == CIM_MULTI);

			myslot = CopyMultiInsertInfoNextFreeSlot(&multiInsertInfo,
													 resultRelInfo);
		}

		/*
		 * Switch to per-tuple context before calling NextCopyFrom, which does
		 * evaluate default expressions etc. and requires per-tuple context.
		 */
		MemoryContextSwitchTo(GetPerTupleMemoryContext(estate));

		ExecClearTuple(myslot);

		/* Directly store the values/nulls array in the slot */
		if (!NextCopyFrom(cstate, econtext, myslot->tts_values, myslot->tts_isnull))
			break;

		if (cstate->opts.on_error == COPY_ON_ERROR_IGNORE &&
			cstate->escontext->error_occurred)
		{
			/*
			 * Soft error occurred, skip this tuple and just make
			 * ErrorSaveContext ready for the next NextCopyFrom. Since we
			 * don't set details_wanted and error_data is not to be filled,
			 * just resetting error_occurred is enough.
			 */
			cstate->escontext->error_occurred = false;

			/* Report that this tuple was skipped by the ON_ERROR clause */
			pgstat_progress_update_param(PROGRESS_COPY_TUPLES_SKIPPED,
										 cstate->num_errors);

			if (cstate->opts.reject_limit > 0 &&
				cstate->num_errors > cstate->opts.reject_limit)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("skipped more than REJECT_LIMIT (%lld) rows due to data type incompatibility",
								(long long) cstate->opts.reject_limit)));

			/* Repeat NextCopyFrom() until no soft error occurs */
			continue;
		}

		ExecStoreVirtualTuple(myslot);

		/*
		 * Constraints and where clause might reference the tableoid column,
		 * so (re-)initialize tts_tableOid before evaluating them.
		 */
		myslot->tts_tableOid = RelationGetRelid(target_resultRelInfo->ri_RelationDesc);

		/* Triggers and stuff need to be invoked in query context. */
		MemoryContextSwitchTo(oldcontext);

		if (cstate->whereClause)
		{
			econtext->ecxt_scantuple = myslot;
			/* Skip items that don't match COPY's WHERE clause */
			if (!ExecQual(cstate->qualexpr, econtext))
			{
				/*
				 * Report that this tuple was filtered out by the WHERE
				 * clause.
				 */
				pgstat_progress_update_param(PROGRESS_COPY_TUPLES_EXCLUDED,
											 ++excluded);
				continue;
			}
		}

		/* Determine the partition to insert the tuple into */
		if (proute)
		{
			TupleConversionMap *map;

			/*
			 * Attempt to find a partition suitable for this tuple.
			 * ExecFindPartition() will raise an error if none can be found or
			 * if the found partition is not suitable for INSERTs.
			 */
			resultRelInfo = ExecFindPartition(mtstate, target_resultRelInfo,
											  proute, myslot, estate);

			if (prevResultRelInfo != resultRelInfo)
			{
				/* Determine which triggers exist on this partition */
				has_before_insert_row_trig = (resultRelInfo->ri_TrigDesc &&
											  resultRelInfo->ri_TrigDesc->trig_insert_before_row);

				has_instead_insert_row_trig = (resultRelInfo->ri_TrigDesc &&
											   resultRelInfo->ri_TrigDesc->trig_insert_instead_row);

				/*
				 * Disable multi-inserts when the partition has BEFORE/INSTEAD
				 * OF triggers, or if the partition is a foreign table that
				 * can't use batching.
				 */
				leafpart_use_multi_insert = insertMethod == CIM_MULTI_CONDITIONAL &&
					!has_before_insert_row_trig &&
					!has_instead_insert_row_trig &&
					(resultRelInfo->ri_FdwRoutine == NULL ||
					 resultRelInfo->ri_BatchSize > 1);

				/* Set the multi-insert buffer to use for this partition. */
				if (leafpart_use_multi_insert)
				{
					if (resultRelInfo->ri_CopyMultiInsertBuffer == NULL)
						CopyMultiInsertInfoSetupBuffer(&multiInsertInfo,
													   resultRelInfo);
				}
				else if (insertMethod == CIM_MULTI_CONDITIONAL &&
						 !CopyMultiInsertInfoIsEmpty(&multiInsertInfo))
				{
					/*
					 * Flush pending inserts if this partition can't use
					 * batching, so rows are visible to triggers etc.
					 */
					CopyMultiInsertInfoFlush(&multiInsertInfo,
											 resultRelInfo,
											 &processed);
				}

				if (bistate != NULL)
					ReleaseBulkInsertStatePin(bistate);
				prevResultRelInfo = resultRelInfo;
			}

			/*
			 * If we're capturing transition tuples, we might need to convert
			 * from the partition rowtype to root rowtype. But if there are no
			 * BEFORE triggers on the partition that could change the tuple,
			 * we can just remember the original unconverted tuple to avoid a
			 * needless round trip conversion.
			 */
			if (cstate->transition_capture != NULL)
				cstate->transition_capture->tcs_original_insert_tuple =
					!has_before_insert_row_trig ? myslot : NULL;

			/*
			 * We might need to convert from the root rowtype to the partition
			 * rowtype.
			 */
			map = ExecGetRootToChildMap(resultRelInfo, estate);
			if (insertMethod == CIM_SINGLE || !leafpart_use_multi_insert)
			{
				/* non batch insert */
				if (map != NULL)
				{
					TupleTableSlot *new_slot;

					new_slot = resultRelInfo->ri_PartitionTupleSlot;
					myslot = execute_attr_map_slot(map->attrMap, myslot, new_slot);
				}
			}
			else
			{
				/*
				 * Prepare to queue up tuple for later batch insert into
				 * current partition.
				 */
				TupleTableSlot *batchslot;

				/* no other path available for partitioned table */
				Assert(insertMethod == CIM_MULTI_CONDITIONAL);

				batchslot = CopyMultiInsertInfoNextFreeSlot(&multiInsertInfo,
															resultRelInfo);

				if (map != NULL)
					myslot = execute_attr_map_slot(map->attrMap, myslot,
												   batchslot);
				else
				{
					/*
					 * This looks more expensive than it is (Believe me, I
					 * optimized it away. Twice.). The input is in virtual
					 * form, and we'll materialize the slot below - for most
					 * slot types the copy performs the work materialization
					 * would later require anyway.
					 */
					ExecCopySlot(batchslot, myslot);
					myslot = batchslot;
				}
			}

			/* ensure that triggers etc see the right relation  */
			myslot->tts_tableOid = RelationGetRelid(resultRelInfo->ri_RelationDesc);
		}

		skip_tuple = false;

		/* BEFORE ROW INSERT Triggers */
		if (has_before_insert_row_trig)
		{
			if (!ExecBRInsertTriggers(estate, resultRelInfo, myslot))
				skip_tuple = true;	/* "do nothing" */
		}

		if (!skip_tuple)
		{
			/*
			 * If there is an INSTEAD OF INSERT ROW trigger, let it handle the
			 * tuple.  Otherwise, proceed with inserting the tuple into the
			 * table or foreign table.
			 */
			if (has_instead_insert_row_trig)
			{
				ExecIRInsertTriggers(estate, resultRelInfo, myslot);
			}
			else
			{
				/* Compute stored generated columns */
				if (resultRelInfo->ri_RelationDesc->rd_att->constr &&
					resultRelInfo->ri_RelationDesc->rd_att->constr->has_generated_stored)
					ExecComputeStoredGenerated(resultRelInfo, estate, myslot,
											   CMD_INSERT);

				/*
				 * If the target is a plain table, check the constraints of
				 * the tuple.
				 */
				if (resultRelInfo->ri_FdwRoutine == NULL &&
					resultRelInfo->ri_RelationDesc->rd_att->constr)
					ExecConstraints(resultRelInfo, myslot, estate);

				/*
				 * Also check the tuple against the partition constraint, if
				 * there is one; except that if we got here via tuple-routing,
				 * we don't need to if there's no BR trigger defined on the
				 * partition.
				 */
				if (resultRelInfo->ri_RelationDesc->rd_rel->relispartition &&
					(proute == NULL || has_before_insert_row_trig))
					ExecPartitionCheck(resultRelInfo, myslot, estate, true);

				/* Store the slot in the multi-insert buffer, when enabled. */
				if (insertMethod == CIM_MULTI || leafpart_use_multi_insert)
				{
					/*
					 * The slot previously might point into the per-tuple
					 * context. For batching it needs to be longer lived.
					 */
					ExecMaterializeSlot(myslot);

					/* Add this tuple to the tuple buffer */
					CopyMultiInsertInfoStore(&multiInsertInfo,
											 resultRelInfo, myslot,
											 cstate->line_buf.len,
											 cstate->cur_lineno);

					/*
					 * If enough inserts have queued up, then flush all
					 * buffers out to their tables.
					 */
					if (CopyMultiInsertInfoIsFull(&multiInsertInfo))
						CopyMultiInsertInfoFlush(&multiInsertInfo,
												 resultRelInfo,
												 &processed);

					/*
					 * We delay updating the row counter and progress of the
					 * COPY command until after writing the tuples stored in
					 * the buffer out to the table, as in single insert mode.
					 * See CopyMultiInsertBufferFlush().
					 */
					continue;	/* next tuple please */
				}
				else
				{
					List	   *recheckIndexes = NIL;

					/* OK, store the tuple */
					if (resultRelInfo->ri_FdwRoutine != NULL)
					{
						myslot = resultRelInfo->ri_FdwRoutine->ExecForeignInsert(estate,
																				 resultRelInfo,
																				 myslot,
																				 NULL);

						if (myslot == NULL) /* "do nothing" */
							continue;	/* next tuple please */

						/*
						 * AFTER ROW Triggers might reference the tableoid
						 * column, so (re-)initialize tts_tableOid before
						 * evaluating them.
						 */
						myslot->tts_tableOid = RelationGetRelid(resultRelInfo->ri_RelationDesc);
					}
					else
					{
						/* OK, store the tuple and create index entries for it */
						table_tuple_insert(resultRelInfo->ri_RelationDesc,
										   myslot, mycid, ti_options, bistate);

						if (resultRelInfo->ri_NumIndices > 0)
							recheckIndexes = ExecInsertIndexTuples(resultRelInfo,
																   myslot,
																   estate,
																   false,
																   false,
																   NULL,
																   NIL,
																   false);
					}

					/* AFTER ROW INSERT Triggers */
					ExecARInsertTriggers(estate, resultRelInfo, myslot,
										 recheckIndexes, cstate->transition_capture);

					list_free(recheckIndexes);
				}
			}

			/*
			 * We count only tuples not suppressed by a BEFORE INSERT trigger
			 * or FDW; this is the same definition used by nodeModifyTable.c
			 * for counting tuples inserted by an INSERT command.  Update
			 * progress of the COPY command as well.
			 */
			pgstat_progress_update_param(PROGRESS_COPY_TUPLES_PROCESSED,
										 ++processed);
		}
	}

	/* Flush any remaining buffered tuples */
	if (insertMethod != CIM_SINGLE)
	{
		if (!CopyMultiInsertInfoIsEmpty(&multiInsertInfo))
			CopyMultiInsertInfoFlush(&multiInsertInfo, NULL, &processed);
	}

	/* Done, clean up */
	error_context_stack = errcallback.previous;

	if (cstate->opts.on_error != COPY_ON_ERROR_STOP &&
		cstate->num_errors > 0 &&
		cstate->opts.log_verbosity >= COPY_LOG_VERBOSITY_DEFAULT)
		ereport(NOTICE,
				errmsg_plural("%llu row was skipped due to data type incompatibility",
							  "%llu rows were skipped due to data type incompatibility",
							  (unsigned long long) cstate->num_errors,
							  (unsigned long long) cstate->num_errors));

	if (bistate != NULL)
		FreeBulkInsertState(bistate);

	MemoryContextSwitchTo(oldcontext);

	/* Execute AFTER STATEMENT insertion triggers */
	ExecASInsertTriggers(estate, target_resultRelInfo, cstate->transition_capture);

	/* Handle queued AFTER triggers */
	AfterTriggerEndQuery(estate);

	ExecResetTupleTable(estate->es_tupleTable, false);

	/* Allow the FDW to shut down */
	if (target_resultRelInfo->ri_FdwRoutine != NULL &&
		target_resultRelInfo->ri_FdwRoutine->EndForeignInsert != NULL)
		target_resultRelInfo->ri_FdwRoutine->EndForeignInsert(estate,
															  target_resultRelInfo);

	/* Tear down the multi-insert buffer data */
	if (insertMethod != CIM_SINGLE)
		CopyMultiInsertInfoCleanup(&multiInsertInfo);

	/* Close all the partitioned tables, leaf partitions, and their indices */
	if (proute)
		ExecCleanupTupleRouting(mtstate, proute);

	/* Close the result relations, including any trigger target relations */
	ExecCloseResultRelations(estate);
	ExecCloseRangeTableRelations(estate);

	FreeExecutorState(estate);

	return processed;
}

/*
 * Setup to read tuples from a file for COPY FROM.
 *
 * 'rel': Used as a template for the tuples
 * 'whereClause': WHERE clause from the COPY FROM command
 * 'filename': Name of server-local file to read, NULL for STDIN
 * 'is_program': true if 'filename' is program to execute
 * 'data_source_cb': callback that provides the input data
 * 'attnamelist': List of char *, columns to include. NIL selects all cols.
 * 'options': List of DefElem. See copy_opt_item in gram.y for selections.
 *
 * Returns a CopyFromState, to be passed to NextCopyFrom and related functions.
 */
CopyFromState
BeginCopyFrom(ParseState *pstate,
			  Relation rel,
			  Node *whereClause,
			  const char *filename,
			  bool is_program,
			  copy_data_source_cb data_source_cb,
			  List *attnamelist,
			  List *options)
{
	CopyFromState cstate;
	bool		pipe = (filename == NULL);
	TupleDesc	tupDesc;
	AttrNumber	num_phys_attrs,
				num_defaults;
	FmgrInfo   *in_functions;
	Oid		   *typioparams;
	int		   *defmap;
	ExprState **defexprs;
	MemoryContext oldcontext;
	bool		volatile_defexprs;
	const int	progress_cols[] = {
		PROGRESS_COPY_COMMAND,
		PROGRESS_COPY_TYPE,
		PROGRESS_COPY_BYTES_TOTAL
	};
	int64		progress_vals[] = {
		PROGRESS_COPY_COMMAND_FROM,
		0,
		0
	};

	/* Allocate workspace and zero all fields */
	cstate = (CopyFromStateData *) palloc0(sizeof(CopyFromStateData));

	/*
	 * We allocate everything used by a cstate in a new memory context. This
	 * avoids memory leaks during repeated use of COPY in a query.
	 */
	cstate->copycontext = AllocSetContextCreate(CurrentMemoryContext,
												"COPY",
												ALLOCSET_DEFAULT_SIZES);

	oldcontext = MemoryContextSwitchTo(cstate->copycontext);

	/* Extract options from the statement node tree */
	ProcessCopyOptions(pstate, &cstate->opts, true /* is_from */ , options);

	/* Set the format routine */
	cstate->routine = CopyFromGetRoutine(&cstate->opts);

	/* Process the target relation */
	cstate->rel = rel;

	tupDesc = RelationGetDescr(cstate->rel);

	/* process common options or initialization */

	/* Generate or convert list of attributes to process */
	cstate->attnumlist = CopyGetAttnums(tupDesc, cstate->rel, attnamelist);

	num_phys_attrs = tupDesc->natts;

	/* Convert FORCE_NOT_NULL name list to per-column flags, check validity */
	cstate->opts.force_notnull_flags = (bool *) palloc0(num_phys_attrs * sizeof(bool));
	if (cstate->opts.force_notnull_all)
		MemSet(cstate->opts.force_notnull_flags, true, num_phys_attrs * sizeof(bool));
	else if (cstate->opts.force_notnull)
	{
		List	   *attnums;
		ListCell   *cur;

		attnums = CopyGetAttnums(tupDesc, cstate->rel, cstate->opts.force_notnull);

		foreach(cur, attnums)
		{
			int			attnum = lfirst_int(cur);
			Form_pg_attribute attr = TupleDescAttr(tupDesc, attnum - 1);

			if (!list_member_int(cstate->attnumlist, attnum))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
				/*- translator: first %s is the name of a COPY option, e.g. FORCE_NOT_NULL */
						 errmsg("%s column \"%s\" not referenced by COPY",
								"FORCE_NOT_NULL", NameStr(attr->attname))));
			cstate->opts.force_notnull_flags[attnum - 1] = true;
		}
	}

	/* Set up soft error handler for ON_ERROR */
	if (cstate->opts.on_error != COPY_ON_ERROR_STOP)
	{
		cstate->escontext = makeNode(ErrorSaveContext);
		cstate->escontext->type = T_ErrorSaveContext;
		cstate->escontext->error_occurred = false;

		/*
		 * Currently we only support COPY_ON_ERROR_IGNORE. We'll add other
		 * options later
		 */
		if (cstate->opts.on_error == COPY_ON_ERROR_IGNORE)
			cstate->escontext->details_wanted = false;
	}
	else
		cstate->escontext = NULL;

	/* Convert FORCE_NULL name list to per-column flags, check validity */
	cstate->opts.force_null_flags = (bool *) palloc0(num_phys_attrs * sizeof(bool));
	if (cstate->opts.force_null_all)
		MemSet(cstate->opts.force_null_flags, true, num_phys_attrs * sizeof(bool));
	else if (cstate->opts.force_null)
	{
		List	   *attnums;
		ListCell   *cur;

		attnums = CopyGetAttnums(tupDesc, cstate->rel, cstate->opts.force_null);

		foreach(cur, attnums)
		{
			int			attnum = lfirst_int(cur);
			Form_pg_attribute attr = TupleDescAttr(tupDesc, attnum - 1);

			if (!list_member_int(cstate->attnumlist, attnum))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
				/*- translator: first %s is the name of a COPY option, e.g. FORCE_NOT_NULL */
						 errmsg("%s column \"%s\" not referenced by COPY",
								"FORCE_NULL", NameStr(attr->attname))));
			cstate->opts.force_null_flags[attnum - 1] = true;
		}
	}

	/* Convert convert_selectively name list to per-column flags */
	if (cstate->opts.convert_selectively)
	{
		List	   *attnums;
		ListCell   *cur;

		cstate->convert_select_flags = (bool *) palloc0(num_phys_attrs * sizeof(bool));

		attnums = CopyGetAttnums(tupDesc, cstate->rel, cstate->opts.convert_select);

		foreach(cur, attnums)
		{
			int			attnum = lfirst_int(cur);
			Form_pg_attribute attr = TupleDescAttr(tupDesc, attnum - 1);

			if (!list_member_int(cstate->attnumlist, attnum))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
						 errmsg_internal("selected column \"%s\" not referenced by COPY",
										 NameStr(attr->attname))));
			cstate->convert_select_flags[attnum - 1] = true;
		}
	}

	/* Use client encoding when ENCODING option is not specified. */
	if (cstate->opts.file_encoding < 0)
		cstate->file_encoding = pg_get_client_encoding();
	else
		cstate->file_encoding = cstate->opts.file_encoding;

	/*
	 * Look up encoding conversion function.
	 */
	if (cstate->file_encoding == GetDatabaseEncoding() ||
		cstate->file_encoding == PG_SQL_ASCII ||
		GetDatabaseEncoding() == PG_SQL_ASCII)
	{
		cstate->need_transcoding = false;
	}
	else
	{
		cstate->need_transcoding = true;
		cstate->conversion_proc = FindDefaultConversionProc(cstate->file_encoding,
															GetDatabaseEncoding());
		if (!OidIsValid(cstate->conversion_proc))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_FUNCTION),
					 errmsg("default conversion function for encoding \"%s\" to \"%s\" does not exist",
							pg_encoding_to_char(cstate->file_encoding),
							pg_encoding_to_char(GetDatabaseEncoding()))));
	}

	cstate->copy_src = COPY_FILE;	/* default */

	cstate->whereClause = whereClause;

	/* Initialize state variables */
	cstate->eol_type = EOL_UNKNOWN;
	cstate->cur_relname = RelationGetRelationName(cstate->rel);
	cstate->cur_lineno = 0;
	cstate->cur_attname = NULL;
	cstate->cur_attval = NULL;
	cstate->relname_only = false;

	/*
	 * Allocate buffers for the input pipeline.
	 *
	 * attribute_buf and raw_buf are used in both text and binary modes, but
	 * input_buf and line_buf only in text mode.
	 */
	cstate->raw_buf = palloc(RAW_BUF_SIZE + 1);
	cstate->raw_buf_index = cstate->raw_buf_len = 0;
	cstate->raw_reached_eof = false;

	initStringInfo(&cstate->attribute_buf);

	/* Assign range table and rteperminfos, we'll need them in CopyFrom. */
	if (pstate)
	{
		cstate->range_table = pstate->p_rtable;
		cstate->rteperminfos = pstate->p_rteperminfos;
	}

	num_defaults = 0;
	volatile_defexprs = false;

	/*
	 * Pick up the required catalog information for each attribute in the
	 * relation, including the input function, the element type (to pass to
	 * the input function), and info about defaults and constraints. (Which
	 * input function we use depends on text/binary format choice.)
	 */
	in_functions = (FmgrInfo *) palloc(num_phys_attrs * sizeof(FmgrInfo));
	typioparams = (Oid *) palloc(num_phys_attrs * sizeof(Oid));
	defmap = (int *) palloc(num_phys_attrs * sizeof(int));
	defexprs = (ExprState **) palloc(num_phys_attrs * sizeof(ExprState *));

	for (int attnum = 1; attnum <= num_phys_attrs; attnum++)
	{
		Form_pg_attribute att = TupleDescAttr(tupDesc, attnum - 1);

		/* We don't need info for dropped attributes */
		if (att->attisdropped)
			continue;

		/* Fetch the input function and typioparam info */
		cstate->routine->CopyFromInFunc(cstate, att->atttypid,
										&in_functions[attnum - 1],
										&typioparams[attnum - 1]);

		/* Get default info if available */
		defexprs[attnum - 1] = NULL;

		/*
		 * We only need the default values for columns that do not appear in
		 * the column list, unless the DEFAULT option was given. We never need
		 * default values for generated columns.
		 */
		if ((cstate->opts.default_print != NULL ||
			 !list_member_int(cstate->attnumlist, attnum)) &&
			!att->attgenerated)
		{
			Expr	   *defexpr = (Expr *) build_column_default(cstate->rel,
																attnum);

			if (defexpr != NULL)
			{
				/* Run the expression through planner */
				defexpr = expression_planner(defexpr);

				/* Initialize executable expression in copycontext */
				defexprs[attnum - 1] = ExecInitExpr(defexpr, NULL);

				/* if NOT copied from input */
				/* use default value if one exists */
				if (!list_member_int(cstate->attnumlist, attnum))
				{
					defmap[num_defaults] = attnum - 1;
					num_defaults++;
				}

				/*
				 * If a default expression looks at the table being loaded,
				 * then it could give the wrong answer when using
				 * multi-insert. Since database access can be dynamic this is
				 * hard to test for exactly, so we use the much wider test of
				 * whether the default expression is volatile. We allow for
				 * the special case of when the default expression is the
				 * nextval() of a sequence which in this specific case is
				 * known to be safe for use with the multi-insert
				 * optimization. Hence we use this special case function
				 * checker rather than the standard check for
				 * contain_volatile_functions().  Note also that we already
				 * ran the expression through expression_planner().
				 */
				if (!volatile_defexprs)
					volatile_defexprs = contain_volatile_functions_not_nextval((Node *) defexpr);
			}
		}
	}

	cstate->defaults = (bool *) palloc0(tupDesc->natts * sizeof(bool));

	/* initialize progress */
	pgstat_progress_start_command(PROGRESS_COMMAND_COPY,
								  cstate->rel ? RelationGetRelid(cstate->rel) : InvalidOid);
	cstate->bytes_processed = 0;

	/* We keep those variables in cstate. */
	cstate->in_functions = in_functions;
	cstate->typioparams = typioparams;
	cstate->defmap = defmap;
	cstate->defexprs = defexprs;
	cstate->volatile_defexprs = volatile_defexprs;
	cstate->num_defaults = num_defaults;
	cstate->is_program = is_program;

	if (data_source_cb)
	{
		progress_vals[1] = PROGRESS_COPY_TYPE_CALLBACK;
		cstate->copy_src = COPY_CALLBACK;
		cstate->data_source_cb = data_source_cb;
	}
	else if (pipe)
	{
		progress_vals[1] = PROGRESS_COPY_TYPE_PIPE;
		Assert(!is_program);	/* the grammar does not allow this */
		if (whereToSendOutput == DestRemote)
			ReceiveCopyBegin(cstate);
		else
			cstate->copy_file = stdin;
	}
	else
	{
		cstate->filename = pstrdup(filename);

		if (cstate->is_program)
		{
			progress_vals[1] = PROGRESS_COPY_TYPE_PROGRAM;
			cstate->copy_file = OpenPipeStream(cstate->filename, PG_BINARY_R);
			if (cstate->copy_file == NULL)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not execute command \"%s\": %m",
								cstate->filename)));
		}
		else
		{
			struct stat st;

			progress_vals[1] = PROGRESS_COPY_TYPE_FILE;
			cstate->copy_file = AllocateFile(cstate->filename, PG_BINARY_R);
			if (cstate->copy_file == NULL)
			{
				/* copy errno because ereport subfunctions might change it */
				int			save_errno = errno;

				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not open file \"%s\" for reading: %m",
								cstate->filename),
						 (save_errno == ENOENT || save_errno == EACCES) ?
						 errhint("COPY FROM instructs the PostgreSQL server process to read a file. "
								 "You may want a client-side facility such as psql's \\copy.") : 0));
			}

			if (fstat(fileno(cstate->copy_file), &st))
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not stat file \"%s\": %m",
								cstate->filename)));

			if (S_ISDIR(st.st_mode))
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("\"%s\" is a directory", cstate->filename)));

			progress_vals[2] = st.st_size;
		}
	}

	pgstat_progress_update_multi_param(3, progress_cols, progress_vals);

	cstate->routine->CopyFromStart(cstate, tupDesc);

	MemoryContextSwitchTo(oldcontext);

	return cstate;
}

/*
 * Clean up storage and release resources for COPY FROM.
 */
void
EndCopyFrom(CopyFromState cstate)
{
	/* Invoke the end callback */
	cstate->routine->CopyFromEnd(cstate);

	/* No COPY FROM related resources except memory. */
	if (cstate->is_program)
	{
		ClosePipeFromProgram(cstate);
	}
	else
	{
		if (cstate->filename != NULL && FreeFile(cstate->copy_file))
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not close file \"%s\": %m",
							cstate->filename)));
	}

	pgstat_progress_end_command();

	MemoryContextDelete(cstate->copycontext);
	pfree(cstate);
}

/*
 * Closes the pipe from an external program, checking the pclose() return code.
 */
static void
ClosePipeFromProgram(CopyFromState cstate)
{
	int			pclose_rc;

	Assert(cstate->is_program);

	pclose_rc = ClosePipeStream(cstate->copy_file);
	if (pclose_rc == -1)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close pipe to external command: %m")));
	else if (pclose_rc != 0)
	{
		/*
		 * If we ended a COPY FROM PROGRAM before reaching EOF, then it's
		 * expectable for the called program to fail with SIGPIPE, and we
		 * should not report that as an error.  Otherwise, SIGPIPE indicates a
		 * problem.
		 */
		if (!cstate->raw_reached_eof &&
			wait_result_is_signal(pclose_rc, SIGPIPE))
			return;

		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("program \"%s\" failed",
						cstate->filename),
				 errdetail_internal("%s", wait_result_to_str(pclose_rc))));
	}
}
