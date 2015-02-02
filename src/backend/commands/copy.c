/*-------------------------------------------------------------------------
 *
 * copy.c
 *		Implements the COPY utility command
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/commands/copy.c,v 1.327 2010/04/28 16:10:41 heikki Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>
#include <unistd.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "access/heapam.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "commands/copy.h"
#include "commands/defrem.h"
#include "commands/trigger.h"
#include "executor/executor.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "optimizer/planner.h"
#include "parser/parse_relation.h"
#include "rewrite/rewriteHandler.h"
#include "storage/fd.h"
#include "tcop/tcopprot.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"


#define ISOCTAL(c) (((c) >= '0') && ((c) <= '7'))
#define OCTVALUE(c) ((c) - '0')

/*
 * Represents the different source/dest cases we need to worry about at
 * the bottom level
 */
typedef enum CopyDest
{
	COPY_FILE,					/* to/from file */
	COPY_OLD_FE,				/* to/from frontend (2.0 protocol) */
	COPY_NEW_FE					/* to/from frontend (3.0 protocol) */
} CopyDest;

/*
 *	Represents the end-of-line terminator type of the input
 */
typedef enum EolType
{
	EOL_UNKNOWN,
	EOL_NL,
	EOL_CR,
	EOL_CRNL
} EolType;

/*
 * This struct contains all the state variables used throughout a COPY
 * operation. For simplicity, we use the same struct for all variants of COPY,
 * even though some fields are used in only some cases.
 *
 * Multi-byte encodings: all supported client-side encodings encode multi-byte
 * characters by having the first byte's high bit set. Subsequent bytes of the
 * character can have the high bit not set. When scanning data in such an
 * encoding to look for a match to a single-byte (ie ASCII) character, we must
 * use the full pg_encoding_mblen() machinery to skip over multibyte
 * characters, else we might find a false match to a trailing byte. In
 * supported server encodings, there is no possibility of a false match, and
 * it's faster to make useless comparisons to trailing bytes than it is to
 * invoke pg_encoding_mblen() to skip over them. encoding_embeds_ascii is TRUE
 * when we have to do it the hard way.
 */
typedef struct CopyStateData
{
	/* low-level state data */
	CopyDest	copy_dest;		/* type of copy source/destination */
	FILE	   *copy_file;		/* used if copy_dest == COPY_FILE */
	StringInfo	fe_msgbuf;		/* used for all dests during COPY TO, only for
								 * dest == COPY_NEW_FE in COPY FROM */
	bool		fe_copy;		/* true for all FE copy dests */
	bool		fe_eof;			/* true if detected end of copy data */
	EolType		eol_type;		/* EOL type of input */
	int			client_encoding;	/* remote side's character encoding */
	bool		need_transcoding;		/* client encoding diff from server? */
	bool		encoding_embeds_ascii;	/* ASCII can be non-first byte? */
	uint64		processed;		/* # of tuples processed */

	/* parameters from the COPY command */
	Relation	rel;			/* relation to copy to or from */
	QueryDesc  *queryDesc;		/* executable query to copy from */
	List	   *attnumlist;		/* integer list of attnums to copy */
	char	   *filename;		/* filename, or NULL for STDIN/STDOUT */
	bool		binary;			/* binary format? */
	bool		oids;			/* include OIDs? */
	bool		csv_mode;		/* Comma Separated Value format? */
	bool		header_line;	/* CSV header line? */
	char	   *null_print;		/* NULL marker string (server encoding!) */
	int			null_print_len; /* length of same */
	char	   *null_print_client;		/* same converted to client encoding */
	char	   *delim;			/* column delimiter (must be 1 byte) */
	char	   *quote;			/* CSV quote char (must be 1 byte) */
	char	   *escape;			/* CSV escape char (must be 1 byte) */
	bool	   *force_quote_flags;		/* per-column CSV FQ flags */
	bool	   *force_notnull_flags;	/* per-column CSV FNN flags */

	/* these are just for error messages, see copy_in_error_callback */
	const char *cur_relname;	/* table name for error messages */
	int			cur_lineno;		/* line number for error messages */
	const char *cur_attname;	/* current att for error messages */
	const char *cur_attval;		/* current att value for error messages */

	/*
	 * Working state for COPY TO
	 */
	FmgrInfo   *out_functions;	/* lookup info for output functions */
	MemoryContext rowcontext;	/* per-row evaluation context */

	/*
	 * These variables are used to reduce overhead in textual COPY FROM.
	 *
	 * attribute_buf holds the separated, de-escaped text for each field of
	 * the current line.  The CopyReadAttributes functions return arrays of
	 * pointers into this buffer.  We avoid palloc/pfree overhead by re-using
	 * the buffer on each cycle.
	 */
	StringInfoData attribute_buf;

	/*
	 * Similarly, line_buf holds the whole input line being processed. The
	 * input cycle is first to read the whole line into line_buf, convert it
	 * to server encoding there, and then extract the individual attribute
	 * fields into attribute_buf.  line_buf is preserved unmodified so that we
	 * can display it in error messages if appropriate.
	 */
	StringInfoData line_buf;
	bool		line_buf_converted;		/* converted to server encoding? */

	/*
	 * Finally, raw_buf holds raw data read from the data source (file or
	 * client connection).  CopyReadLine parses this data sufficiently to
	 * locate line boundaries, then transfers the data to line_buf and
	 * converts it.  Note: we guarantee that there is a \0 at
	 * raw_buf[raw_buf_len].
	 */
#define RAW_BUF_SIZE 65536		/* we palloc RAW_BUF_SIZE+1 bytes */
	char	   *raw_buf;
	int			raw_buf_index;	/* next byte to process */
	int			raw_buf_len;	/* total # of bytes stored */
} CopyStateData;

typedef CopyStateData *CopyState;

/* DestReceiver for COPY (SELECT) TO */
typedef struct
{
	DestReceiver pub;			/* publicly-known function pointers */
	CopyState	cstate;			/* CopyStateData for the command */
} DR_copy;


/*
 * These macros centralize code used to process line_buf and raw_buf buffers.
 * They are macros because they often do continue/break control and to avoid
 * function call overhead in tight COPY loops.
 *
 * We must use "if (1)" because the usual "do {...} while(0)" wrapper would
 * prevent the continue/break processing from working.  We end the "if (1)"
 * with "else ((void) 0)" to ensure the "if" does not unintentionally match
 * any "else" in the calling code, and to avoid any compiler warnings about
 * empty statements.  See http://www.cit.gu.edu.au/~anthony/info/C/C.macros.
 */

/*
 * This keeps the character read at the top of the loop in the buffer
 * even if there is more than one read-ahead.
 */
#define IF_NEED_REFILL_AND_NOT_EOF_CONTINUE(extralen) \
if (1) \
{ \
	if (raw_buf_ptr + (extralen) >= copy_buf_len && !hit_eof) \
	{ \
		raw_buf_ptr = prev_raw_ptr; /* undo fetch */ \
		need_data = true; \
		continue; \
	} \
} else ((void) 0)

/* This consumes the remainder of the buffer and breaks */
#define IF_NEED_REFILL_AND_EOF_BREAK(extralen) \
if (1) \
{ \
	if (raw_buf_ptr + (extralen) >= copy_buf_len && hit_eof) \
	{ \
		if (extralen) \
			raw_buf_ptr = copy_buf_len; /* consume the partial character */ \
		/* backslash just before EOF, treat as data char */ \
		result = true; \
		break; \
	} \
} else ((void) 0)

/*
 * Transfer any approved data to line_buf; must do this to be sure
 * there is some room in raw_buf.
 */
#define REFILL_LINEBUF \
if (1) \
{ \
	if (raw_buf_ptr > cstate->raw_buf_index) \
	{ \
		appendBinaryStringInfo(&cstate->line_buf, \
							 cstate->raw_buf + cstate->raw_buf_index, \
							   raw_buf_ptr - cstate->raw_buf_index); \
		cstate->raw_buf_index = raw_buf_ptr; \
	} \
} else ((void) 0)

/* Undo any read-ahead and jump out of the block. */
#define NO_END_OF_COPY_GOTO \
if (1) \
{ \
	raw_buf_ptr = prev_raw_ptr + 1; \
	goto not_end_of_copy; \
} else ((void) 0)

static const char BinarySignature[11] = "PGCOPY\n\377\r\n\0";


/* non-export function prototypes */
static void DoCopyTo(CopyState cstate);
static void CopyTo(CopyState cstate);
static void CopyOneRowTo(CopyState cstate, Oid tupleOid,
			 Datum *values, bool *nulls);
static void CopyFrom(CopyState cstate);
static bool CopyReadLine(CopyState cstate);
static bool CopyReadLineText(CopyState cstate);
static int CopyReadAttributesText(CopyState cstate, int maxfields,
					   char **fieldvals);
static int CopyReadAttributesCSV(CopyState cstate, int maxfields,
					  char **fieldvals);
static Datum CopyReadBinaryAttribute(CopyState cstate,
						int column_no, FmgrInfo *flinfo,
						Oid typioparam, int32 typmod,
						bool *isnull);
static void CopyAttributeOutText(CopyState cstate, char *string);
static void CopyAttributeOutCSV(CopyState cstate, char *string,
					bool use_quote, bool single_attr);
static List *CopyGetAttnums(TupleDesc tupDesc, Relation rel,
			   List *attnamelist);
static char *limit_printout_length(const char *str);

/* Low-level communications functions */
static void SendCopyBegin(CopyState cstate);
static void ReceiveCopyBegin(CopyState cstate);
static void SendCopyEnd(CopyState cstate);
static void CopySendData(CopyState cstate, void *databuf, int datasize);
static void CopySendString(CopyState cstate, const char *str);
static void CopySendChar(CopyState cstate, char c);
static void CopySendEndOfRow(CopyState cstate);
static int CopyGetData(CopyState cstate, void *databuf,
			int minread, int maxread);
static void CopySendInt32(CopyState cstate, int32 val);
static bool CopyGetInt32(CopyState cstate, int32 *val);
static void CopySendInt16(CopyState cstate, int16 val);
static bool CopyGetInt16(CopyState cstate, int16 *val);


/*
 * Send copy start/stop messages for frontend copies.  These have changed
 * in past protocol redesigns.
 */
static void
SendCopyBegin(CopyState cstate)
{
	if (PG_PROTOCOL_MAJOR(FrontendProtocol) >= 3)
	{
		/* new way */
		StringInfoData buf;
		int			natts = list_length(cstate->attnumlist);
		int16		format = (cstate->binary ? 1 : 0);
		int			i;

		pq_beginmessage(&buf, 'H');
		pq_sendbyte(&buf, format);		/* overall format */
		pq_sendint(&buf, natts, 2);
		for (i = 0; i < natts; i++)
			pq_sendint(&buf, format, 2);		/* per-column formats */
		pq_endmessage(&buf);
		cstate->copy_dest = COPY_NEW_FE;
	}
	else if (PG_PROTOCOL_MAJOR(FrontendProtocol) >= 2)
	{
		/* old way */
		if (cstate->binary)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			errmsg("COPY BINARY is not supported to stdout or from stdin")));
		pq_putemptymessage('H');
		/* grottiness needed for old COPY OUT protocol */
		pq_startcopyout();
		cstate->copy_dest = COPY_OLD_FE;
	}
	else
	{
		/* very old way */
		if (cstate->binary)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			errmsg("COPY BINARY is not supported to stdout or from stdin")));
		pq_putemptymessage('B');
		/* grottiness needed for old COPY OUT protocol */
		pq_startcopyout();
		cstate->copy_dest = COPY_OLD_FE;
	}
}

