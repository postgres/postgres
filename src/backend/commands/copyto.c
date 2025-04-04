/*-------------------------------------------------------------------------
 *
 * copyto.c
 *		COPY <table> TO file/program/client
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/copyto.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>
#include <unistd.h>
#include <sys/stat.h>

#include "access/tableam.h"
#include "commands/copyapi.h"
#include "commands/progress.h"
#include "executor/execdesc.h"
#include "executor/executor.h"
#include "executor/tuptable.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/fd.h"
#include "tcop/tcopprot.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

/*
 * Represents the different dest cases we need to worry about at
 * the bottom level
 */
typedef enum CopyDest
{
	COPY_FILE,					/* to file (or a piped program) */
	COPY_FRONTEND,				/* to frontend */
	COPY_CALLBACK,				/* to callback function */
} CopyDest;

/*
 * This struct contains all the state variables used throughout a COPY TO
 * operation.
 *
 * Multi-byte encodings: all supported client-side encodings encode multi-byte
 * characters by having the first byte's high bit set. Subsequent bytes of the
 * character can have the high bit not set. When scanning data in such an
 * encoding to look for a match to a single-byte (ie ASCII) character, we must
 * use the full pg_encoding_mblen() machinery to skip over multibyte
 * characters, else we might find a false match to a trailing byte. In
 * supported server encodings, there is no possibility of a false match, and
 * it's faster to make useless comparisons to trailing bytes than it is to
 * invoke pg_encoding_mblen() to skip over them. encoding_embeds_ascii is true
 * when we have to do it the hard way.
 */
typedef struct CopyToStateData
{
	/* format-specific routines */
	const CopyToRoutine *routine;

	/* low-level state data */
	CopyDest	copy_dest;		/* type of copy source/destination */
	FILE	   *copy_file;		/* used if copy_dest == COPY_FILE */
	StringInfo	fe_msgbuf;		/* used for all dests during COPY TO */

	int			file_encoding;	/* file or remote side's character encoding */
	bool		need_transcoding;	/* file encoding diff from server? */
	bool		encoding_embeds_ascii;	/* ASCII can be non-first byte? */

	/* parameters from the COPY command */
	Relation	rel;			/* relation to copy to */
	QueryDesc  *queryDesc;		/* executable query to copy from */
	List	   *attnumlist;		/* integer list of attnums to copy */
	char	   *filename;		/* filename, or NULL for STDOUT */
	bool		is_program;		/* is 'filename' a program to popen? */
	copy_data_dest_cb data_dest_cb; /* function for writing data */

	CopyFormatOptions opts;
	Node	   *whereClause;	/* WHERE condition (or NULL) */

	/*
	 * Working state
	 */
	MemoryContext copycontext;	/* per-copy execution context */

	FmgrInfo   *out_functions;	/* lookup info for output functions */
	MemoryContext rowcontext;	/* per-row evaluation context */
	uint64		bytes_processed;	/* number of bytes processed so far */
} CopyToStateData;

/* DestReceiver for COPY (query) TO */
typedef struct
{
	DestReceiver pub;			/* publicly-known function pointers */
	CopyToState cstate;			/* CopyToStateData for the command */
	uint64		processed;		/* # of tuples processed */
} DR_copy;

/* NOTE: there's a copy of this in copyfromparse.c */
static const char BinarySignature[11] = "PGCOPY\n\377\r\n\0";


/* non-export function prototypes */
static void EndCopy(CopyToState cstate);
static void ClosePipeToProgram(CopyToState cstate);
static void CopyOneRowTo(CopyToState cstate, TupleTableSlot *slot);
static void CopyAttributeOutText(CopyToState cstate, const char *string);
static void CopyAttributeOutCSV(CopyToState cstate, const char *string,
								bool use_quote);

/* built-in format-specific routines */
static void CopyToTextLikeStart(CopyToState cstate, TupleDesc tupDesc);
static void CopyToTextLikeOutFunc(CopyToState cstate, Oid atttypid, FmgrInfo *finfo);
static void CopyToTextOneRow(CopyToState cstate, TupleTableSlot *slot);
static void CopyToCSVOneRow(CopyToState cstate, TupleTableSlot *slot);
static void CopyToTextLikeOneRow(CopyToState cstate, TupleTableSlot *slot,
								 bool is_csv);
static void CopyToTextLikeEnd(CopyToState cstate);
static void CopyToBinaryStart(CopyToState cstate, TupleDesc tupDesc);
static void CopyToBinaryOutFunc(CopyToState cstate, Oid atttypid, FmgrInfo *finfo);
static void CopyToBinaryOneRow(CopyToState cstate, TupleTableSlot *slot);
static void CopyToBinaryEnd(CopyToState cstate);

/* Low-level communications functions */
static void SendCopyBegin(CopyToState cstate);
static void SendCopyEnd(CopyToState cstate);
static void CopySendData(CopyToState cstate, const void *databuf, int datasize);
static void CopySendString(CopyToState cstate, const char *str);
static void CopySendChar(CopyToState cstate, char c);
static void CopySendEndOfRow(CopyToState cstate);
static void CopySendTextLikeEndOfRow(CopyToState cstate);
static void CopySendInt32(CopyToState cstate, int32 val);
static void CopySendInt16(CopyToState cstate, int16 val);

