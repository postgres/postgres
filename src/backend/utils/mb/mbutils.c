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
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
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
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/syscache.h"

/*
 * When converting strings between different encodings, we assume that space
 * for converted result is 4-to-1 growth in the worst case. The rate for
 * currently supported encoding pairs are within 3 (SJIS JIS X0201 half width
 * kanna -> UTF8 is the worst case).  So "4" should be enough for the moment.
 *
 * Note that this is not the same as the maximum character width in any
 * particular encoding.
 */
#define MAX_CONVERSION_GROWTH  4

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
	ListCell   *prev;
	ListCell   *next;

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
	prev = NULL;
	for (lc = list_head(ConvProcList); lc; lc = next)
	{
		ConvProcInfo *convinfo = (ConvProcInfo *) lfirst(lc);

		next = lnext(lc);

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
				ConvProcList = list_delete_cell(ConvProcList, lc, prev);
				pfree(convinfo);
				continue;		/* prev mustn't advance */
			}
		}

		prev = lc;
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
	 * Allocate space for conversion result, being wary of integer overflow
	 */
	if ((Size) len >= (MaxAllocSize / (Size) MAX_CONVERSION_GROWTH))
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("out of memory"),
		 errdetail("String of %d bytes is too long for encoding conversion.",
				   len)));

	result = palloc(len * MAX_CONVERSION_GROWTH + 1);

	OidFunctionCall5(proc,
					 Int32GetDatum(src_encoding),
					 Int32GetDatum(dest_encoding),
					 CStringGetDatum(src),
					 CStringGetDatum(result),
					 Int32GetDatum(len));
	return result;
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
	pg_verify_mbstr_len(src_encoding, src_str, len, false);

	/* perform conversion */
	dest_str = (char *) pg_do_encoding_conversion((unsigned char *) src_str,
												  len,
												  src_encoding,
												  dest_encoding);

	/* update len if conversion actually happened */
	if (dest_str != src_str)
		len = strlen(dest_str);

	/*
	 * build bytea data type structure.
	 */
	retval = (bytea *) palloc(len + VARHDRSZ);
	SET_VARSIZE(retval, len + VARHDRSZ);
	memcpy(VARDATA(retval), dest_str, len);

	if (dest_str != src_str)
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
		return (char *) s;		/* empty string is always valid */

	if (encoding == DatabaseEncoding->encoding ||
		encoding == PG_SQL_ASCII)
	{
		/*
		 * No conversion is needed, but we must still validate the data.
		 */
		(void) pg_verify_mbstr(DatabaseEncoding->encoding, s, len, false);
		return (char *) s;
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
		return (char *) s;
	}

	/* Fast path if we can use cached conversion function */
	if (encoding == ClientEncoding->encoding)
		return perform_default_encoding_conversion(s, len, true);

	/* General case ... will not work outside transactions */
	return (char *) pg_do_encoding_conversion((unsigned char *) s,
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
		return (char *) s;		/* empty string is always valid */

	if (encoding == DatabaseEncoding->encoding ||
		encoding == PG_SQL_ASCII)
		return (char *) s;		/* assume data is valid */

	if (DatabaseEncoding->encoding == PG_SQL_ASCII)
	{
		/* No conversion is possible, but we must validate the result */
		(void) pg_verify_mbstr(encoding, s, len, false);
		return (char *) s;
	}

	/* Fast path if we can use cached conversion function */
	if (encoding == ClientEncoding->encoding)
		return perform_default_encoding_conversion(s, len, false);

	/* General case ... will not work outside transactions */
	return (char *) pg_do_encoding_conversion((unsigned char *) s,
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
		return (char *) src;

	/*
	 * Allocate space for conversion result, being wary of integer overflow
	 */
	if ((Size) len >= (MaxAllocSize / (Size) MAX_CONVERSION_GROWTH))
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("out of memory"),
		 errdetail("String of %d bytes is too long for encoding conversion.",
				   len)));

	result = palloc(len * MAX_CONVERSION_GROWTH + 1);

	FunctionCall5(flinfo,
				  Int32GetDatum(src_encoding),
				  Int32GetDatum(dest_encoding),
				  CStringGetDatum(src),
				  CStringGetDatum(result),
				  Int32GetDatum(len));
	return result;
}


/* convert a multibyte string to a wchar */
int
pg_mb2wchar(const char *from, pg_wchar *to)
{
	return (*pg_wchar_table[DatabaseEncoding->encoding].mb2wchar_with_len) ((const unsigned char *) from, to, strlen(from));
}

/* convert a multibyte string to a wchar with a limited length */
int
pg_mb2wchar_with_len(const char *from, pg_wchar *to, int len)
{
	return (*pg_wchar_table[DatabaseEncoding->encoding].mb2wchar_with_len) ((const unsigned char *) from, to, len);
}

/* same, with any encoding */
int
pg_encoding_mb2wchar_with_len(int encoding,
							  const char *from, pg_wchar *to, int len)
{
	return (*pg_wchar_table[encoding].mb2wchar_with_len) ((const unsigned char *) from, to, len);
}

/* convert a wchar string to a multibyte */
int
pg_wchar2mb(const pg_wchar *from, char *to)
{
	return (*pg_wchar_table[DatabaseEncoding->encoding].wchar2mb_with_len) (from, (unsigned char *) to, pg_wchar_strlen(from));
}

/* convert a wchar string to a multibyte with a limited length */
int
pg_wchar2mb_with_len(const pg_wchar *from, char *to, int len)
{
	return (*pg_wchar_table[DatabaseEncoding->encoding].wchar2mb_with_len) (from, (unsigned char *) to, len);
}

/* same, with any encoding */
int
pg_encoding_wchar2mb_with_len(int encoding,
							  const pg_wchar *from, char *to, int len)
{
	return (*pg_wchar_table[encoding].wchar2mb_with_len) (from, (unsigned char *) to, len);
}

/* returns the byte length of a multibyte character */
int
pg_mblen(const char *mbstr)
{
	return ((*pg_wchar_table[DatabaseEncoding->encoding].mblen) ((const unsigned char *) mbstr));
}

/* returns the display length of a multibyte character */
int
pg_dsplen(const char *mbstr)
{
	return ((*pg_wchar_table[DatabaseEncoding->encoding].dsplen) ((const unsigned char *) mbstr));
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
	int			i;

	for (i = 0; pg_enc2gettext_tbl[i].name != NULL; i++)
	{
		if (pg_enc2gettext_tbl[i].encoding == encoding)
		{
			if (bind_textdomain_codeset(domainname,
										pg_enc2gettext_tbl[i].name) != NULL)
				return true;

			if (elog_ok)
				elog(LOG, "bind_textdomain_codeset failed");
			else
				write_stderr("bind_textdomain_codeset failed");

			break;
		}
	}

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

#ifdef WIN32
/*
 * Result is palloc'ed null-terminated utf16 string. The character length
 * is also passed to utf16len if not null. Returns NULL iff failed.
 */
WCHAR *
pgwin32_message_to_UTF16(const char *str, int len, int *utf16len)
{
	WCHAR	   *utf16;
	int			dstlen;
	UINT		codepage;

	codepage = pg_enc2name_tbl[GetMessageEncoding()].codepage;

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
													  GetMessageEncoding(),
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

#endif
