/*-------------------------------------------------------------------------
 *
 * copyfrom_internal.h
 *	  Internal definitions for COPY FROM command.
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/commands/copyfrom_internal.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef COPYFROM_INTERNAL_H
#define COPYFROM_INTERNAL_H

#include "commands/copy.h"
#include "commands/trigger.h"
#include "nodes/miscnodes.h"

/*
 * Represents the different source cases we need to worry about at
 * the bottom level
 */
typedef enum CopySource
{
	COPY_FILE,					/* from file (or a piped program) */
	COPY_FRONTEND,				/* from frontend */
	COPY_CALLBACK,				/* from callback function */
} CopySource;

/*
 *	Represents the end-of-line terminator type of the input
 */
typedef enum EolType
{
	EOL_UNKNOWN,
	EOL_NL,
	EOL_CR,
	EOL_CRNL,
} EolType;

/*
 * Represents the insert method to be used during COPY FROM.
 */
typedef enum CopyInsertMethod
{
	CIM_SINGLE,					/* use table_tuple_insert or ExecForeignInsert */
	CIM_MULTI,					/* always use table_multi_insert or
								 * ExecForeignBatchInsert */
	CIM_MULTI_CONDITIONAL,		/* use table_multi_insert or
								 * ExecForeignBatchInsert only if valid */
} CopyInsertMethod;

/*
 * This struct contains all the state variables used throughout a COPY FROM
 * operation.
 */
typedef struct CopyFromStateData
{
	/* low-level state data */
	CopySource	copy_src;		/* type of copy source */
	FILE	   *copy_file;		/* used if copy_src == COPY_FILE */
	StringInfo	fe_msgbuf;		/* used if copy_src == COPY_FRONTEND */

	EolType		eol_type;		/* EOL type of input */
	int			file_encoding;	/* file or remote side's character encoding */
	bool		need_transcoding;	/* file encoding diff from server? */
	Oid			conversion_proc;	/* encoding conversion function */

	/* parameters from the COPY command */
	Relation	rel;			/* relation to copy from */
	List	   *attnumlist;		/* integer list of attnums to copy */
	char	   *filename;		/* filename, or NULL for STDIN */
	bool		is_program;		/* is 'filename' a program to popen? */
	copy_data_source_cb data_source_cb; /* function for reading data */

	CopyFormatOptions opts;
	bool	   *convert_select_flags;	/* per-column CSV/TEXT CS flags */
	Node	   *whereClause;	/* WHERE condition (or NULL) */

	/* these are just for error messages, see CopyFromErrorCallback */
	const char *cur_relname;	/* table name for error messages */
	uint64		cur_lineno;		/* line number for error messages */
	const char *cur_attname;	/* current att for error messages */
	const char *cur_attval;		/* current att value for error messages */
	bool		relname_only;	/* don't output line number, att, etc. */

	/*
	 * Working state
	 */
	MemoryContext copycontext;	/* per-copy execution context */

	AttrNumber	num_defaults;	/* count of att that are missing and have
								 * default value */
	FmgrInfo   *in_functions;	/* array of input functions for each attrs */
	Oid		   *typioparams;	/* array of element types for in_functions */
	ErrorSaveContext *escontext;	/* soft error trapper during in_functions
									 * execution */
	uint64		num_errors;		/* total number of rows which contained soft
								 * errors */
	int		   *defmap;			/* array of default att numbers related to
								 * missing att */
	ExprState **defexprs;		/* array of default att expressions for all
								 * att */
	bool	   *defaults;		/* if DEFAULT marker was found for
								 * corresponding att */
	bool		volatile_defexprs;	/* is any of defexprs volatile? */
	List	   *range_table;	/* single element list of RangeTblEntry */
	List	   *rteperminfos;	/* single element list of RTEPermissionInfo */
	ExprState  *qualexpr;

	TransitionCaptureState *transition_capture;

	/*
	 * These variables are used to reduce overhead in COPY FROM.
	 *
	 * attribute_buf holds the separated, de-escaped text for each field of
	 * the current line.  The CopyReadAttributes functions return arrays of
	 * pointers into this buffer.  We avoid palloc/pfree overhead by re-using
	 * the buffer on each cycle.
	 *
	 * In binary COPY FROM, attribute_buf holds the binary data for the
	 * current field, but the usage is otherwise similar.
	 */
	StringInfoData attribute_buf;

	/* field raw data pointers found by COPY FROM */

	int			max_fields;
	char	  **raw_fields;

	/*
	 * Similarly, line_buf holds the whole input line being processed. The
	 * input cycle is first to read the whole line into line_buf, and then
	 * extract the individual attribute fields into attribute_buf.  line_buf
	 * is preserved unmodified so that we can display it in error messages if
	 * appropriate.  (In binary mode, line_buf is not used.)
	 */
	StringInfoData line_buf;
	bool		line_buf_valid; /* contains the row being processed? */

	/*
	 * input_buf holds input data, already converted to database encoding.
	 *
	 * In text mode, CopyReadLine parses this data sufficiently to locate line
	 * boundaries, then transfers the data to line_buf. We guarantee that
	 * there is a \0 at input_buf[input_buf_len] at all times.  (In binary
	 * mode, input_buf is not used.)
	 *
	 * If encoding conversion is not required, input_buf is not a separate
	 * buffer but points directly to raw_buf.  In that case, input_buf_len
	 * tracks the number of bytes that have been verified as valid in the
	 * database encoding, and raw_buf_len is the total number of bytes stored
	 * in the buffer.
	 */
#define INPUT_BUF_SIZE 65536	/* we palloc INPUT_BUF_SIZE+1 bytes */
	char	   *input_buf;
	int			input_buf_index;	/* next byte to process */
	int			input_buf_len;	/* total # of bytes stored */
	bool		input_reached_eof;	/* true if we reached EOF */
	bool		input_reached_error;	/* true if a conversion error happened */
	/* Shorthand for number of unconsumed bytes available in input_buf */
#define INPUT_BUF_BYTES(cstate) ((cstate)->input_buf_len - (cstate)->input_buf_index)

	/*
	 * raw_buf holds raw input data read from the data source (file or client
	 * connection), not yet converted to the database encoding.  Like with
	 * 'input_buf', we guarantee that there is a \0 at raw_buf[raw_buf_len].
	 */
#define RAW_BUF_SIZE 65536		/* we palloc RAW_BUF_SIZE+1 bytes */
	char	   *raw_buf;
	int			raw_buf_index;	/* next byte to process */
	int			raw_buf_len;	/* total # of bytes stored */
	bool		raw_reached_eof;	/* true if we reached EOF */

	/* Shorthand for number of unconsumed bytes available in raw_buf */
#define RAW_BUF_BYTES(cstate) ((cstate)->raw_buf_len - (cstate)->raw_buf_index)

	uint64		bytes_processed;	/* number of bytes processed so far */
} CopyFromStateData;

extern void ReceiveCopyBegin(CopyFromState cstate);
extern void ReceiveCopyBinaryHeader(CopyFromState cstate);

#endif							/* COPYFROM_INTERNAL_H */
