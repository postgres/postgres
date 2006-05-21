/*-------------------------------------------------------------------------
 *
 * copy.c
 *		Implements the COPY utility command.
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/commands/copy.c,v 1.254.2.4 2006/05/21 20:05:48 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>
#include <unistd.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "access/genam.h"
#include "access/heapam.h"
#include "access/printtup.h"
#include "catalog/index.h"
#include "catalog/namespace.h"
#include "catalog/pg_index.h"
#include "catalog/pg_type.h"
#include "commands/copy.h"
#include "commands/trigger.h"
#include "executor/executor.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "parser/parse_coerce.h"
#include "parser/parse_relation.h"
#include "rewrite/rewriteHandler.h"
#include "storage/fd.h"
#include "tcop/pquery.h"
#include "tcop/tcopprot.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/relcache.h"
#include "utils/syscache.h"


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
 * operation.  For simplicity, we use the same struct for all variants
 * of COPY, even though some fields are used in only some cases.
 *
 * A word about encoding considerations: encodings that are only supported on
 * the client side are those where multibyte characters may have second or
 * later bytes with the high bit not set.  When scanning data in such an
 * encoding to look for a match to a single-byte (ie ASCII) character,
 * we must use the full pg_encoding_mblen() machinery to skip over
 * multibyte characters, else we might find a false match to a trailing
 * byte.  In supported server encodings, there is no possibility of
 * a false match, and it's faster to make useless comparisons to trailing
 * bytes than it is to invoke pg_encoding_mblen() to skip over them.
 * client_only_encoding is TRUE when we have to do it the hard way.
 */
typedef struct CopyStateData
{
	/* low-level state data */
	CopyDest	copy_dest;		/* type of copy source/destination */
	FILE	   *copy_file;		/* used if copy_dest == COPY_FILE */
	StringInfo	fe_msgbuf;		/* used if copy_dest == COPY_NEW_FE */
	bool		fe_copy;		/* true for all FE copy dests */
	bool		fe_eof;			/* true if detected end of copy data */
	EolType		eol_type;		/* EOL type of input */
	int			client_encoding;	/* remote side's character encoding */
	bool		need_transcoding;		/* client encoding diff from server? */
	bool		client_only_encoding;	/* encoding not valid on server? */

	/* parameters from the COPY command */
	Relation	rel;			/* relation to copy to or from */
	List	   *attnumlist;		/* integer list of attnums to copy */
	bool		binary;			/* binary format? */
	bool		oids;			/* include OIDs? */
	bool		csv_mode;		/* Comma Separated Value format? */
	bool		header_line;	/* CSV header line? */
	char	   *null_print;		/* NULL marker string (server encoding!) */
	int			null_print_len; /* length of same */
	char	   *delim;			/* column delimiter (must be 1 byte) */
	char	   *quote;			/* CSV quote char (must be 1 byte) */
	char	   *escape;			/* CSV escape char (must be 1 byte) */
	List	   *force_quote_atts;		/* integer list of attnums to FQ */
	List	   *force_notnull_atts;		/* integer list of attnums to FNN */

	/* these are just for error messages, see copy_in_error_callback */
	const char *cur_relname;	/* table name for error messages */
	int			cur_lineno;		/* line number for error messages */
	const char *cur_attname;	/* current att for error messages */
	const char *cur_attval;		/* current att value for error messages */

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
	 * client connection).	CopyReadLine parses this data sufficiently to
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


static const char BinarySignature[11] = "PGCOPY\n\377\r\n\0";


/* non-export function prototypes */
static void DoCopyTo(CopyState cstate);
static void CopyTo(CopyState cstate);
static void CopyFrom(CopyState cstate);
static bool CopyReadLine(CopyState cstate);
static bool CopyReadLineText(CopyState cstate);
static bool CopyReadLineCSV(CopyState cstate);
static int CopyReadAttributesText(CopyState cstate, int maxfields,
					   char **fieldvals);
static int CopyReadAttributesCSV(CopyState cstate, int maxfields,
					  char **fieldvals);
static Datum CopyReadBinaryAttribute(CopyState cstate,
						int column_no, FmgrInfo *flinfo,
						Oid typioparam, int32 typmod,
						bool *isnull);
static void CopyAttributeOutText(CopyState cstate, char *server_string);
static void CopyAttributeOutCSV(CopyState cstate, char *server_string,
					bool use_quote, bool single_attr);
static List *CopyGetAttnums(Relation rel, List *attnamelist);
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
		cstate->fe_msgbuf = makeStringInfo();
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
		if (cstate->binary)
		{
			/* Need to flush out file trailer word */
			CopySendEndOfRow(cstate);
		}
		else
		{
			/* Shouldn't have any unsent data */
			Assert(cstate->fe_msgbuf->len == 0);
		}
		/* Send Copy Done message */
		pq_putemptymessage('c');
	}
	else
	{
		/* The FE/BE protocol uses \n as newline for all platforms */
		CopySendData(cstate, "\\.\n", 3);
		pq_endcopyout(false);
	}
}

/*----------
 * CopySendData sends output data to the destination (file or frontend)
 * CopySendString does the same for null-terminated strings
 * CopySendChar does the same for single characters
 * CopySendEndOfRow does the appropriate thing at end of each data row
 *
 * NB: no data conversion is applied by these functions
 *----------
 */
static void
CopySendData(CopyState cstate, void *databuf, int datasize)
{
	switch (cstate->copy_dest)
	{
		case COPY_FILE:
			fwrite(databuf, datasize, 1, cstate->copy_file);
			if (ferror(cstate->copy_file))
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not write to COPY file: %m")));
			break;
		case COPY_OLD_FE:
			if (pq_putbytes((char *) databuf, datasize))
			{
				/* no hope of recovering connection sync, so FATAL */
				ereport(FATAL,
						(errcode(ERRCODE_CONNECTION_FAILURE),
						 errmsg("connection lost during COPY to stdout")));
			}
			break;
		case COPY_NEW_FE:
			appendBinaryStringInfo(cstate->fe_msgbuf,
								   (char *) databuf, datasize);
			break;
	}
}

static void
CopySendString(CopyState cstate, const char *str)
{
	CopySendData(cstate, (void *) str, strlen(str));
}

static void
CopySendChar(CopyState cstate, char c)
{
	CopySendData(cstate, &c, 1);
}

