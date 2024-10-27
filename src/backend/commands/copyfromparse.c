/*-------------------------------------------------------------------------
 *
 * copyfromparse.c
 *		Parse CSV/text/binary format for COPY FROM.
 *
 * This file contains routines to parse the text, CSV and binary input
 * formats.  The main entry point is NextCopyFrom(), which parses the
 * next input line and returns it as Datums.
 *
 * In text/CSV mode, the parsing happens in multiple stages:
 *
 * [data source] --> raw_buf --> input_buf --> line_buf --> attribute_buf
 *                1.          2.            3.           4.
 *
 * 1. CopyLoadRawBuf() reads raw data from the input file or client, and
 *    places it into 'raw_buf'.
 *
 * 2. CopyConvertBuf() calls the encoding conversion function to convert
 *    the data in 'raw_buf' from client to server encoding, placing the
 *    converted result in 'input_buf'.
 *
 * 3. CopyReadLine() parses the data in 'input_buf', one line at a time.
 *    It is responsible for finding the next newline marker, taking quote and
 *    escape characters into account according to the COPY options.  The line
 *    is copied into 'line_buf', with quotes and escape characters still
 *    intact.
 *
 * 4. CopyReadAttributesText/CSV() function takes the input line from
 *    'line_buf', and splits it into fields, unescaping the data as required.
 *    The fields are stored in 'attribute_buf', and 'raw_fields' array holds
 *    pointers to each field.
 *
 * If encoding conversion is not required, a shortcut is taken in step 2 to
 * avoid copying the data unnecessarily.  The 'input_buf' pointer is set to
 * point directly to 'raw_buf', so that CopyLoadRawBuf() loads the raw data
 * directly into 'input_buf'.  CopyConvertBuf() then merely validates that
 * the data is valid in the current encoding.
 *
 * In binary mode, the pipeline is much simpler.  Input is loaded into
 * 'raw_buf', and encoding conversion is done in the datatype-specific
 * receive functions, if required.  'input_buf' and 'line_buf' are not used,
 * but 'attribute_buf' is used as a temporary buffer to hold one attribute's
 * data when it's passed the receive function.
 *
 * 'raw_buf' is always 64 kB in size (RAW_BUF_SIZE).  'input_buf' is also
 * 64 kB (INPUT_BUF_SIZE), if encoding conversion is required.  'line_buf'
 * and 'attribute_buf' are expanded on demand, to hold the longest line
 * encountered so far.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/copyfromparse.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>
#include <unistd.h>
#include <sys/stat.h>

#include "commands/copy.h"
#include "commands/copyfrom_internal.h"
#include "commands/progress.h"
#include "executor/executor.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "port/pg_bswap.h"
#include "utils/builtins.h"
#include "utils/rel.h"

#define ISOCTAL(c) (((c) >= '0') && ((c) <= '7'))
#define OCTVALUE(c) ((c) - '0')

/*
 * These macros centralize code used to process line_buf and input_buf buffers.
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
	if (input_buf_ptr + (extralen) >= copy_buf_len && !hit_eof) \
	{ \
		input_buf_ptr = prev_raw_ptr; /* undo fetch */ \
		need_data = true; \
		continue; \
	} \
} else ((void) 0)

/* This consumes the remainder of the buffer and breaks */
#define IF_NEED_REFILL_AND_EOF_BREAK(extralen) \
if (1) \
{ \
	if (input_buf_ptr + (extralen) >= copy_buf_len && hit_eof) \
	{ \
		if (extralen) \
			input_buf_ptr = copy_buf_len; /* consume the partial character */ \
		/* backslash just before EOF, treat as data char */ \
		result = true; \
		break; \
	} \
} else ((void) 0)

/*
 * Transfer any approved data to line_buf; must do this to be sure
 * there is some room in input_buf.
 */
#define REFILL_LINEBUF \
if (1) \
{ \
	if (input_buf_ptr > cstate->input_buf_index) \
	{ \
		appendBinaryStringInfo(&cstate->line_buf, \
							 cstate->input_buf + cstate->input_buf_index, \
							   input_buf_ptr - cstate->input_buf_index); \
		cstate->input_buf_index = input_buf_ptr; \
	} \
} else ((void) 0)

/* NOTE: there's a copy of this in copyto.c */
static const char BinarySignature[11] = "PGCOPY\n\377\r\n\0";


/* non-export function prototypes */
static bool CopyReadLine(CopyFromState cstate);
static bool CopyReadLineText(CopyFromState cstate);
static int	CopyReadAttributesText(CopyFromState cstate);
static int	CopyReadAttributesCSV(CopyFromState cstate);
static Datum CopyReadBinaryAttribute(CopyFromState cstate, FmgrInfo *flinfo,
									 Oid typioparam, int32 typmod,
									 bool *isnull);


/* Low-level communications functions */
static int	CopyGetData(CopyFromState cstate, void *databuf,
						int minread, int maxread);
static inline bool CopyGetInt32(CopyFromState cstate, int32 *val);
static inline bool CopyGetInt16(CopyFromState cstate, int16 *val);
static void CopyLoadInputBuf(CopyFromState cstate);
static int	CopyReadBinaryData(CopyFromState cstate, char *dest, int nbytes);

void
ReceiveCopyBegin(CopyFromState cstate)
{
	StringInfoData buf;
	int			natts = list_length(cstate->attnumlist);
	int16		format = (cstate->opts.binary ? 1 : 0);
	int			i;

	pq_beginmessage(&buf, PqMsg_CopyInResponse);
	pq_sendbyte(&buf, format);	/* overall format */
	pq_sendint16(&buf, natts);
	for (i = 0; i < natts; i++)
		pq_sendint16(&buf, format); /* per-column formats */
	pq_endmessage(&buf);
	cstate->copy_src = COPY_FRONTEND;
	cstate->fe_msgbuf = makeStringInfo();
	/* We *must* flush here to ensure FE knows it can send. */
	pq_flush();
}