static void
ReceiveCopyBegin(CopyState cstate)
{
	if (PG_PROTOCOL_MAJOR(FrontendProtocol) >= 3)
	{
		/* new way */
		StringInfoData buf;
		int			natts = list_length(cstate->attnumlist);
		int16		format = (cstate->binary ? 1 : 0);
		int			i;

		pq_beginmessage(&buf, 'G');
		pq_sendbyte(&buf, format);		/* overall format */
		pq_sendint(&buf, natts, 2);
		for (i = 0; i < natts; i++)
			pq_sendint(&buf, format, 2);		/* per-column formats */
		pq_endmessage(&buf);
		cstate->copy_dest = COPY_NEW_FE;
		cstate->fe_msgbuf = makeStringInfo();
	}
	else if (PG_PROTOCOL_MAJOR(FrontendProtocol) >= 2)
	{
		/* old way */
		if (cstate->binary)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			errmsg("COPY BINARY is not supported to stdout or from stdin")));
		pq_putemptymessage('G');
		/* any error in old protocol will make us lose sync */
		pq_startmsgread();
		cstate->copy_dest = COPY_OLD_FE;
	}
	else
	{
		/* very old way */
		if (cstate->binary)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			errmsg("COPY BINARY is not supported to stdout or from stdin")));
		pq_putemptymessage('D');
		/* any error in old protocol will make us lose sync */
		pq_startmsgread();
		cstate->copy_dest = COPY_OLD_FE;
	}
	/* We *must* flush here to ensure FE knows it can send. */
	pq_flush();
}