static void
CopySendEndOfRow(CopyState cstate)
{
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
			break;
		case COPY_OLD_FE:
			/* The FE/BE protocol uses \n as newline for all platforms */
			if (!cstate->binary)
				CopySendChar(cstate, '\n');
			break;
		case COPY_NEW_FE:
			/* The FE/BE protocol uses \n as newline for all platforms */
			if (!cstate->binary)
				CopySendChar(cstate, '\n');
			/* Dump the accumulated row as one CopyData message */
			(void) pq_putmessage('d', cstate->fe_msgbuf->data,
								 cstate->fe_msgbuf->len);
			/* Reset fe_msgbuf to empty */
			cstate->fe_msgbuf->len = 0;
			cstate->fe_msgbuf->data[0] = '\0';
			break;
	}
}

/*
 * CopyGetData reads data from the source (file or frontend)
 *
 * We attempt to read at least minread, and at most maxread, bytes from
 * the source.	The actual number of bytes read is returned; if this is
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
					mtype = pq_getbyte();
					if (mtype == EOF)
						ereport(ERROR,
								(errcode(ERRCODE_CONNECTION_FAILURE),
							 errmsg("unexpected EOF on client connection")));
					if (pq_getmessage(cstate->fe_msgbuf, 0))
						ereport(ERROR,
								(errcode(ERRCODE_CONNECTION_FAILURE),
							 errmsg("unexpected EOF on client connection")));
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
 *	 DoCopy executes the SQL COPY statement.
 *
 * Either unload or reload contents of table <relation>, depending on <from>.
 * (<from> = TRUE means we are inserting into the table.)
 *
 * If <pipe> is false, transfer is between the table and the file named
 * <filename>.	Otherwise, transfer is between the table and our regular
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
 * the table.
 */
void
DoCopy(const CopyStmt *stmt)
{
	CopyState	cstate;
	RangeVar   *relation = stmt->relation;
	char	   *filename = stmt->filename;
	bool		is_from = stmt->is_from;
	bool		pipe = (stmt->filename == NULL);
	List	   *attnamelist = stmt->attlist;
	List	   *force_quote = NIL;
	List	   *force_notnull = NIL;
	AclMode		required_access = (is_from ? ACL_INSERT : ACL_SELECT);
	AclResult	aclresult;
	ListCell   *option;

	/* Allocate workspace and zero all fields */
	cstate = (CopyStateData *) palloc0(sizeof(CopyStateData));

	/* Extract options from the statement node tree */
	foreach(option, stmt->options)
	{
		DefElem    *defel = (DefElem *) lfirst(option);

		if (strcmp(defel->defname, "binary") == 0)
		{
			if (cstate->binary)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			cstate->binary = intVal(defel->arg);
		}
		else if (strcmp(defel->defname, "oids") == 0)
		{
			if (cstate->oids)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			cstate->oids = intVal(defel->arg);
		}
		else if (strcmp(defel->defname, "delimiter") == 0)
		{
			if (cstate->delim)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			cstate->delim = strVal(defel->arg);
		}
		else if (strcmp(defel->defname, "null") == 0)
		{
			if (cstate->null_print)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			cstate->null_print = strVal(defel->arg);
		}
		else if (strcmp(defel->defname, "csv") == 0)
		{
			if (cstate->csv_mode)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			cstate->csv_mode = intVal(defel->arg);
		}
		else if (strcmp(defel->defname, "header") == 0)
		{
			if (cstate->header_line)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			cstate->header_line = intVal(defel->arg);
		}
		else if (strcmp(defel->defname, "quote") == 0)
		{
			if (cstate->quote)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			cstate->quote = strVal(defel->arg);
		}
		else if (strcmp(defel->defname, "escape") == 0)
		{
			if (cstate->escape)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			cstate->escape = strVal(defel->arg);
		}
		else if (strcmp(defel->defname, "force_quote") == 0)
		{
			if (force_quote)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			force_quote = (List *) defel->arg;
		}
		else if (strcmp(defel->defname, "force_notnull") == 0)
		{
			if (force_notnull)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			force_notnull = (List *) defel->arg;
		}
		else
			elog(ERROR, "option \"%s\" not recognized",
				 defel->defname);
	}

	/* Check for incompatible options */
	if (cstate->binary && cstate->delim)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("cannot specify DELIMITER in BINARY mode")));

	if (cstate->binary && cstate->csv_mode)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("cannot specify CSV in BINARY mode")));

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

	/* Only single-character delimiter strings are supported. */
	if (strlen(cstate->delim) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("COPY delimiter must be a single character")));

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
				 errmsg("COPY quote must be a single character")));

	/* Check escape */
	if (!cstate->csv_mode && cstate->escape != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("COPY escape available only in CSV mode")));

	if (cstate->csv_mode && strlen(cstate->escape) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("COPY escape must be a single character")));

	/* Check force_quote */
	if (!cstate->csv_mode && force_quote != NIL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("COPY force quote available only in CSV mode")));
	if (force_quote != NIL && is_from)
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

	/* Open and lock the relation, using the appropriate lock type. */
	cstate->rel = heap_openrv(relation,
							  (is_from ? RowExclusiveLock : AccessShareLock));

	/* check read-only transaction */
	if (XactReadOnly && is_from &&
		!isTempNamespace(RelationGetNamespace(cstate->rel)))
		ereport(ERROR,
				(errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION),
				 errmsg("transaction is read-only")));

	/* Check permissions. */
	aclresult = pg_class_aclcheck(RelationGetRelid(cstate->rel), GetUserId(),
								  required_access);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, ACL_KIND_CLASS,
					   RelationGetRelationName(cstate->rel));
	if (!pipe && !superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to COPY to or from a file"),
				 errhint("Anyone can COPY to stdout or from stdin. "
						 "psql's \\copy command also works for anyone.")));

	/* Don't allow COPY w/ OIDs to or from a table without them */
	if (cstate->oids && !cstate->rel->rd_rel->relhasoids)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("table \"%s\" does not have OIDs",
						RelationGetRelationName(cstate->rel))));

	/* Generate or convert list of attributes to process */
	cstate->attnumlist = CopyGetAttnums(cstate->rel, attnamelist);

	/* Convert FORCE QUOTE name list to column numbers, check validity */
	if (force_quote)
	{
		TupleDesc	tupDesc = RelationGetDescr(cstate->rel);
		Form_pg_attribute *attr = tupDesc->attrs;
		ListCell   *cur;

		cstate->force_quote_atts = CopyGetAttnums(cstate->rel, force_quote);

		foreach(cur, cstate->force_quote_atts)
		{
			int			attnum = lfirst_int(cur);

			if (!list_member_int(cstate->attnumlist, attnum))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
				   errmsg("FORCE QUOTE column \"%s\" not referenced by COPY",
						  NameStr(attr[attnum - 1]->attname))));
		}
	}

	/* Convert FORCE NOT NULL name list to column numbers, check validity */
	if (force_notnull)
	{
		TupleDesc	tupDesc = RelationGetDescr(cstate->rel);
		Form_pg_attribute *attr = tupDesc->attrs;
		ListCell   *cur;

		cstate->force_notnull_atts = CopyGetAttnums(cstate->rel,
													force_notnull);

		foreach(cur, cstate->force_notnull_atts)
		{
			int			attnum = lfirst_int(cur);

			if (!list_member_int(cstate->attnumlist, attnum))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
				errmsg("FORCE NOT NULL column \"%s\" not referenced by COPY",
					   NameStr(attr[attnum - 1]->attname))));
		}
	}

	/* Set up variables to avoid per-attribute overhead. */
	initStringInfo(&cstate->attribute_buf);
	initStringInfo(&cstate->line_buf);
	cstate->line_buf_converted = false;
	cstate->raw_buf = (char *) palloc(RAW_BUF_SIZE + 1);
	cstate->raw_buf_index = cstate->raw_buf_len = 0;

	/*
	 * Set up encoding conversion info.  Even if the client and server
	 * encodings are the same, we must apply pg_client_to_server() to
	 * validate data in multibyte encodings.
	 */
	cstate->client_encoding = pg_get_client_encoding();
	cstate->need_transcoding =
		(cstate->client_encoding != GetDatabaseEncoding() ||
		 pg_database_encoding_max_length() > 1);
	cstate->client_only_encoding = PG_ENCODING_IS_CLIENT_ONLY(cstate->client_encoding);

	cstate->copy_dest = COPY_FILE;		/* default */

	if (is_from)
	{							/* copy from file to database */
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

			cstate->copy_file = AllocateFile(filename, PG_BINARY_R);

			if (cstate->copy_file == NULL)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not open file \"%s\" for reading: %m",
								filename)));

			fstat(fileno(cstate->copy_file), &st);
			if (S_ISDIR(st.st_mode))
			{
				FreeFile(cstate->copy_file);
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("\"%s\" is a directory", filename)));
			}
		}

		CopyFrom(cstate);
	}
	else
	{							/* copy from database to file */
		if (cstate->rel->rd_rel->relkind != RELKIND_RELATION)
		{
			if (cstate->rel->rd_rel->relkind == RELKIND_VIEW)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("cannot copy from view \"%s\"",
								RelationGetRelationName(cstate->rel))));
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
		if (pipe)
		{
			if (whereToSendOutput == DestRemote)
				cstate->fe_copy = true;
			else
				cstate->copy_file = stdout;
		}
		else
		{
			mode_t		oumask; /* Pre-existing umask value */
			struct stat st;

			/*
			 * Prevent write to relative path ... too easy to shoot oneself in
			 * the foot by overwriting a database file ...
			 */
			if (!is_absolute_path(filename))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_NAME),
					  errmsg("relative path not allowed for COPY to file")));

			oumask = umask((mode_t) 022);
			cstate->copy_file = AllocateFile(filename, PG_BINARY_W);
			umask(oumask);

			if (cstate->copy_file == NULL)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not open file \"%s\" for writing: %m",
								filename)));

			fstat(fileno(cstate->copy_file), &st);
			if (S_ISDIR(st.st_mode))
			{
				FreeFile(cstate->copy_file);
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("\"%s\" is a directory", filename)));
			}
		}

		DoCopyTo(cstate);
	}

	if (!pipe)
	{
		/* we assume only the write case could fail here */
		if (FreeFile(cstate->copy_file))
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not write to file \"%s\": %m",
							filename)));
	}

	/*
	 * Close the relation.	If reading, we can release the AccessShareLock we
	 * got; if writing, we should hold the lock until end of transaction to
	 * ensure that updates will be committed before lock is released.
	 */
	heap_close(cstate->rel, (is_from ? NoLock : AccessShareLock));

	/* Clean up storage (probably not really necessary) */
	pfree(cstate->attribute_buf.data);
	pfree(cstate->line_buf.data);
	pfree(cstate->raw_buf);
	pfree(cstate);
}