void
ReceiveCopyBinaryHeader(CopyFromState cstate)
{
	char		readSig[11];
	int32		tmp;

	/* Signature */
	if (CopyReadBinaryData(cstate, readSig, 11) != 11 ||
		memcmp(readSig, BinarySignature, 11) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
				 errmsg("COPY file signature not recognized")));
	/* Flags field */
	if (!CopyGetInt32(cstate, &tmp))
		ereport(ERROR,
				(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
				 errmsg("invalid COPY file header (missing flags)")));
	if ((tmp & (1 << 16)) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
				 errmsg("invalid COPY file header (WITH OIDS)")));
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
		if (CopyReadBinaryData(cstate, readSig, 1) != 1)
			ereport(ERROR,
					(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
					 errmsg("invalid COPY file header (wrong length)")));
	}
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
CopyGetData(CopyFromState cstate, void *databuf, int minread, int maxread)
{
	int			bytesread = 0;

	switch (cstate->copy_src)
	{
		case COPY_FILE:
			bytesread = fread(databuf, 1, maxread, cstate->copy_file);
			if (ferror(cstate->copy_file))
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not read from COPY file: %m")));
			if (bytesread == 0)
				cstate->raw_reached_eof = true;
			break;
		case COPY_FRONTEND:
			while (maxread > 0 && bytesread < minread && !cstate->raw_reached_eof)
			{
				int			avail;

				while (cstate->fe_msgbuf->cursor >= cstate->fe_msgbuf->len)
				{
					/* Try to receive another message */
					int			mtype;
					int			maxmsglen;

			readmessage:
					HOLD_CANCEL_INTERRUPTS();
					pq_startmsgread();
					mtype = pq_getbyte();
					if (mtype == EOF)
						ereport(ERROR,
								(errcode(ERRCODE_CONNECTION_FAILURE),
								 errmsg("unexpected EOF on client connection with an open transaction")));
					/* Validate message type and set packet size limit */
					switch (mtype)
					{
						case PqMsg_CopyData:
							maxmsglen = PQ_LARGE_MESSAGE_LIMIT;
							break;
						case PqMsg_CopyDone:
						case PqMsg_CopyFail:
						case PqMsg_Flush:
						case PqMsg_Sync:
							maxmsglen = PQ_SMALL_MESSAGE_LIMIT;
							break;
						default:
							ereport(ERROR,
									(errcode(ERRCODE_PROTOCOL_VIOLATION),
									 errmsg("unexpected message type 0x%02X during COPY from stdin",
											mtype)));
							maxmsglen = 0;	/* keep compiler quiet */
							break;
					}
					/* Now collect the message body */
					if (pq_getmessage(cstate->fe_msgbuf, maxmsglen))
						ereport(ERROR,
								(errcode(ERRCODE_CONNECTION_FAILURE),
								 errmsg("unexpected EOF on client connection with an open transaction")));
					RESUME_CANCEL_INTERRUPTS();
					/* ... and process it */
					switch (mtype)
					{
						case PqMsg_CopyData:
							break;
						case PqMsg_CopyDone:
							/* COPY IN correctly terminated by frontend */
							cstate->raw_reached_eof = true;
							return bytesread;
						case PqMsg_CopyFail:
							ereport(ERROR,
									(errcode(ERRCODE_QUERY_CANCELED),
									 errmsg("COPY from stdin failed: %s",
											pq_getmsgstring(cstate->fe_msgbuf))));
							break;
						case PqMsg_Flush:
						case PqMsg_Sync:

							/*
							 * Ignore Flush/Sync for the convenience of client
							 * libraries (such as libpq) that may send those
							 * without noticing that the command they just
							 * sent was COPY.
							 */
							goto readmessage;
						default:
							Assert(false);	/* NOT REACHED */
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
		case COPY_CALLBACK:
			bytesread = cstate->data_source_cb(databuf, minread, maxread);
			break;
	}

	return bytesread;
}


/*
 * These functions do apply some data conversion
 */

/*
 * CopyGetInt32 reads an int32 that appears in network byte order
 *
 * Returns true if OK, false if EOF
 */
static inline bool
CopyGetInt32(CopyFromState cstate, int32 *val)
{
	uint32		buf;

	if (CopyReadBinaryData(cstate, (char *) &buf, sizeof(buf)) != sizeof(buf))
	{
		*val = 0;				/* suppress compiler warning */
		return false;
	}
	*val = (int32) pg_ntoh32(buf);
	return true;
}

/*
 * CopyGetInt16 reads an int16 that appears in network byte order
 */
static inline bool
CopyGetInt16(CopyFromState cstate, int16 *val)
{
	uint16		buf;

	if (CopyReadBinaryData(cstate, (char *) &buf, sizeof(buf)) != sizeof(buf))
	{
		*val = 0;				/* suppress compiler warning */
		return false;
	}
	*val = (int16) pg_ntoh16(buf);
	return true;
}


/*
 * Perform encoding conversion on data in 'raw_buf', writing the converted
 * data into 'input_buf'.
 *
 * On entry, there must be some data to convert in 'raw_buf'.
 */
static void
CopyConvertBuf(CopyFromState cstate)
{
	/*
	 * If the file and server encoding are the same, no encoding conversion is
	 * required.  However, we still need to verify that the input is valid for
	 * the encoding.
	 */
	if (!cstate->need_transcoding)
	{
		/*
		 * When conversion is not required, input_buf and raw_buf are the
		 * same.  raw_buf_len is the total number of bytes in the buffer, and
		 * input_buf_len tracks how many of those bytes have already been
		 * verified.
		 */
		int			preverifiedlen = cstate->input_buf_len;
		int			unverifiedlen = cstate->raw_buf_len - cstate->input_buf_len;
		int			nverified;

		if (unverifiedlen == 0)
		{
			/*
			 * If no more raw data is coming, report the EOF to the caller.
			 */
			if (cstate->raw_reached_eof)
				cstate->input_reached_eof = true;
			return;
		}

		/*
		 * Verify the new data, including any residual unverified bytes from
		 * previous round.
		 */
		nverified = pg_encoding_verifymbstr(cstate->file_encoding,
											cstate->raw_buf + preverifiedlen,
											unverifiedlen);
		if (nverified == 0)
		{
			/*
			 * Could not verify anything.
			 *
			 * If there is no more raw input data coming, it means that there
			 * was an incomplete multi-byte sequence at the end.  Also, if
			 * there's "enough" input left, we should be able to verify at
			 * least one character, and a failure to do so means that we've
			 * hit an invalid byte sequence.
			 */
			if (cstate->raw_reached_eof || unverifiedlen >= pg_encoding_max_length(cstate->file_encoding))
				cstate->input_reached_error = true;
			return;
		}
		cstate->input_buf_len += nverified;
	}
	else
	{
		/*
		 * Encoding conversion is needed.
		 */
		int			nbytes;
		unsigned char *src;
		int			srclen;
		unsigned char *dst;
		int			dstlen;
		int			convertedlen;

		if (RAW_BUF_BYTES(cstate) == 0)
		{
			/*
			 * If no more raw data is coming, report the EOF to the caller.
			 */
			if (cstate->raw_reached_eof)
				cstate->input_reached_eof = true;
			return;
		}

		/*
		 * First, copy down any unprocessed data.
		 */
		nbytes = INPUT_BUF_BYTES(cstate);
		if (nbytes > 0 && cstate->input_buf_index > 0)
			memmove(cstate->input_buf, cstate->input_buf + cstate->input_buf_index,
					nbytes);
		cstate->input_buf_index = 0;
		cstate->input_buf_len = nbytes;
		cstate->input_buf[nbytes] = '\0';

		src = (unsigned char *) cstate->raw_buf + cstate->raw_buf_index;
		srclen = cstate->raw_buf_len - cstate->raw_buf_index;
		dst = (unsigned char *) cstate->input_buf + cstate->input_buf_len;
		dstlen = INPUT_BUF_SIZE - cstate->input_buf_len + 1;

		/*
		 * Do the conversion.  This might stop short, if there is an invalid
		 * byte sequence in the input.  We'll convert as much as we can in
		 * that case.
		 *
		 * Note: Even if we hit an invalid byte sequence, we don't report the
		 * error until all the valid bytes have been consumed.  The input
		 * might contain an end-of-input marker (\.), and we don't want to
		 * report an error if the invalid byte sequence is after the
		 * end-of-input marker.  We might unnecessarily convert some data
		 * after the end-of-input marker as long as it's valid for the
		 * encoding, but that's harmless.
		 */
		convertedlen = pg_do_encoding_conversion_buf(cstate->conversion_proc,
													 cstate->file_encoding,
													 GetDatabaseEncoding(),
													 src, srclen,
													 dst, dstlen,
													 true);
		if (convertedlen == 0)
		{
			/*
			 * Could not convert anything.  If there is no more raw input data
			 * coming, it means that there was an incomplete multi-byte
			 * sequence at the end.  Also, if there is plenty of input left,
			 * we should be able to convert at least one character, so a
			 * failure to do so must mean that we've hit a byte sequence
			 * that's invalid.
			 */
			if (cstate->raw_reached_eof || srclen >= MAX_CONVERSION_INPUT_LENGTH)
				cstate->input_reached_error = true;
			return;
		}
		cstate->raw_buf_index += convertedlen;
		cstate->input_buf_len += strlen((char *) dst);
	}
}

/*
 * Report an encoding or conversion error.
 */
static void
CopyConversionError(CopyFromState cstate)
{
	Assert(cstate->raw_buf_len > 0);
	Assert(cstate->input_reached_error);

	if (!cstate->need_transcoding)
	{
		/*
		 * Everything up to input_buf_len was successfully verified, and
		 * input_buf_len points to the invalid or incomplete character.
		 */
		report_invalid_encoding(cstate->file_encoding,
								cstate->raw_buf + cstate->input_buf_len,
								cstate->raw_buf_len - cstate->input_buf_len);
	}
	else
	{
		/*
		 * raw_buf_index points to the invalid or untranslatable character. We
		 * let the conversion routine report the error, because it can provide
		 * a more specific error message than we could here.  An earlier call
		 * to the conversion routine in CopyConvertBuf() detected that there
		 * is an error, now we call the conversion routine again with
		 * noError=false, to have it throw the error.
		 */
		unsigned char *src;
		int			srclen;
		unsigned char *dst;
		int			dstlen;

		src = (unsigned char *) cstate->raw_buf + cstate->raw_buf_index;
		srclen = cstate->raw_buf_len - cstate->raw_buf_index;
		dst = (unsigned char *) cstate->input_buf + cstate->input_buf_len;
		dstlen = INPUT_BUF_SIZE - cstate->input_buf_len + 1;

		(void) pg_do_encoding_conversion_buf(cstate->conversion_proc,
											 cstate->file_encoding,
											 GetDatabaseEncoding(),
											 src, srclen,
											 dst, dstlen,
											 false);

		/*
		 * The conversion routine should have reported an error, so this
		 * should not be reached.
		 */
		elog(ERROR, "encoding conversion failed without error");
	}
}

/*
 * Load more data from data source to raw_buf.
 *
 * If RAW_BUF_BYTES(cstate) > 0, the unprocessed bytes are moved to the
 * beginning of the buffer, and we load new data after that.
 */
static void
CopyLoadRawBuf(CopyFromState cstate)
{
	int			nbytes;
	int			inbytes;

	/*
	 * In text mode, if encoding conversion is not required, raw_buf and
	 * input_buf point to the same buffer.  Their len/index better agree, too.
	 */
	if (cstate->raw_buf == cstate->input_buf)
	{
		Assert(!cstate->need_transcoding);
		Assert(cstate->raw_buf_index == cstate->input_buf_index);
		Assert(cstate->input_buf_len <= cstate->raw_buf_len);
	}

	/*
	 * Copy down the unprocessed data if any.
	 */
	nbytes = RAW_BUF_BYTES(cstate);
	if (nbytes > 0 && cstate->raw_buf_index > 0)
		memmove(cstate->raw_buf, cstate->raw_buf + cstate->raw_buf_index,
				nbytes);
	cstate->raw_buf_len -= cstate->raw_buf_index;
	cstate->raw_buf_index = 0;

	/*
	 * If raw_buf and input_buf are in fact the same buffer, adjust the
	 * input_buf variables, too.
	 */
	if (cstate->raw_buf == cstate->input_buf)
	{
		cstate->input_buf_len -= cstate->input_buf_index;
		cstate->input_buf_index = 0;
	}

	/* Load more data */
	inbytes = CopyGetData(cstate, cstate->raw_buf + cstate->raw_buf_len,
						  1, RAW_BUF_SIZE - cstate->raw_buf_len);
	nbytes += inbytes;
	cstate->raw_buf[nbytes] = '\0';
	cstate->raw_buf_len = nbytes;

	cstate->bytes_processed += inbytes;
	pgstat_progress_update_param(PROGRESS_COPY_BYTES_PROCESSED, cstate->bytes_processed);

	if (inbytes == 0)
		cstate->raw_reached_eof = true;
}

/*
 * CopyLoadInputBuf loads some more data into input_buf
 *
 * On return, at least one more input character is loaded into
 * input_buf, or input_reached_eof is set.
 *
 * If INPUT_BUF_BYTES(cstate) > 0, the unprocessed bytes are moved to the start
 * of the buffer and then we load more data after that.
 */
static void
CopyLoadInputBuf(CopyFromState cstate)
{
	int			nbytes = INPUT_BUF_BYTES(cstate);

	/*
	 * The caller has updated input_buf_index to indicate how much of the
	 * input has been consumed and isn't needed anymore.  If input_buf is the
	 * same physical area as raw_buf, update raw_buf_index accordingly.
	 */
	if (cstate->raw_buf == cstate->input_buf)
	{
		Assert(!cstate->need_transcoding);
		Assert(cstate->input_buf_index >= cstate->raw_buf_index);
		cstate->raw_buf_index = cstate->input_buf_index;
	}

	for (;;)
	{
		/* If we now have some unconverted data, try to convert it */
		CopyConvertBuf(cstate);

		/* If we now have some more input bytes ready, return them */
		if (INPUT_BUF_BYTES(cstate) > nbytes)
			return;

		/*
		 * If we reached an invalid byte sequence, or we're at an incomplete
		 * multi-byte character but there is no more raw input data, report
		 * conversion error.
		 */
		if (cstate->input_reached_error)
			CopyConversionError(cstate);

		/* no more input, and everything has been converted */
		if (cstate->input_reached_eof)
			break;

		/* Try to load more raw data */
		Assert(!cstate->raw_reached_eof);
		CopyLoadRawBuf(cstate);
	}
}

/*
 * CopyReadBinaryData
 *
 * Reads up to 'nbytes' bytes from cstate->copy_file via cstate->raw_buf
 * and writes them to 'dest'.  Returns the number of bytes read (which
 * would be less than 'nbytes' only if we reach EOF).
 */
static int
CopyReadBinaryData(CopyFromState cstate, char *dest, int nbytes)
{
	int			copied_bytes = 0;

	if (RAW_BUF_BYTES(cstate) >= nbytes)
	{
		/* Enough bytes are present in the buffer. */
		memcpy(dest, cstate->raw_buf + cstate->raw_buf_index, nbytes);
		cstate->raw_buf_index += nbytes;
		copied_bytes = nbytes;
	}
	else
	{
		/*
		 * Not enough bytes in the buffer, so must read from the file.  Need
		 * to loop since 'nbytes' could be larger than the buffer size.
		 */
		do
		{
			int			copy_bytes;

			/* Load more data if buffer is empty. */
			if (RAW_BUF_BYTES(cstate) == 0)
			{
				CopyLoadRawBuf(cstate);
				if (cstate->raw_reached_eof)
					break;		/* EOF */
			}

			/* Transfer some bytes. */
			copy_bytes = Min(nbytes - copied_bytes, RAW_BUF_BYTES(cstate));
			memcpy(dest, cstate->raw_buf + cstate->raw_buf_index, copy_bytes);
			cstate->raw_buf_index += copy_bytes;
			dest += copy_bytes;
			copied_bytes += copy_bytes;
		} while (copied_bytes < nbytes);
	}

	return copied_bytes;
}

/*
 * Read raw fields in the next line for COPY FROM in text or csv mode.
 * Return false if no more lines.
 *
 * An internal temporary buffer is returned via 'fields'. It is valid until
 * the next call of the function. Since the function returns all raw fields
 * in the input file, 'nfields' could be different from the number of columns
 * in the relation.
 *
 * NOTE: force_not_null option are not applied to the returned fields.
 */
bool
NextCopyFromRawFields(CopyFromState cstate, char ***fields, int *nfields)
{
	int			fldct;
	bool		done;

	/* only available for text or csv input */
	Assert(!cstate->opts.binary);

	/* on input check that the header line is correct if needed */
	if (cstate->cur_lineno == 0 && cstate->opts.header_line)
	{
		ListCell   *cur;
		TupleDesc	tupDesc;

		tupDesc = RelationGetDescr(cstate->rel);

		cstate->cur_lineno++;
		done = CopyReadLine(cstate);

		if (cstate->opts.header_line == COPY_HEADER_MATCH)
		{
			int			fldnum;

			if (cstate->opts.csv_mode)
				fldct = CopyReadAttributesCSV(cstate);
			else
				fldct = CopyReadAttributesText(cstate);

			if (fldct != list_length(cstate->attnumlist))
				ereport(ERROR,
						(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
						 errmsg("wrong number of fields in header line: got %d, expected %d",
								fldct, list_length(cstate->attnumlist))));

			fldnum = 0;
			foreach(cur, cstate->attnumlist)
			{
				int			attnum = lfirst_int(cur);
				char	   *colName;
				Form_pg_attribute attr = TupleDescAttr(tupDesc, attnum - 1);

				Assert(fldnum < cstate->max_fields);

				colName = cstate->raw_fields[fldnum++];
				if (colName == NULL)
					ereport(ERROR,
							(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
							 errmsg("column name mismatch in header line field %d: got null value (\"%s\"), expected \"%s\"",
									fldnum, cstate->opts.null_print, NameStr(attr->attname))));

				if (namestrcmp(&attr->attname, colName) != 0)
				{
					ereport(ERROR,
							(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
							 errmsg("column name mismatch in header line field %d: got \"%s\", expected \"%s\"",
									fldnum, colName, NameStr(attr->attname))));
				}
			}
		}

		if (done)
			return false;
	}

	cstate->cur_lineno++;

	/* Actually read the line into memory here */
	done = CopyReadLine(cstate);

	/*
	 * EOF at start of line means we're done.  If we see EOF after some
	 * characters, we act as though it was newline followed by EOF, ie,
	 * process the line and then exit loop on next iteration.
	 */
	if (done && cstate->line_buf.len == 0)
		return false;

	/* Parse the line into de-escaped field values */
	if (cstate->opts.csv_mode)
		fldct = CopyReadAttributesCSV(cstate);
	else
		fldct = CopyReadAttributesText(cstate);

	*fields = cstate->raw_fields;
	*nfields = fldct;
	return true;
}

/*
 * Read next tuple from file for COPY FROM. Return false if no more tuples.
 *
 * 'econtext' is used to evaluate default expression for each column that is
 * either not read from the file or is using the DEFAULT option of COPY FROM.
 * It can be NULL when no default values are used, i.e. when all columns are
 * read from the file, and DEFAULT option is unset.
 *
 * 'values' and 'nulls' arrays must be the same length as columns of the
 * relation passed to BeginCopyFrom. This function fills the arrays.
 */
bool
NextCopyFrom(CopyFromState cstate, ExprContext *econtext,
			 Datum *values, bool *nulls)
{
	TupleDesc	tupDesc;
	AttrNumber	num_phys_attrs,
				attr_count,
				num_defaults = cstate->num_defaults;
	FmgrInfo   *in_functions = cstate->in_functions;
	Oid		   *typioparams = cstate->typioparams;
	int			i;
	int		   *defmap = cstate->defmap;
	ExprState **defexprs = cstate->defexprs;

	tupDesc = RelationGetDescr(cstate->rel);
	num_phys_attrs = tupDesc->natts;
	attr_count = list_length(cstate->attnumlist);

	/* Initialize all values for row to NULL */
	MemSet(values, 0, num_phys_attrs * sizeof(Datum));
	MemSet(nulls, true, num_phys_attrs * sizeof(bool));
	MemSet(cstate->defaults, false, num_phys_attrs * sizeof(bool));

	if (!cstate->opts.binary)
	{
		char	  **field_strings;
		ListCell   *cur;
		int			fldct;
		int			fieldno;
		char	   *string;

		/* read raw fields in the next line */
		if (!NextCopyFromRawFields(cstate, &field_strings, &fldct))
			return false;

		/* check for overflowing fields */
		if (attr_count > 0 && fldct > attr_count)
			ereport(ERROR,
					(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
					 errmsg("extra data after last expected column")));

		fieldno = 0;

		/* Loop to read the user attributes on the line. */
		foreach(cur, cstate->attnumlist)
		{
			int			attnum = lfirst_int(cur);
			int			m = attnum - 1;
			Form_pg_attribute att = TupleDescAttr(tupDesc, m);

			if (fieldno >= fldct)
				ereport(ERROR,
						(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
						 errmsg("missing data for column \"%s\"",
								NameStr(att->attname))));
			string = field_strings[fieldno++];

			if (cstate->convert_select_flags &&
				!cstate->convert_select_flags[m])
			{
				/* ignore input field, leaving column as NULL */
				continue;
			}

			if (cstate->opts.csv_mode)
			{
				if (string == NULL &&
					cstate->opts.force_notnull_flags[m])
				{
					/*
					 * FORCE_NOT_NULL option is set and column is NULL -
					 * convert it to the NULL string.
					 */
					string = cstate->opts.null_print;
				}
				else if (string != NULL && cstate->opts.force_null_flags[m]
						 && strcmp(string, cstate->opts.null_print) == 0)
				{
					/*
					 * FORCE_NULL option is set and column matches the NULL
					 * string. It must have been quoted, or otherwise the
					 * string would already have been set to NULL. Convert it
					 * to NULL as specified.
					 */
					string = NULL;
				}
			}

			cstate->cur_attname = NameStr(att->attname);
			cstate->cur_attval = string;

			if (string != NULL)
				nulls[m] = false;

			if (cstate->defaults[m])
			{
				/*
				 * The caller must supply econtext and have switched into the
				 * per-tuple memory context in it.
				 */
				Assert(econtext != NULL);
				Assert(CurrentMemoryContext == econtext->ecxt_per_tuple_memory);

				values[m] = ExecEvalExpr(defexprs[m], econtext, &nulls[m]);
			}

			/*
			 * If ON_ERROR is specified with IGNORE, skip rows with soft
			 * errors
			 */
			else if (!InputFunctionCallSafe(&in_functions[m],
											string,
											typioparams[m],
											att->atttypmod,
											(Node *) cstate->escontext,
											&values[m]))
			{
				Assert(cstate->opts.on_error != COPY_ON_ERROR_STOP);

				cstate->num_errors++;

				if (cstate->opts.log_verbosity == COPY_LOG_VERBOSITY_VERBOSE)
				{
					/*
					 * Since we emit line number and column info in the below
					 * notice message, we suppress error context information
					 * other than the relation name.
					 */
					Assert(!cstate->relname_only);
					cstate->relname_only = true;

					if (cstate->cur_attval)
					{
						char	   *attval;

						attval = CopyLimitPrintoutLength(cstate->cur_attval);
						ereport(NOTICE,
								errmsg("skipping row due to data type incompatibility at line %llu for column \"%s\": \"%s\"",
									   (unsigned long long) cstate->cur_lineno,
									   cstate->cur_attname,
									   attval));
						pfree(attval);
					}
					else
						ereport(NOTICE,
								errmsg("skipping row due to data type incompatibility at line %llu for column \"%s\": null input",
									   (unsigned long long) cstate->cur_lineno,
									   cstate->cur_attname));

					/* reset relname_only */
					cstate->relname_only = false;
				}

				return true;
			}

			cstate->cur_attname = NULL;
			cstate->cur_attval = NULL;
		}

		Assert(fieldno == attr_count);
	}
	else
	{
		/* binary */
		int16		fld_count;
		ListCell   *cur;

		cstate->cur_lineno++;

		if (!CopyGetInt16(cstate, &fld_count))
		{
			/* EOF detected (end of file, or protocol-level EOF) */
			return false;
		}

		if (fld_count == -1)
		{
			/*
			 * Received EOF marker.  Wait for the protocol-level EOF, and
			 * complain if it doesn't come immediately.  In COPY FROM STDIN,
			 * this ensures that we correctly handle CopyFail, if client
			 * chooses to send that now.  When copying from file, we could
			 * ignore the rest of the file like in text mode, but we choose to
			 * be consistent with the COPY FROM STDIN case.
			 */
			char		dummy;

			if (CopyReadBinaryData(cstate, &dummy, 1) > 0)
				ereport(ERROR,
						(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
						 errmsg("received copy data after EOF marker")));
			return false;
		}

		if (fld_count != attr_count)
			ereport(ERROR,
					(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
					 errmsg("row field count is %d, expected %d",
							(int) fld_count, attr_count)));

		foreach(cur, cstate->attnumlist)
		{
			int			attnum = lfirst_int(cur);
			int			m = attnum - 1;
			Form_pg_attribute att = TupleDescAttr(tupDesc, m);

			cstate->cur_attname = NameStr(att->attname);
			values[m] = CopyReadBinaryAttribute(cstate,
												&in_functions[m],
												typioparams[m],
												att->atttypmod,
												&nulls[m]);
			cstate->cur_attname = NULL;
		}
	}

	/*
	 * Now compute and insert any defaults available for the columns not
	 * provided by the input data.  Anything not processed here or above will
	 * remain NULL.
	 */
	for (i = 0; i < num_defaults; i++)
	{
		/*
		 * The caller must supply econtext and have switched into the
		 * per-tuple memory context in it.
		 */
		Assert(econtext != NULL);
		Assert(CurrentMemoryContext == econtext->ecxt_per_tuple_memory);

		values[defmap[i]] = ExecEvalExpr(defexprs[defmap[i]], econtext,
										 &nulls[defmap[i]]);
	}

	return true;
}

/*
 * Read the next input line and stash it in line_buf.
 *
 * Result is true if read was terminated by EOF, false if terminated
 * by newline.  The terminating newline or EOF marker is not included
 * in the final value of line_buf.
 */
static bool
CopyReadLine(CopyFromState cstate)
{
	bool		result;

	resetStringInfo(&cstate->line_buf);
	cstate->line_buf_valid = false;

	/* Parse data and transfer into line_buf */
	result = CopyReadLineText(cstate);

	if (result)
	{
		/*
		 * Reached EOF.  In protocol version 3, we should ignore anything
		 * after \. up to the protocol end of copy data.  (XXX maybe better
		 * not to treat \. as special?)
		 */
		if (cstate->copy_src == COPY_FRONTEND)
		{
			int			inbytes;

			do
			{
				inbytes = CopyGetData(cstate, cstate->input_buf,
									  1, INPUT_BUF_SIZE);
			} while (inbytes > 0);
			cstate->input_buf_index = 0;
			cstate->input_buf_len = 0;
			cstate->raw_buf_index = 0;
			cstate->raw_buf_len = 0;
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

	/* Now it's safe to use the buffer in error messages */
	cstate->line_buf_valid = true;

	return result;
}

/*
 * CopyReadLineText - inner loop of CopyReadLine for text mode
 */
static bool
CopyReadLineText(CopyFromState cstate)
{
	char	   *copy_input_buf;
	int			input_buf_ptr;
	int			copy_buf_len;
	bool		need_data = false;
	bool		hit_eof = false;
	bool		result = false;

	/* CSV variables */
	bool		in_quote = false,
				last_was_esc = false;
	char		quotec = '\0';
	char		escapec = '\0';

	if (cstate->opts.csv_mode)
	{
		quotec = cstate->opts.quote[0];
		escapec = cstate->opts.escape[0];
		/* ignore special escape processing if it's the same as quotec */
		if (quotec == escapec)
			escapec = '\0';
	}

	/*
	 * The objective of this loop is to transfer the entire next input line
	 * into line_buf.  Hence, we only care for detecting newlines (\r and/or
	 * \n) and the end-of-copy marker (\.).
	 *
	 * In CSV mode, \r and \n inside a quoted field are just part of the data
	 * value and are put in line_buf.  We keep just enough state to know if we
	 * are currently in a quoted field or not.
	 *
	 * The input has already been converted to the database encoding.  All
	 * supported server encodings have the property that all bytes in a
	 * multi-byte sequence have the high bit set, so a multibyte character
	 * cannot contain any newline or escape characters embedded in the
	 * multibyte sequence.  Therefore, we can process the input byte-by-byte,
	 * regardless of the encoding.
	 *
	 * For speed, we try to move data from input_buf to line_buf in chunks
	 * rather than one character at a time.  input_buf_ptr points to the next
	 * character to examine; any characters from input_buf_index to
	 * input_buf_ptr have been determined to be part of the line, but not yet
	 * transferred to line_buf.
	 *
	 * For a little extra speed within the loop, we copy input_buf and
	 * input_buf_len into local variables.
	 */
	copy_input_buf = cstate->input_buf;
	input_buf_ptr = cstate->input_buf_index;
	copy_buf_len = cstate->input_buf_len;

	for (;;)
	{
		int			prev_raw_ptr;
		char		c;

		/*
		 * Load more data if needed.
		 *
		 * TODO: We could just force four bytes of read-ahead and avoid the
		 * many calls to IF_NEED_REFILL_AND_NOT_EOF_CONTINUE().  That was
		 * unsafe with the old v2 COPY protocol, but we don't support that
		 * anymore.
		 */
		if (input_buf_ptr >= copy_buf_len || need_data)
		{
			REFILL_LINEBUF;

			CopyLoadInputBuf(cstate);
			/* update our local variables */
			hit_eof = cstate->input_reached_eof;
			input_buf_ptr = cstate->input_buf_index;
			copy_buf_len = cstate->input_buf_len;

			/*
			 * If we are completely out of data, break out of the loop,
			 * reporting EOF.
			 */
			if (INPUT_BUF_BYTES(cstate) <= 0)
			{
				result = true;
				break;
			}
			need_data = false;
		}

		/* OK to fetch a character */
		prev_raw_ptr = input_buf_ptr;
		c = copy_input_buf[input_buf_ptr++];

		if (cstate->opts.csv_mode)
		{
			/*
			 * If character is '\r', we may need to look ahead below.  Force
			 * fetch of the next character if we don't already have it.  We
			 * need to do this before changing CSV state, in case '\r' is also
			 * the quote or escape character.
			 */
			if (c == '\r')
			{
				IF_NEED_REFILL_AND_NOT_EOF_CONTINUE(0);
			}

			/*
			 * Dealing with quotes and escapes here is mildly tricky. If the
			 * quote char is also the escape char, there's no problem - we
			 * just use the char as a toggle. If they are different, we need
			 * to ensure that we only take account of an escape inside a
			 * quoted field and immediately preceding a quote char, and not
			 * the second in an escape-escape sequence.
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
		if (c == '\r' && (!cstate->opts.csv_mode || !in_quote))
		{
			/* Check for \r\n on first line, _and_ handle \r\n. */
			if (cstate->eol_type == EOL_UNKNOWN ||
				cstate->eol_type == EOL_CRNL)
			{
				/*
				 * If need more data, go back to loop top to load it.
				 *
				 * Note that if we are at EOF, c will wind up as '\0' because
				 * of the guaranteed pad of input_buf.
				 */
				IF_NEED_REFILL_AND_NOT_EOF_CONTINUE(0);

				/* get next char */
				c = copy_input_buf[input_buf_ptr];

				if (c == '\n')
				{
					input_buf_ptr++;	/* eat newline */
					cstate->eol_type = EOL_CRNL;	/* in case not set yet */
				}
				else
				{
					/* found \r, but no \n */
					if (cstate->eol_type == EOL_CRNL)
						ereport(ERROR,
								(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
								 !cstate->opts.csv_mode ?
								 errmsg("literal carriage return found in data") :
								 errmsg("unquoted carriage return found in data"),
								 !cstate->opts.csv_mode ?
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
						 !cstate->opts.csv_mode ?
						 errmsg("literal carriage return found in data") :
						 errmsg("unquoted carriage return found in data"),
						 !cstate->opts.csv_mode ?
						 errhint("Use \"\\r\" to represent carriage return.") :
						 errhint("Use quoted CSV field to represent carriage return.")));
			/* If reach here, we have found the line terminator */
			break;
		}

		/* Process \n */
		if (c == '\n' && (!cstate->opts.csv_mode || !in_quote))
		{
			if (cstate->eol_type == EOL_CR || cstate->eol_type == EOL_CRNL)
				ereport(ERROR,
						(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
						 !cstate->opts.csv_mode ?
						 errmsg("literal newline found in data") :
						 errmsg("unquoted newline found in data"),
						 !cstate->opts.csv_mode ?
						 errhint("Use \"\\n\" to represent newline.") :
						 errhint("Use quoted CSV field to represent newline.")));
			cstate->eol_type = EOL_NL;	/* in case not set yet */
			/* If reach here, we have found the line terminator */
			break;
		}

		/*
		 * Process backslash, except in CSV mode where backslash is a normal
		 * character.
		 */
		if (c == '\\' && !cstate->opts.csv_mode)
		{
			char		c2;

			IF_NEED_REFILL_AND_NOT_EOF_CONTINUE(0);
			IF_NEED_REFILL_AND_EOF_BREAK(0);

			/* -----
			 * get next character
			 * Note: we do not change c so if it isn't \., we can fall
			 * through and continue processing.
			 * -----
			 */
			c2 = copy_input_buf[input_buf_ptr];

			if (c2 == '.')
			{
				input_buf_ptr++;	/* consume the '.' */
				if (cstate->eol_type == EOL_CRNL)
				{
					/* Get the next character */
					IF_NEED_REFILL_AND_NOT_EOF_CONTINUE(0);
					/* if hit_eof, c2 will become '\0' */
					c2 = copy_input_buf[input_buf_ptr++];

					if (c2 == '\n')
						ereport(ERROR,
								(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
								 errmsg("end-of-copy marker does not match previous newline style")));
					else if (c2 != '\r')
						ereport(ERROR,
								(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
								 errmsg("end-of-copy marker is not alone on its line")));
				}

				/* Get the next character */
				IF_NEED_REFILL_AND_NOT_EOF_CONTINUE(0);
				/* if hit_eof, c2 will become '\0' */
				c2 = copy_input_buf[input_buf_ptr++];

				if (c2 != '\r' && c2 != '\n')
					ereport(ERROR,
							(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
							 errmsg("end-of-copy marker is not alone on its line")));

				if ((cstate->eol_type == EOL_NL && c2 != '\n') ||
					(cstate->eol_type == EOL_CRNL && c2 != '\n') ||
					(cstate->eol_type == EOL_CR && c2 != '\r'))
					ereport(ERROR,
							(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
							 errmsg("end-of-copy marker does not match previous newline style")));

				/*
				 * If there is any data on this line before the \., complain.
				 */
				if (cstate->line_buf.len > 0 ||
					prev_raw_ptr > cstate->input_buf_index)
					ereport(ERROR,
							(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
							 errmsg("end-of-copy marker is not alone on its line")));

				/*
				 * Discard the \. and newline, then report EOF.
				 */
				cstate->input_buf_index = input_buf_ptr;
				result = true;	/* report EOF */
				break;
			}
			else
			{
				/*
				 * If we are here, it means we found a backslash followed by
				 * something other than a period.  In non-CSV mode, anything
				 * after a backslash is special, so we skip over that second
				 * character too.  If we didn't do that \\. would be
				 * considered an eof-of copy, while in non-CSV mode it is a
				 * literal backslash followed by a period.
				 */
				input_buf_ptr++;
			}
		}
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
 * strings.  cstate->raw_fields[k] is set to point to the k'th attribute
 * string, or NULL when the input matches the null marker string.
 * This array is expanded as necessary.
 *
 * (Note that the caller cannot check for nulls since the returned
 * string would be the post-de-escaping equivalent, which may look
 * the same as some valid data string.)
 *
 * delim is the column delimiter string (must be just one byte for now).
 * null_print is the null marker string.  Note that this is compared to
 * the pre-de-escaped input string.
 *
 * The return value is the number of fields actually read.
 */
static int
CopyReadAttributesText(CopyFromState cstate)
{
	char		delimc = cstate->opts.delim[0];
	int			fieldno;
	char	   *output_ptr;
	char	   *cur_ptr;
	char	   *line_end_ptr;

	/*
	 * We need a special case for zero-column tables: check that the input
	 * line is empty, and return.
	 */
	if (cstate->max_fields <= 0)
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
	 * pointers already stored into cstate->raw_fields[].
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

		/* Make sure there is enough space for the next value */
		if (fieldno >= cstate->max_fields)
		{
			cstate->max_fields *= 2;
			cstate->raw_fields =
				repalloc(cstate->raw_fields, cstate->max_fields * sizeof(char *));
		}

		/* Remember start of field on both input and output sides */
		start_ptr = cur_ptr;
		cstate->raw_fields[fieldno] = output_ptr;

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
		if (input_len == cstate->opts.null_print_len &&
			strncmp(start_ptr, cstate->opts.null_print, input_len) == 0)
			cstate->raw_fields[fieldno] = NULL;
		/* Check whether raw input matched default marker */
		else if (fieldno < list_length(cstate->attnumlist) &&
				 cstate->opts.default_print &&
				 input_len == cstate->opts.default_print_len &&
				 strncmp(start_ptr, cstate->opts.default_print, input_len) == 0)
		{
			/* fieldno is 0-indexed and attnum is 1-indexed */
			int			m = list_nth_int(cstate->attnumlist, fieldno) - 1;

			if (cstate->defexprs[m] != NULL)
			{
				/* defaults contain entries for all physical attributes */
				cstate->defaults[m] = true;
			}
			else
			{
				TupleDesc	tupDesc = RelationGetDescr(cstate->rel);
				Form_pg_attribute att = TupleDescAttr(tupDesc, m);

				ereport(ERROR,
						(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
						 errmsg("unexpected default marker in COPY data"),
						 errdetail("Column \"%s\" has no default value.",
								   NameStr(att->attname))));
			}
		}
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
				char	   *fld = cstate->raw_fields[fieldno];

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
CopyReadAttributesCSV(CopyFromState cstate)
{
	char		delimc = cstate->opts.delim[0];
	char		quotec = cstate->opts.quote[0];
	char		escapec = cstate->opts.escape[0];
	int			fieldno;
	char	   *output_ptr;
	char	   *cur_ptr;
	char	   *line_end_ptr;

	/*
	 * We need a special case for zero-column tables: check that the input
	 * line is empty, and return.
	 */
	if (cstate->max_fields <= 0)
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
	 * pointers already stored into cstate->raw_fields[].
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

		/* Make sure there is enough space for the next value */
		if (fieldno >= cstate->max_fields)
		{
			cstate->max_fields *= 2;
			cstate->raw_fields =
				repalloc(cstate->raw_fields, cstate->max_fields * sizeof(char *));
		}

		/* Remember start of field on both input and output sides */
		start_ptr = cur_ptr;
		cstate->raw_fields[fieldno] = output_ptr;

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
		if (!saw_quote && input_len == cstate->opts.null_print_len &&
			strncmp(start_ptr, cstate->opts.null_print, input_len) == 0)
			cstate->raw_fields[fieldno] = NULL;
		/* Check whether raw input matched default marker */
		else if (fieldno < list_length(cstate->attnumlist) &&
				 cstate->opts.default_print &&
				 input_len == cstate->opts.default_print_len &&
				 strncmp(start_ptr, cstate->opts.default_print, input_len) == 0)
		{
			/* fieldno is 0-index and attnum is 1-index */
			int			m = list_nth_int(cstate->attnumlist, fieldno) - 1;

			if (cstate->defexprs[m] != NULL)
			{
				/* defaults contain entries for all physical attributes */
				cstate->defaults[m] = true;
			}
			else
			{
				TupleDesc	tupDesc = RelationGetDescr(cstate->rel);
				Form_pg_attribute att = TupleDescAttr(tupDesc, m);

				ereport(ERROR,
						(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
						 errmsg("unexpected default marker in COPY data"),
						 errdetail("Column \"%s\" has no default value.",
								   NameStr(att->attname))));
			}
		}

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
CopyReadBinaryAttribute(CopyFromState cstate, FmgrInfo *flinfo,
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
	if (CopyReadBinaryData(cstate, cstate->attribute_buf.data,
						   fld_size) != fld_size)
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
