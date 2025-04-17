/*-------------------------------------------------------------------------
 *
 * mbutils.c
 *	  This file contains functions for encoding conversion.
 *
 * The string-conversion functions in this file share some API quirks.
 * Note the following:
 *
 * The functions return a palloc'd, null-terminated string if conversion
 * is required.  However, if no conversion is performed, the given source
 * string pointer is returned as-is.
 *
 * Although the presence of a length argument means that callers can pass
 * non-null-terminated strings, care is required because the same string
 * will be passed back if no conversion occurs.  Such callers *must* check
 * whether result == src and handle that case differently.
 *
 * If the source and destination encodings are the same, the source string
 * is returned without any verification; it's assumed to be valid data.
 * If that might not be the case, the caller is responsible for validating
 * the string using a separate call to pg_verify_mbstr().  Whenever the
 * source and destination encodings are different, the functions ensure that
 * the result is validly encoded according to the destination encoding.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/mb/mbutils.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "catalog/namespace.h"
#include "mb/pg_wchar.h"
#include "utils/fmgrprotos.h"
#include "utils/memutils.h"
#include "varatt.h"

/*
 * We maintain a simple linked list caching the fmgr lookup info for the
 * currently selected conversion functions, as well as any that have been
 * selected previously in the current session.  (We remember previous
 * settings because we must be able to restore a previous setting during
 * transaction rollback, without doing any fresh catalog accesses.)
 *
 * Since we'll never release this data, we just keep it in TopMemoryContext.
 */
typedef struct ConvProcInfo
{
	int			s_encoding;		/* server and client encoding IDs */
	int			c_encoding;
	FmgrInfo	to_server_info; /* lookup info for conversion procs */
	FmgrInfo	to_client_info;
} ConvProcInfo;

static List *ConvProcList = NIL;	/* List of ConvProcInfo */

/*
 * These variables point to the currently active conversion functions,
 * or are NULL when no conversion is needed.
 */
static FmgrInfo *ToServerConvProc = NULL;
static FmgrInfo *ToClientConvProc = NULL;

/*
 * This variable stores the conversion function to convert from UTF-8
 * to the server encoding.  It's NULL if the server encoding *is* UTF-8,
 * or if we lack a conversion function for this.
 */
static FmgrInfo *Utf8ToServerConvProc = NULL;

/*
 * These variables track the currently-selected encodings.
 */
static const pg_enc2name *ClientEncoding = &pg_enc2name_tbl[PG_SQL_ASCII];
static const pg_enc2name *DatabaseEncoding = &pg_enc2name_tbl[PG_SQL_ASCII];
static const pg_enc2name *MessageEncoding = &pg_enc2name_tbl[PG_SQL_ASCII];

/*
 * During backend startup we can't set client encoding because we (a)
 * can't look up the conversion functions, and (b) may not know the database
 * encoding yet either.  So SetClientEncoding() just accepts anything and
 * remembers it for InitializeClientEncoding() to apply later.
 */
static bool backend_startup_complete = false;
static int	pending_client_encoding = PG_SQL_ASCII;


/* Internal functions */
static char *perform_default_encoding_conversion(const char *src,
												 int len, bool is_client_to_server);
static int	cliplen(const char *str, int len, int limit);


/*
 * Prepare for a future call to SetClientEncoding.  Success should mean
 * that SetClientEncoding is guaranteed to succeed for this encoding request.
 *
 * (But note that success before backend_startup_complete does not guarantee
 * success after ...)
 *
 * Returns 0 if okay, -1 if not (bad encoding or can't support conversion)
 */
int
PrepareClientEncoding(int encoding)
{
	int			current_server_encoding;
	ListCell   *lc;

	if (!PG_VALID_FE_ENCODING(encoding))
		return -1;

	/* Can't do anything during startup, per notes above */
	if (!backend_startup_complete)
		return 0;

	current_server_encoding = GetDatabaseEncoding();

	/*
	 * Check for cases that require no conversion function.
	 */
	if (current_server_encoding == encoding ||
		current_server_encoding == PG_SQL_ASCII ||
		encoding == PG_SQL_ASCII)
		return 0;

	if (IsTransactionState())
	{
		/*
		 * If we're in a live transaction, it's safe to access the catalogs,
		 * so look up the functions.  We repeat the lookup even if the info is
		 * already cached, so that we can react to changes in the contents of
		 * pg_conversion.
		 */
		Oid			to_server_proc,
					to_client_proc;
		ConvProcInfo *convinfo;
		MemoryContext oldcontext;

		to_server_proc = FindDefaultConversionProc(encoding,
												   current_server_encoding);
		if (!OidIsValid(to_server_proc))
			return -1;
		to_client_proc = FindDefaultConversionProc(current_server_encoding,
												   encoding);
		if (!OidIsValid(to_client_proc))
			return -1;

		/*
		 * Load the fmgr info into TopMemoryContext (could still fail here)
		 */
		convinfo = (ConvProcInfo *) MemoryContextAlloc(TopMemoryContext,
													   sizeof(ConvProcInfo));
		convinfo->s_encoding = current_server_encoding;
		convinfo->c_encoding = encoding;
		fmgr_info_cxt(to_server_proc, &convinfo->to_server_info,
					  TopMemoryContext);
		fmgr_info_cxt(to_client_proc, &convinfo->to_client_info,
					  TopMemoryContext);

		/* Attach new info to head of list */
		oldcontext = MemoryContextSwitchTo(TopMemoryContext);
		ConvProcList = lcons(convinfo, ConvProcList);
		MemoryContextSwitchTo(oldcontext);

		/*
		 * We cannot yet remove any older entry for the same encoding pair,
		 * since it could still be in use.  SetClientEncoding will clean up.
		 */

		return 0;				/* success */
	}
	else
	{
		/*
		 * If we're not in a live transaction, the only thing we can do is
		 * restore a previous setting using the cache.  This covers all
		 * transaction-rollback cases.  The only case it might not work for is
		 * trying to change client_encoding on the fly by editing
		 * postgresql.conf and SIGHUP'ing.  Which would probably be a stupid
		 * thing to do anyway.
		 */
		foreach(lc, ConvProcList)
		{
			ConvProcInfo *oldinfo = (ConvProcInfo *) lfirst(lc);

			if (oldinfo->s_encoding == current_server_encoding &&
				oldinfo->c_encoding == encoding)
				return 0;
		}

		return -1;				/* it's not cached, so fail */
	}
}

/*
 * Set the active client encoding and set up the conversion-function pointers.
 * PrepareClientEncoding should have been called previously for this encoding.
 *
 * Returns 0 if okay, -1 if not (bad encoding or can't support conversion)
 */