static void
SendCopyEnd(CopyState cstate)
{
	if (cstate->copy_dest == COPY_NEW_FE)
	{
		/* Shouldn't have any unsent data */
		Assert(cstate->fe_msgbuf->len == 0);
		/* Send Copy Done message */
		pq_putemptymessage('c');
	}
	else
	{
		CopySendData(cstate, "\\.", 2);
		/* Need to flush out the trailer (this also appends a newline) */
		CopySendEndOfRow(cstate);
		pq_endcopyout(false);
	}
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
CopySendData(CopyState cstate, void *databuf, int datasize)
{
	appendBinaryStringInfo(cstate->fe_msgbuf, (char *) databuf, datasize);
}

static void
CopySendString(CopyState cstate, const char *str)
{
	appendBinaryStringInfo(cstate->fe_msgbuf, str, strlen(str));
}

static void
CopySendChar(CopyState cstate, char c)
{
	appendStringInfoCharMacro(cstate->fe_msgbuf, c);
}

static void
CopySendEndOfRow(CopyState cstate)
{
	StringInfo	fe_msgbuf = cstate->fe_msgbuf;

	switch (cstate->copy_dest)
	{
		case COPY_FILE:
			if (!cstate->binary)
			{
				/* Default line termination depends on platform */
#ifndef WIN32
				CopySendChar(cstate, '\n');
#else
				CopySendString(cstate, "\r\n");
#endif
			}

			(void) fwrite(fe_msgbuf->data, fe_msgbuf->len,
						  1, cstate->copy_file);
			if (ferror(cstate->copy_file))
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not write to COPY file: %m")));
			break;
		case COPY_OLD_FE:
			/* The FE/BE protocol uses \n as newline for all platforms */
			if (!cstate->binary)
				CopySendChar(cstate, '\n');

			if (pq_putbytes(fe_msgbuf->data, fe_msgbuf->len))
			{
				/* no hope of recovering connection sync, so FATAL */
				ereport(FATAL,
						(errcode(ERRCODE_CONNECTION_FAILURE),
						 errmsg("connection lost during COPY to stdout")));
			}
			break;
		case COPY_NEW_FE:
			/* The FE/BE protocol uses \n as newline for all platforms */
			if (!cstate->binary)
				CopySendChar(cstate, '\n');

			/* Dump the accumulated row as one CopyData message */
			(void) pq_putmessage('d', fe_msgbuf->data, fe_msgbuf->len);
			break;
	}

	resetStringInfo(fe_msgbuf);
}

/*
 * CopyGetData reads data from the source (file or frontend)
 *
 * We attempt to read at least minread, and at most maxread, bytes from
 * the source.  The actual number of bytes read is returned; if this is
 * less than minread, EOF was detected.
 *
 * Note: when copying from the frontend, we expect a proper EOF mark per
 * protocol; if the frontend simply drops the connection, we raise error.
 * It seems unwise to allow the COPY IN to complete normally in that case.
 *
 * NB: no data conversion is applied here.
 */
static int
CopyGetData(CopyState cstate, void *databuf, int minread, int maxread)
{
	int			bytesread = 0;

	switch (cstate->copy_dest)
	{
		case COPY_FILE:
			bytesread = fread(databuf, 1, maxread, cstate->copy_file);
			if (ferror(cstate->copy_file))
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not read from COPY file: %m")));
			break;
		case COPY_OLD_FE:

			/*
			 * We cannot read more than minread bytes (which in practice is 1)
			 * because old protocol doesn't have any clear way of separating
			 * the COPY stream from following data.  This is slow, but not any
			 * slower than the code path was originally, and we don't care
			 * much anymore about the performance of old protocol.
			 */
			if (pq_getbytes((char *) databuf, minread))
			{
				/* Only a \. terminator is legal EOF in old protocol */
				ereport(ERROR,
						(errcode(ERRCODE_CONNECTION_FAILURE),
						 errmsg("unexpected EOF on client connection")));
			}
			bytesread = minread;
			break;
		case COPY_NEW_FE:
			while (maxread > 0 && bytesread < minread && !cstate->fe_eof)
			{
				int			avail;

				while (cstate->fe_msgbuf->cursor >= cstate->fe_msgbuf->len)
				{
					/* Try to receive another message */
					int			mtype;

			readmessage:
					HOLD_CANCEL_INTERRUPTS();
					pq_startmsgread();
					mtype = pq_getbyte();
					if (mtype == EOF)
						ereport(ERROR,
								(errcode(ERRCODE_CONNECTION_FAILURE),
							 errmsg("unexpected EOF on client connection")));
					if (pq_getmessage(cstate->fe_msgbuf, 0))
						ereport(ERROR,
								(errcode(ERRCODE_CONNECTION_FAILURE),
							 errmsg("unexpected EOF on client connection")));
					RESUME_CANCEL_INTERRUPTS();
					switch (mtype)
					{
						case 'd':		/* CopyData */
							break;
						case 'c':		/* CopyDone */
							/* COPY IN correctly terminated by frontend */
							cstate->fe_eof = true;
							return bytesread;
						case 'f':		/* CopyFail */
							ereport(ERROR,
									(errcode(ERRCODE_QUERY_CANCELED),
									 errmsg("COPY from stdin failed: %s",
									   pq_getmsgstring(cstate->fe_msgbuf))));
							break;
						case 'H':		/* Flush */
						case 'S':		/* Sync */

							/*
							 * Ignore Flush/Sync for the convenience of client
							 * libraries (such as libpq) that may send those
							 * without noticing that the command they just
							 * sent was COPY.
							 */
							goto readmessage;
						default:
							ereport(ERROR,
									(errcode(ERRCODE_PROTOCOL_VIOLATION),
									 errmsg("unexpected message type 0x%02X during COPY from stdin",
											mtype)));
							break;
					}
				}
				avail = cstate->fe_msgbuf->len - cstate->fe_msgbuf->cursor;
				if (avail > maxread)
					avail = maxread;
				pq_copymsgbytes(cstate->fe_msgbuf, databuf, avail);
				databuf = (void *) ((char *) databuf + avail);
				maxread -= avail;
				bytesread += avail;
			}
			break;
	}

	return bytesread;
}


/*
 * These functions do apply some data conversion
 */

/*
 * CopySendInt32 sends an int32 in network byte order
 */
static void
CopySendInt32(CopyState cstate, int32 val)
{
	uint32		buf;

	buf = htonl((uint32) val);
	CopySendData(cstate, &buf, sizeof(buf));
}

/*
 * CopyGetInt32 reads an int32 that appears in network byte order
 *
 * Returns true if OK, false if EOF
 */
static bool
CopyGetInt32(CopyState cstate, int32 *val)
{
	uint32		buf;

	if (CopyGetData(cstate, &buf, sizeof(buf), sizeof(buf)) != sizeof(buf))
	{
		*val = 0;				/* suppress compiler warning */
		return false;
	}
	*val = (int32) ntohl(buf);
	return true;
}

/*
 * CopySendInt16 sends an int16 in network byte order
 */
static void
CopySendInt16(CopyState cstate, int16 val)
{
	uint16		buf;

	buf = htons((uint16) val);
	CopySendData(cstate, &buf, sizeof(buf));
}

/*
 * CopyGetInt16 reads an int16 that appears in network byte order
 */
static bool
CopyGetInt16(CopyState cstate, int16 *val)
{
	uint16		buf;

	if (CopyGetData(cstate, &buf, sizeof(buf), sizeof(buf)) != sizeof(buf))
	{
		*val = 0;				/* suppress compiler warning */
		return false;
	}
	*val = (int16) ntohs(buf);
	return true;
}


/*
 * CopyLoadRawBuf loads some more data into raw_buf
 *
 * Returns TRUE if able to obtain at least one more byte, else FALSE.
 *
 * If raw_buf_index < raw_buf_len, the unprocessed bytes are transferred
 * down to the start of the buffer and then we load more data after that.
 * This case is used only when a frontend multibyte character crosses a
 * bufferload boundary.
 */
static bool
CopyLoadRawBuf(CopyState cstate)
{
	int			nbytes;
	int			inbytes;

	if (cstate->raw_buf_index < cstate->raw_buf_len)
	{
		/* Copy down the unprocessed data */
		nbytes = cstate->raw_buf_len - cstate->raw_buf_index;
		memmove(cstate->raw_buf, cstate->raw_buf + cstate->raw_buf_index,
				nbytes);
	}
	else
		nbytes = 0;				/* no data need be saved */

	inbytes = CopyGetData(cstate, cstate->raw_buf + nbytes,
						  1, RAW_BUF_SIZE - nbytes);
	nbytes += inbytes;
	cstate->raw_buf[nbytes] = '\0';
	cstate->raw_buf_index = 0;
	cstate->raw_buf_len = nbytes;
	return (inbytes > 0);
}


/*
 *	 DoCopy executes the SQL COPY statement
 *
 * Either unload or reload contents of table <relation>, depending on <from>.
 * (<from> = TRUE means we are inserting into the table.)  In the "TO" case
 * we also support copying the output of an arbitrary SELECT query.
 *
 * If <pipe> is false, transfer is between the table and the file named
 * <filename>.  Otherwise, transfer is between the table and our regular
 * input/output stream. The latter could be either stdin/stdout or a
 * socket, depending on whether we're running under Postmaster control.
 *
 * Iff <binary>, unload or reload in the binary format, as opposed to the
 * more wasteful but more robust and portable text format.
 *
 * Iff <oids>, unload or reload the format that includes OID information.
 * On input, we accept OIDs whether or not the table has an OID column,
 * but silently drop them if it does not.  On output, we report an error
 * if the user asks for OIDs in a table that has none (not providing an
 * OID column might seem friendlier, but could seriously confuse programs).
 *
 * If in the text format, delimit columns with delimiter <delim> and print
 * NULL values as <null_print>.
 *
 * Do not allow a Postgres user without superuser privilege to read from
 * or write to a file.
 *
 * Do not allow the copy if user doesn't have proper permission to access
 * the table or the specifically requested columns.
 */
uint64
DoCopy(const CopyStmt *stmt, const char *queryString)
{
	CopyState	cstate;
	bool		is_from = stmt->is_from;
	bool		pipe = (stmt->filename == NULL);
	List	   *attnamelist = stmt->attlist;
	List	   *force_quote = NIL;
	List	   *force_notnull = NIL;
	bool		force_quote_all = false;
	bool		format_specified = false;
	AclMode		required_access = (is_from ? ACL_INSERT : ACL_SELECT);
	AclMode		relPerms;
	AclMode		remainingPerms;
	ListCell   *option;
	TupleDesc	tupDesc;
	int			num_phys_attrs;
	uint64		processed;

	/* Allocate workspace and zero all fields */
	cstate = (CopyStateData *) palloc0(sizeof(CopyStateData));

	/* Extract options from the statement node tree */
	foreach(option, stmt->options)
	{
		DefElem    *defel = (DefElem *) lfirst(option);

		if (strcmp(defel->defname, "format") == 0)
		{
			char	   *fmt = defGetString(defel);

			if (format_specified)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			format_specified = true;
			if (strcmp(fmt, "text") == 0)
				 /* default format */ ;
			else if (strcmp(fmt, "csv") == 0)
				cstate->csv_mode = true;
			else if (strcmp(fmt, "binary") == 0)
				cstate->binary = true;
			else
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("COPY format \"%s\" not recognized", fmt)));
		}
		else if (strcmp(defel->defname, "oids") == 0)
		{
			if (cstate->oids)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			cstate->oids = defGetBoolean(defel);
		}
		else if (strcmp(defel->defname, "delimiter") == 0)
		{
			if (cstate->delim)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			cstate->delim = defGetString(defel);
		}
		else if (strcmp(defel->defname, "null") == 0)
		{
			if (cstate->null_print)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			cstate->null_print = defGetString(defel);
		}
		else if (strcmp(defel->defname, "header") == 0)
		{
			if (cstate->header_line)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			cstate->header_line = defGetBoolean(defel);
		}
		else if (strcmp(defel->defname, "quote") == 0)
		{
			if (cstate->quote)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			cstate->quote = defGetString(defel);
		}
		else if (strcmp(defel->defname, "escape") == 0)
		{
			if (cstate->escape)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			cstate->escape = defGetString(defel);
		}
		else if (strcmp(defel->defname, "force_quote") == 0)
		{
			if (force_quote || force_quote_all)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			if (defel->arg && IsA(defel->arg, A_Star))
				force_quote_all = true;
			else if (defel->arg && IsA(defel->arg, List))
				force_quote = (List *) defel->arg;
			else
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("argument to option \"%s\" must be a list of column names",
								defel->defname)));
		}
		else if (strcmp(defel->defname, "force_not_null") == 0)
		{
			if (force_notnull)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			if (defel->arg && IsA(defel->arg, List))
				force_notnull = (List *) defel->arg;
			else
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("argument to option \"%s\" must be a list of column names",
								defel->defname)));
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("option \"%s\" not recognized",
							defel->defname)));
	}

	/*
	 * Check for incompatible options (must do these two before inserting
	 * defaults)
	 */
	if (cstate->binary && cstate->delim)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("cannot specify DELIMITER in BINARY mode")));

	if (cstate->binary && cstate->null_print)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("cannot specify NULL in BINARY mode")));

	/* Set defaults for omitted options */
	if (!cstate->delim)
		cstate->delim = cstate->csv_mode ? "," : "\t";

	if (!cstate->null_print)
		cstate->null_print = cstate->csv_mode ? "" : "\\N";
	cstate->null_print_len = strlen(cstate->null_print);

	if (cstate->csv_mode)
	{
		if (!cstate->quote)
			cstate->quote = "\"";
		if (!cstate->escape)
			cstate->escape = cstate->quote;
	}

	/* Only single-byte delimiter strings are supported. */
	if (strlen(cstate->delim) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			  errmsg("COPY delimiter must be a single one-byte character")));

	/* Disallow end-of-line characters */
	if (strchr(cstate->delim, '\r') != NULL ||
		strchr(cstate->delim, '\n') != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("COPY delimiter cannot be newline or carriage return")));

	if (strchr(cstate->null_print, '\r') != NULL ||
		strchr(cstate->null_print, '\n') != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("COPY null representation cannot use newline or carriage return")));

	/*
	 * Disallow unsafe delimiter characters in non-CSV mode.  We can't allow
	 * backslash because it would be ambiguous.  We can't allow the other
	 * cases because data characters matching the delimiter must be
	 * backslashed, and certain backslash combinations are interpreted
	 * non-literally by COPY IN.  Disallowing all lower case ASCII letters is
	 * more than strictly necessary, but seems best for consistency and
	 * future-proofing.  Likewise we disallow all digits though only octal
	 * digits are actually dangerous.
	 */
	if (!cstate->csv_mode &&
		strchr("\\.abcdefghijklmnopqrstuvwxyz0123456789",
			   cstate->delim[0]) != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("COPY delimiter cannot be \"%s\"", cstate->delim)));

	/* Check header */
	if (!cstate->csv_mode && cstate->header_line)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("COPY HEADER available only in CSV mode")));

	/* Check quote */
	if (!cstate->csv_mode && cstate->quote != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("COPY quote available only in CSV mode")));

	if (cstate->csv_mode && strlen(cstate->quote) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("COPY quote must be a single one-byte character")));

	if (cstate->csv_mode && cstate->delim[0] == cstate->quote[0])
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("COPY delimiter and quote must be different")));

	/* Check escape */
	if (!cstate->csv_mode && cstate->escape != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("COPY escape available only in CSV mode")));

	if (cstate->csv_mode && strlen(cstate->escape) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("COPY escape must be a single one-byte character")));

	/* Check force_quote */
	if (!cstate->csv_mode && (force_quote != NIL || force_quote_all))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("COPY force quote available only in CSV mode")));
	if ((force_quote != NIL || force_quote_all) && is_from)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("COPY force quote only available using COPY TO")));

	/* Check force_notnull */
	if (!cstate->csv_mode && force_notnull != NIL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("COPY force not null available only in CSV mode")));
	if (force_notnull != NIL && !is_from)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			  errmsg("COPY force not null only available using COPY FROM")));

	/* Don't allow the delimiter to appear in the null string. */
	if (strchr(cstate->null_print, cstate->delim[0]) != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		errmsg("COPY delimiter must not appear in the NULL specification")));

	/* Don't allow the CSV quote char to appear in the null string. */
	if (cstate->csv_mode &&
		strchr(cstate->null_print, cstate->quote[0]) != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("CSV quote character must not appear in the NULL specification")));

	/* Disallow file COPY except to superusers. */
	if (!pipe && !superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to COPY to or from a file"),
				 errhint("Anyone can COPY to stdout or from stdin. "
						 "psql's \\copy command also works for anyone.")));

	if (stmt->relation)
	{
		Assert(!stmt->query);
		cstate->queryDesc = NULL;

		/* Open and lock the relation, using the appropriate lock type. */
		cstate->rel = heap_openrv(stmt->relation,
							 (is_from ? RowExclusiveLock : AccessShareLock));

		tupDesc = RelationGetDescr(cstate->rel);

		/* Check relation permissions. */
		relPerms = pg_class_aclmask(RelationGetRelid(cstate->rel), GetUserId(),
									required_access, ACLMASK_ALL);
		remainingPerms = required_access & ~relPerms;
		if (remainingPerms != 0)
		{
			/* We don't have table permissions, check per-column permissions */
			List	   *attnums;
			ListCell   *cur;

			attnums = CopyGetAttnums(tupDesc, cstate->rel, attnamelist);
			foreach(cur, attnums)
			{
				int			attnum = lfirst_int(cur);

				if (pg_attribute_aclcheck(RelationGetRelid(cstate->rel),
										  attnum,
										  GetUserId(),
										  remainingPerms) != ACLCHECK_OK)
					aclcheck_error(ACLCHECK_NO_PRIV, ACL_KIND_CLASS,
								   RelationGetRelationName(cstate->rel));
			}
		}

		/* check read-only transaction */
		if (XactReadOnly && is_from && !cstate->rel->rd_islocaltemp)
			PreventCommandIfReadOnly("COPY FROM");

		/* Don't allow COPY w/ OIDs to or from a table without them */
		if (cstate->oids && !cstate->rel->rd_rel->relhasoids)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("table \"%s\" does not have OIDs",
							RelationGetRelationName(cstate->rel))));
	}
	else
	{
		List	   *rewritten;
		Query	   *query;
		PlannedStmt *plan;
		DestReceiver *dest;

		Assert(!is_from);
		cstate->rel = NULL;

		/* Don't allow COPY w/ OIDs from a select */
		if (cstate->oids)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("COPY (SELECT) WITH OIDS is not supported")));

		/*
		 * Run parse analysis and rewrite.  Note this also acquires sufficient
		 * locks on the source table(s).
		 *
		 * Because the parser and planner tend to scribble on their input, we
		 * make a preliminary copy of the source querytree.  This prevents
		 * problems in the case that the COPY is in a portal or plpgsql
		 * function and is executed repeatedly.  (See also the same hack in
		 * DECLARE CURSOR and PREPARE.)  XXX FIXME someday.
		 */
		rewritten = pg_analyze_and_rewrite((Node *) copyObject(stmt->query),
										   queryString, NULL, 0);

		/* We don't expect more or less than one result query */
		if (list_length(rewritten) != 1)
			elog(ERROR, "unexpected rewrite result");

		query = (Query *) linitial(rewritten);
		Assert(query->commandType == CMD_SELECT);
		Assert(query->utilityStmt == NULL);

		/* Query mustn't use INTO, either */
		if (query->intoClause)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("COPY (SELECT INTO) is not supported")));

		/* plan the query */
		plan = planner(query, 0, NULL);

		/*
		 * Use a snapshot with an updated command ID to ensure this query sees
		 * results of any previously executed queries.
		 */
		PushUpdatedSnapshot(GetActiveSnapshot());

		/* Create dest receiver for COPY OUT */
		dest = CreateDestReceiver(DestCopyOut);
		((DR_copy *) dest)->cstate = cstate;

		/* Create a QueryDesc requesting no output */
		cstate->queryDesc = CreateQueryDesc(plan, queryString,
											GetActiveSnapshot(),
											InvalidSnapshot,
											dest, NULL, 0);

		/*
		 * Call ExecutorStart to prepare the plan for execution.
		 *
		 * ExecutorStart computes a result tupdesc for us
		 */
		ExecutorStart(cstate->queryDesc, 0);

		tupDesc = cstate->queryDesc->tupDesc;
	}

	/* Generate or convert list of attributes to process */
	cstate->attnumlist = CopyGetAttnums(tupDesc, cstate->rel, attnamelist);

	num_phys_attrs = tupDesc->natts;

	/* Convert FORCE QUOTE name list to per-column flags, check validity */
	cstate->force_quote_flags = (bool *) palloc0(num_phys_attrs * sizeof(bool));
	if (force_quote_all)
	{
		int			i;

		for (i = 0; i < num_phys_attrs; i++)
			cstate->force_quote_flags[i] = true;
	}
	else if (force_quote)
	{
		List	   *attnums;
		ListCell   *cur;

		attnums = CopyGetAttnums(tupDesc, cstate->rel, force_quote);

		foreach(cur, attnums)
		{
			int			attnum = lfirst_int(cur);

			if (!list_member_int(cstate->attnumlist, attnum))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
				   errmsg("FORCE QUOTE column \"%s\" not referenced by COPY",
						  NameStr(tupDesc->attrs[attnum - 1]->attname))));
			cstate->force_quote_flags[attnum - 1] = true;
		}
	}

	/* Convert FORCE NOT NULL name list to per-column flags, check validity */
	cstate->force_notnull_flags = (bool *) palloc0(num_phys_attrs * sizeof(bool));
	if (force_notnull)
	{
		List	   *attnums;
		ListCell   *cur;

		attnums = CopyGetAttnums(tupDesc, cstate->rel, force_notnull);

		foreach(cur, attnums)
		{
			int			attnum = lfirst_int(cur);

			if (!list_member_int(cstate->attnumlist, attnum))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
				errmsg("FORCE NOT NULL column \"%s\" not referenced by COPY",
					   NameStr(tupDesc->attrs[attnum - 1]->attname))));
			cstate->force_notnull_flags[attnum - 1] = true;
		}
	}

	/* Set up variables to avoid per-attribute overhead. */
	initStringInfo(&cstate->attribute_buf);
	initStringInfo(&cstate->line_buf);
	cstate->line_buf_converted = false;
	cstate->raw_buf = (char *) palloc(RAW_BUF_SIZE + 1);
	cstate->raw_buf_index = cstate->raw_buf_len = 0;
	cstate->processed = 0;

	/*
	 * Set up encoding conversion info.  Even if the client and server
	 * encodings are the same, we must apply pg_client_to_server() to validate
	 * data in multibyte encodings.
	 */
	cstate->client_encoding = pg_get_client_encoding();
	cstate->need_transcoding =
		(cstate->client_encoding != GetDatabaseEncoding() ||
		 pg_database_encoding_max_length() > 1);
	/* See Multibyte encoding comment above */
	cstate->encoding_embeds_ascii = PG_ENCODING_IS_CLIENT_ONLY(cstate->client_encoding);

	cstate->copy_dest = COPY_FILE;		/* default */
	cstate->filename = stmt->filename;

	if (is_from)
		CopyFrom(cstate);		/* copy from file to database */
	else
		DoCopyTo(cstate);		/* copy from database to file */

	/*
	 * Close the relation or query.  If reading, we can release the
	 * AccessShareLock we got; if writing, we should hold the lock until end
	 * of transaction to ensure that updates will be committed before lock is
	 * released.
	 */
	if (cstate->rel)
		heap_close(cstate->rel, (is_from ? NoLock : AccessShareLock));
	else
	{
		/* Close down the query and free resources. */
		ExecutorEnd(cstate->queryDesc);
		FreeQueryDesc(cstate->queryDesc);
		PopActiveSnapshot();
	}

	/* Clean up storage (probably not really necessary) */
	processed = cstate->processed;

	pfree(cstate->attribute_buf.data);
	pfree(cstate->line_buf.data);
	pfree(cstate->raw_buf);
	pfree(cstate);

	return processed;
}