/*
 * COPY TO routines for built-in formats.
 *
 * CSV and text formats share the same TextLike routines except for the
 * one-row callback.
 */

/* text format */
static const CopyToRoutine CopyToRoutineText = {
	.CopyToStart = CopyToTextLikeStart,
	.CopyToOutFunc = CopyToTextLikeOutFunc,
	.CopyToOneRow = CopyToTextOneRow,
	.CopyToEnd = CopyToTextLikeEnd,
};

/* CSV format */
static const CopyToRoutine CopyToRoutineCSV = {
	.CopyToStart = CopyToTextLikeStart,
	.CopyToOutFunc = CopyToTextLikeOutFunc,
	.CopyToOneRow = CopyToCSVOneRow,
	.CopyToEnd = CopyToTextLikeEnd,
};

/* binary format */
static const CopyToRoutine CopyToRoutineBinary = {
	.CopyToStart = CopyToBinaryStart,
	.CopyToOutFunc = CopyToBinaryOutFunc,
	.CopyToOneRow = CopyToBinaryOneRow,
	.CopyToEnd = CopyToBinaryEnd,
};

/* Return a COPY TO routine for the given options */
static const CopyToRoutine *
CopyToGetRoutine(const CopyFormatOptions *opts)
{
	if (opts->csv_mode)
		return &CopyToRoutineCSV;
	else if (opts->binary)
		return &CopyToRoutineBinary;

	/* default is text */
	return &CopyToRoutineText;
}

/* Implementation of the start callback for text and CSV formats */
static void
CopyToTextLikeStart(CopyToState cstate, TupleDesc tupDesc)
{
	/*
	 * For non-binary copy, we need to convert null_print to file encoding,
	 * because it will be sent directly with CopySendString.
	 */
	if (cstate->need_transcoding)
		cstate->opts.null_print_client = pg_server_to_any(cstate->opts.null_print,
														  cstate->opts.null_print_len,
														  cstate->file_encoding);

	/* if a header has been requested send the line */
	if (cstate->opts.header_line)
	{
		ListCell   *cur;
		bool		hdr_delim = false;

		foreach(cur, cstate->attnumlist)
		{
			int			attnum = lfirst_int(cur);
			char	   *colname;

			if (hdr_delim)
				CopySendChar(cstate, cstate->opts.delim[0]);
			hdr_delim = true;

			colname = NameStr(TupleDescAttr(tupDesc, attnum - 1)->attname);

			if (cstate->opts.csv_mode)
				CopyAttributeOutCSV(cstate, colname, false);
			else
				CopyAttributeOutText(cstate, colname);
		}

		CopySendTextLikeEndOfRow(cstate);
	}
}

/*
 * Implementation of the outfunc callback for text and CSV formats. Assign
 * the output function data to the given *finfo.
 */
static void
CopyToTextLikeOutFunc(CopyToState cstate, Oid atttypid, FmgrInfo *finfo)
{
	Oid			func_oid;
	bool		is_varlena;

	/* Set output function for an attribute */
	getTypeOutputInfo(atttypid, &func_oid, &is_varlena);
	fmgr_info(func_oid, finfo);
}

/* Implementation of the per-row callback for text format */
static void
CopyToTextOneRow(CopyToState cstate, TupleTableSlot *slot)
{
	CopyToTextLikeOneRow(cstate, slot, false);
}

/* Implementation of the per-row callback for CSV format */
static void
CopyToCSVOneRow(CopyToState cstate, TupleTableSlot *slot)
{
	CopyToTextLikeOneRow(cstate, slot, true);
}

/*
 * Workhorse for CopyToTextOneRow() and CopyToCSVOneRow().
 *
 * We use pg_attribute_always_inline to reduce function call overhead
 * and to help compilers to optimize away the 'is_csv' condition.
 */
static pg_attribute_always_inline void
CopyToTextLikeOneRow(CopyToState cstate,
					 TupleTableSlot *slot,
					 bool is_csv)
{
	bool		need_delim = false;
	FmgrInfo   *out_functions = cstate->out_functions;

	foreach_int(attnum, cstate->attnumlist)
	{
		Datum		value = slot->tts_values[attnum - 1];
		bool		isnull = slot->tts_isnull[attnum - 1];

		if (need_delim)
			CopySendChar(cstate, cstate->opts.delim[0]);
		need_delim = true;

		if (isnull)
		{
			CopySendString(cstate, cstate->opts.null_print_client);
		}
		else
		{
			char	   *string;

			string = OutputFunctionCall(&out_functions[attnum - 1],
										value);

			if (is_csv)
				CopyAttributeOutCSV(cstate, string,
									cstate->opts.force_quote_flags[attnum - 1]);
			else
				CopyAttributeOutText(cstate, string);
		}
	}

	CopySendTextLikeEndOfRow(cstate);
}

/* Implementation of the end callback for text and CSV formats */
static void
CopyToTextLikeEnd(CopyToState cstate)
{
	/* Nothing to do here */
}

/*
 * Implementation of the start callback for binary format. Send a header
 * for a binary copy.
 */
static void
CopyToBinaryStart(CopyToState cstate, TupleDesc tupDesc)
{
	int32		tmp;

	/* Signature */
	CopySendData(cstate, BinarySignature, 11);
	/* Flags field */
	tmp = 0;
	CopySendInt32(cstate, tmp);
	/* No header extension */
	tmp = 0;
	CopySendInt32(cstate, tmp);
}