int
SetClientEncoding(int encoding)
{
	int			current_server_encoding;
	bool		found;
	ListCell   *lc;

	if (!PG_VALID_FE_ENCODING(encoding))
		return -1;

	/* Can't do anything during startup, per notes above */
	if (!backend_startup_complete)
	{
		pending_client_encoding = encoding;
		return 0;
	}

	current_server_encoding = GetDatabaseEncoding();

	/*
	 * Check for cases that require no conversion function.
	 */
	if (current_server_encoding == encoding ||
		current_server_encoding == PG_SQL_ASCII ||
		encoding == PG_SQL_ASCII)
	{
		ClientEncoding = &pg_enc2name_tbl[encoding];
		ToServerConvProc = NULL;
		ToClientConvProc = NULL;
		return 0;
	}

	/*
	 * Search the cache for the entry previously prepared by
	 * PrepareClientEncoding; if there isn't one, we lose.  While at it,
	 * release any duplicate entries so that repeated Prepare/Set cycles don't
	 * leak memory.
	 */
	found = false;
	foreach(lc, ConvProcList)
	{
		ConvProcInfo *convinfo = (ConvProcInfo *) lfirst(lc);

		if (convinfo->s_encoding == current_server_encoding &&
			convinfo->c_encoding == encoding)
		{
			if (!found)
			{
				/* Found newest entry, so set up */
				ClientEncoding = &pg_enc2name_tbl[encoding];
				ToServerConvProc = &convinfo->to_server_info;
				ToClientConvProc = &convinfo->to_client_info;
				found = true;
			}
			else
			{
				/* Duplicate entry, release it */
				ConvProcList = foreach_delete_current(ConvProcList, lc);
				pfree(convinfo);
			}
		}
	}

	if (found)
		return 0;				/* success */
	else
		return -1;				/* it's not cached, so fail */
}

/*
 * Initialize client encoding conversions.
 *		Called from InitPostgres() once during backend startup.
 */
void
InitializeClientEncoding(void)
{
	int			current_server_encoding;

	Assert(!backend_startup_complete);
	backend_startup_complete = true;

	if (PrepareClientEncoding(pending_client_encoding) < 0 ||
		SetClientEncoding(pending_client_encoding) < 0)
	{
		/*
		 * Oops, the requested conversion is not available. We couldn't fail
		 * before, but we can now.
		 */
		ereport(FATAL,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("conversion between %s and %s is not supported",
						pg_enc2name_tbl[pending_client_encoding].name,
						GetDatabaseEncodingName())));
	}

	/*
	 * Also look up the UTF8-to-server conversion function if needed.  Since
	 * the server encoding is fixed within any one backend process, we don't
	 * have to do this more than once.
	 */
	current_server_encoding = GetDatabaseEncoding();
	if (current_server_encoding != PG_UTF8 &&
		current_server_encoding != PG_SQL_ASCII)
	{
		Oid			utf8_to_server_proc;

		AssertCouldGetRelation();
		utf8_to_server_proc =
			FindDefaultConversionProc(PG_UTF8,
									  current_server_encoding);
		/* If there's no such conversion, just leave the pointer as NULL */
		if (OidIsValid(utf8_to_server_proc))
		{
			FmgrInfo   *finfo;

			finfo = (FmgrInfo *) MemoryContextAlloc(TopMemoryContext,
													sizeof(FmgrInfo));
			fmgr_info_cxt(utf8_to_server_proc, finfo,
						  TopMemoryContext);
			/* Set Utf8ToServerConvProc only after data is fully valid */
			Utf8ToServerConvProc = finfo;
		}
	}
}

/*
 * returns the current client encoding
 */
int
pg_get_client_encoding(void)
{
	return ClientEncoding->encoding;
}

/*
 * returns the current client encoding name
 */
const char *
pg_get_client_encoding_name(void)
{
	return ClientEncoding->name;
}

/*
 * Convert src string to another encoding (general case).
 *
 * See the notes about string conversion functions at the top of this file.
 */
unsigned char *
pg_do_encoding_conversion(unsigned char *src, int len,
						  int src_encoding, int dest_encoding)
{
	unsigned char *result;
	Oid			proc;

	if (len <= 0)
		return src;				/* empty string is always valid */

	if (src_encoding == dest_encoding)
		return src;				/* no conversion required, assume valid */

	if (dest_encoding == PG_SQL_ASCII)
		return src;				/* any string is valid in SQL_ASCII */

	if (src_encoding == PG_SQL_ASCII)
	{
		/* No conversion is possible, but we must validate the result */
		(void) pg_verify_mbstr(dest_encoding, (const char *) src, len, false);
		return src;
	}

	if (!IsTransactionState())	/* shouldn't happen */
		elog(ERROR, "cannot perform encoding conversion outside a transaction");

	proc = FindDefaultConversionProc(src_encoding, dest_encoding);
	if (!OidIsValid(proc))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("default conversion function for encoding \"%s\" to \"%s\" does not exist",
						pg_encoding_to_char(src_encoding),
						pg_encoding_to_char(dest_encoding))));

	/*
	 * Allocate space for conversion result, being wary of integer overflow.
	 *
	 * len * MAX_CONVERSION_GROWTH is typically a vast overestimate of the
	 * required space, so it might exceed MaxAllocSize even though the result
	 * would actually fit.  We do not want to hand back a result string that
	 * exceeds MaxAllocSize, because callers might not cope gracefully --- but
	 * if we just allocate more than that, and don't use it, that's fine.
	 */
	if ((Size) len >= (MaxAllocHugeSize / (Size) MAX_CONVERSION_GROWTH))
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("out of memory"),
				 errdetail("String of %d bytes is too long for encoding conversion.",
						   len)));

	result = (unsigned char *)
		MemoryContextAllocHuge(CurrentMemoryContext,
							   (Size) len * MAX_CONVERSION_GROWTH + 1);

	(void) OidFunctionCall6(proc,
							Int32GetDatum(src_encoding),
							Int32GetDatum(dest_encoding),
							CStringGetDatum((char *) src),
							CStringGetDatum((char *) result),
							Int32GetDatum(len),
							BoolGetDatum(false));

	/*
	 * If the result is large, it's worth repalloc'ing to release any extra
	 * space we asked for.  The cutoff here is somewhat arbitrary, but we
	 * *must* check when len * MAX_CONVERSION_GROWTH exceeds MaxAllocSize.
	 */
	if (len > 1000000)
	{
		Size		resultlen = strlen((char *) result);

		if (resultlen >= MaxAllocSize)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("out of memory"),
					 errdetail("String of %d bytes is too long for encoding conversion.",
							   len)));

		result = (unsigned char *) repalloc(result, resultlen + 1);
	}

	return result;
}