/*
 * This intermediate routine exists mainly to localize the effects of setjmp
 * so we don't need to plaster a lot of variables with "volatile".
 */
static void
DoCopyTo(CopyState cstate)
{
	bool		pipe = (cstate->filename == NULL);

	if (cstate->rel)
	{
		if (cstate->rel->rd_rel->relkind != RELKIND_RELATION)
		{
			if (cstate->rel->rd_rel->relkind == RELKIND_VIEW)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("cannot copy from view \"%s\"",
								RelationGetRelationName(cstate->rel)),
						 errhint("Try the COPY (SELECT ...) TO variant.")));
			else if (cstate->rel->rd_rel->relkind == RELKIND_SEQUENCE)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("cannot copy from sequence \"%s\"",
								RelationGetRelationName(cstate->rel))));
			else
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("cannot copy from non-table relation \"%s\"",
								RelationGetRelationName(cstate->rel))));
		}
	}

	if (pipe)
	{
		if (whereToSendOutput == DestRemote)
			cstate->fe_copy = true;
		else
			cstate->copy_file = stdout;
	}
	else
	{
		mode_t		oumask;		/* Pre-existing umask value */
		struct stat st;

		/*
		 * Prevent write to relative path ... too easy to shoot oneself in the
		 * foot by overwriting a database file ...
		 */
		if (!is_absolute_path(cstate->filename))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_NAME),
					 errmsg("relative path not allowed for COPY to file")));

		oumask = umask((mode_t) 022);
		cstate->copy_file = AllocateFile(cstate->filename, PG_BINARY_W);
		umask(oumask);

		if (cstate->copy_file == NULL)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not open file \"%s\" for writing: %m",
							cstate->filename)));

		fstat(fileno(cstate->copy_file), &st);
		if (S_ISDIR(st.st_mode))
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is a directory", cstate->filename)));
	}

	PG_TRY();
	{
		if (cstate->fe_copy)
			SendCopyBegin(cstate);

		CopyTo(cstate);

		if (cstate->fe_copy)
			SendCopyEnd(cstate);
	}
	PG_CATCH();
	{
		/*
		 * Make sure we turn off old-style COPY OUT mode upon error. It is
		 * okay to do this in all cases, since it does nothing if the mode is
		 * not on.
		 */
		pq_endcopyout(true);
		PG_RE_THROW();
	}
	PG_END_TRY();

	if (!pipe)
	{
		if (FreeFile(cstate->copy_file))
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not write to file \"%s\": %m",
							cstate->filename)));
	}
}

/*
 * Copy from relation or query TO file.
 */
static void
CopyTo(CopyState cstate)
{
	TupleDesc	tupDesc;
	int			num_phys_attrs;
	Form_pg_attribute *attr;
	ListCell   *cur;

	if (cstate->rel)
		tupDesc = RelationGetDescr(cstate->rel);
	else
		tupDesc = cstate->queryDesc->tupDesc;
	attr = tupDesc->attrs;
	num_phys_attrs = tupDesc->natts;
	cstate->null_print_client = cstate->null_print;		/* default */

	/* We use fe_msgbuf as a per-row buffer regardless of copy_dest */
	cstate->fe_msgbuf = makeStringInfo();

	/* Get info about the columns we need to process. */
	cstate->out_functions = (FmgrInfo *) palloc(num_phys_attrs * sizeof(FmgrInfo));
	foreach(cur, cstate->attnumlist)
	{
		int			attnum = lfirst_int(cur);
		Oid			out_func_oid;
		bool		isvarlena;

		if (cstate->binary)
			getTypeBinaryOutputInfo(attr[attnum - 1]->atttypid,
									&out_func_oid,
									&isvarlena);
		else
			getTypeOutputInfo(attr[attnum - 1]->atttypid,
							  &out_func_oid,
							  &isvarlena);
		fmgr_info(out_func_oid, &cstate->out_functions[attnum - 1]);
	}

	/*
	 * Create a temporary memory context that we can reset once per row to
	 * recover palloc'd memory.  This avoids any problems with leaks inside
	 * datatype output routines, and should be faster than retail pfree's
	 * anyway.  (We don't need a whole econtext as CopyFrom does.)
	 */
	cstate->rowcontext = AllocSetContextCreate(CurrentMemoryContext,
											   "COPY TO",
											   ALLOCSET_DEFAULT_MINSIZE,
											   ALLOCSET_DEFAULT_INITSIZE,
											   ALLOCSET_DEFAULT_MAXSIZE);

	if (cstate->binary)
	{
		/* Generate header for a binary copy */
		int32		tmp;

		/* Signature */
		CopySendData(cstate, (char *) BinarySignature, 11);
		/* Flags field */
		tmp = 0;
		if (cstate->oids)
			tmp |= (1 << 16);
		CopySendInt32(cstate, tmp);
		/* No header extension */
		tmp = 0;
		CopySendInt32(cstate, tmp);
	}
	else
	{
		/*
		 * For non-binary copy, we need to convert null_print to client
		 * encoding, because it will be sent directly with CopySendString.
		 */
		if (cstate->need_transcoding)
			cstate->null_print_client = pg_server_to_client(cstate->null_print,
													 cstate->null_print_len);

		/* if a header has been requested send the line */
		if (cstate->header_line)
		{
			bool		hdr_delim = false;

			foreach(cur, cstate->attnumlist)
			{
				int			attnum = lfirst_int(cur);
				char	   *colname;

				if (hdr_delim)
					CopySendChar(cstate, cstate->delim[0]);
				hdr_delim = true;

				colname = NameStr(attr[attnum - 1]->attname);

				CopyAttributeOutCSV(cstate, colname, false,
									list_length(cstate->attnumlist) == 1);
			}

			CopySendEndOfRow(cstate);
		}
	}

	if (cstate->rel)
	{
		Datum	   *values;
		bool	   *nulls;
		HeapScanDesc scandesc;
		HeapTuple	tuple;

		values = (Datum *) palloc(num_phys_attrs * sizeof(Datum));
		nulls = (bool *) palloc(num_phys_attrs * sizeof(bool));

		scandesc = heap_beginscan(cstate->rel, GetActiveSnapshot(), 0, NULL);

		while ((tuple = heap_getnext(scandesc, ForwardScanDirection)) != NULL)
		{
			CHECK_FOR_INTERRUPTS();

			/* Deconstruct the tuple ... faster than repeated heap_getattr */
			heap_deform_tuple(tuple, tupDesc, values, nulls);

			/* Format and send the data */
			CopyOneRowTo(cstate, HeapTupleGetOid(tuple), values, nulls);
		}

		heap_endscan(scandesc);
	}
	else
	{
		/* run the plan --- the dest receiver will send tuples */
		ExecutorRun(cstate->queryDesc, ForwardScanDirection, 0L);
	}

	if (cstate->binary)
	{
		/* Generate trailer for a binary copy */
		CopySendInt16(cstate, -1);
		/* Need to flush out the trailer */
		CopySendEndOfRow(cstate);
	}

	MemoryContextDelete(cstate->rowcontext);
}

/*
 * Emit one row during CopyTo().
 */