/*
 * Implementation of the outfunc callback for binary format. Assign
 * the binary output function to the given *finfo.
 */
static void
CopyToBinaryOutFunc(CopyToState cstate, Oid atttypid, FmgrInfo *finfo)
{
	Oid			func_oid;
	bool		is_varlena;

	/* Set output function for an attribute */
	getTypeBinaryOutputInfo(atttypid, &func_oid, &is_varlena);
	fmgr_info(func_oid, finfo);
}

/* Implementation of the per-row callback for binary format */
static void
CopyToBinaryOneRow(CopyToState cstate, TupleTableSlot *slot)
{
	FmgrInfo   *out_functions = cstate->out_functions;

	/* Binary per-tuple header */
	CopySendInt16(cstate, list_length(cstate->attnumlist));

	foreach_int(attnum, cstate->attnumlist)
	{
		Datum		value = slot->tts_values[attnum - 1];
		bool		isnull = slot->tts_isnull[attnum - 1];

		if (isnull)
		{
			CopySendInt32(cstate, -1);
		}
		else
		{
			bytea	   *outputbytes;

			outputbytes = SendFunctionCall(&out_functions[attnum - 1],
										   value);
			CopySendInt32(cstate, VARSIZE(outputbytes) - VARHDRSZ);
			CopySendData(cstate, VARDATA(outputbytes),
						 VARSIZE(outputbytes) - VARHDRSZ);
		}
	}

	CopySendEndOfRow(cstate);
}

/* Implementation of the end callback for binary format */
static void
CopyToBinaryEnd(CopyToState cstate)
{
	/* Generate trailer for a binary copy */
	CopySendInt16(cstate, -1);
	/* Need to flush out the trailer */
	CopySendEndOfRow(cstate);
}

/*
 * Send copy start/stop messages for frontend copies.  These have changed
 * in past protocol redesigns.
 */
static void
SendCopyBegin(CopyToState cstate)
{
	StringInfoData buf;
	int			natts = list_length(cstate->attnumlist);
	int16		format = (cstate->opts.binary ? 1 : 0);
	int			i;

	pq_beginmessage(&buf, PqMsg_CopyOutResponse);
	pq_sendbyte(&buf, format);	/* overall format */
	pq_sendint16(&buf, natts);
	for (i = 0; i < natts; i++)
		pq_sendint16(&buf, format); /* per-column formats */
	pq_endmessage(&buf);
	cstate->copy_dest = COPY_FRONTEND;
}

static void
SendCopyEnd(CopyToState cstate)
{
	/* Shouldn't have any unsent data */
	Assert(cstate->fe_msgbuf->len == 0);
	/* Send Copy Done message */
	pq_putemptymessage(PqMsg_CopyDone);
}

/*----------
 * CopySendData sends output data to the destination (file or frontend)
 * CopySendString does the same for null-terminated strings
 * CopySendChar does the same for single characters
 * CopySendEndOfRow does the appropriate thing at end of each data row
 *	(data is not actually flushed except by CopySendEndOfRow)
 *
 * NB: no data conversion is applied by these functions
 *----------
 */
static void
CopySendData(CopyToState cstate, const void *databuf, int datasize)
{
	appendBinaryStringInfo(cstate->fe_msgbuf, databuf, datasize);
}

static void
CopySendString(CopyToState cstate, const char *str)
{
	appendBinaryStringInfo(cstate->fe_msgbuf, str, strlen(str));
}

static void
CopySendChar(CopyToState cstate, char c)
{
	appendStringInfoCharMacro(cstate->fe_msgbuf, c);
}

static void
CopySendEndOfRow(CopyToState cstate)
{
	StringInfo	fe_msgbuf = cstate->fe_msgbuf;

	switch (cstate->copy_dest)
	{
		case COPY_FILE:
			if (fwrite(fe_msgbuf->data, fe_msgbuf->len, 1,
					   cstate->copy_file) != 1 ||
				ferror(cstate->copy_file))
			{
				if (cstate->is_program)
				{
					if (errno == EPIPE)
					{
						/*
						 * The pipe will be closed automatically on error at
						 * the end of transaction, but we might get a better
						 * error message from the subprocess' exit code than
						 * just "Broken Pipe"
						 */
						ClosePipeToProgram(cstate);

						/*
						 * If ClosePipeToProgram() didn't throw an error, the
						 * program terminated normally, but closed the pipe
						 * first. Restore errno, and throw an error.
						 */
						errno = EPIPE;
					}
					ereport(ERROR,
							(errcode_for_file_access(),
							 errmsg("could not write to COPY program: %m")));
				}
				else
					ereport(ERROR,
							(errcode_for_file_access(),
							 errmsg("could not write to COPY file: %m")));
			}
			break;
		case COPY_FRONTEND:
			/* Dump the accumulated row as one CopyData message */
			(void) pq_putmessage(PqMsg_CopyData, fe_msgbuf->data, fe_msgbuf->len);
			break;
		case COPY_CALLBACK:
			cstate->data_dest_cb(fe_msgbuf->data, fe_msgbuf->len);
			break;
	}

	/* Update the progress */
	cstate->bytes_processed += fe_msgbuf->len;
	pgstat_progress_update_param(PROGRESS_COPY_BYTES_PROCESSED, cstate->bytes_processed);

	resetStringInfo(fe_msgbuf);
}