/*
 * Convert src string to another encoding.
 *
 * This function has a different API than the other conversion functions.
 * The caller should've looked up the conversion function using
 * FindDefaultConversionProc().  Unlike the other functions, the converted
 * result is not palloc'd.  It is written to the caller-supplied buffer
 * instead.
 *
 * src_encoding   - encoding to convert from
 * dest_encoding  - encoding to convert to
 * src, srclen    - input buffer and its length in bytes
 * dest, destlen  - destination buffer and its size in bytes
 *
 * The output is null-terminated.
 *
 * If destlen < srclen * MAX_CONVERSION_INPUT_LENGTH + 1, the converted output
 * wouldn't necessarily fit in the output buffer, and the function will not
 * convert the whole input.
 *
 * TODO: The conversion function interface is not great.  Firstly, it
 * would be nice to pass through the destination buffer size to the
 * conversion function, so that if you pass a shorter destination buffer, it
 * could still continue to fill up the whole buffer.  Currently, we have to
 * assume worst case expansion and stop the conversion short, even if there
 * is in fact space left in the destination buffer.  Secondly, it would be
 * nice to return the number of bytes written to the caller, to avoid a call
 * to strlen().
 */
int
pg_do_encoding_conversion_buf(Oid proc,
							  int src_encoding,
							  int dest_encoding,
							  unsigned char *src, int srclen,
							  unsigned char *dest, int destlen,
							  bool noError)
{
	Datum		result;

	/*
	 * If the destination buffer is not large enough to hold the result in the
	 * worst case, limit the input size passed to the conversion function.
	 */
	if ((Size) srclen >= ((destlen - 1) / (Size) MAX_CONVERSION_GROWTH))
		srclen = ((destlen - 1) / (Size) MAX_CONVERSION_GROWTH);

	result = OidFunctionCall6(proc,
							  Int32GetDatum(src_encoding),
							  Int32GetDatum(dest_encoding),
							  CStringGetDatum((char *) src),
							  CStringGetDatum((char *) dest),
							  Int32GetDatum(srclen),
							  BoolGetDatum(noError));
	return DatumGetInt32(result);
}

/*
 * Convert string to encoding encoding_name. The source
 * encoding is the DB encoding.
 *
 * BYTEA convert_to(TEXT string, NAME encoding_name) */
Datum
pg_convert_to(PG_FUNCTION_ARGS)
{
	Datum		string = PG_GETARG_DATUM(0);
	Datum		dest_encoding_name = PG_GETARG_DATUM(1);
	Datum		src_encoding_name = DirectFunctionCall1(namein,
														CStringGetDatum(DatabaseEncoding->name));
	Datum		result;

	/*
	 * pg_convert expects a bytea as its first argument. We're passing it a
	 * text argument here, relying on the fact that they are both in fact
	 * varlena types, and thus structurally identical.
	 */
	result = DirectFunctionCall3(pg_convert, string,
								 src_encoding_name, dest_encoding_name);

	PG_RETURN_DATUM(result);
}

/*
 * Convert string from encoding encoding_name. The destination
 * encoding is the DB encoding.
 *
 * TEXT convert_from(BYTEA string, NAME encoding_name) */
Datum
pg_convert_from(PG_FUNCTION_ARGS)
{
	Datum		string = PG_GETARG_DATUM(0);
	Datum		src_encoding_name = PG_GETARG_DATUM(1);
	Datum		dest_encoding_name = DirectFunctionCall1(namein,
														 CStringGetDatum(DatabaseEncoding->name));
	Datum		result;

	result = DirectFunctionCall3(pg_convert, string,
								 src_encoding_name, dest_encoding_name);

	/*
	 * pg_convert returns a bytea, which we in turn return as text, relying on
	 * the fact that they are both in fact varlena types, and thus
	 * structurally identical. Although not all bytea values are valid text,
	 * in this case it will be because we've told pg_convert to return one
	 * that is valid as text in the current database encoding.
	 */
	PG_RETURN_DATUM(result);
}

/*
 * Convert string between two arbitrary encodings.
 *
 * BYTEA convert(BYTEA string, NAME src_encoding_name, NAME dest_encoding_name)
 */
Datum
pg_convert(PG_FUNCTION_ARGS)
{
	bytea	   *string = PG_GETARG_BYTEA_PP(0);
	char	   *src_encoding_name = NameStr(*PG_GETARG_NAME(1));
	int			src_encoding = pg_char_to_encoding(src_encoding_name);
	char	   *dest_encoding_name = NameStr(*PG_GETARG_NAME(2));
	int			dest_encoding = pg_char_to_encoding(dest_encoding_name);
	const char *src_str;
	char	   *dest_str;
	bytea	   *retval;
	int			len;

	if (src_encoding < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid source encoding name \"%s\"",
						src_encoding_name)));
	if (dest_encoding < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid destination encoding name \"%s\"",
						dest_encoding_name)));

	/* make sure that source string is valid */
	len = VARSIZE_ANY_EXHDR(string);
	src_str = VARDATA_ANY(string);
	(void) pg_verify_mbstr(src_encoding, src_str, len, false);

	/* perform conversion */
	dest_str = (char *) pg_do_encoding_conversion((unsigned char *) unconstify(char *, src_str),
												  len,
												  src_encoding,
												  dest_encoding);


	/* return source string if no conversion happened */
	if (dest_str == src_str)
		PG_RETURN_BYTEA_P(string);

	/*
	 * build bytea data type structure.
	 */
	len = strlen(dest_str);
	retval = (bytea *) palloc(len + VARHDRSZ);
	SET_VARSIZE(retval, len + VARHDRSZ);
	memcpy(VARDATA(retval), dest_str, len);
	pfree(dest_str);

	/* free memory if allocated by the toaster */
	PG_FREE_IF_COPY(string, 0);

	PG_RETURN_BYTEA_P(retval);
}

/*
 * get the length of the string considered as text in the specified
 * encoding. Raises an error if the data is not valid in that
 * encoding.
 *
 * INT4 length (BYTEA string, NAME src_encoding_name)
 */
Datum
length_in_encoding(PG_FUNCTION_ARGS)
{
	bytea	   *string = PG_GETARG_BYTEA_PP(0);
	char	   *src_encoding_name = NameStr(*PG_GETARG_NAME(1));
	int			src_encoding = pg_char_to_encoding(src_encoding_name);
	const char *src_str;
	int			len;
	int			retval;

	if (src_encoding < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid encoding name \"%s\"",
						src_encoding_name)));

	len = VARSIZE_ANY_EXHDR(string);
	src_str = VARDATA_ANY(string);

	retval = pg_verify_mbstr_len(src_encoding, src_str, len, false);

	PG_RETURN_INT32(retval);
}

/*
 * Get maximum multibyte character length in the specified encoding.
 *
 * Note encoding is specified numerically, not by name as above.
 */
Datum
pg_encoding_max_length_sql(PG_FUNCTION_ARGS)
{
	int			encoding = PG_GETARG_INT32(0);

	if (PG_VALID_ENCODING(encoding))
		PG_RETURN_INT32(pg_wchar_table[encoding].maxmblen);
	else
		PG_RETURN_NULL();
}