static void
CopyOneRowTo(CopyState cstate, Oid tupleOid, Datum *values, bool *nulls)
{
	bool		need_delim = false;
	FmgrInfo   *out_functions = cstate->out_functions;
	MemoryContext oldcontext;
	ListCell   *cur;
	char	   *string;

	MemoryContextReset(cstate->rowcontext);
	oldcontext = MemoryContextSwitchTo(cstate->rowcontext);

	if (cstate->binary)
	{
		/* Binary per-tuple header */
		CopySendInt16(cstate, list_length(cstate->attnumlist));
		/* Send OID if wanted --- note attnumlist doesn't include it */
		if (cstate->oids)
		{
			/* Hack --- assume Oid is same size as int32 */
			CopySendInt32(cstate, sizeof(int32));
			CopySendInt32(cstate, tupleOid);
		}
	}
	else
	{
		/* Text format has no per-tuple header, but send OID if wanted */
		/* Assume digits don't need any quoting or encoding conversion */
		if (cstate->oids)
		{
			string = DatumGetCString(DirectFunctionCall1(oidout,
												ObjectIdGetDatum(tupleOid)));
			CopySendString(cstate, string);
			need_delim = true;
		}
	}

	foreach(cur, cstate->attnumlist)
	{
		int			attnum = lfirst_int(cur);
		Datum		value = values[attnum - 1];
		bool		isnull = nulls[attnum - 1];

		if (!cstate->binary)
		{
			if (need_delim)
				CopySendChar(cstate, cstate->delim[0]);
			need_delim = true;
		}

		if (isnull)
		{
			if (!cstate->binary)
				CopySendString(cstate, cstate->null_print_client);
			else
				CopySendInt32(cstate, -1);
		}
		else
		{
			if (!cstate->binary)
			{
				string = OutputFunctionCall(&out_functions[attnum - 1],
											value);
				if (cstate->csv_mode)
					CopyAttributeOutCSV(cstate, string,
										cstate->force_quote_flags[attnum - 1],
										list_length(cstate->attnumlist) == 1);
				else
					CopyAttributeOutText(cstate, string);
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
	}

	CopySendEndOfRow(cstate);

	MemoryContextSwitchTo(oldcontext);

	cstate->processed++;
}


/*
 * error context callback for COPY FROM
 */
static void
copy_in_error_callback(void *arg)
{
	CopyState	cstate = (CopyState) arg;

	if (cstate->binary)
	{
		/* can't usefully display the data */
		if (cstate->cur_attname)
			errcontext("COPY %s, line %d, column %s",
					   cstate->cur_relname, cstate->cur_lineno,
					   cstate->cur_attname);
		else
			errcontext("COPY %s, line %d",
					   cstate->cur_relname, cstate->cur_lineno);
	}
	else
	{
		if (cstate->cur_attname && cstate->cur_attval)
		{
			/* error is relevant to a particular column */
			char	   *attval;

			attval = limit_printout_length(cstate->cur_attval);
			errcontext("COPY %s, line %d, column %s: \"%s\"",
					   cstate->cur_relname, cstate->cur_lineno,
					   cstate->cur_attname, attval);
			pfree(attval);
		}
		else if (cstate->cur_attname)
		{
			/* error is relevant to a particular column, value is NULL */
			errcontext("COPY %s, line %d, column %s: null input",
					   cstate->cur_relname, cstate->cur_lineno,
					   cstate->cur_attname);
		}
		else
		{
			/* error is relevant to a particular line */
			if (cstate->line_buf_converted || !cstate->need_transcoding)
			{
				char	   *lineval;

				lineval = limit_printout_length(cstate->line_buf.data);
				errcontext("COPY %s, line %d: \"%s\"",
						   cstate->cur_relname, cstate->cur_lineno, lineval);
				pfree(lineval);
			}
			else
			{
				/*
				 * Here, the line buffer is still in a foreign encoding, and
				 * indeed it's quite likely that the error is precisely a
				 * failure to do encoding conversion (ie, bad data).  We dare
				 * not try to convert it, and at present there's no way to
				 * regurgitate it without conversion.  So we have to punt and
				 * just report the line number.
				 */
				errcontext("COPY %s, line %d",
						   cstate->cur_relname, cstate->cur_lineno);
			}
		}
	}
}

/*
 * Make sure we don't print an unreasonable amount of COPY data in a message.
 *
 * It would seem a lot easier to just use the sprintf "precision" limit to
 * truncate the string.  However, some versions of glibc have a bug/misfeature
 * that vsnprintf will always fail (return -1) if it is asked to truncate
 * a string that contains invalid byte sequences for the current encoding.
 * So, do our own truncation.  We return a pstrdup'd copy of the input.
 */
static char *
limit_printout_length(const char *str)
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
 * Copy FROM file to relation.
 */
static void
CopyFrom(CopyState cstate)
{
	bool		pipe = (cstate->filename == NULL);
	HeapTuple	tuple;
	TupleDesc	tupDesc;
	Form_pg_attribute *attr;
	AttrNumber	num_phys_attrs,
				attr_count,
				num_defaults;
	FmgrInfo   *in_functions;
	FmgrInfo	oid_in_function;
	Oid		   *typioparams;
	Oid			oid_typioparam;
	int			attnum;
	int			i;
	Oid			in_func_oid;
	Datum	   *values;
	bool	   *nulls;
	int			nfields;
	char	  **field_strings;
	bool		done = false;
	bool		isnull;
	ResultRelInfo *resultRelInfo;
	RangeTblEntry *rte;
	EState	   *estate = CreateExecutorState(); /* for ExecConstraints() */
	TupleTableSlot *slot;
	bool		file_has_oids;
	int		   *defmap;
	ExprState **defexprs;		/* array of default att expressions */
	ExprContext *econtext;		/* used for ExecEvalExpr for default atts */
	MemoryContext oldcontext = CurrentMemoryContext;
	ErrorContextCallback errcontext;
	CommandId	mycid = GetCurrentCommandId(true);
	int			hi_options = 0; /* start with default heap_insert options */
	BulkInsertState bistate;

	Assert(cstate->rel);

	if (cstate->rel->rd_rel->relkind != RELKIND_RELATION)
	{
		if (cstate->rel->rd_rel->relkind == RELKIND_VIEW)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot copy to view \"%s\"",
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

	/*----------
	 * Check to see if we can avoid writing WAL
	 *
	 * If archive logging/streaming is not enabled *and* either
	 *	- table was created in same transaction as this COPY
	 *	- data is being written to relfilenode created in this transaction
	 * then we can skip writing WAL.  It's safe because if the transaction
	 * doesn't commit, we'll discard the table (or the new relfilenode file).
	 * If it does commit, we'll have done the heap_sync at the bottom of this
	 * routine first.
	 *
	 * As mentioned in comments in utils/rel.h, the in-same-transaction test
	 * is not completely reliable, since in rare cases rd_createSubid or
	 * rd_newRelfilenodeSubid can be cleared before the end of the transaction.
	 * However this is OK since at worst we will fail to make the optimization.
	 *
	 * Also, if the target file is new-in-transaction, we assume that checking
	 * FSM for free space is a waste of time, even if we must use WAL because
	 * of archiving.  This could possibly be wrong, but it's unlikely.
	 *
	 * The comments for heap_insert and RelationGetBufferForTuple specify that
	 * skipping WAL logging is only safe if we ensure that our tuples do not
	 * go into pages containing tuples from any other transactions --- but this
	 * must be the case if we have a new table or new relfilenode, so we need
	 * no additional work to enforce that.
	 *----------
	 */
	if (cstate->rel->rd_createSubid != InvalidSubTransactionId ||
		cstate->rel->rd_newRelfilenodeSubid != InvalidSubTransactionId)
	{
		hi_options |= HEAP_INSERT_SKIP_FSM;
		if (!XLogIsNeeded())
			hi_options |= HEAP_INSERT_SKIP_WAL;
	}

	if (pipe)
	{
		if (whereToSendOutput == DestRemote)
			ReceiveCopyBegin(cstate);
		else
			cstate->copy_file = stdin;
	}
	else
	{
		struct stat st;

		cstate->copy_file = AllocateFile(cstate->filename, PG_BINARY_R);

		if (cstate->copy_file == NULL)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not open file \"%s\" for reading: %m",
							cstate->filename)));

		fstat(fileno(cstate->copy_file), &st);
		if (S_ISDIR(st.st_mode))
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is a directory", cstate->filename)));
	}

	tupDesc = RelationGetDescr(cstate->rel);
	attr = tupDesc->attrs;
	num_phys_attrs = tupDesc->natts;
	attr_count = list_length(cstate->attnumlist);
	num_defaults = 0;

	/*
	 * We need a ResultRelInfo so we can use the regular executor's
	 * index-entry-making machinery.  (There used to be a huge amount of code
	 * here that basically duplicated execUtils.c ...)
	 */
	resultRelInfo = makeNode(ResultRelInfo);
	resultRelInfo->ri_RangeTableIndex = 1;		/* dummy */
	resultRelInfo->ri_RelationDesc = cstate->rel;
	resultRelInfo->ri_TrigDesc = CopyTriggerDesc(cstate->rel->trigdesc);
	if (resultRelInfo->ri_TrigDesc)
	{
		resultRelInfo->ri_TrigFunctions = (FmgrInfo *)
			palloc0(resultRelInfo->ri_TrigDesc->numtriggers * sizeof(FmgrInfo));
		resultRelInfo->ri_TrigWhenExprs = (List **)
			palloc0(resultRelInfo->ri_TrigDesc->numtriggers * sizeof(List *));
	}
	resultRelInfo->ri_TrigInstrument = NULL;

	ExecOpenIndices(resultRelInfo);

	/* Build an RTE to make into a RangeTbl for estate */
	rte = makeNode(RangeTblEntry);
	rte->rtekind = RTE_RELATION;
	rte->relid = RelationGetRelid(cstate->rel);
	rte->requiredPerms = ACL_INSERT;

	estate->es_result_relations = resultRelInfo;
	estate->es_num_result_relations = 1;
	estate->es_result_relation_info = resultRelInfo;
	estate->es_range_table = list_make1(rte);

	/* Set up a tuple slot too */
	slot = ExecInitExtraTupleSlot(estate);
	ExecSetSlotDescriptor(slot, tupDesc);

	econtext = GetPerTupleExprContext(estate);

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

	for (attnum = 1; attnum <= num_phys_attrs; attnum++)
	{
		/* We don't need info for dropped attributes */
		if (attr[attnum - 1]->attisdropped)
			continue;

		/* Fetch the input function and typioparam info */
		if (cstate->binary)
			getTypeBinaryInputInfo(attr[attnum - 1]->atttypid,
								   &in_func_oid, &typioparams[attnum - 1]);
		else
			getTypeInputInfo(attr[attnum - 1]->atttypid,
							 &in_func_oid, &typioparams[attnum - 1]);
		fmgr_info(in_func_oid, &in_functions[attnum - 1]);

		/* Get default info if needed */
		if (!list_member_int(cstate->attnumlist, attnum))
		{
			/* attribute is NOT to be copied from input */
			/* use default value if one exists */
			Node	   *defexpr = build_column_default(cstate->rel, attnum);

			if (defexpr != NULL)
			{
				defexprs[num_defaults] = ExecPrepareExpr((Expr *) defexpr,
														 estate);
				defmap[num_defaults] = attnum - 1;
				num_defaults++;
			}
		}
	}

	/* Prepare to catch AFTER triggers. */
	AfterTriggerBeginQuery();

	/*
	 * Check BEFORE STATEMENT insertion triggers. It's debateable whether we
	 * should do this for COPY, since it's not really an "INSERT" statement as
	 * such. However, executing these triggers maintains consistency with the
	 * EACH ROW triggers that we already fire on COPY.
	 */
	ExecBSInsertTriggers(estate, resultRelInfo);

	if (!cstate->binary)
		file_has_oids = cstate->oids;	/* must rely on user to tell us... */
	else
	{
		/* Read and verify binary header */
		char		readSig[11];
		int32		tmp;

		/* Signature */
		if (CopyGetData(cstate, readSig, 11, 11) != 11 ||
			memcmp(readSig, BinarySignature, 11) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
					 errmsg("COPY file signature not recognized")));
		/* Flags field */
		if (!CopyGetInt32(cstate, &tmp))
			ereport(ERROR,
					(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
					 errmsg("invalid COPY file header (missing flags)")));
		file_has_oids = (tmp & (1 << 16)) != 0;
		tmp &= ~(1 << 16);
		if ((tmp >> 16) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
				 errmsg("unrecognized critical flags in COPY file header")));
		/* Header extension length */
		if (!CopyGetInt32(cstate, &tmp) ||
			tmp < 0)
			ereport(ERROR,
					(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
					 errmsg("invalid COPY file header (missing length)")));
		/* Skip extension header, if present */
		while (tmp-- > 0)
		{
			if (CopyGetData(cstate, readSig, 1, 1) != 1)
				ereport(ERROR,
						(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
						 errmsg("invalid COPY file header (wrong length)")));
		}
	}

	if (file_has_oids && cstate->binary)
	{
		getTypeBinaryInputInfo(OIDOID,
							   &in_func_oid, &oid_typioparam);
		fmgr_info(in_func_oid, &oid_in_function);
	}

	values = (Datum *) palloc(num_phys_attrs * sizeof(Datum));
	nulls = (bool *) palloc(num_phys_attrs * sizeof(bool));

	/* create workspace for CopyReadAttributes results */
	nfields = file_has_oids ? (attr_count + 1) : attr_count;
	field_strings = (char **) palloc(nfields * sizeof(char *));

	/* Initialize state variables */
	cstate->fe_eof = false;
	cstate->eol_type = EOL_UNKNOWN;
	cstate->cur_relname = RelationGetRelationName(cstate->rel);
	cstate->cur_lineno = 0;
	cstate->cur_attname = NULL;
	cstate->cur_attval = NULL;

	bistate = GetBulkInsertState();

	/* Set up callback to identify error line number */
	errcontext.callback = copy_in_error_callback;
	errcontext.arg = (void *) cstate;
	errcontext.previous = error_context_stack;
	error_context_stack = &errcontext;

	/* on input just throw the header line away */
	if (cstate->header_line)
	{
		cstate->cur_lineno++;
		done = CopyReadLine(cstate);
	}

	while (!done)
	{
		bool		skip_tuple;
		Oid			loaded_oid = InvalidOid;

		CHECK_FOR_INTERRUPTS();

		cstate->cur_lineno++;

		/* Reset the per-tuple exprcontext */
		ResetPerTupleExprContext(estate);

		/* Switch into its memory context */
		MemoryContextSwitchTo(GetPerTupleMemoryContext(estate));

		/* Initialize all values for row to NULL */
		MemSet(values, 0, num_phys_attrs * sizeof(Datum));
		MemSet(nulls, true, num_phys_attrs * sizeof(bool));

		if (!cstate->binary)
		{
			ListCell   *cur;
			int			fldct;
			int			fieldno;
			char	   *string;

			/* Actually read the line into memory here */
			done = CopyReadLine(cstate);

			/*
			 * EOF at start of line means we're done.  If we see EOF after
			 * some characters, we act as though it was newline followed by
			 * EOF, ie, process the line and then exit loop on next iteration.
			 */
			if (done && cstate->line_buf.len == 0)
				break;

			/* Parse the line into de-escaped field values */
			if (cstate->csv_mode)
				fldct = CopyReadAttributesCSV(cstate, nfields, field_strings);
			else
				fldct = CopyReadAttributesText(cstate, nfields, field_strings);
			fieldno = 0;

			/* Read the OID field if present */
			if (file_has_oids)
			{
				if (fieldno >= fldct)
					ereport(ERROR,
							(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
							 errmsg("missing data for OID column")));
				string = field_strings[fieldno++];

				if (string == NULL)
					ereport(ERROR,
							(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
							 errmsg("null OID in COPY data")));
				else
				{
					cstate->cur_attname = "oid";
					cstate->cur_attval = string;
					loaded_oid = DatumGetObjectId(DirectFunctionCall1(oidin,
												   CStringGetDatum(string)));
					if (loaded_oid == InvalidOid)
						ereport(ERROR,
								(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
								 errmsg("invalid OID in COPY data")));
					cstate->cur_attname = NULL;
					cstate->cur_attval = NULL;
				}
			}

			/* Loop to read the user attributes on the line. */
			foreach(cur, cstate->attnumlist)
			{
				int			attnum = lfirst_int(cur);
				int			m = attnum - 1;

				if (fieldno >= fldct)
					ereport(ERROR,
							(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
							 errmsg("missing data for column \"%s\"",
									NameStr(attr[m]->attname))));
				string = field_strings[fieldno++];

				if (cstate->csv_mode && string == NULL &&
					cstate->force_notnull_flags[m])
				{
					/* Go ahead and read the NULL string */
					string = cstate->null_print;
				}

				cstate->cur_attname = NameStr(attr[m]->attname);
				cstate->cur_attval = string;
				values[m] = InputFunctionCall(&in_functions[m],
											  string,
											  typioparams[m],
											  attr[m]->atttypmod);
				if (string != NULL)
					nulls[m] = false;
				cstate->cur_attname = NULL;
				cstate->cur_attval = NULL;
			}

			Assert(fieldno == nfields);
		}
		else
		{
			/* binary */
			int16		fld_count;
			ListCell   *cur;

			if (!CopyGetInt16(cstate, &fld_count) ||
				fld_count == -1)
			{
				done = true;
				break;
			}

			if (fld_count != attr_count)
				ereport(ERROR,
						(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
						 errmsg("row field count is %d, expected %d",
								(int) fld_count, attr_count)));

			if (file_has_oids)
			{
				cstate->cur_attname = "oid";
				loaded_oid =
					DatumGetObjectId(CopyReadBinaryAttribute(cstate,
															 0,
															 &oid_in_function,
															 oid_typioparam,
															 -1,
															 &isnull));
				if (isnull || loaded_oid == InvalidOid)
					ereport(ERROR,
							(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
							 errmsg("invalid OID in COPY data")));
				cstate->cur_attname = NULL;
			}

			i = 0;
			foreach(cur, cstate->attnumlist)
			{
				int			attnum = lfirst_int(cur);
				int			m = attnum - 1;

				cstate->cur_attname = NameStr(attr[m]->attname);
				i++;
				values[m] = CopyReadBinaryAttribute(cstate,
													i,
													&in_functions[m],
													typioparams[m],
													attr[m]->atttypmod,
													&nulls[m]);
				cstate->cur_attname = NULL;
			}
		}

		/*
		 * Now compute and insert any defaults available for the columns not
		 * provided by the input data.  Anything not processed here or above
		 * will remain NULL.
		 */
		for (i = 0; i < num_defaults; i++)
		{
			values[defmap[i]] = ExecEvalExpr(defexprs[i], econtext,
											 &nulls[defmap[i]], NULL);
		}

		/* And now we can form the input tuple. */
		tuple = heap_form_tuple(tupDesc, values, nulls);

		if (cstate->oids && file_has_oids)
			HeapTupleSetOid(tuple, loaded_oid);

		/* Triggers and stuff need to be invoked in query context. */
		MemoryContextSwitchTo(oldcontext);

		skip_tuple = false;

		/* BEFORE ROW INSERT Triggers */
		if (resultRelInfo->ri_TrigDesc &&
		  resultRelInfo->ri_TrigDesc->n_before_row[TRIGGER_EVENT_INSERT] > 0)
		{
			HeapTuple	newtuple;

			newtuple = ExecBRInsertTriggers(estate, resultRelInfo, tuple);

			if (newtuple == NULL)		/* "do nothing" */
				skip_tuple = true;
			else if (newtuple != tuple) /* modified by Trigger(s) */
			{
				heap_freetuple(tuple);
				tuple = newtuple;
			}
		}

		if (!skip_tuple)
		{
			List	   *recheckIndexes = NIL;

			/* Place tuple in tuple slot */
			ExecStoreTuple(tuple, slot, InvalidBuffer, false);

			/* Check the constraints of the tuple */
			if (cstate->rel->rd_att->constr)
				ExecConstraints(resultRelInfo, slot, estate);

			/* OK, store the tuple and create index entries for it */
			heap_insert(cstate->rel, tuple, mycid, hi_options, bistate);

			if (resultRelInfo->ri_NumIndices > 0)
				recheckIndexes = ExecInsertIndexTuples(slot, &(tuple->t_self),
													   estate);

			/* AFTER ROW INSERT Triggers */
			ExecARInsertTriggers(estate, resultRelInfo, tuple,
								 recheckIndexes);

			list_free(recheckIndexes);

			/*
			 * We count only tuples not suppressed by a BEFORE INSERT trigger;
			 * this is the same definition used by execMain.c for counting
			 * tuples inserted by an INSERT command.
			 */
			cstate->processed++;
		}
	}

	/* Done, clean up */
	error_context_stack = errcontext.previous;

	FreeBulkInsertState(bistate);

	MemoryContextSwitchTo(oldcontext);

	/*
	 * In the old protocol, tell pqcomm that we can process normal protocol
	 * messages again.
	 */
	if (cstate->copy_dest == COPY_OLD_FE)
		pq_endmsgread();

	/* Execute AFTER STATEMENT insertion triggers */
	ExecASInsertTriggers(estate, resultRelInfo);

	/* Handle queued AFTER triggers */
	AfterTriggerEndQuery(estate);

	pfree(values);
	pfree(nulls);
	pfree(field_strings);

	pfree(in_functions);
	pfree(typioparams);
	pfree(defmap);
	pfree(defexprs);

	ExecResetTupleTable(estate->es_tupleTable, false);

	ExecCloseIndices(resultRelInfo);

	FreeExecutorState(estate);

	if (!pipe)
	{
		if (FreeFile(cstate->copy_file))
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read from file \"%s\": %m",
							cstate->filename)));
	}

	/*
	 * If we skipped writing WAL, then we need to sync the heap (but not
	 * indexes since those use WAL anyway)
	 */
	if (hi_options & HEAP_INSERT_SKIP_WAL)
		heap_sync(cstate->rel);
}