/*
 * This intermediate routine just exists to localize the effects of setjmp
 * so we don't need to plaster a lot of variables with "volatile".
 */
static void
DoCopyTo(CopyState cstate)
{
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
}

/*
 * Copy from relation TO file.
 */
static void
CopyTo(CopyState cstate)
{
	HeapTuple	tuple;
	TupleDesc	tupDesc;
	HeapScanDesc scandesc;
	int			num_phys_attrs;
	int			attr_count;
	Form_pg_attribute *attr;
	FmgrInfo   *out_functions;
	bool	   *force_quote;
	char	   *string;
	char	   *null_print_client;
	ListCell   *cur;
	MemoryContext oldcontext;
	MemoryContext mycontext;

	tupDesc = cstate->rel->rd_att;
	attr = tupDesc->attrs;
	num_phys_attrs = tupDesc->natts;
	attr_count = list_length(cstate->attnumlist);
	null_print_client = cstate->null_print;		/* default */

	/* Get info about the columns we need to process. */
	out_functions = (FmgrInfo *) palloc(num_phys_attrs * sizeof(FmgrInfo));
	force_quote = (bool *) palloc(num_phys_attrs * sizeof(bool));
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
		fmgr_info(out_func_oid, &out_functions[attnum - 1]);

		if (list_member_int(cstate->force_quote_atts, attnum))
			force_quote[attnum - 1] = true;
		else
			force_quote[attnum - 1] = false;
	}

	/*
	 * Create a temporary memory context that we can reset once per row to
	 * recover palloc'd memory.  This avoids any problems with leaks inside
	 * datatype output routines, and should be faster than retail pfree's
	 * anyway.	(We don't need a whole econtext as CopyFrom does.)
	 */
	mycontext = AllocSetContextCreate(CurrentMemoryContext,
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
			null_print_client = pg_server_to_client(cstate->null_print,
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

	scandesc = heap_beginscan(cstate->rel, ActiveSnapshot, 0, NULL);

	while ((tuple = heap_getnext(scandesc, ForwardScanDirection)) != NULL)
	{
		bool		need_delim = false;

		CHECK_FOR_INTERRUPTS();

		MemoryContextReset(mycontext);
		oldcontext = MemoryContextSwitchTo(mycontext);

		if (cstate->binary)
		{
			/* Binary per-tuple header */
			CopySendInt16(cstate, attr_count);
			/* Send OID if wanted --- note attr_count doesn't include it */
			if (cstate->oids)
			{
				Oid			oid = HeapTupleGetOid(tuple);

				/* Hack --- assume Oid is same size as int32 */
				CopySendInt32(cstate, sizeof(int32));
				CopySendInt32(cstate, oid);
			}
		}
		else
		{
			/* Text format has no per-tuple header, but send OID if wanted */
			/* Assume digits don't need any quoting or encoding conversion */
			if (cstate->oids)
			{
				string = DatumGetCString(DirectFunctionCall1(oidout,
								  ObjectIdGetDatum(HeapTupleGetOid(tuple))));
				CopySendString(cstate, string);
				need_delim = true;
			}
		}

		foreach(cur, cstate->attnumlist)
		{
			int			attnum = lfirst_int(cur);
			Datum		value;
			bool		isnull;

			value = heap_getattr(tuple, attnum, tupDesc, &isnull);

			if (!cstate->binary)
			{
				if (need_delim)
					CopySendChar(cstate, cstate->delim[0]);
				need_delim = true;
			}

			if (isnull)
			{
				if (!cstate->binary)
					CopySendString(cstate, null_print_client);
				else
					CopySendInt32(cstate, -1);
			}
			else
			{
				if (!cstate->binary)
				{
					string = DatumGetCString(FunctionCall1(&out_functions[attnum - 1],
														   value));
					if (cstate->csv_mode)
						CopyAttributeOutCSV(cstate, string,
											force_quote[attnum - 1],
											list_length(cstate->attnumlist) == 1);
					else
						CopyAttributeOutText(cstate, string);
				}
				else
				{
					bytea	   *outputbytes;

					outputbytes = DatumGetByteaP(FunctionCall1(&out_functions[attnum - 1],
															   value));
					/* We assume the result will not have been toasted */
					CopySendInt32(cstate, VARSIZE(outputbytes) - VARHDRSZ);
					CopySendData(cstate, VARDATA(outputbytes),
								 VARSIZE(outputbytes) - VARHDRSZ);
				}
			}
		}

		CopySendEndOfRow(cstate);

		MemoryContextSwitchTo(oldcontext);
	}

	heap_endscan(scandesc);

	if (cstate->binary)
	{
		/* Generate trailer for a binary copy */
		CopySendInt16(cstate, -1);
	}

	MemoryContextDelete(mycontext);

	pfree(out_functions);
	pfree(force_quote);
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
	ExprState **constraintexprs;
	bool	   *force_notnull;
	bool		hasConstraints = false;
	int			attnum;
	int			i;
	Oid			in_func_oid;
	Datum	   *values;
	char	   *nulls;
	int			nfields;
	char	  **field_strings;
	bool		done = false;
	bool		isnull;
	ResultRelInfo *resultRelInfo;
	EState	   *estate = CreateExecutorState(); /* for ExecConstraints() */
	TupleTableSlot *slot;
	bool		file_has_oids;
	int		   *defmap;
	ExprState **defexprs;		/* array of default att expressions */
	ExprContext *econtext;		/* used for ExecEvalExpr for default atts */
	MemoryContext oldcontext = CurrentMemoryContext;
	ErrorContextCallback errcontext;

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
		resultRelInfo->ri_TrigFunctions = (FmgrInfo *)
			palloc0(resultRelInfo->ri_TrigDesc->numtriggers * sizeof(FmgrInfo));
	resultRelInfo->ri_TrigInstrument = NULL;

	ExecOpenIndices(resultRelInfo);

	estate->es_result_relations = resultRelInfo;
	estate->es_num_result_relations = 1;
	estate->es_result_relation_info = resultRelInfo;

	/* Set up a tuple slot too */
	slot = MakeSingleTupleTableSlot(tupDesc);

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
	constraintexprs = (ExprState **) palloc0(num_phys_attrs * sizeof(ExprState *));
	force_notnull = (bool *) palloc(num_phys_attrs * sizeof(bool));

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

		if (list_member_int(cstate->force_notnull_atts, attnum))
			force_notnull[attnum - 1] = true;
		else
			force_notnull[attnum - 1] = false;

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

		/* If it's a domain type, set up to check domain constraints */
		if (get_typtype(attr[attnum - 1]->atttypid) == 'd')
		{
			Param	   *prm;
			Node	   *node;

			/*
			 * Easiest way to do this is to use parse_coerce.c to set up an
			 * expression that checks the constraints.	(At present, the
			 * expression might contain a length-coercion-function call and/or
			 * CoerceToDomain nodes.)  The bottom of the expression is a Param
			 * node so that we can fill in the actual datum during the data
			 * input loop.
			 */
			prm = makeNode(Param);
			prm->paramkind = PARAM_EXEC;
			prm->paramid = 0;
			prm->paramtype = getBaseType(attr[attnum - 1]->atttypid);

			node = coerce_to_domain((Node *) prm,
									prm->paramtype,
									attr[attnum - 1]->atttypid,
									COERCE_IMPLICIT_CAST, false, false);

			constraintexprs[attnum - 1] = ExecPrepareExpr((Expr *) node,
														  estate);
			hasConstraints = true;
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
	nulls = (char *) palloc(num_phys_attrs * sizeof(char));

	/* create workspace for CopyReadAttributes results */
	nfields = file_has_oids ? (attr_count + 1) : attr_count;
	field_strings = (char **) palloc(nfields * sizeof(char *));

	/* Make room for a PARAM_EXEC value for domain constraint checks */
	if (hasConstraints)
		econtext->ecxt_param_exec_vals = (ParamExecData *)
			palloc0(sizeof(ParamExecData));

	/* Initialize state variables */
	cstate->fe_eof = false;
	cstate->eol_type = EOL_UNKNOWN;
	cstate->cur_relname = RelationGetRelationName(cstate->rel);
	cstate->cur_lineno = 0;
	cstate->cur_attname = NULL;
	cstate->cur_attval = NULL;

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
		MemSet(nulls, 'n', num_phys_attrs * sizeof(char));

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

				if (cstate->csv_mode && string == NULL && force_notnull[m])
				{
					/* Go ahead and read the NULL string */
					string = cstate->null_print;
				}

				/* If we read an SQL NULL, no need to do anything */
				if (string != NULL)
				{
					cstate->cur_attname = NameStr(attr[m]->attname);
					cstate->cur_attval = string;
					values[m] = FunctionCall3(&in_functions[m],
											  CStringGetDatum(string),
											ObjectIdGetDatum(typioparams[m]),
										  Int32GetDatum(attr[m]->atttypmod));
					nulls[m] = ' ';
					cstate->cur_attname = NULL;
					cstate->cur_attval = NULL;
				}
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
													&isnull);
				nulls[m] = isnull ? 'n' : ' ';
				cstate->cur_attname = NULL;
			}
		}

		/*
		 * Now compute and insert any defaults available for the columns not
		 * provided by the input data.	Anything not processed here or above
		 * will remain NULL.
		 */
		for (i = 0; i < num_defaults; i++)
		{
			values[defmap[i]] = ExecEvalExpr(defexprs[i], econtext,
											 &isnull, NULL);
			if (!isnull)
				nulls[defmap[i]] = ' ';
		}

		/* Next apply any domain constraints */
		if (hasConstraints)
		{
			ParamExecData *prmdata = &econtext->ecxt_param_exec_vals[0];

			for (i = 0; i < num_phys_attrs; i++)
			{
				ExprState  *exprstate = constraintexprs[i];

				if (exprstate == NULL)
					continue;	/* no constraint for this attr */

				/* Insert current row's value into the Param value */
				prmdata->value = values[i];
				prmdata->isnull = (nulls[i] == 'n');

				/*
				 * Execute the constraint expression.  Allow the expression to
				 * replace the value (consider e.g. a timestamp precision
				 * restriction).
				 */
				values[i] = ExecEvalExpr(exprstate, econtext,
										 &isnull, NULL);
				nulls[i] = isnull ? 'n' : ' ';
			}
		}

		/* And now we can form the input tuple. */
		tuple = heap_formtuple(tupDesc, values, nulls);

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
			/* Place tuple in tuple slot */
			ExecStoreTuple(tuple, slot, InvalidBuffer, false);

			/* Check the constraints of the tuple */
			if (cstate->rel->rd_att->constr)
				ExecConstraints(resultRelInfo, slot, estate);

			/* OK, store the tuple and create index entries for it */
			simple_heap_insert(cstate->rel, tuple);

			if (resultRelInfo->ri_NumIndices > 0)
				ExecInsertIndexTuples(slot, &(tuple->t_self), estate, false);

			/* AFTER ROW INSERT Triggers */
			ExecARInsertTriggers(estate, resultRelInfo, tuple);
		}
	}

	/* Done, clean up */
	error_context_stack = errcontext.previous;

	MemoryContextSwitchTo(oldcontext);

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
	pfree(constraintexprs);
	pfree(force_notnull);

	ExecDropSingleTupleTableSlot(slot);

	ExecCloseIndices(resultRelInfo);

	FreeExecutorState(estate);
}