/*
 * Convert client encoding to server encoding.
 *
 * See the notes about string conversion functions at the top of this file.
 */
char *
pg_client_to_server(const char *s, int len)
{
	return pg_any_to_server(s, len, ClientEncoding->encoding);
}

/*
 * Convert any encoding to server encoding.
 *
 * See the notes about string conversion functions at the top of this file.
 *
 * Unlike the other string conversion functions, this will apply validation
 * even if encoding == DatabaseEncoding->encoding.  This is because this is
 * used to process data coming in from outside the database, and we never
 * want to just assume validity.
 */
char *
pg_any_to_server(const char *s, int len, int encoding)
{
	if (len <= 0)
		return unconstify(char *, s);	/* empty string is always valid */

	if (encoding == DatabaseEncoding->encoding ||
		encoding == PG_SQL_ASCII)
	{
		/*
		 * No conversion is needed, but we must still validate the data.
		 */
		(void) pg_verify_mbstr(DatabaseEncoding->encoding, s, len, false);
		return unconstify(char *, s);
	}

	if (DatabaseEncoding->encoding == PG_SQL_ASCII)
	{
		/*
		 * No conversion is possible, but we must still validate the data,
		 * because the client-side code might have done string escaping using
		 * the selected client_encoding.  If the client encoding is ASCII-safe
		 * then we just do a straight validation under that encoding.  For an
		 * ASCII-unsafe encoding we have a problem: we dare not pass such data
		 * to the parser but we have no way to convert it.  We compromise by
		 * rejecting the data if it contains any non-ASCII characters.
		 */
		if (PG_VALID_BE_ENCODING(encoding))
			(void) pg_verify_mbstr(encoding, s, len, false);
		else
		{
			int			i;

			for (i = 0; i < len; i++)
			{
				if (s[i] == '\0' || IS_HIGHBIT_SET(s[i]))
					ereport(ERROR,
							(errcode(ERRCODE_CHARACTER_NOT_IN_REPERTOIRE),
							 errmsg("invalid byte value for encoding \"%s\": 0x%02x",
									pg_enc2name_tbl[PG_SQL_ASCII].name,
									(unsigned char) s[i])));
			}
		}
		return unconstify(char *, s);
	}

	/* Fast path if we can use cached conversion function */
	if (encoding == ClientEncoding->encoding)
		return perform_default_encoding_conversion(s, len, true);

	/* General case ... will not work outside transactions */
	return (char *) pg_do_encoding_conversion((unsigned char *) unconstify(char *, s),
											  len,
											  encoding,
											  DatabaseEncoding->encoding);
}

/*
 * Convert server encoding to client encoding.
 *
 * See the notes about string conversion functions at the top of this file.
 */
char *
pg_server_to_client(const char *s, int len)
{
	return pg_server_to_any(s, len, ClientEncoding->encoding);
}

/*
 * Convert server encoding to any encoding.
 *
 * See the notes about string conversion functions at the top of this file.
 */
char *
pg_server_to_any(const char *s, int len, int encoding)
{
	if (len <= 0)
		return unconstify(char *, s);	/* empty string is always valid */

	if (encoding == DatabaseEncoding->encoding ||
		encoding == PG_SQL_ASCII)
		return unconstify(char *, s);	/* assume data is valid */

	if (DatabaseEncoding->encoding == PG_SQL_ASCII)
	{
		/* No conversion is possible, but we must validate the result */
		(void) pg_verify_mbstr(encoding, s, len, false);
		return unconstify(char *, s);
	}

	/* Fast path if we can use cached conversion function */
	if (encoding == ClientEncoding->encoding)
		return perform_default_encoding_conversion(s, len, false);

	/* General case ... will not work outside transactions */
	return (char *) pg_do_encoding_conversion((unsigned char *) unconstify(char *, s),
											  len,
											  DatabaseEncoding->encoding,
											  encoding);
}

/*
 *	Perform default encoding conversion using cached FmgrInfo. Since
 *	this function does not access database at all, it is safe to call
 *	outside transactions.  If the conversion has not been set up by
 *	SetClientEncoding(), no conversion is performed.
 */
static char *
perform_default_encoding_conversion(const char *src, int len,
									bool is_client_to_server)
{
	char	   *result;
	int			src_encoding,
				dest_encoding;
	FmgrInfo   *flinfo;

	if (is_client_to_server)
	{
		src_encoding = ClientEncoding->encoding;
		dest_encoding = DatabaseEncoding->encoding;
		flinfo = ToServerConvProc;
	}
	else
	{
		src_encoding = DatabaseEncoding->encoding;
		dest_encoding = ClientEncoding->encoding;
		flinfo = ToClientConvProc;
	}

	if (flinfo == NULL)
		return unconstify(char *, src);

	/*
	 * Allocate space for conversion result, being wary of integer overflow.
	 * See comments in pg_do_encoding_conversion.
	 */
	if ((Size) len >= (MaxAllocHugeSize / (Size) MAX_CONVERSION_GROWTH))
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("out of memory"),
				 errdetail("String of %d bytes is too long for encoding conversion.",
						   len)));

	result = (char *)
		MemoryContextAllocHuge(CurrentMemoryContext,
							   (Size) len * MAX_CONVERSION_GROWTH + 1);

	FunctionCall6(flinfo,
				  Int32GetDatum(src_encoding),
				  Int32GetDatum(dest_encoding),
				  CStringGetDatum(src),
				  CStringGetDatum(result),
				  Int32GetDatum(len),
				  BoolGetDatum(false));

	/*
	 * Release extra space if there might be a lot --- see comments in
	 * pg_do_encoding_conversion.
	 */
	if (len > 1000000)
	{
		Size		resultlen = strlen(result);

		if (resultlen >= MaxAllocSize)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("out of memory"),
					 errdetail("String of %d bytes is too long for encoding conversion.",
							   len)));

		result = (char *) repalloc(result, resultlen + 1);
	}

	return result;
}

/*
 * Convert a single Unicode code point into a string in the server encoding.
 *
 * The code point given by "c" is converted and stored at *s, which must
 * have at least MAX_UNICODE_EQUIVALENT_STRING+1 bytes available.
 * The output will have a trailing '\0'.  Throws error if the conversion
 * cannot be performed.
 *
 * Note that this relies on having previously looked up any required
 * conversion function.  That's partly for speed but mostly because the parser
 * may call this outside any transaction, or in an aborted transaction.
 */