/*
 * Wrapper function of CopySendEndOfRow for text and CSV formats. Sends the
 * line termination and do common appropriate things for the end of row.
 */
static inline void
CopySendTextLikeEndOfRow(CopyToState cstate)
{
	switch (cstate->copy_dest)
	{
		case COPY_FILE:
			/* Default line termination depends on platform */
#ifndef WIN32
			CopySendChar(cstate, '\n');
#else
			CopySendString(cstate, "\r\n");
#endif
			break;
		case COPY_FRONTEND:
			/* The FE/BE protocol uses \n as newline for all platforms */
			CopySendChar(cstate, '\n');
			break;
		default:
			break;
	}

	/* Now take the actions related to the end of a row */
	CopySendEndOfRow(cstate);
}

/*
 * These functions do apply some data conversion
 */

/*
 * CopySendInt32 sends an int32 in network byte order
 */
static inline void
CopySendInt32(CopyToState cstate, int32 val)
{
	uint32		buf;

	buf = pg_hton32((uint32) val);
	CopySendData(cstate, &buf, sizeof(buf));
}

/*
 * CopySendInt16 sends an int16 in network byte order
 */
static inline void
CopySendInt16(CopyToState cstate, int16 val)
{
	uint16		buf;

	buf = pg_hton16((uint16) val);
	CopySendData(cstate, &buf, sizeof(buf));
}

/*
 * Closes the pipe to an external program, checking the pclose() return code.
 */
static void
ClosePipeToProgram(CopyToState cstate)
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
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("program \"%s\" failed",
						cstate->filename),
				 errdetail_internal("%s", wait_result_to_str(pclose_rc))));
	}
}

/*
 * Release resources allocated in a cstate for COPY TO/FROM.
 */