/*
 * Read the next input line and stash it in line_buf, with conversion to
 * server encoding.
 *
 * Result is true if read was terminated by EOF, false if terminated
 * by newline.  The terminating newline or EOF marker is not included
 * in the final value of line_buf.
 */
static bool
CopyReadLine(CopyState cstate)
{
	bool		result;

	resetStringInfo(&cstate->line_buf);

	/* Mark that encoding conversion hasn't occurred yet */
	cstate->line_buf_converted = false;

	/* Parse data and transfer into line_buf */
	result = CopyReadLineText(cstate);

	if (result)
	{
		/*
		 * Reached EOF.  In protocol version 3, we should ignore anything
		 * after \. up to the protocol end of copy data.  (XXX maybe better
		 * not to treat \. as special?)
		 */
		if (cstate->copy_dest == COPY_NEW_FE)
		{
			do
			{
				cstate->raw_buf_index = cstate->raw_buf_len;
			} while (CopyLoadRawBuf(cstate));
		}
	}
	else
	{
		/*
		 * If we didn't hit EOF, then we must have transferred the EOL marker
		 * to line_buf along with the data.  Get rid of it.
		 */
		switch (cstate->eol_type)
		{
			case EOL_NL:
				Assert(cstate->line_buf.len >= 1);
				Assert(cstate->line_buf.data[cstate->line_buf.len - 1] == '\n');
				cstate->line_buf.len--;
				cstate->line_buf.data[cstate->line_buf.len] = '\0';
				break;
			case EOL_CR:
				Assert(cstate->line_buf.len >= 1);
				Assert(cstate->line_buf.data[cstate->line_buf.len - 1] == '\r');
				cstate->line_buf.len--;
				cstate->line_buf.data[cstate->line_buf.len] = '\0';
				break;
			case EOL_CRNL:
				Assert(cstate->line_buf.len >= 2);
				Assert(cstate->line_buf.data[cstate->line_buf.len - 2] == '\r');
				Assert(cstate->line_buf.data[cstate->line_buf.len - 1] == '\n');
				cstate->line_buf.len -= 2;
				cstate->line_buf.data[cstate->line_buf.len] = '\0';
				break;
			case EOL_UNKNOWN:
				/* shouldn't get here */
				Assert(false);
				break;
		}
	}

	/* Done reading the line.  Convert it to server encoding. */
	if (cstate->need_transcoding)
	{
		char	   *cvt;

		cvt = pg_client_to_server(cstate->line_buf.data,
								  cstate->line_buf.len);
		if (cvt != cstate->line_buf.data)
		{
			/* transfer converted data back to line_buf */
			resetStringInfo(&cstate->line_buf);
			appendBinaryStringInfo(&cstate->line_buf, cvt, strlen(cvt));
			pfree(cvt);
		}
	}

	/* Now it's safe to use the buffer in error messages */
	cstate->line_buf_converted = true;

	return result;
}

/*
 * CopyReadLineText - inner loop of CopyReadLine for text mode
 */