void
pg_unicode_to_server(pg_wchar c, unsigned char *s)
{
	unsigned char c_as_utf8[MAX_MULTIBYTE_CHAR_LEN + 1];
	int			c_as_utf8_len;
	int			server_encoding;

	/*
	 * Complain if invalid Unicode code point.  The choice of errcode here is
	 * debatable, but really our caller should have checked this anyway.
	 */
	if (!is_valid_unicode_codepoint(c))
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("invalid Unicode code point")));

	/* Otherwise, if it's in ASCII range, conversion is trivial */
	if (c <= 0x7F)
	{
		s[0] = (unsigned char) c;
		s[1] = '\0';
		return;
	}

	/* If the server encoding is UTF-8, we just need to reformat the code */
	server_encoding = GetDatabaseEncoding();
	if (server_encoding == PG_UTF8)
	{
		unicode_to_utf8(c, s);
		s[pg_utf_mblen(s)] = '\0';
		return;
	}

	/* For all other cases, we must have a conversion function available */
	if (Utf8ToServerConvProc == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("conversion between %s and %s is not supported",
						pg_enc2name_tbl[PG_UTF8].name,
						GetDatabaseEncodingName())));

	/* Construct UTF-8 source string */
	unicode_to_utf8(c, c_as_utf8);
	c_as_utf8_len = pg_utf_mblen(c_as_utf8);
	c_as_utf8[c_as_utf8_len] = '\0';

	/* Convert, or throw error if we can't */
	FunctionCall6(Utf8ToServerConvProc,
				  Int32GetDatum(PG_UTF8),
				  Int32GetDatum(server_encoding),
				  CStringGetDatum((char *) c_as_utf8),
				  CStringGetDatum((char *) s),
				  Int32GetDatum(c_as_utf8_len),
				  BoolGetDatum(false));
}

/*
 * Convert a single Unicode code point into a string in the server encoding.
 *
 * Same as pg_unicode_to_server(), except that we don't throw errors,
 * but simply return false on conversion failure.
 */
bool
pg_unicode_to_server_noerror(pg_wchar c, unsigned char *s)
{
	unsigned char c_as_utf8[MAX_MULTIBYTE_CHAR_LEN + 1];
	int			c_as_utf8_len;
	int			converted_len;
	int			server_encoding;

	/* Fail if invalid Unicode code point */
	if (!is_valid_unicode_codepoint(c))
		return false;

	/* Otherwise, if it's in ASCII range, conversion is trivial */
	if (c <= 0x7F)
	{
		s[0] = (unsigned char) c;
		s[1] = '\0';
		return true;
	}

	/* If the server encoding is UTF-8, we just need to reformat the code */
	server_encoding = GetDatabaseEncoding();
	if (server_encoding == PG_UTF8)
	{
		unicode_to_utf8(c, s);
		s[pg_utf_mblen(s)] = '\0';
		return true;
	}

	/* For all other cases, we must have a conversion function available */
	if (Utf8ToServerConvProc == NULL)
		return false;

	/* Construct UTF-8 source string */
	unicode_to_utf8(c, c_as_utf8);
	c_as_utf8_len = pg_utf_mblen(c_as_utf8);
	c_as_utf8[c_as_utf8_len] = '\0';

	/* Convert, but without throwing error if we can't */
	converted_len = DatumGetInt32(FunctionCall6(Utf8ToServerConvProc,
												Int32GetDatum(PG_UTF8),
												Int32GetDatum(server_encoding),
												CStringGetDatum((char *) c_as_utf8),
												CStringGetDatum((char *) s),
												Int32GetDatum(c_as_utf8_len),
												BoolGetDatum(true)));

	/* Conversion was successful iff it consumed the whole input */
	return (converted_len == c_as_utf8_len);
}


/* convert a multibyte string to a wchar */
int
pg_mb2wchar(const char *from, pg_wchar *to)
{
	return pg_wchar_table[DatabaseEncoding->encoding].mb2wchar_with_len((const unsigned char *) from, to, strlen(from));
}

/* convert a multibyte string to a wchar with a limited length */
int
pg_mb2wchar_with_len(const char *from, pg_wchar *to, int len)
{
	return pg_wchar_table[DatabaseEncoding->encoding].mb2wchar_with_len((const unsigned char *) from, to, len);
}

/* same, with any encoding */
int
pg_encoding_mb2wchar_with_len(int encoding,
							  const char *from, pg_wchar *to, int len)
{
	return pg_wchar_table[encoding].mb2wchar_with_len((const unsigned char *) from, to, len);
}

/* convert a wchar string to a multibyte */
int
pg_wchar2mb(const pg_wchar *from, char *to)
{
	return pg_wchar_table[DatabaseEncoding->encoding].wchar2mb_with_len(from, (unsigned char *) to, pg_wchar_strlen(from));
}

/* convert a wchar string to a multibyte with a limited length */
int
pg_wchar2mb_with_len(const pg_wchar *from, char *to, int len)
{
	return pg_wchar_table[DatabaseEncoding->encoding].wchar2mb_with_len(from, (unsigned char *) to, len);
}

/* same, with any encoding */
int
pg_encoding_wchar2mb_with_len(int encoding,
							  const pg_wchar *from, char *to, int len)
{
	return pg_wchar_table[encoding].wchar2mb_with_len(from, (unsigned char *) to, len);
}

/* returns the byte length of a multibyte character */
int
pg_mblen(const char *mbstr)
{
	return pg_wchar_table[DatabaseEncoding->encoding].mblen((const unsigned char *) mbstr);
}

/* returns the display length of a multibyte character */
int
pg_dsplen(const char *mbstr)
{
	return pg_wchar_table[DatabaseEncoding->encoding].dsplen((const unsigned char *) mbstr);
}

/* returns the length (counted in wchars) of a multibyte string */
int
pg_mbstrlen(const char *mbstr)
{
	int			len = 0;

	/* optimization for single byte encoding */
	if (pg_database_encoding_max_length() == 1)
		return strlen(mbstr);

	while (*mbstr)
	{
		mbstr += pg_mblen(mbstr);
		len++;
	}
	return len;
}

/* returns the length (counted in wchars) of a multibyte string
 * (not necessarily NULL terminated)
 */
int
pg_mbstrlen_with_len(const char *mbstr, int limit)
{
	int			len = 0;

	/* optimization for single byte encoding */
	if (pg_database_encoding_max_length() == 1)
		return limit;

	while (limit > 0 && *mbstr)
	{
		int			l = pg_mblen(mbstr);

		limit -= l;
		mbstr += l;
		len++;
	}
	return len;
}

/*
 * returns the byte length of a multibyte string
 * (not necessarily NULL terminated)
 * that is no longer than limit.
 * this function does not break multibyte character boundary.
 */
int
pg_mbcliplen(const char *mbstr, int len, int limit)
{
	return pg_encoding_mbcliplen(DatabaseEncoding->encoding, mbstr,
								 len, limit);
}

/*
 * pg_mbcliplen with specified encoding
 */
int
pg_encoding_mbcliplen(int encoding, const char *mbstr,
					  int len, int limit)
{
	mblen_converter mblen_fn;
	int			clen = 0;
	int			l;

	/* optimization for single byte encoding */
	if (pg_encoding_max_length(encoding) == 1)
		return cliplen(mbstr, len, limit);

	mblen_fn = pg_wchar_table[encoding].mblen;

	while (len > 0 && *mbstr)
	{
		l = (*mblen_fn) ((const unsigned char *) mbstr);
		if ((clen + l) > limit)
			break;
		clen += l;
		if (clen == limit)
			break;
		len -= l;
		mbstr += l;
	}
	return clen;
}