static void
EndCopy(CopyToState cstate)
{
	if (cstate->is_program)
	{
		ClosePipeToProgram(cstate);
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
 * Setup CopyToState to read tuples from a table or a query for COPY TO.
 *
 * 'rel': Relation to be copied
 * 'raw_query': Query whose results are to be copied
 * 'queryRelId': OID of base relation to convert to a query (for RLS)
 * 'filename': Name of server-local file to write, NULL for STDOUT
 * 'is_program': true if 'filename' is program to execute
 * 'data_dest_cb': Callback that processes the output data
 * 'attnamelist': List of char *, columns to include. NIL selects all cols.
 * 'options': List of DefElem. See copy_opt_item in gram.y for selections.
 *
 * Returns a CopyToState, to be passed to DoCopyTo() and related functions.
 */
CopyToState
BeginCopyTo(ParseState *pstate,
			Relation rel,
			RawStmt *raw_query,
			Oid queryRelId,
			const char *filename,
			bool is_program,
			copy_data_dest_cb data_dest_cb,
			List *attnamelist,
			List *options)
{
	CopyToState cstate;
	bool		pipe = (filename == NULL && data_dest_cb == NULL);
	TupleDesc	tupDesc;
	int			num_phys_attrs;
	MemoryContext oldcontext;
	const int	progress_cols[] = {
		PROGRESS_COPY_COMMAND,
		PROGRESS_COPY_TYPE
	};
	int64		progress_vals[] = {
		PROGRESS_COPY_COMMAND_TO,
		0
	};

	if (rel != NULL && rel->rd_rel->relkind != RELKIND_RELATION)
	{
		if (rel->rd_rel->relkind == RELKIND_VIEW)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot copy from view \"%s\"",
							RelationGetRelationName(rel)),
					 errhint("Try the COPY (SELECT ...) TO variant.")));
		else if (rel->rd_rel->relkind == RELKIND_MATVIEW)
		{
			if (!RelationIsPopulated(rel))
				ereport(ERROR,
						errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("cannot copy from unpopulated materialized view \"%s\"",
							   RelationGetRelationName(rel)),
						errhint("Use the REFRESH MATERIALIZED VIEW command."));
		}
		else if (rel->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot copy from foreign table \"%s\"",
							RelationGetRelationName(rel)),
					 errhint("Try the COPY (SELECT ...) TO variant.")));
		else if (rel->rd_rel->relkind == RELKIND_SEQUENCE)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot copy from sequence \"%s\"",
							RelationGetRelationName(rel))));
		else if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot copy from partitioned table \"%s\"",
							RelationGetRelationName(rel)),
					 errhint("Try the COPY (SELECT ...) TO variant.")));
		else
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot copy from non-table relation \"%s\"",
							RelationGetRelationName(rel))));
	}


	/* Allocate workspace and zero all fields */
	cstate = (CopyToStateData *) palloc0(sizeof(CopyToStateData));

	/*
	 * We allocate everything used by a cstate in a new memory context. This
	 * avoids memory leaks during repeated use of COPY in a query.
	 */
	cstate->copycontext = AllocSetContextCreate(CurrentMemoryContext,
												"COPY",
												ALLOCSET_DEFAULT_SIZES);

	oldcontext = MemoryContextSwitchTo(cstate->copycontext);

	/* Extract options from the statement node tree */
	ProcessCopyOptions(pstate, &cstate->opts, false /* is_from */ , options);

	/* Set format routine */
	cstate->routine = CopyToGetRoutine(&cstate->opts);

	/* Process the source/target relation or query */
	if (rel)
	{
		Assert(!raw_query);

		cstate->rel = rel;

		tupDesc = RelationGetDescr(cstate->rel);
	}
	else
	{
		List	   *rewritten;
		Query	   *query;
		PlannedStmt *plan;
		DestReceiver *dest;

		cstate->rel = NULL;

		/*
		 * Run parse analysis and rewrite.  Note this also acquires sufficient
		 * locks on the source table(s).
		 */
		rewritten = pg_analyze_and_rewrite_fixedparams(raw_query,
													   pstate->p_sourcetext, NULL, 0,
													   NULL);

		/* check that we got back something we can work with */
		if (rewritten == NIL)
		{
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("DO INSTEAD NOTHING rules are not supported for COPY")));
		}
		else if (list_length(rewritten) > 1)
		{
			ListCell   *lc;

			/* examine queries to determine which error message to issue */
			foreach(lc, rewritten)
			{
				Query	   *q = lfirst_node(Query, lc);

				if (q->querySource == QSRC_QUAL_INSTEAD_RULE)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("conditional DO INSTEAD rules are not supported for COPY")));
				if (q->querySource == QSRC_NON_INSTEAD_RULE)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("DO ALSO rules are not supported for COPY")));
			}

			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("multi-statement DO INSTEAD rules are not supported for COPY")));
		}

		query = linitial_node(Query, rewritten);

		/* The grammar allows SELECT INTO, but we don't support that */
		if (query->utilityStmt != NULL &&
			IsA(query->utilityStmt, CreateTableAsStmt))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("COPY (SELECT INTO) is not supported")));

		/* The only other utility command we could see is NOTIFY */
		if (query->utilityStmt != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("COPY query must not be a utility command")));

		/*
		 * Similarly the grammar doesn't enforce the presence of a RETURNING
		 * clause, but this is required here.
		 */
		if (query->commandType != CMD_SELECT &&
			query->returningList == NIL)
		{
			Assert(query->commandType == CMD_INSERT ||
				   query->commandType == CMD_UPDATE ||
				   query->commandType == CMD_DELETE ||
				   query->commandType == CMD_MERGE);

			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("COPY query must have a RETURNING clause")));
		}

		/* plan the query */
		plan = pg_plan_query(query, pstate->p_sourcetext,
							 CURSOR_OPT_PARALLEL_OK, NULL);

		/*
		 * With row-level security and a user using "COPY relation TO", we
		 * have to convert the "COPY relation TO" to a query-based COPY (eg:
		 * "COPY (SELECT * FROM ONLY relation) TO"), to allow the rewriter to
		 * add in any RLS clauses.
		 *
		 * When this happens, we are passed in the relid of the originally
		 * found relation (which we have locked).  As the planner will look up
		 * the relation again, we double-check here to make sure it found the
		 * same one that we have locked.
		 */
		if (queryRelId != InvalidOid)
		{
			/*
			 * Note that with RLS involved there may be multiple relations,
			 * and while the one we need is almost certainly first, we don't
			 * make any guarantees of that in the planner, so check the whole
			 * list and make sure we find the original relation.
			 */
			if (!list_member_oid(plan->relationOids, queryRelId))
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("relation referenced by COPY statement has changed")));
		}

		/*
		 * Use a snapshot with an updated command ID to ensure this query sees
		 * results of any previously executed queries.
		 */
		PushCopiedSnapshot(GetActiveSnapshot());
		UpdateActiveSnapshotCommandId();

		/* Create dest receiver for COPY OUT */
		dest = CreateDestReceiver(DestCopyOut);
		((DR_copy *) dest)->cstate = cstate;

		/* Create a QueryDesc requesting no output */
		cstate->queryDesc = CreateQueryDesc(plan, NULL, pstate->p_sourcetext,
											GetActiveSnapshot(),
											InvalidSnapshot,
											dest, NULL, NULL, 0);

		/*
		 * Call ExecutorStart to prepare the plan for execution.
		 *
		 * ExecutorStart computes a result tupdesc for us
		 */
		if (!ExecutorStart(cstate->queryDesc, 0))
			elog(ERROR, "ExecutorStart() failed unexpectedly");

		tupDesc = cstate->queryDesc->tupDesc;
	}

	/* Generate or convert list of attributes to process */
	cstate->attnumlist = CopyGetAttnums(tupDesc, cstate->rel, attnamelist);

	num_phys_attrs = tupDesc->natts;

	/* Convert FORCE_QUOTE name list to per-column flags, check validity */
	cstate->opts.force_quote_flags = (bool *) palloc0(num_phys_attrs * sizeof(bool));
	if (cstate->opts.force_quote_all)
	{
		MemSet(cstate->opts.force_quote_flags, true, num_phys_attrs * sizeof(bool));
	}
	else if (cstate->opts.force_quote)
	{
		List	   *attnums;
		ListCell   *cur;

		attnums = CopyGetAttnums(tupDesc, cstate->rel, cstate->opts.force_quote);

		foreach(cur, attnums)
		{
			int			attnum = lfirst_int(cur);
			Form_pg_attribute attr = TupleDescAttr(tupDesc, attnum - 1);

			if (!list_member_int(cstate->attnumlist, attnum))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
				/*- translator: %s is the name of a COPY option, e.g. FORCE_NOT_NULL */
						 errmsg("%s column \"%s\" not referenced by COPY",
								"FORCE_QUOTE", NameStr(attr->attname))));
			cstate->opts.force_quote_flags[attnum - 1] = true;
		}
	}

	/* Use client encoding when ENCODING option is not specified. */
	if (cstate->opts.file_encoding < 0)
		cstate->file_encoding = pg_get_client_encoding();
	else
		cstate->file_encoding = cstate->opts.file_encoding;

	/*
	 * Set up encoding conversion info if the file and server encodings differ
	 * (see also pg_server_to_any).
	 */
	if (cstate->file_encoding == GetDatabaseEncoding() ||
		cstate->file_encoding == PG_SQL_ASCII)
		cstate->need_transcoding = false;
	else
		cstate->need_transcoding = true;

	/* See Multibyte encoding comment above */
	cstate->encoding_embeds_ascii = PG_ENCODING_IS_CLIENT_ONLY(cstate->file_encoding);

	cstate->copy_dest = COPY_FILE;	/* default */

	if (data_dest_cb)
	{
		progress_vals[1] = PROGRESS_COPY_TYPE_CALLBACK;
		cstate->copy_dest = COPY_CALLBACK;
		cstate->data_dest_cb = data_dest_cb;
	}
	else if (pipe)
	{
		progress_vals[1] = PROGRESS_COPY_TYPE_PIPE;

		Assert(!is_program);	/* the grammar does not allow this */
		if (whereToSendOutput != DestRemote)
			cstate->copy_file = stdout;
	}
	else
	{
		cstate->filename = pstrdup(filename);
		cstate->is_program = is_program;

		if (is_program)
		{
			progress_vals[1] = PROGRESS_COPY_TYPE_PROGRAM;
			cstate->copy_file = OpenPipeStream(cstate->filename, PG_BINARY_W);
			if (cstate->copy_file == NULL)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not execute command \"%s\": %m",
								cstate->filename)));
		}
		else
		{
			mode_t		oumask; /* Pre-existing umask value */
			struct stat st;

			progress_vals[1] = PROGRESS_COPY_TYPE_FILE;

			/*
			 * Prevent write to relative path ... too easy to shoot oneself in
			 * the foot by overwriting a database file ...
			 */
			if (!is_absolute_path(filename))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_NAME),
						 errmsg("relative path not allowed for COPY to file")));

			oumask = umask(S_IWGRP | S_IWOTH);
			PG_TRY();
			{
				cstate->copy_file = AllocateFile(cstate->filename, PG_BINARY_W);
			}
			PG_FINALLY();
			{
				umask(oumask);
			}
			PG_END_TRY();
			if (cstate->copy_file == NULL)
			{
				/* copy errno because ereport subfunctions might change it */
				int			save_errno = errno;

				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not open file \"%s\" for writing: %m",
								cstate->filename),
						 (save_errno == ENOENT || save_errno == EACCES) ?
						 errhint("COPY TO instructs the PostgreSQL server process to write a file. "
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
		}
	}

	/* initialize progress */
	pgstat_progress_start_command(PROGRESS_COMMAND_COPY,
								  cstate->rel ? RelationGetRelid(cstate->rel) : InvalidOid);
	pgstat_progress_update_multi_param(2, progress_cols, progress_vals);

	cstate->bytes_processed = 0;

	MemoryContextSwitchTo(oldcontext);

	return cstate;
}