static bool
CopyReadLineText(CopyState cstate)
{
	char	   *copy_raw_buf;
	int			raw_buf_ptr;
	int			copy_buf_len;
	bool		need_data = false;
	bool		hit_eof = false;
	bool		result = false;
	char		mblen_str[2];

	/* CSV variables */
	bool		first_char_in_line = true;
	bool		in_quote = false,
				last_was_esc = false;
	char		quotec = '\0';
	char		escapec = '\0';

	if (cstate->csv_mode)
	{
		quotec = cstate->quote[0];
		escapec = cstate->escape[0];
		/* ignore special escape processing if it's the same as quotec */
		if (quotec == escapec)
			escapec = '\0';
	}

	mblen_str[1] = '\0';

	/*
	 * The objective of this loop is to transfer the entire next input line
	 * into line_buf.  Hence, we only care for detecting newlines (\r and/or
	 * \n) and the end-of-copy marker (\.).
	 *
	 * In CSV mode, \r and \n inside a quoted field are just part of the data
	 * value and are put in line_buf.  We keep just enough state to know if we
	 * are currently in a quoted field or not.
	 *
	 * These four characters, and the CSV escape and quote characters, are
	 * assumed the same in frontend and backend encodings.
	 *
	 * For speed, we try to move data from raw_buf to line_buf in chunks
	 * rather than one character at a time.  raw_buf_ptr points to the next
	 * character to examine; any characters from raw_buf_index to raw_buf_ptr
	 * have been determined to be part of the line, but not yet transferred to
	 * line_buf.
	 *
	 * For a little extra speed within the loop, we copy raw_buf and
	 * raw_buf_len into local variables.
	 */
	copy_raw_buf = cstate->raw_buf;
	raw_buf_ptr = cstate->raw_buf_index;
	copy_buf_len = cstate->raw_buf_len;

	for (;;)
	{
		int			prev_raw_ptr;
		char		c;

		/*
		 * Load more data if needed.  Ideally we would just force four bytes
		 * of read-ahead and avoid the many calls to
		 * IF_NEED_REFILL_AND_NOT_EOF_CONTINUE(), but the COPY_OLD_FE protocol
		 * does not allow us to read too far ahead or we might read into the
		 * next data, so we read-ahead only as far we know we can.  One
		 * optimization would be to read-ahead four byte here if
		 * cstate->copy_dest != COPY_OLD_FE, but it hardly seems worth it,
		 * considering the size of the buffer.
		 */
		if (raw_buf_ptr >= copy_buf_len || need_data)
		{
			REFILL_LINEBUF;

			/*
			 * Try to read some more data.  This will certainly reset
			 * raw_buf_index to zero, and raw_buf_ptr must go with it.
			 */
			if (!CopyLoadRawBuf(cstate))
				hit_eof = true;
			raw_buf_ptr = 0;
			copy_buf_len = cstate->raw_buf_len;

			/*
			 * If we are completely out of data, break out of the loop,
			 * reporting EOF.
			 */
			if (copy_buf_len <= 0)
			{
				result = true;
				break;
			}
			need_data = false;
		}

		/* OK to fetch a character */
		prev_raw_ptr = raw_buf_ptr;
		c = copy_raw_buf[raw_buf_ptr++];

		if (cstate->csv_mode)
		{
			/*
			 * If character is '\\' or '\r', we may need to look ahead below.
			 * Force fetch of the next character if we don't already have it.
			 * We need to do this before changing CSV state, in case one of
			 * these characters is also the quote or escape character.
			 *
			 * Note: old-protocol does not like forced prefetch, but it's OK
			 * here since we cannot validly be at EOF.
			 */
			if (c == '\\' || c == '\r')
			{
				IF_NEED_REFILL_AND_NOT_EOF_CONTINUE(0);
			}

			/*
			 * Dealing with quotes and escapes here is mildly tricky. If the
			 * quote char is also the escape char, there's no problem - we
			 * just use the char as a toggle. If they are different, we need
			 * to ensure that we only take account of an escape inside a
			 * quoted field and immediately preceding a quote char, and not
			 * the second in a escape-escape sequence.
			 */
			if (in_quote && c == escapec)
				last_was_esc = !last_was_esc;
			if (c == quotec && !last_was_esc)
				in_quote = !in_quote;
			if (c != escapec)
				last_was_esc = false;

			/*
			 * Updating the line count for embedded CR and/or LF chars is
			 * necessarily a little fragile - this test is probably about the
			 * best we can do.  (XXX it's arguable whether we should do this
			 * at all --- is cur_lineno a physical or logical count?)
			 */
			if (in_quote && c == (cstate->eol_type == EOL_NL ? '\n' : '\r'))
				cstate->cur_lineno++;
		}

		/* Process \r */
		if (c == '\r' && (!cstate->csv_mode || !in_quote))
		{
			/* Check for \r\n on first line, _and_ handle \r\n. */
			if (cstate->eol_type == EOL_UNKNOWN ||
				cstate->eol_type == EOL_CRNL)
			{
				/*
				 * If need more data, go back to loop top to load it.
				 *
				 * Note that if we are at EOF, c will wind up as '\0' because
				 * of the guaranteed pad of raw_buf.
				 */
				IF_NEED_REFILL_AND_NOT_EOF_CONTINUE(0);

				/* get next char */
				c = copy_raw_buf[raw_buf_ptr];

				if (c == '\n')
				{
					raw_buf_ptr++;		/* eat newline */
					cstate->eol_type = EOL_CRNL;		/* in case not set yet */
				}
				else
				{
					/* found \r, but no \n */
					if (cstate->eol_type == EOL_CRNL)
						ereport(ERROR,
								(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
								 !cstate->csv_mode ?
							errmsg("literal carriage return found in data") :
							errmsg("unquoted carriage return found in data"),
								 !cstate->csv_mode ?
						errhint("Use \"\\r\" to represent carriage return.") :
								 errhint("Use quoted CSV field to represent carriage return.")));

					/*
					 * if we got here, it is the first line and we didn't find
					 * \n, so don't consume the peeked character
					 */
					cstate->eol_type = EOL_CR;
				}
			}
			else if (cstate->eol_type == EOL_NL)
				ereport(ERROR,
						(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
						 !cstate->csv_mode ?
						 errmsg("literal carriage return found in data") :
						 errmsg("unquoted carriage return found in data"),
						 !cstate->csv_mode ?
					   errhint("Use \"\\r\" to represent carriage return.") :
						 errhint("Use quoted CSV field to represent carriage return.")));
			/* If reach here, we have found the line terminator */
			break;
		}

		/* Process \n */
		if (c == '\n' && (!cstate->csv_mode || !in_quote))
		{
			if (cstate->eol_type == EOL_CR || cstate->eol_type == EOL_CRNL)
				ereport(ERROR,
						(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
						 !cstate->csv_mode ?
						 errmsg("literal newline found in data") :
						 errmsg("unquoted newline found in data"),
						 !cstate->csv_mode ?
						 errhint("Use \"\\n\" to represent newline.") :
					 errhint("Use quoted CSV field to represent newline.")));
			cstate->eol_type = EOL_NL;	/* in case not set yet */
			/* If reach here, we have found the line terminator */
			break;
		}

		/*
		 * In CSV mode, we only recognize \. alone on a line.  This is because
		 * \. is a valid CSV data value.
		 */
		if (c == '\\' && (!cstate->csv_mode || first_char_in_line))
		{
			char		c2;

			IF_NEED_REFILL_AND_NOT_EOF_CONTINUE(0);
			IF_NEED_REFILL_AND_EOF_BREAK(0);

			/* -----
			 * get next character
			 * Note: we do not change c so if it isn't \., we can fall
			 * through and continue processing for client encoding.
			 * -----
			 */
			c2 = copy_raw_buf[raw_buf_ptr];

			if (c2 == '.')
			{
				raw_buf_ptr++;	/* consume the '.' */

				/*
				 * Note: if we loop back for more data here, it does not
				 * matter that the CSV state change checks are re-executed; we
				 * will come back here with no important state changed.
				 */
				if (cstate->eol_type == EOL_CRNL)
				{
					/* Get the next character */
					IF_NEED_REFILL_AND_NOT_EOF_CONTINUE(0);
					/* if hit_eof, c2 will become '\0' */
					c2 = copy_raw_buf[raw_buf_ptr++];

					if (c2 == '\n')
					{
						if (!cstate->csv_mode)
							ereport(ERROR,
									(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
									 errmsg("end-of-copy marker does not match previous newline style")));
						else
							NO_END_OF_COPY_GOTO;
					}
					else if (c2 != '\r')
					{
						if (!cstate->csv_mode)
							ereport(ERROR,
									(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
									 errmsg("end-of-copy marker corrupt")));
						else
							NO_END_OF_COPY_GOTO;
					}
				}

				/* Get the next character */
				IF_NEED_REFILL_AND_NOT_EOF_CONTINUE(0);
				/* if hit_eof, c2 will become '\0' */
				c2 = copy_raw_buf[raw_buf_ptr++];

				if (c2 != '\r' && c2 != '\n')
				{
					if (!cstate->csv_mode)
						ereport(ERROR,
								(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
								 errmsg("end-of-copy marker corrupt")));
					else
						NO_END_OF_COPY_GOTO;
				}

				if ((cstate->eol_type == EOL_NL && c2 != '\n') ||
					(cstate->eol_type == EOL_CRNL && c2 != '\n') ||
					(cstate->eol_type == EOL_CR && c2 != '\r'))
				{
					ereport(ERROR,
							(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
							 errmsg("end-of-copy marker does not match previous newline style")));
				}

				/*
				 * Transfer only the data before the \. into line_buf, then
				 * discard the data and the \. sequence.
				 */
				if (prev_raw_ptr > cstate->raw_buf_index)
					appendBinaryStringInfo(&cstate->line_buf,
									 cstate->raw_buf + cstate->raw_buf_index,
									   prev_raw_ptr - cstate->raw_buf_index);
				cstate->raw_buf_index = raw_buf_ptr;
				result = true;	/* report EOF */
				break;
			}
			else if (!cstate->csv_mode)

				/*
				 * If we are here, it means we found a backslash followed by
				 * something other than a period.  In non-CSV mode, anything
				 * after a backslash is special, so we skip over that second
				 * character too.  If we didn't do that \\. would be
				 * considered an eof-of copy, while in non-CSV mode it is a
				 * literal backslash followed by a period.  In CSV mode,
				 * backslashes are not special, so we want to process the
				 * character after the backslash just like a normal character,
				 * so we don't increment in those cases.
				 */
				raw_buf_ptr++;
		}

		/*
		 * This label is for CSV cases where \. appears at the start of a
		 * line, but there is more text after it, meaning it was a data value.
		 * We are more strict for \. in CSV mode because \. could be a data
		 * value, while in non-CSV mode, \. cannot be a data value.
		 */
not_end_of_copy:

		/*
		 * Process all bytes of a multi-byte character as a group.
		 *
		 * We only support multi-byte sequences where the first byte has the
		 * high-bit set, so as an optimization we can avoid this block
		 * entirely if it is not set.
		 */
		if (cstate->encoding_embeds_ascii && IS_HIGHBIT_SET(c))
		{
			int			mblen;

			mblen_str[0] = c;
			/* All our encodings only read the first byte to get the length */
			mblen = pg_encoding_mblen(cstate->client_encoding, mblen_str);
			IF_NEED_REFILL_AND_NOT_EOF_CONTINUE(mblen - 1);
			IF_NEED_REFILL_AND_EOF_BREAK(mblen - 1);
			raw_buf_ptr += mblen - 1;
		}
		first_char_in_line = false;
	}							/* end of outer loop */

	/*
	 * Transfer any still-uncopied data to line_buf.
	 */
	REFILL_LINEBUF;

	return result;
}

/*
 *	Return decimal value for a hexadecimal digit
 */
static int
GetDecimalFromHex(char hex)
{
	if (isdigit((unsigned char) hex))
		return hex - '0';
	else
		return tolower((unsigned char) hex) - 'a' + 10;
}

/*
 * Parse the current line into separate attributes (fields),
 * performing de-escaping as needed.
 *
 * The input is in line_buf.  We use attribute_buf to hold the result
 * strings.  fieldvals[k] is set to point to the k'th attribute string,
 * or NULL when the input matches the null marker string.  (Note that the
 * caller cannot check for nulls since the returned string would be the
 * post-de-escaping equivalent, which may look the same as some valid data
 * string.)
 *
 * delim is the column delimiter string (must be just one byte for now).
 * null_print is the null marker string.  Note that this is compared to
 * the pre-de-escaped input string.
 *
 * The return value is the number of fields actually read.  (We error out
 * if this would exceed maxfields, which is the length of fieldvals[].)
 */
static int
CopyReadAttributesText(CopyState cstate, int maxfields, char **fieldvals)
{
	char		delimc = cstate->delim[0];
	int			fieldno;
	char	   *output_ptr;
	char	   *cur_ptr;
	char	   *line_end_ptr;

	/*
	 * We need a special case for zero-column tables: check that the input
	 * line is empty, and return.
	 */
	if (maxfields <= 0)
	{
		if (cstate->line_buf.len != 0)
			ereport(ERROR,
					(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
					 errmsg("extra data after last expected column")));
		return 0;
	}

	resetStringInfo(&cstate->attribute_buf);

	/*
	 * The de-escaped attributes will certainly not be longer than the input
	 * data line, so we can just force attribute_buf to be large enough and
	 * then transfer data without any checks for enough space.  We need to do
	 * it this way because enlarging attribute_buf mid-stream would invalidate
	 * pointers already stored into fieldvals[].
	 */
	if (cstate->attribute_buf.maxlen <= cstate->line_buf.len)
		enlargeStringInfo(&cstate->attribute_buf, cstate->line_buf.len);
	output_ptr = cstate->attribute_buf.data;

	/* set pointer variables for loop */
	cur_ptr = cstate->line_buf.data;
	line_end_ptr = cstate->line_buf.data + cstate->line_buf.len;

	/* Outer loop iterates over fields */
	fieldno = 0;
	for (;;)
	{
		bool		found_delim = false;
		char	   *start_ptr;
		char	   *end_ptr;
		int			input_len;
		bool		saw_non_ascii = false;

		/* Make sure space remains in fieldvals[] */
		if (fieldno >= maxfields)
			ereport(ERROR,
					(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
					 errmsg("extra data after last expected column")));

		/* Remember start of field on both input and output sides */
		start_ptr = cur_ptr;
		fieldvals[fieldno] = output_ptr;

		/*
		 * Scan data for field.
		 *
		 * Note that in this loop, we are scanning to locate the end of field
		 * and also speculatively performing de-escaping.  Once we find the
		 * end-of-field, we can match the raw field contents against the null
		 * marker string.  Only after that comparison fails do we know that
		 * de-escaping is actually the right thing to do; therefore we *must
		 * not* throw any syntax errors before we've done the null-marker
		 * check.
		 */
		for (;;)
		{
			char		c;

			end_ptr = cur_ptr;
			if (cur_ptr >= line_end_ptr)
				break;
			c = *cur_ptr++;
			if (c == delimc)
			{
				found_delim = true;
				break;
			}
			if (c == '\\')
			{
				if (cur_ptr >= line_end_ptr)
					break;
				c = *cur_ptr++;
				switch (c)
				{
					case '0':
					case '1':
					case '2':
					case '3':
					case '4':
					case '5':
					case '6':
					case '7':
						{
							/* handle \013 */
							int			val;

							val = OCTVALUE(c);
							if (cur_ptr < line_end_ptr)
							{
								c = *cur_ptr;
								if (ISOCTAL(c))
								{
									cur_ptr++;
									val = (val << 3) + OCTVALUE(c);
									if (cur_ptr < line_end_ptr)
									{
										c = *cur_ptr;
										if (ISOCTAL(c))
										{
											cur_ptr++;
											val = (val << 3) + OCTVALUE(c);
										}
									}
								}
							}
							c = val & 0377;
							if (c == '\0' || IS_HIGHBIT_SET(c))
								saw_non_ascii = true;
						}
						break;
					case 'x':
						/* Handle \x3F */
						if (cur_ptr < line_end_ptr)
						{
							char		hexchar = *cur_ptr;

							if (isxdigit((unsigned char) hexchar))
							{
								int			val = GetDecimalFromHex(hexchar);

								cur_ptr++;
								if (cur_ptr < line_end_ptr)
								{
									hexchar = *cur_ptr;
									if (isxdigit((unsigned char) hexchar))
									{
										cur_ptr++;
										val = (val << 4) + GetDecimalFromHex(hexchar);
									}
								}
								c = val & 0xff;
								if (c == '\0' || IS_HIGHBIT_SET(c))
									saw_non_ascii = true;
							}
						}
						break;
					case 'b':
						c = '\b';
						break;
					case 'f':
						c = '\f';
						break;
					case 'n':
						c = '\n';
						break;
					case 'r':
						c = '\r';
						break;
					case 't':
						c = '\t';
						break;
					case 'v':
						c = '\v';
						break;

						/*
						 * in all other cases, take the char after '\'
						 * literally
						 */
				}
			}

			/* Add c to output string */
			*output_ptr++ = c;
		}

		/* Check whether raw input matched null marker */
		input_len = end_ptr - start_ptr;
		if (input_len == cstate->null_print_len &&
			strncmp(start_ptr, cstate->null_print, input_len) == 0)
			fieldvals[fieldno] = NULL;
		else
		{
			/*
			 * At this point we know the field is supposed to contain data.
			 *
			 * If we de-escaped any non-7-bit-ASCII chars, make sure the
			 * resulting string is valid data for the db encoding.
			 */
			if (saw_non_ascii)
			{
				char	   *fld = fieldvals[fieldno];

				pg_verifymbstr(fld, output_ptr - fld, false);
			}
		}

		/* Terminate attribute value in output area */
		*output_ptr++ = '\0';

		fieldno++;
		/* Done if we hit EOL instead of a delim */
		if (!found_delim)
			break;
	}

	/* Clean up state of attribute_buf */
	output_ptr--;
	Assert(*output_ptr == '\0');
	cstate->attribute_buf.len = (output_ptr - cstate->attribute_buf.data);

	return fieldno;
}