/*
 * Similar to pg_mbcliplen except the limit parameter specifies the
 * character length, not the byte length.
 */
int
pg_mbcharcliplen(const char *mbstr, int len, int limit)
{
	int			clen = 0;
	int			nch = 0;
	int			l;

	/* optimization for single byte encoding */
	if (pg_database_encoding_max_length() == 1)
		return cliplen(mbstr, len, limit);

	while (len > 0 && *mbstr)
	{
		l = pg_mblen(mbstr);
		nch++;
		if (nch > limit)
			break;
		clen += l;
		len -= l;
		mbstr += l;
	}
	return clen;
}

/* mbcliplen for any single-byte encoding */
static int
cliplen(const char *str, int len, int limit)
{
	int			l = 0;

	len = Min(len, limit);
	while (l < len && str[l])
		l++;
	return l;
}

void
SetDatabaseEncoding(int encoding)
{
	if (!PG_VALID_BE_ENCODING(encoding))
		elog(ERROR, "invalid database encoding: %d", encoding);

	DatabaseEncoding = &pg_enc2name_tbl[encoding];
	Assert(DatabaseEncoding->encoding == encoding);
}

void
SetMessageEncoding(int encoding)
{
	/* Some calls happen before we can elog()! */
	Assert(PG_VALID_ENCODING(encoding));

	MessageEncoding = &pg_enc2name_tbl[encoding];
	Assert(MessageEncoding->encoding == encoding);
}

#ifdef ENABLE_NLS
/*
 * Make one bind_textdomain_codeset() call, translating a pg_enc to a gettext
 * codeset.  Fails for MULE_INTERNAL, an encoding unknown to gettext; can also
 * fail for gettext-internal causes like out-of-memory.
 */
static bool
raw_pg_bind_textdomain_codeset(const char *domainname, int encoding)
{
	bool		elog_ok = (CurrentMemoryContext != NULL);

	if (!PG_VALID_ENCODING(encoding) || pg_enc2gettext_tbl[encoding] == NULL)
		return false;

	if (bind_textdomain_codeset(domainname,
								pg_enc2gettext_tbl[encoding]) != NULL)
		return true;

	if (elog_ok)
		elog(LOG, "bind_textdomain_codeset failed");
	else
		write_stderr("bind_textdomain_codeset failed");

	return false;
}

/*
 * Bind a gettext message domain to the codeset corresponding to the database
 * encoding.  For SQL_ASCII, instead bind to the codeset implied by LC_CTYPE.
 * Return the MessageEncoding implied by the new settings.
 *
 * On most platforms, gettext defaults to the codeset implied by LC_CTYPE.
 * When that matches the database encoding, we don't need to do anything.  In
 * CREATE DATABASE, we enforce or trust that the locale's codeset matches the
 * database encoding, except for the C locale.  (On Windows, we also permit a
 * discrepancy under the UTF8 encoding.)  For the C locale, explicitly bind
 * gettext to the right codeset.
 *
 * On Windows, gettext defaults to the Windows ANSI code page.  This is a
 * convenient departure for software that passes the strings to Windows ANSI
 * APIs, but we don't do that.  Compel gettext to use database encoding or,
 * failing that, the LC_CTYPE encoding as it would on other platforms.
 *
 * This function is called before elog() and palloc() are usable.
 */
int
pg_bind_textdomain_codeset(const char *domainname)
{
	bool		elog_ok = (CurrentMemoryContext != NULL);
	int			encoding = GetDatabaseEncoding();
	int			new_msgenc;

#ifndef WIN32
	const char *ctype = setlocale(LC_CTYPE, NULL);

	if (pg_strcasecmp(ctype, "C") == 0 || pg_strcasecmp(ctype, "POSIX") == 0)
#endif
		if (encoding != PG_SQL_ASCII &&
			raw_pg_bind_textdomain_codeset(domainname, encoding))
			return encoding;

	new_msgenc = pg_get_encoding_from_locale(NULL, elog_ok);
	if (new_msgenc < 0)
		new_msgenc = PG_SQL_ASCII;

#ifdef WIN32
	if (!raw_pg_bind_textdomain_codeset(domainname, new_msgenc))
		/* On failure, the old message encoding remains valid. */
		return GetMessageEncoding();
#endif

	return new_msgenc;
}
#endif

/*
 * The database encoding, also called the server encoding, represents the
 * encoding of data stored in text-like data types.  Affected types include
 * cstring, text, varchar, name, xml, and json.
 */
int
GetDatabaseEncoding(void)
{
	return DatabaseEncoding->encoding;
}

const char *
GetDatabaseEncodingName(void)
{
	return DatabaseEncoding->name;
}

Datum
getdatabaseencoding(PG_FUNCTION_ARGS)
{
	return DirectFunctionCall1(namein, CStringGetDatum(DatabaseEncoding->name));
}

Datum
pg_client_encoding(PG_FUNCTION_ARGS)
{
	return DirectFunctionCall1(namein, CStringGetDatum(ClientEncoding->name));
}

Datum
PG_char_to_encoding(PG_FUNCTION_ARGS)
{
	Name		s = PG_GETARG_NAME(0);

	PG_RETURN_INT32(pg_char_to_encoding(NameStr(*s)));
}

Datum
PG_encoding_to_char(PG_FUNCTION_ARGS)
{
	int32		encoding = PG_GETARG_INT32(0);
	const char *encoding_name = pg_encoding_to_char(encoding);

	return DirectFunctionCall1(namein, CStringGetDatum(encoding_name));
}

/*
 * gettext() returns messages in this encoding.  This often matches the
 * database encoding, but it differs for SQL_ASCII databases, for processes
 * not attached to a database, and under a database encoding lacking iconv
 * support (MULE_INTERNAL).
 */
int
GetMessageEncoding(void)
{
	return MessageEncoding->encoding;
}


/*
 * Generic character incrementer function.
 *
 * Not knowing anything about the properties of the encoding in use, we just
 * keep incrementing the last byte until we get a validly-encoded result,
 * or we run out of values to try.  We don't bother to try incrementing
 * higher-order bytes, so there's no growth in runtime for wider characters.
 * (If we did try to do that, we'd need to consider the likelihood that 255
 * is not a valid final byte in the encoding.)
 */
static bool
pg_generic_charinc(unsigned char *charptr, int len)
{
	unsigned char *lastbyte = charptr + len - 1;
	mbchar_verifier mbverify;

	/* We can just invoke the character verifier directly. */
	mbverify = pg_wchar_table[GetDatabaseEncoding()].mbverifychar;

	while (*lastbyte < (unsigned char) 255)
	{
		(*lastbyte)++;
		if ((*mbverify) (charptr, len) == len)
			return true;
	}

	return false;
}