/*
 * Clean up storage and release resources for COPY TO.
 */
void
EndCopyTo(CopyToState cstate)
{
	if (cstate->queryDesc != NULL)
	{
		/* Close down the query and free resources. */
		ExecutorFinish(cstate->queryDesc);
		ExecutorEnd(cstate->queryDesc);
		FreeQueryDesc(cstate->queryDesc);
		PopActiveSnapshot();
	}

	/* Clean up storage */
	EndCopy(cstate);
}

/*
 * Copy from relation or query TO file.
 *
 * Returns the number of rows processed.
 */
uint64
DoCopyTo(CopyToState cstate)
{
	bool		pipe = (cstate->filename == NULL && cstate->data_dest_cb == NULL);
	bool		fe_copy = (pipe && whereToSendOutput == DestRemote);
	TupleDesc	tupDesc;
	int			num_phys_attrs;
	ListCell   *cur;
	uint64		processed;

	if (fe_copy)
		SendCopyBegin(cstate);

	if (cstate->rel)
		tupDesc = RelationGetDescr(cstate->rel);
	else
		tupDesc = cstate->queryDesc->tupDesc;
	num_phys_attrs = tupDesc->natts;
	cstate->opts.null_print_client = cstate->opts.null_print;	/* default */

	/* We use fe_msgbuf as a per-row buffer regardless of copy_dest */
	cstate->fe_msgbuf = makeStringInfo();

	/* Get info about the columns we need to process. */
	cstate->out_functions = (FmgrInfo *) palloc(num_phys_attrs * sizeof(FmgrInfo));
	foreach(cur, cstate->attnumlist)
	{
		int			attnum = lfirst_int(cur);
		Form_pg_attribute attr = TupleDescAttr(tupDesc, attnum - 1);

		cstate->routine->CopyToOutFunc(cstate, attr->atttypid,
									   &cstate->out_functions[attnum - 1]);
	}

	/*
	 * Create a temporary memory context that we can reset once per row to
	 * recover palloc'd memory.  This avoids any problems with leaks inside
	 * datatype output routines, and should be faster than retail pfree's
	 * anyway.  (We don't need a whole econtext as CopyFrom does.)
	 */
	cstate->rowcontext = AllocSetContextCreate(CurrentMemoryContext,
											   "COPY TO",
											   ALLOCSET_DEFAULT_SIZES);

	cstate->routine->CopyToStart(cstate, tupDesc);

	if (cstate->rel)
	{
		TupleTableSlot *slot;
		TableScanDesc scandesc;

		scandesc = table_beginscan(cstate->rel, GetActiveSnapshot(), 0, NULL);
		slot = table_slot_create(cstate->rel, NULL);

		processed = 0;
		while (table_scan_getnextslot(scandesc, ForwardScanDirection, slot))
		{
			CHECK_FOR_INTERRUPTS();

			/* Deconstruct the tuple ... */
			slot_getallattrs(slot);

			/* Format and send the data */
			CopyOneRowTo(cstate, slot);

			/*
			 * Increment the number of processed tuples, and report the
			 * progress.
			 */
			pgstat_progress_update_param(PROGRESS_COPY_TUPLES_PROCESSED,
										 ++processed);
		}

		ExecDropSingleTupleTableSlot(slot);
		table_endscan(scandesc);
	}
	else
	{
		/* run the plan --- the dest receiver will send tuples */
		ExecutorRun(cstate->queryDesc, ForwardScanDirection, 0);
		processed = ((DR_copy *) cstate->queryDesc->dest)->processed;
	}

	cstate->routine->CopyToEnd(cstate);

	MemoryContextDelete(cstate->rowcontext);

	if (fe_copy)
		SendCopyEnd(cstate);

	return processed;
}