/*
 * Read the next input line and stash it in line_buf, with conversion to
 * server encoding.
 *
 * Result is true if read was terminated by EOF, false if terminated
 * by newline.	The terminating newline or EOF marker is not included
 * in the final value of line_buf.
 */
static bool
CopyReadLine(CopyState cstate)
{
	bool		result;

	/* Reset line_buf to empty */
	cstate->line_buf.len = 0;
	cstate->line_buf.data[0] = '\0';

	/* Mark that encoding conversion hasn't occurred yet */
	cstate->line_buf_converted = false;

	/* Parse data and transfer into line_buf */
	if (cstate->csv_mode)
		result = CopyReadLineCSV(cstate);
	else
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
			cstate->line_buf.len = 0;
			cstate->line_buf.data[0] = '\0';
			appendBinaryStringInfo(&cstate->line_buf, cvt, strlen(cvt));
			pfree(cvt);
		}
	}

	/* Now it's safe to use the buffer in error messages */
	cstate->line_buf_converted = true;

	return result;
}

/*
 * CopyReadLineText - inner loop of CopyReadLine for non-CSV mode
 *
 * If you need to change this, better look at CopyReadLineCSV too
 */
static bool
CopyReadLineText(CopyState cstate)
{
	bool		result;
	char	   *copy_raw_buf;
	int			raw_buf_ptr;
	int			copy_buf_len;
	bool		need_data;
	bool		hit_eof;
	char		s[2];

	s[1] = 0;

	/* set default status */
	result = false;

	/*
	 * The objective of this loop is to transfer the entire next input line
	 * into line_buf.  Hence, we only care for detecting newlines (\r and/or
	 * \n) and the end-of-copy marker (\.).
	 *
	 * For backwards compatibility we allow backslashes to escape newline
	 * characters.	Backslashes other than the end marker get put into the
	 * line_buf, since CopyReadAttributesText does its own escape processing.
	 *
	 * These four characters, and only these four, are assumed the same in
	 * frontend and backend encodings.
	 *
	 * For speed, we try to move data to line_buf in chunks rather than one
	 * character at a time.  raw_buf_ptr points to the next character to
	 * examine; any characters from raw_buf_index to raw_buf_ptr have been
	 * determined to be part of the line, but not yet transferred to line_buf.
	 *
	 * For a little extra speed within the loop, we copy raw_buf and
	 * raw_buf_len into local variables.
	 */
	copy_raw_buf = cstate->raw_buf;
	raw_buf_ptr = cstate->raw_buf_index;
	copy_buf_len = cstate->raw_buf_len;
	need_data = false;			/* flag to force reading more data */
	hit_eof = false;			/* flag indicating no more data available */

	for (;;)
	{
		int			prev_raw_ptr;
		char		c;

		/* Load more data if needed */
		if (raw_buf_ptr >= copy_buf_len || need_data)
		{
			/*
			 * Transfer any approved data to line_buf; must do this to be sure
			 * there is some room in raw_buf.
			 */
			if (raw_buf_ptr > cstate->raw_buf_index)
			{
				appendBinaryStringInfo(&cstate->line_buf,
									 cstate->raw_buf + cstate->raw_buf_index,
									   raw_buf_ptr - cstate->raw_buf_index);
				cstate->raw_buf_index = raw_buf_ptr;
			}

			/*
			 * Try to read some more data.	This will certainly reset
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

		if (c == '\r')
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
				if (raw_buf_ptr >= copy_buf_len && !hit_eof)
				{
					raw_buf_ptr = prev_raw_ptr; /* undo fetch */
					need_data = true;
					continue;
				}
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
							 errmsg("literal carriage return found in data"),
								 errhint("Use \"\\r\" to represent carriage return.")));

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
						 errmsg("literal carriage return found in data"),
					  errhint("Use \"\\r\" to represent carriage return.")));
			/* If reach here, we have found the line terminator */
			break;
		}

		if (c == '\n')
		{
			if (cstate->eol_type == EOL_CR || cstate->eol_type == EOL_CRNL)
				ereport(ERROR,
						(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
						 errmsg("literal newline found in data"),
						 errhint("Use \"\\n\" to represent newline.")));
			cstate->eol_type = EOL_NL;	/* in case not set yet */
			/* If reach here, we have found the line terminator */
			break;
		}

		if (c == '\\')
		{
			/*
			 * If need more data, go back to loop top to load it.
			 */
			if (raw_buf_ptr >= copy_buf_len)
			{
				if (hit_eof)
				{
					/* backslash just before EOF, treat as data char */
					result = true;
					break;
				}
				raw_buf_ptr = prev_raw_ptr;		/* undo fetch */
				need_data = true;
				continue;
			}

			/*
			 * In non-CSV mode, backslash quotes the following character even
			 * if it's a newline, so we always advance to next character
			 */
			c = copy_raw_buf[raw_buf_ptr++];

			if (c == '.')
			{
				if (cstate->eol_type == EOL_CRNL)
				{
					if (raw_buf_ptr >= copy_buf_len && !hit_eof)
					{
						raw_buf_ptr = prev_raw_ptr;		/* undo fetch */
						need_data = true;
						continue;
					}
					/* if hit_eof, c will become '\0' */
					c = copy_raw_buf[raw_buf_ptr++];
					if (c == '\n')
						ereport(ERROR,
								(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
								 errmsg("end-of-copy marker does not match previous newline style")));
					if (c != '\r')
						ereport(ERROR,
								(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
								 errmsg("end-of-copy marker corrupt")));
				}
				if (raw_buf_ptr >= copy_buf_len && !hit_eof)
				{
					raw_buf_ptr = prev_raw_ptr; /* undo fetch */
					need_data = true;
					continue;
				}
				/* if hit_eof, c will become '\0' */
				c = copy_raw_buf[raw_buf_ptr++];
				if (c != '\r' && c != '\n')
					ereport(ERROR,
							(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
							 errmsg("end-of-copy marker corrupt")));
				if ((cstate->eol_type == EOL_NL && c != '\n') ||
					(cstate->eol_type == EOL_CRNL && c != '\n') ||
					(cstate->eol_type == EOL_CR && c != '\r'))
					ereport(ERROR,
							(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
							 errmsg("end-of-copy marker does not match previous newline style")));

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
		}

		/*
		 * Do we need to be careful about trailing bytes of multibyte
		 * characters?	(See note above about client_only_encoding)
		 *
		 * We assume here that pg_encoding_mblen only looks at the first byte
		 * of the character!
		 */
		if (cstate->client_only_encoding)
		{
			int			mblen;

			s[0] = c;
			mblen = pg_encoding_mblen(cstate->client_encoding, s);
			if (raw_buf_ptr + (mblen - 1) > copy_buf_len)
			{
				if (hit_eof)
				{
					/* consume the partial character (conversion will fail) */
					raw_buf_ptr = copy_buf_len;
					result = true;
					break;
				}
				raw_buf_ptr = prev_raw_ptr;		/* undo fetch */
				need_data = true;
				continue;
			}
			raw_buf_ptr += mblen - 1;
		}
	}							/* end of outer loop */

	/*
	 * Transfer any still-uncopied data to line_buf.
	 */
	if (raw_buf_ptr > cstate->raw_buf_index)
	{
		appendBinaryStringInfo(&cstate->line_buf,
							   cstate->raw_buf + cstate->raw_buf_index,
							   raw_buf_ptr - cstate->raw_buf_index);
		cstate->raw_buf_index = raw_buf_ptr;
	}

	return result;
}