/*
 * UTF-8 character incrementer function.
 *
 * For a one-byte character less than 0x7F, we just increment the byte.
 *
 * For a multibyte character, every byte but the first must fall between 0x80
 * and 0xBF; and the first byte must be between 0xC0 and 0xF4.  We increment
 * the last byte that's not already at its maximum value.  If we can't find a
 * byte that's less than the maximum allowable value, we simply fail.  We also
 * need some special-case logic to skip regions used for surrogate pair
 * handling, as those should not occur in valid UTF-8.
 *
 * Note that we don't reset lower-order bytes back to their minimums, since
 * we can't afford to make an exhaustive search (see make_greater_string).
 */
static bool
pg_utf8_increment(unsigned char *charptr, int length)
{
	unsigned char a;
	unsigned char limit;

	switch (length)
	{
		default:
			/* reject lengths 5 and 6 for now */
			return false;
		case 4:
			a = charptr[3];
			if (a < 0xBF)
			{
				charptr[3]++;
				break;
			}
			/* FALL THRU */
		case 3:
			a = charptr[2];
			if (a < 0xBF)
			{
				charptr[2]++;
				break;
			}
			/* FALL THRU */
		case 2:
			a = charptr[1];
			switch (*charptr)
			{
				case 0xED:
					limit = 0x9F;
					break;
				case 0xF4:
					limit = 0x8F;
					break;
				default:
					limit = 0xBF;
					break;
			}
			if (a < limit)
			{
				charptr[1]++;
				break;
			}
			/* FALL THRU */
		case 1:
			a = *charptr;
			if (a == 0x7F || a == 0xDF || a == 0xEF || a == 0xF4)
				return false;
			charptr[0]++;
			break;
	}

	return true;
}

/*
 * EUC-JP character incrementer function.
 *
 * If the sequence starts with SS2 (0x8e), it must be a two-byte sequence
 * representing JIS X 0201 characters with the second byte ranging between
 * 0xa1 and 0xdf.  We just increment the last byte if it's less than 0xdf,
 * and otherwise rewrite the whole sequence to 0xa1 0xa1.
 *
 * If the sequence starts with SS3 (0x8f), it must be a three-byte sequence
 * in which the last two bytes range between 0xa1 and 0xfe.  The last byte
 * is incremented if possible, otherwise the second-to-last byte.
 *
 * If the sequence starts with a value other than the above and its MSB
 * is set, it must be a two-byte sequence representing JIS X 0208 characters
 * with both bytes ranging between 0xa1 and 0xfe.  The last byte is
 * incremented if possible, otherwise the second-to-last byte.
 *
 * Otherwise, the sequence is a single-byte ASCII character. It is
 * incremented up to 0x7f.
 */
static bool
pg_eucjp_increment(unsigned char *charptr, int length)
{
	unsigned char c1,
				c2;
	int			i;

	c1 = *charptr;

	switch (c1)
	{
		case SS2:				/* JIS X 0201 */
			if (length != 2)
				return false;

			c2 = charptr[1];

			if (c2 >= 0xdf)
				charptr[0] = charptr[1] = 0xa1;
			else if (c2 < 0xa1)
				charptr[1] = 0xa1;
			else
				charptr[1]++;
			break;

		case SS3:				/* JIS X 0212 */
			if (length != 3)
				return false;

			for (i = 2; i > 0; i--)
			{
				c2 = charptr[i];
				if (c2 < 0xa1)
				{
					charptr[i] = 0xa1;
					return true;
				}
				else if (c2 < 0xfe)
				{
					charptr[i]++;
					return true;
				}
			}

			/* Out of 3-byte code region */
			return false;

		default:
			if (IS_HIGHBIT_SET(c1)) /* JIS X 0208? */
			{
				if (length != 2)
					return false;

				for (i = 1; i >= 0; i--)
				{
					c2 = charptr[i];
					if (c2 < 0xa1)
					{
						charptr[i] = 0xa1;
						return true;
					}
					else if (c2 < 0xfe)
					{
						charptr[i]++;
						return true;
					}
				}

				/* Out of 2 byte code region */
				return false;
			}
			else
			{					/* ASCII, single byte */
				if (c1 > 0x7e)
					return false;
				(*charptr)++;
			}
			break;
	}

	return true;
}

/*
 * get the character incrementer for the encoding for the current database
 */
mbcharacter_incrementer
pg_database_encoding_character_incrementer(void)
{
	/*
	 * Eventually it might be best to add a field to pg_wchar_table[], but for
	 * now we just use a switch.
	 */
	switch (GetDatabaseEncoding())
	{
		case PG_UTF8:
			return pg_utf8_increment;

		case PG_EUC_JP:
			return pg_eucjp_increment;

		default:
			return pg_generic_charinc;
	}
}

/*
 * fetch maximum length of the encoding for the current database
 */
int
pg_database_encoding_max_length(void)
{
	return pg_wchar_table[GetDatabaseEncoding()].maxmblen;
}

/*
 * Verify mbstr to make sure that it is validly encoded in the current
 * database encoding.  Otherwise same as pg_verify_mbstr().
 */
bool
pg_verifymbstr(const char *mbstr, int len, bool noError)
{
	return pg_verify_mbstr(GetDatabaseEncoding(), mbstr, len, noError);
}

/*
 * Verify mbstr to make sure that it is validly encoded in the specified
 * encoding.
 */
bool
pg_verify_mbstr(int encoding, const char *mbstr, int len, bool noError)
{
	int			oklen;

	Assert(PG_VALID_ENCODING(encoding));

	oklen = pg_wchar_table[encoding].mbverifystr((const unsigned char *) mbstr, len);
	if (oklen != len)
	{
		if (noError)
			return false;
		report_invalid_encoding(encoding, mbstr + oklen, len - oklen);
	}
	return true;
}

/*
 * Verify mbstr to make sure that it is validly encoded in the specified
 * encoding.
 *
 * mbstr is not necessarily zero terminated; length of mbstr is
 * specified by len.
 *
 * If OK, return length of string in the encoding.
 * If a problem is found, return -1 when noError is
 * true; when noError is false, ereport() a descriptive message.
 *
 * Note: We cannot use the faster encoding-specific mbverifystr() function
 * here, because we need to count the number of characters in the string.
 */