/*
 * Emit one row during DoCopyTo().
 */
static inline void
CopyOneRowTo(CopyToState cstate, TupleTableSlot *slot)
{
	MemoryContext oldcontext;

	MemoryContextReset(cstate->rowcontext);
	oldcontext = MemoryContextSwitchTo(cstate->rowcontext);

	/* Make sure the tuple is fully deconstructed */
	slot_getallattrs(slot);

	cstate->routine->CopyToOneRow(cstate, slot);

	MemoryContextSwitchTo(oldcontext);
}

/*
 * Send text representation of one attribute, with conversion and escaping
 */
#define DUMPSOFAR() \
	do { \
		if (ptr > start) \
			CopySendData(cstate, start, ptr - start); \
	} while (0)

static void
CopyAttributeOutText(CopyToState cstate, const char *string)
{
	const char *ptr;
	const char *start;
	char		c;
	char		delimc = cstate->opts.delim[0];

	if (cstate->need_transcoding)
		ptr = pg_server_to_any(string, strlen(string), cstate->file_encoding);
	else
		ptr = string;

	/*
	 * We have to grovel through the string searching for control characters
	 * and instances of the delimiter character.  In most cases, though, these
	 * are infrequent.  To avoid overhead from calling CopySendData once per
	 * character, we dump out all characters between escaped characters in a
	 * single call.  The loop invariant is that the data from "start" to "ptr"
	 * can be sent literally, but hasn't yet been.
	 *
	 * We can skip pg_encoding_mblen() overhead when encoding is safe, because
	 * in valid backend encodings, extra bytes of a multibyte character never
	 * look like ASCII.  This loop is sufficiently performance-critical that
	 * it's worth making two copies of it to get the IS_HIGHBIT_SET() test out
	 * of the normal safe-encoding path.
	 */
	if (cstate->encoding_embeds_ascii)
	{
		start = ptr;
		while ((c = *ptr) != '\0')
		{
			if ((unsigned char) c < (unsigned char) 0x20)
			{
				/*
				 * \r and \n must be escaped, the others are traditional. We
				 * prefer to dump these using the C-like notation, rather than
				 * a backslash and the literal character, because it makes the
				 * dump file a bit more proof against Microsoftish data
				 * mangling.
				 */
				switch (c)
				{
					case '\b':
						c = 'b';
						break;
					case '\f':
						c = 'f';
						break;
					case '\n':
						c = 'n';
						break;
					case '\r':
						c = 'r';
						break;
					case '\t':
						c = 't';
						break;
					case '\v':
						c = 'v';
						break;
					default:
						/* If it's the delimiter, must backslash it */
						if (c == delimc)
							break;
						/* All ASCII control chars are length 1 */
						ptr++;
						continue;	/* fall to end of loop */
				}
				/* if we get here, we need to convert the control char */
				DUMPSOFAR();
				CopySendChar(cstate, '\\');
				CopySendChar(cstate, c);
				start = ++ptr;	/* do not include char in next run */
			}
			else if (c == '\\' || c == delimc)
			{
				DUMPSOFAR();
				CopySendChar(cstate, '\\');
				start = ptr++;	/* we include char in next run */
			}
			else if (IS_HIGHBIT_SET(c))
				ptr += pg_encoding_mblen(cstate->file_encoding, ptr);
			else
				ptr++;
		}
	}
	else
	{
		start = ptr;
		while ((c = *ptr) != '\0')
		{
			if ((unsigned char) c < (unsigned char) 0x20)
			{
				/*
				 * \r and \n must be escaped, the others are traditional. We
				 * prefer to dump these using the C-like notation, rather than
				 * a backslash and the literal character, because it makes the
				 * dump file a bit more proof against Microsoftish data
				 * mangling.
				 */
				switch (c)
				{
					case '\b':
						c = 'b';
						break;
					case '\f':
						c = 'f';
						break;
					case '\n':
						c = 'n';
						break;
					case '\r':
						c = 'r';
						break;
					case '\t':
						c = 't';
						break;
					case '\v':
						c = 'v';
						break;
					default:
						/* If it's the delimiter, must backslash it */
						if (c == delimc)
							break;
						/* All ASCII control chars are length 1 */
						ptr++;
						continue;	/* fall to end of loop */
				}
				/* if we get here, we need to convert the control char */
				DUMPSOFAR();
				CopySendChar(cstate, '\\');
				CopySendChar(cstate, c);
				start = ++ptr;	/* do not include char in next run */
			}
			else if (c == '\\' || c == delimc)
			{
				DUMPSOFAR();
				CopySendChar(cstate, '\\');
				start = ptr++;	/* we include char in next run */
			}
			else
				ptr++;
		}
	}

	DUMPSOFAR();
}