/*
 * CopyReadLineCSV - inner loop of CopyReadLine for CSV mode
 *
 * If you need to change this, better look at CopyReadLineText too
 */
static bool
CopyReadLineCSV(CopyState cstate)
{
	bool		result;
	char	   *copy_raw_buf;
	int			raw_buf_ptr;
	int			copy_buf_len;
	bool		need_data;
	bool		hit_eof;
	char		s[2];
	bool        first_char_in_line = true;
	bool		in_quote = false,
				last_was_esc = false;
	char		quotec = cstate->quote[0];
	char		escapec = cstate->escape[0];

	/* ignore special escape processing if it's the same as quotec */
	if (quotec == escapec)
		escapec = '\0';

	s[1] = 0;

	/* set default status */
	result = false;

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
	 * For speed, we try to move data to line_buf in chunks rather than one
	 * character at a time.  raw_buf_ptr points to the next character to
	 * examine; any characters from raw_buf_index to raw_buf_ptr have been
	 * determined to be part of the line, but not yet transferred to line_buf.
	 *
	 * For a little extra speed within the loop, we copy raw_buf and
	 * raw_buf_len into local variables.
	 */
	copy_raw_buf = cstate->raw_buf;
	raw_buf_ptr = cstate->raw_buf_index;
	copy_buf_len = cstate->raw_buf_len;
	need_data = false;			/* flag to force reading more data */
	hit_eof = false;			/* flag indicating no more data available */

	for (;;)
	{
		int			prev_raw_ptr;
		char		c;

		/* Load more data if needed */
		if (raw_buf_ptr >= copy_buf_len || need_data)
		{
			/*
			 * Transfer any approved data to line_buf; must do this to be sure
			 * there is some room in raw_buf.
			 */
			if (raw_buf_ptr > cstate->raw_buf_index)
			{
				appendBinaryStringInfo(&cstate->line_buf,
									 cstate->raw_buf + cstate->raw_buf_index,
									   raw_buf_ptr - cstate->raw_buf_index);
				cstate->raw_buf_index = raw_buf_ptr;
			}

			/*
			 * Try to read some more data.	This will certainly reset
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

		/*
		 * If character is '\\' or '\r', we may need to look ahead below.
		 * Force fetch of the next character if we don't already have it. We
		 * need to do this before changing CSV state, in case one of these
		 * characters is also the quote or escape character.
		 *
		 * Note: old-protocol does not like forced prefetch, but it's OK here
		 * since we cannot validly be at EOF.
		 */
		if (c == '\\' || c == '\r')
		{
			if (raw_buf_ptr >= copy_buf_len && !hit_eof)
			{
				raw_buf_ptr = prev_raw_ptr;		/* undo fetch */
				need_data = true;
				continue;
			}
		}

		/*
		 * Dealing with quotes and escapes here is mildly tricky. If the quote
		 * char is also the escape char, there's no problem - we  just use the
		 * char as a toggle. If they are different, we need to ensure that we
		 * only take account of an escape inside a quoted field and
		 * immediately preceding a quote char, and not the second in a
		 * escape-escape sequence.
		 */
		if (in_quote && c == escapec)
			last_was_esc = !last_was_esc;
		if (c == quotec && !last_was_esc)
			in_quote = !in_quote;
		if (c != escapec)
			last_was_esc = false;

		/*
		 * Updating the line count for embedded CR and/or LF chars is
		 * necessarily a little fragile - this test is probably about the best
		 * we can do.  (XXX it's arguable whether we should do this at all ---
		 * is cur_lineno a physical or logical count?)
		 */
		if (in_quote && c == (cstate->eol_type == EOL_NL ? '\n' : '\r'))
			cstate->cur_lineno++;

		if (c == '\r' && !in_quote)
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
				if (raw_buf_ptr >= copy_buf_len && !hit_eof)
				{
					raw_buf_ptr = prev_raw_ptr; /* undo fetch */
					need_data = true;
					continue;
				}
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
							errmsg("unquoted carriage return found in data"),
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
						 errmsg("unquoted carriage return found in CSV data"),
						 errhint("Use quoted CSV field to represent carriage return.")));
			/* If reach here, we have found the line terminator */
			break;
		}

		if (c == '\n' && !in_quote)
		{
			if (cstate->eol_type == EOL_CR || cstate->eol_type == EOL_CRNL)
				ereport(ERROR,
						(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
						 errmsg("unquoted newline found in data"),
					 errhint("Use quoted CSV field to represent newline.")));
			cstate->eol_type = EOL_NL;	/* in case not set yet */
			/* If reach here, we have found the line terminator */
			break;
		}

		/*
		 * In CSV mode, we only recognize \. at start of line
		 */
		if (c == '\\' && first_char_in_line)
		{
			char		c2;

			/*
			 * If need more data, go back to loop top to load it.
			 */
			if (raw_buf_ptr >= copy_buf_len)
			{
				if (hit_eof)
				{
					/* backslash just before EOF, treat as data char */
					result = true;
					break;
				}
				raw_buf_ptr = prev_raw_ptr;		/* undo fetch */
				need_data = true;
				continue;
			}

			/*
			 * Note: we do not change c here since we aren't treating \ as
			 * escaping the next character.
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
					if (raw_buf_ptr >= copy_buf_len && !hit_eof)
					{
						raw_buf_ptr = prev_raw_ptr;		/* undo fetch */
						need_data = true;
						continue;
					}
					/* if hit_eof, c2 will become '\0' */
					c2 = copy_raw_buf[raw_buf_ptr++];
					if (c2 == '\n')
					{
						raw_buf_ptr = prev_raw_ptr + 1;
						goto not_end_of_copy;
					}
					if (c2 != '\r')
					{
						raw_buf_ptr = prev_raw_ptr + 1;
						goto not_end_of_copy;
					}
				}
				if (raw_buf_ptr >= copy_buf_len && !hit_eof)
				{
					raw_buf_ptr = prev_raw_ptr; /* undo fetch */
					need_data = true;
					continue;
				}
				/* if hit_eof, c2 will become '\0' */
				c2 = copy_raw_buf[raw_buf_ptr++];
				if (c2 != '\r' && c2 != '\n')
				{
					raw_buf_ptr = prev_raw_ptr + 1;
					goto not_end_of_copy;
				}
				if ((cstate->eol_type == EOL_NL && c2 != '\n') ||
					(cstate->eol_type == EOL_CRNL && c2 != '\n') ||
					(cstate->eol_type == EOL_CR && c2 != '\r'))
					ereport(ERROR,
							(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
							 errmsg("end-of-copy marker does not match previous newline style")));
				/*
				 * Transfer only the data before the \. into line_buf, then
				 * discard the data and the \. sequence.
				 */
				if (prev_raw_ptr > cstate->raw_buf_index)
					appendBinaryStringInfo(&cstate->line_buf, cstate->raw_buf + cstate->raw_buf_index,
									   prev_raw_ptr - cstate->raw_buf_index);
				cstate->raw_buf_index = raw_buf_ptr;
				result = true;	/* report EOF */
				break;
			}
		}

		/*
		 * This label is for CSV cases where \. appears at the start of a line,
		 * but there is more text after it, meaning it was a data value.
		 * We are more strict for \. in CSV mode because \. could be a data
		 * value, while in non-CSV mode, \. cannot be a data value.
		 */