int
pg_verify_mbstr_len(int encoding, const char *mbstr, int len, bool noError)
{
	mbchar_verifier mbverifychar;
	int			mb_len;

	Assert(PG_VALID_ENCODING(encoding));

	/*
	 * In single-byte encodings, we need only reject nulls (\0).
	 */
	if (pg_encoding_max_length(encoding) <= 1)
	{
		const char *nullpos = memchr(mbstr, 0, len);

		if (nullpos == NULL)
			return len;
		if (noError)
			return -1;
		report_invalid_encoding(encoding, nullpos, 1);
	}

	/* fetch function pointer just once */
	mbverifychar = pg_wchar_table[encoding].mbverifychar;

	mb_len = 0;

	while (len > 0)
	{
		int			l;

		/* fast path for ASCII-subset characters */
		if (!IS_HIGHBIT_SET(*mbstr))
		{
			if (*mbstr != '\0')
			{
				mb_len++;
				mbstr++;
				len--;
				continue;
			}
			if (noError)
				return -1;
			report_invalid_encoding(encoding, mbstr, len);
		}

		l = (*mbverifychar) ((const unsigned char *) mbstr, len);

		if (l < 0)
		{
			if (noError)
				return -1;
			report_invalid_encoding(encoding, mbstr, len);
		}

		mbstr += l;
		len -= l;
		mb_len++;
	}
	return mb_len;
}

/*
 * check_encoding_conversion_args: check arguments of a conversion function
 *
 * "expected" arguments can be either an encoding ID or -1 to indicate that
 * the caller will check whether it accepts the ID.
 *
 * Note: the errors here are not really user-facing, so elog instead of
 * ereport seems sufficient.  Also, we trust that the "expected" encoding
 * arguments are valid encoding IDs, but we don't trust the actuals.
 */
void
check_encoding_conversion_args(int src_encoding,
							   int dest_encoding,
							   int len,
							   int expected_src_encoding,
							   int expected_dest_encoding)
{
	if (!PG_VALID_ENCODING(src_encoding))
		elog(ERROR, "invalid source encoding ID: %d", src_encoding);
	if (src_encoding != expected_src_encoding && expected_src_encoding >= 0)
		elog(ERROR, "expected source encoding \"%s\", but got \"%s\"",
			 pg_enc2name_tbl[expected_src_encoding].name,
			 pg_enc2name_tbl[src_encoding].name);
	if (!PG_VALID_ENCODING(dest_encoding))
		elog(ERROR, "invalid destination encoding ID: %d", dest_encoding);
	if (dest_encoding != expected_dest_encoding && expected_dest_encoding >= 0)
		elog(ERROR, "expected destination encoding \"%s\", but got \"%s\"",
			 pg_enc2name_tbl[expected_dest_encoding].name,
			 pg_enc2name_tbl[dest_encoding].name);
	if (len < 0)
		elog(ERROR, "encoding conversion length must not be negative");
}

/*
 * report_invalid_encoding: complain about invalid multibyte character
 *
 * note: len is remaining length of string, not length of character;
 * len must be greater than zero, as we always examine the first byte.
 */
void
report_invalid_encoding(int encoding, const char *mbstr, int len)
{
	int			l = pg_encoding_mblen(encoding, mbstr);
	char		buf[8 * 5 + 1];
	char	   *p = buf;
	int			j,
				jlimit;

	jlimit = Min(l, len);
	jlimit = Min(jlimit, 8);	/* prevent buffer overrun */

	for (j = 0; j < jlimit; j++)
	{
		p += sprintf(p, "0x%02x", (unsigned char) mbstr[j]);
		if (j < jlimit - 1)
			p += sprintf(p, " ");
	}

	ereport(ERROR,
			(errcode(ERRCODE_CHARACTER_NOT_IN_REPERTOIRE),
			 errmsg("invalid byte sequence for encoding \"%s\": %s",
					pg_enc2name_tbl[encoding].name,
					buf)));
}

/*
 * report_untranslatable_char: complain about untranslatable character
 *
 * note: len is remaining length of string, not length of character;
 * len must be greater than zero, as we always examine the first byte.
 */
void
report_untranslatable_char(int src_encoding, int dest_encoding,
						   const char *mbstr, int len)
{
	int			l = pg_encoding_mblen(src_encoding, mbstr);
	char		buf[8 * 5 + 1];
	char	   *p = buf;
	int			j,
				jlimit;

	jlimit = Min(l, len);
	jlimit = Min(jlimit, 8);	/* prevent buffer overrun */

	for (j = 0; j < jlimit; j++)
	{
		p += sprintf(p, "0x%02x", (unsigned char) mbstr[j]);
		if (j < jlimit - 1)
			p += sprintf(p, " ");
	}

	ereport(ERROR,
			(errcode(ERRCODE_UNTRANSLATABLE_CHARACTER),
			 errmsg("character with byte sequence %s in encoding \"%s\" has no equivalent in encoding \"%s\"",
					buf,
					pg_enc2name_tbl[src_encoding].name,
					pg_enc2name_tbl[dest_encoding].name)));
}


#ifdef WIN32
/*
 * Convert from MessageEncoding to a palloc'ed, null-terminated utf16
 * string. The character length is also passed to utf16len if not
 * null. Returns NULL iff failed. Before MessageEncoding initialization, "str"
 * should be ASCII-only; this will function as though MessageEncoding is UTF8.
 */
WCHAR *
pgwin32_message_to_UTF16(const char *str, int len, int *utf16len)
{
	int			msgenc = GetMessageEncoding();
	WCHAR	   *utf16;
	int			dstlen;
	UINT		codepage;

	if (msgenc == PG_SQL_ASCII)
		/* No conversion is possible, and SQL_ASCII is never utf16. */
		return NULL;

	codepage = pg_enc2name_tbl[msgenc].codepage;

	/*
	 * Use MultiByteToWideChar directly if there is a corresponding codepage,
	 * or double conversion through UTF8 if not.  Double conversion is needed,
	 * for example, in an ENCODING=LATIN8, LC_CTYPE=C database.
	 */
	if (codepage != 0)
	{
		utf16 = (WCHAR *) palloc(sizeof(WCHAR) * (len + 1));
		dstlen = MultiByteToWideChar(codepage, 0, str, len, utf16, len);
		utf16[dstlen] = (WCHAR) 0;
	}
	else
	{
		char	   *utf8;

		/*
		 * XXX pg_do_encoding_conversion() requires a transaction.  In the
		 * absence of one, hope for the input to be valid UTF8.
		 */
		if (IsTransactionState())
		{
			utf8 = (char *) pg_do_encoding_conversion((unsigned char *) str,
													  len,
													  msgenc,
													  PG_UTF8);
			if (utf8 != str)
				len = strlen(utf8);
		}
		else
			utf8 = (char *) str;

		utf16 = (WCHAR *) palloc(sizeof(WCHAR) * (len + 1));
		dstlen = MultiByteToWideChar(CP_UTF8, 0, utf8, len, utf16, len);
		utf16[dstlen] = (WCHAR) 0;

		if (utf8 != str)
			pfree(utf8);
	}

	if (dstlen == 0 && len > 0)
	{
		pfree(utf16);
		return NULL;			/* error */
	}

	if (utf16len)
		*utf16len = dstlen;
	return utf16;
}

#endif							/* WIN32 */