/*
 * Send text representation of one attribute, with conversion and
 * CSV-style escaping
 */
static void
CopyAttributeOutCSV(CopyToState cstate, const char *string,
					bool use_quote)
{
	const char *ptr;
	const char *start;
	char		c;
	char		delimc = cstate->opts.delim[0];
	char		quotec = cstate->opts.quote[0];
	char		escapec = cstate->opts.escape[0];
	bool		single_attr = (list_length(cstate->attnumlist) == 1);

	/* force quoting if it matches null_print (before conversion!) */
	if (!use_quote && strcmp(string, cstate->opts.null_print) == 0)
		use_quote = true;

	if (cstate->need_transcoding)
		ptr = pg_server_to_any(string, strlen(string), cstate->file_encoding);
	else
		ptr = string;

	/*
	 * Make a preliminary pass to discover if it needs quoting
	 */
	if (!use_quote)
	{
		/*
		 * Quote '\.' if it appears alone on a line, so that it will not be
		 * interpreted as an end-of-data marker.  (PG 18 and up will not
		 * interpret '\.' in CSV that way, except in embedded-in-SQL data; but
		 * we want the data to be loadable by older versions too.  Also, this
		 * avoids breaking clients that are still using PQgetline().)
		 */
		if (single_attr && strcmp(ptr, "\\.") == 0)
			use_quote = true;
		else
		{
			const char *tptr = ptr;

			while ((c = *tptr) != '\0')
			{
				if (c == delimc || c == quotec || c == '\n' || c == '\r')
				{
					use_quote = true;
					break;
				}
				if (IS_HIGHBIT_SET(c) && cstate->encoding_embeds_ascii)
					tptr += pg_encoding_mblen(cstate->file_encoding, tptr);
				else
					tptr++;
			}
		}
	}

	if (use_quote)
	{
		CopySendChar(cstate, quotec);

		/*
		 * We adopt the same optimization strategy as in CopyAttributeOutText
		 */
		start = ptr;
		while ((c = *ptr) != '\0')
		{
			if (c == quotec || c == escapec)
			{
				DUMPSOFAR();
				CopySendChar(cstate, escapec);
				start = ptr;	/* we include char in next run */
			}
			if (IS_HIGHBIT_SET(c) && cstate->encoding_embeds_ascii)
				ptr += pg_encoding_mblen(cstate->file_encoding, ptr);
			else
				ptr++;
		}
		DUMPSOFAR();

		CopySendChar(cstate, quotec);
	}
	else
	{
		/* If it doesn't need quoting, we can just dump it as-is */
		CopySendString(cstate, ptr);
	}
}

/*
 * copy_dest_startup --- executor startup
 */
static void
copy_dest_startup(DestReceiver *self, int operation, TupleDesc typeinfo)
{
	/* no-op */
}

/*
 * copy_dest_receive --- receive one tuple
 */
static bool
copy_dest_receive(TupleTableSlot *slot, DestReceiver *self)
{
	DR_copy    *myState = (DR_copy *) self;
	CopyToState cstate = myState->cstate;

	/* Send the data */
	CopyOneRowTo(cstate, slot);

	/* Increment the number of processed tuples, and report the progress */
	pgstat_progress_update_param(PROGRESS_COPY_TUPLES_PROCESSED,
								 ++myState->processed);

	return true;
}

/*
 * copy_dest_shutdown --- executor end
 */
static void
copy_dest_shutdown(DestReceiver *self)
{
	/* no-op */
}

/*
 * copy_dest_destroy --- release DestReceiver object
 */
static void
copy_dest_destroy(DestReceiver *self)
{
	pfree(self);
}

/*
 * CreateCopyDestReceiver -- create a suitable DestReceiver object
 */
DestReceiver *
CreateCopyDestReceiver(void)
{
	DR_copy    *self = (DR_copy *) palloc(sizeof(DR_copy));

	self->pub.receiveSlot = copy_dest_receive;
	self->pub.rStartup = copy_dest_startup;
	self->pub.rShutdown = copy_dest_shutdown;
	self->pub.rDestroy = copy_dest_destroy;
	self->pub.mydest = DestCopyOut;

	self->cstate = NULL;		/* will be set later */
	self->processed = 0;

	return (DestReceiver *) self;
}