/*
 * Parse the current line into separate attributes (fields),
 * performing de-escaping as needed.  This has exactly the same API as
 * CopyReadAttributesText, except we parse the fields according to
 * "standard" (i.e. common) CSV usage.
 */
static int
CopyReadAttributesCSV(CopyState cstate, int maxfields, char **fieldvals)
{
	char		delimc = cstate->delim[0];
	char		quotec = cstate->quote[0];
	char		escapec = cstate->escape[0];
	int			fieldno;
	char	   *output_ptr;
	char	   *cur_ptr;
	char	   *line_end_ptr;

	/*
	 * We need a special case for zero-column tables: check that the input
	 * line is empty, and return.
	 */
	if (maxfields <= 0)
	{
		if (cstate->line_buf.len != 0)
			ereport(ERROR,
					(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
					 errmsg("extra data after last expected column")));
		return 0;
	}

	resetStringInfo(&cstate->attribute_buf);

	/*
	 * The de-escaped attributes will certainly not be longer than the input
	 * data line, so we can just force attribute_buf to be large enough and
	 * then transfer data without any checks for enough space.  We need to do
	 * it this way because enlarging attribute_buf mid-stream would invalidate
	 * pointers already stored into fieldvals[].
	 */
	if (cstate->attribute_buf.maxlen <= cstate->line_buf.len)
		enlargeStringInfo(&cstate->attribute_buf, cstate->line_buf.len);
	output_ptr = cstate->attribute_buf.data;

	/* set pointer variables for loop */
	cur_ptr = cstate->line_buf.data;
	line_end_ptr = cstate->line_buf.data + cstate->line_buf.len;

	/* Outer loop iterates over fields */
	fieldno = 0;
	for (;;)
	{
		bool		found_delim = false;
		bool		saw_quote = false;
		char	   *start_ptr;
		char	   *end_ptr;
		int			input_len;

		/* Make sure space remains in fieldvals[] */
		if (fieldno >= maxfields)
			ereport(ERROR,
					(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
					 errmsg("extra data after last expected column")));

		/* Remember start of field on both input and output sides */
		start_ptr = cur_ptr;
		fieldvals[fieldno] = output_ptr;

		/*
		 * Scan data for field,
		 *
		 * The loop starts in "not quote" mode and then toggles between that
		 * and "in quote" mode. The loop exits normally if it is in "not
		 * quote" mode and a delimiter or line end is seen.
		 */
		for (;;)
		{
			char		c;

			/* Not in quote */
			for (;;)
			{
				end_ptr = cur_ptr;
				if (cur_ptr >= line_end_ptr)
					goto endfield;
				c = *cur_ptr++;
				/* unquoted field delimiter */
				if (c == delimc)
				{
					found_delim = true;
					goto endfield;
				}
				/* start of quoted field (or part of field) */
				if (c == quotec)
				{
					saw_quote = true;
					break;
				}
				/* Add c to output string */
				*output_ptr++ = c;
			}

			/* In quote */
			for (;;)
			{
				end_ptr = cur_ptr;
				if (cur_ptr >= line_end_ptr)
					ereport(ERROR,
							(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
							 errmsg("unterminated CSV quoted field")));

				c = *cur_ptr++;

				/* escape within a quoted field */
				if (c == escapec)
				{
					/*
					 * peek at the next char if available, and escape it if it
					 * is an escape char or a quote char
					 */
					if (cur_ptr < line_end_ptr)
					{
						char		nextc = *cur_ptr;

						if (nextc == escapec || nextc == quotec)
						{
							*output_ptr++ = nextc;
							cur_ptr++;
							continue;
						}
					}
				}

				/*
				 * end of quoted field. Must do this test after testing for
				 * escape in case quote char and escape char are the same
				 * (which is the common case).
				 */
				if (c == quotec)
					break;

				/* Add c to output string */
				*output_ptr++ = c;
			}
		}
endfield:

		/* Terminate attribute value in output area */
		*output_ptr++ = '\0';

		/* Check whether raw input matched null marker */
		input_len = end_ptr - start_ptr;
		if (!saw_quote && input_len == cstate->null_print_len &&
			strncmp(start_ptr, cstate->null_print, input_len) == 0)
			fieldvals[fieldno] = NULL;

		fieldno++;
		/* Done if we hit EOL instead of a delim */
		if (!found_delim)
			break;
	}

	/* Clean up state of attribute_buf */
	output_ptr--;
	Assert(*output_ptr == '\0');
	cstate->attribute_buf.len = (output_ptr - cstate->attribute_buf.data);

	return fieldno;
}


/*
 * Read a binary attribute
 */
static Datum
CopyReadBinaryAttribute(CopyState cstate,
						int column_no, FmgrInfo *flinfo,
						Oid typioparam, int32 typmod,
						bool *isnull)
{
	int32		fld_size;
	Datum		result;

	if (!CopyGetInt32(cstate, &fld_size))
		ereport(ERROR,
				(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
				 errmsg("unexpected EOF in COPY data")));
	if (fld_size == -1)
	{
		*isnull = true;
		return ReceiveFunctionCall(flinfo, NULL, typioparam, typmod);
	}
	if (fld_size < 0)
		ereport(ERROR,
				(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
				 errmsg("invalid field size")));

	/* reset attribute_buf to empty, and load raw data in it */
	resetStringInfo(&cstate->attribute_buf);

	enlargeStringInfo(&cstate->attribute_buf, fld_size);
	if (CopyGetData(cstate, cstate->attribute_buf.data,
					fld_size, fld_size) != fld_size)
		ereport(ERROR,
				(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
				 errmsg("unexpected EOF in COPY data")));

	cstate->attribute_buf.len = fld_size;
	cstate->attribute_buf.data[fld_size] = '\0';

	/* Call the column type's binary input converter */
	result = ReceiveFunctionCall(flinfo, &cstate->attribute_buf,
								 typioparam, typmod);

	/* Trouble if it didn't eat the whole buffer */
	if (cstate->attribute_buf.cursor != cstate->attribute_buf.len)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
				 errmsg("incorrect binary data format")));

	*isnull = false;
	return result;
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
CopyAttributeOutText(CopyState cstate, char *string)
{
	char	   *ptr;
	char	   *start;
	char		c;
	char		delimc = cstate->delim[0];

	if (cstate->need_transcoding)
		ptr = pg_server_to_client(string, strlen(string));
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
						continue;		/* fall to end of loop */
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
				ptr += pg_encoding_mblen(cstate->client_encoding, ptr);
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
						continue;		/* fall to end of loop */
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
CopyAttributeOutCSV(CopyState cstate, char *string,
					bool use_quote, bool single_attr)
{
	char	   *ptr;
	char	   *start;
	char		c;
	char		delimc = cstate->delim[0];
	char		quotec = cstate->quote[0];
	char		escapec = cstate->escape[0];

	/* force quoting if it matches null_print (before conversion!) */
	if (!use_quote && strcmp(string, cstate->null_print) == 0)
		use_quote = true;

	if (cstate->need_transcoding)
		ptr = pg_server_to_client(string, strlen(string));
	else
		ptr = string;

	/*
	 * Make a preliminary pass to discover if it needs quoting
	 */
	if (!use_quote)
	{
		/*
		 * Because '\.' can be a data value, quote it if it appears alone on a
		 * line so it is not interpreted as the end-of-data marker.
		 */
		if (single_attr && strcmp(ptr, "\\.") == 0)
			use_quote = true;
		else
		{
			char	   *tptr = ptr;

			while ((c = *tptr) != '\0')
			{
				if (c == delimc || c == quotec || c == '\n' || c == '\r')
				{
					use_quote = true;
					break;
				}
				if (IS_HIGHBIT_SET(c) && cstate->encoding_embeds_ascii)
					tptr += pg_encoding_mblen(cstate->client_encoding, tptr);
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
				ptr += pg_encoding_mblen(cstate->client_encoding, ptr);
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
 * CopyGetAttnums - build an integer list of attnums to be copied
 *
 * The input attnamelist is either the user-specified column list,
 * or NIL if there was none (in which case we want all the non-dropped
 * columns).
 *
 * rel can be NULL ... it's only used for error reports.
 */
static List *
CopyGetAttnums(TupleDesc tupDesc, Relation rel, List *attnamelist)
{
	List	   *attnums = NIL;

	if (attnamelist == NIL)
	{
		/* Generate default column list */
		Form_pg_attribute *attr = tupDesc->attrs;
		int			attr_count = tupDesc->natts;
		int			i;

		for (i = 0; i < attr_count; i++)
		{
			if (attr[i]->attisdropped)
				continue;
			attnums = lappend_int(attnums, i + 1);
		}
	}
	else
	{
		/* Validate the user-supplied list and extract attnums */
		ListCell   *l;

		foreach(l, attnamelist)
		{
			char	   *name = strVal(lfirst(l));
			int			attnum;
			int			i;

			/* Lookup column name */
			attnum = InvalidAttrNumber;
			for (i = 0; i < tupDesc->natts; i++)
			{
				if (tupDesc->attrs[i]->attisdropped)
					continue;
				if (namestrcmp(&(tupDesc->attrs[i]->attname), name) == 0)
				{
					attnum = tupDesc->attrs[i]->attnum;
					break;
				}
			}
			if (attnum == InvalidAttrNumber)
			{
				if (rel != NULL)
					ereport(ERROR,
							(errcode(ERRCODE_UNDEFINED_COLUMN),
					errmsg("column \"%s\" of relation \"%s\" does not exist",
						   name, RelationGetRelationName(rel))));
				else
					ereport(ERROR,
							(errcode(ERRCODE_UNDEFINED_COLUMN),
							 errmsg("column \"%s\" does not exist",
									name)));
			}
			/* Check for duplicates */
			if (list_member_int(attnums, attnum))
				ereport(ERROR,
						(errcode(ERRCODE_DUPLICATE_COLUMN),
						 errmsg("column \"%s\" specified more than once",
								name)));
			attnums = lappend_int(attnums, attnum);
		}
	}

	return attnums;
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
static void
copy_dest_receive(TupleTableSlot *slot, DestReceiver *self)
{
	DR_copy    *myState = (DR_copy *) self;
	CopyState	cstate = myState->cstate;

	/* Make sure the tuple is fully deconstructed */
	slot_getallattrs(slot);

	/* And send the data */
	CopyOneRowTo(cstate, InvalidOid, slot->tts_values, slot->tts_isnull);
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

	return (DestReceiver *) self;
}