not_end_of_copy:

		/*
		 * Process all bytes of a multi-byte character as a group.
		 */
		if (cstate->client_only_encoding)
		{
			int			mblen;

			s[0] = c;
			mblen = pg_encoding_mblen(cstate->client_encoding, s);
			if (raw_buf_ptr + (mblen - 1) > copy_buf_len)
			{
				if (hit_eof)
				{
					/* consume the partial character (will fail below) */
					raw_buf_ptr = copy_buf_len;
					result = true;
					break;
				}
				raw_buf_ptr = prev_raw_ptr;		/* undo fetch */
				need_data = true;
				continue;
			}
			raw_buf_ptr += mblen - 1;
		}
		first_char_in_line = false;
	}							/* end of outer loop */

	/*
	 * Transfer any still-uncopied data to line_buf.
	 */
	if (raw_buf_ptr > cstate->raw_buf_index)
	{
		appendBinaryStringInfo(&cstate->line_buf,
							   cstate->raw_buf + cstate->raw_buf_index,
							   raw_buf_ptr - cstate->raw_buf_index);
		cstate->raw_buf_index = raw_buf_ptr;
	}

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
 * The return value is the number of fields actually read.	(We error out
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

	/* reset attribute_buf to empty */
	cstate->attribute_buf.len = 0;
	cstate->attribute_buf.data[0] = '\0';

	/*
	 * The de-escaped attributes will certainly not be longer than the input
	 * data line, so we can just force attribute_buf to be large enough and
	 * then transfer data without any checks for enough space.	We need to do
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

		/* Make sure space remains in fieldvals[] */
		if (fieldno >= maxfields)
			ereport(ERROR,
					(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
					 errmsg("extra data after last expected column")));

		/* Remember start of field on both input and output sides */
		start_ptr = cur_ptr;
		fieldvals[fieldno] = output_ptr;

		/* Scan data for field */
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

		/* Terminate attribute value in output area */
		*output_ptr++ = '\0';

		/* Check whether raw input matched null marker */
		input_len = end_ptr - start_ptr;
		if (input_len == cstate->null_print_len &&
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

	/* reset attribute_buf to empty */
	cstate->attribute_buf.len = 0;
	cstate->attribute_buf.data[0] = '\0';

	/*
	 * The de-escaped attributes will certainly not be longer than the input
	 * data line, so we can just force attribute_buf to be large enough and
	 * then transfer data without any checks for enough space.	We need to do
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
		bool		in_quote = false;
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

		/* Scan data for field */
		for (;;)
		{
			char		c;

			end_ptr = cur_ptr;
			if (cur_ptr >= line_end_ptr)
				break;
			c = *cur_ptr++;
			/* unquoted field delimiter */
			if (c == delimc && !in_quote)
			{
				found_delim = true;
				break;
			}
			/* start of quoted field (or part of field) */
			if (c == quotec && !in_quote)
			{
				saw_quote = true;
				in_quote = true;
				continue;
			}
			/* escape within a quoted field */
			if (c == escapec && in_quote)
			{
				/*
				 * peek at the next char if available, and escape it if it is
				 * an escape char or a quote char
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
			 * end of quoted field. Must do this test after testing for escape
			 * in case quote char and escape char are the same (which is the
			 * common case).
			 */
			if (c == quotec && in_quote)
			{
				in_quote = false;
				continue;
			}

			/* Add c to output string */
			*output_ptr++ = c;
		}

		/* Terminate attribute value in output area */
		*output_ptr++ = '\0';

		/* Shouldn't still be in quote mode */
		if (in_quote)
			ereport(ERROR,
					(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
					 errmsg("unterminated CSV quoted field")));

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
		return (Datum) 0;
	}
	if (fld_size < 0)
		ereport(ERROR,
				(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
				 errmsg("invalid field size")));

	/* reset attribute_buf to empty, and load raw data in it */
	cstate->attribute_buf.len = 0;
	cstate->attribute_buf.data[0] = '\0';
	cstate->attribute_buf.cursor = 0;

	enlargeStringInfo(&cstate->attribute_buf, fld_size);

	if (CopyGetData(cstate, cstate->attribute_buf.data,
					fld_size, fld_size) != fld_size)
		ereport(ERROR,
				(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
				 errmsg("unexpected EOF in COPY data")));

	cstate->attribute_buf.len = fld_size;
	cstate->attribute_buf.data[fld_size] = '\0';

	/* Call the column type's binary input converter */
	result = FunctionCall3(flinfo,
						   PointerGetDatum(&cstate->attribute_buf),
						   ObjectIdGetDatum(typioparam),
						   Int32GetDatum(typmod));

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
static void
CopyAttributeOutText(CopyState cstate, char *server_string)
{
	char	   *string;
	char		c;
	char		delimc = cstate->delim[0];
	int			mblen;

	if (cstate->need_transcoding)
		string = pg_server_to_client(server_string, strlen(server_string));
	else
		string = server_string;

	for (; (c = *string) != '\0'; string += mblen)
	{
		mblen = 1;

		switch (c)
		{
			case '\b':
				CopySendString(cstate, "\\b");
				break;
			case '\f':
				CopySendString(cstate, "\\f");
				break;
			case '\n':
				CopySendString(cstate, "\\n");
				break;
			case '\r':
				CopySendString(cstate, "\\r");
				break;
			case '\t':
				CopySendString(cstate, "\\t");
				break;
			case '\v':
				CopySendString(cstate, "\\v");
				break;
			case '\\':
				CopySendString(cstate, "\\\\");
				break;
			default:
				if (c == delimc)
					CopySendChar(cstate, '\\');

				/*
				 * We can skip pg_encoding_mblen() overhead when encoding is
				 * safe, because in valid backend encodings, extra bytes of a
				 * multibyte character never look like ASCII.
				 */
				if (cstate->client_only_encoding)
					mblen = pg_encoding_mblen(cstate->client_encoding, string);
				CopySendData(cstate, string, mblen);
				break;
		}
	}
}

/*
 * Send CSV representation of one attribute, with conversion and
 * CSV type escaping
 */
static void
CopyAttributeOutCSV(CopyState cstate, char *server_string,
					bool use_quote, bool single_attr)
{
	char	   *string;
	char		c;
	char		delimc = cstate->delim[0];
	char		quotec = cstate->quote[0];
	char		escapec = cstate->escape[0];
	char	   *tstring;
	int			mblen;

	/* force quoting if it matches null_print */
	if (!use_quote && strcmp(server_string, cstate->null_print) == 0)
		use_quote = true;

	if (cstate->need_transcoding)
		string = pg_server_to_client(server_string, strlen(server_string));
	else
		string = server_string;

	/*
	 * have to run through the string twice, first time to see if it needs
	 * quoting, second to actually send it
	 */
	if (!use_quote)
	{
		/*
		 *	Because '\.' can be a data value, quote it if it appears
		 *	alone on a line so it is not interpreted as the end-of-data
		 *	marker.
		 */
		if (single_attr && strcmp(string, "\\.") == 0)
 			use_quote = true;
 		else
 		{
			for (tstring = string; (c = *tstring) != '\0'; tstring += mblen)
			{
				if (c == delimc || c == quotec || c == '\n' || c == '\r')
				{
					use_quote = true;
					break;
				}
				if (cstate->client_only_encoding)
					mblen = pg_encoding_mblen(cstate->client_encoding, tstring);
				else
					mblen = 1;
			}
		}
	}

	if (use_quote)
		CopySendChar(cstate, quotec);

	for (; (c = *string) != '\0'; string += mblen)
	{
		if (use_quote && (c == quotec || c == escapec))
			CopySendChar(cstate, escapec);
		if (cstate->client_only_encoding)
			mblen = pg_encoding_mblen(cstate->client_encoding, string);
		else
			mblen = 1;
		CopySendData(cstate, string, mblen);
	}

	if (use_quote)
		CopySendChar(cstate, quotec);
}

/*
 * CopyGetAttnums - build an integer list of attnums to be copied
 *
 * The input attnamelist is either the user-specified column list,
 * or NIL if there was none (in which case we want all the non-dropped
 * columns).
 */
static List *
CopyGetAttnums(Relation rel, List *attnamelist)
{
	List	   *attnums = NIL;

	if (attnamelist == NIL)
	{
		/* Generate default column list */
		TupleDesc	tupDesc = RelationGetDescr(rel);
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

			/* Lookup column name, ereport on failure */
			/* Note we disallow system columns here */
			attnum = attnameAttNum(rel, name, false);
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
