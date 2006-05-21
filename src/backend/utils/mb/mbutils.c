/*
 * This file contains public functions for conversion between
 * client encoding and server internal encoding.
 * (currently mule internal code (mic) is used)
 * Tatsuo Ishii
 *
 * $Header: /cvsroot/pgsql/src/backend/utils/mb/mbutils.c,v 1.44.2.1 2006/05/21 20:06:44 tgl Exp $
 */
#include "postgres.h"

#include "access/xact.h"
#include "miscadmin.h"
#include "mb/pg_wchar.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/syscache.h"
#include "catalog/namespace.h"

/*
 * We handle for actual FE and BE encoding setting encoding-identificator
 * and encoding-name too. It prevent searching and conversion from encoding
 * to encoding name in getdatabaseencoding() and other routines.
 */
static pg_enc2name *ClientEncoding = &pg_enc2name_tbl[PG_SQL_ASCII];
static pg_enc2name *DatabaseEncoding = &pg_enc2name_tbl[PG_SQL_ASCII];

/*
 * Caches for conversion function info. Note that these values are
 * allocated in TopMemoryContext so that they survive across
 * transactions. See SetClientEncoding() for more details.
 */
static FmgrInfo *ToServerConvProc = NULL;
static FmgrInfo *ToClientConvProc = NULL;

/*
 * During backend startup we can't set client encoding because we (a)
 * can't look up the conversion functions, and (b) may not know the database
 * encoding yet either.  So SetClientEncoding() just accepts anything and
 * remembers it for InitializeClientEncoding() to apply later.
 */
static bool backend_startup_complete = false;
static int	pending_client_encoding = PG_SQL_ASCII;


/* Internal functions */
static unsigned char *perform_default_encoding_conversion(unsigned char *src,
									int len, bool is_client_to_server);
static int	cliplen(const unsigned char *str, int len, int limit);


/*
 * Set the client encoding and save fmgrinfo for the conversion
 * function if necessary.  Returns 0 if okay, -1 if not (bad encoding
 * or can't support conversion)
 */
int
SetClientEncoding(int encoding, bool doit)
{
	int			current_server_encoding;
	Oid			to_server_proc,
				to_client_proc;
	FmgrInfo   *to_server;
	FmgrInfo   *to_client;
	MemoryContext oldcontext;

	if (!PG_VALID_FE_ENCODING(encoding))
		return (-1);

	/* Can't do anything during startup, per notes above */
	if (!backend_startup_complete)
	{
		if (doit)
			pending_client_encoding = encoding;
		return 0;
	}

	current_server_encoding = GetDatabaseEncoding();

	/*
	 * Check for cases that require no conversion function.
	 */
	if (current_server_encoding == encoding ||
		(current_server_encoding == PG_SQL_ASCII ||
		 encoding == PG_SQL_ASCII))
	{
		if (doit)
		{
			ClientEncoding = &pg_enc2name_tbl[encoding];

			if (ToServerConvProc != NULL)
			{
				if (ToServerConvProc->fn_extra)
					pfree(ToServerConvProc->fn_extra);
				pfree(ToServerConvProc);
			}
			ToServerConvProc = NULL;

			if (ToClientConvProc != NULL)
			{
				if (ToClientConvProc->fn_extra)
					pfree(ToClientConvProc->fn_extra);
				pfree(ToClientConvProc);
			}
			ToClientConvProc = NULL;
		}
		return 0;
	}

	/*
	 * If we're not inside a transaction then we can't do catalog lookups,
	 * so fail.  After backend startup, this could only happen if we are
	 * re-reading postgresql.conf due to SIGHUP --- so basically this just
	 * constrains the ability to change client_encoding on the fly from
	 * postgresql.conf.  Which would probably be a stupid thing to do
	 * anyway.
	 */
	if (!IsTransactionState())
		return -1;

	/*
	 * Look up the conversion functions.
	 */
	to_server_proc = FindDefaultConversionProc(encoding,
											   current_server_encoding);
	if (!OidIsValid(to_server_proc))
		return -1;
	to_client_proc = FindDefaultConversionProc(current_server_encoding,
											   encoding);
	if (!OidIsValid(to_client_proc))
		return -1;

	/*
	 * Done if not wanting to actually apply setting.
	 */
	if (!doit)
		return 0;

	/*
	 * load the fmgr info into TopMemoryContext so that it survives
	 * outside transaction.
	 */
	oldcontext = MemoryContextSwitchTo(TopMemoryContext);
	to_server = palloc(sizeof(FmgrInfo));
	to_client = palloc(sizeof(FmgrInfo));
	fmgr_info(to_server_proc, to_server);
	fmgr_info(to_client_proc, to_client);
	MemoryContextSwitchTo(oldcontext);

	ClientEncoding = &pg_enc2name_tbl[encoding];

	if (ToServerConvProc != NULL)
	{
		if (ToServerConvProc->fn_extra)
			pfree(ToServerConvProc->fn_extra);
		pfree(ToServerConvProc);
	}
	ToServerConvProc = to_server;

	if (ToClientConvProc != NULL)
	{
		if (ToClientConvProc->fn_extra)
			pfree(ToClientConvProc->fn_extra);
		pfree(ToClientConvProc);
	}
	ToClientConvProc = to_client;

	return 0;
}

/*
 * Initialize client encoding if necessary.
 *		called from InitPostgres() once during backend starting up.
 */
void
InitializeClientEncoding(void)
{
	Assert(!backend_startup_complete);
	backend_startup_complete = true;

	if (SetClientEncoding(pending_client_encoding, true) < 0)
	{
		/*
		 * Oops, the requested conversion is not available. We couldn't
		 * fail before, but we can now.
		 */
		ereport(FATAL,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("conversion between %s and %s is not supported",
						pg_enc2name_tbl[pending_client_encoding].name,
						GetDatabaseEncodingName())));
	}
}

/*
 * returns the current client encoding */
int
pg_get_client_encoding(void)
{
	Assert(ClientEncoding);
	return (ClientEncoding->encoding);
}

/*
 * returns the current client encoding name
 */
const char *
pg_get_client_encoding_name(void)
{
	Assert(ClientEncoding);
	return (ClientEncoding->name);
}

/*
 * Apply encoding conversion on src and return it. The encoding
 * conversion function is chosen from the pg_conversion system catalog
 * marked as "default". If it is not found in the schema search path,
 * it's taken from pg_catalog schema. If it even is not in the schema,
 * warn and returns src. We cannot raise an error, since it will cause
 * an infinit loop in error message sending.
 *
 * In the case of no conversion, src is returned.
 *
 * XXX We assume that storage for converted result is 4-to-1 growth in
 * the worst case. The rate for currently supported encoding pares are within 3
 * (SJIS JIS X0201 half width kanna -> UTF-8 is the worst case).
 * So "4" should be enough for the moment.
 */
unsigned char *
pg_do_encoding_conversion(unsigned char *src, int len,
						  int src_encoding, int dest_encoding)
{
	unsigned char *result;
	Oid			proc;

	if (!IsTransactionState())
		return src;

	if (src_encoding == dest_encoding)
		return src;

	if (src_encoding == PG_SQL_ASCII || dest_encoding == PG_SQL_ASCII)
		return src;

	if (len <= 0)
		return src;

	proc = FindDefaultConversionProc(src_encoding, dest_encoding);
	if (!OidIsValid(proc))
	{
		ereport(LOG,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
			errmsg("default conversion function for encoding \"%s\" to \"%s\" does not exist",
				   pg_encoding_to_char(src_encoding),
				   pg_encoding_to_char(dest_encoding))));
		return src;
	}

	/*
	 * XXX we should avoid throwing errors in OidFunctionCall. Otherwise
	 * we are going into infinite loop!  So we have to make sure that the
	 * function exists before calling OidFunctionCall.
	 */
	if (!SearchSysCacheExists(PROCOID,
							  ObjectIdGetDatum(proc),
							  0, 0, 0))
	{
		elog(LOG, "cache lookup failed for function %u", proc);
		return src;
	}

	result = palloc(len * 4 + 1);

	OidFunctionCall5(proc,
					 Int32GetDatum(src_encoding),
					 Int32GetDatum(dest_encoding),
					 CStringGetDatum(src),
					 CStringGetDatum(result),
					 Int32GetDatum(len));
	return result;
}

/*
 * Convert string using encoding_nanme. We assume that string's
 * encoding is same as DB encoding.
 *
 * TEXT convert(TEXT string, NAME encoding_name) */
Datum
pg_convert(PG_FUNCTION_ARGS)
{
	Datum		string = PG_GETARG_DATUM(0);
	Datum		dest_encoding_name = PG_GETARG_DATUM(1);
	Datum		src_encoding_name = DirectFunctionCall1(
						namein, CStringGetDatum(DatabaseEncoding->name));
	Datum		result;

	result = DirectFunctionCall3(
			 pg_convert2, string, src_encoding_name, dest_encoding_name);

	/* free memory allocated by namein */
	pfree((void *) src_encoding_name);

	PG_RETURN_TEXT_P(result);
}

/*
 * Convert string using encoding_nanme.
 *
 * TEXT convert2(TEXT string, NAME src_encoding_name, NAME dest_encoding_name)
 */
Datum
pg_convert2(PG_FUNCTION_ARGS)
{
	text	   *string = PG_GETARG_TEXT_P(0);
	char	   *src_encoding_name = NameStr(*PG_GETARG_NAME(1));
	int			src_encoding = pg_char_to_encoding(src_encoding_name);
	char	   *dest_encoding_name = NameStr(*PG_GETARG_NAME(2));
	int			dest_encoding = pg_char_to_encoding(dest_encoding_name);
	unsigned char *result;
	text	   *retval;
	unsigned char *str;
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

	/* make sure that source string is null terminated */
	len = VARSIZE(string) - VARHDRSZ;
	str = palloc(len + 1);
	memcpy(str, VARDATA(string), len);
	*(str + len) = '\0';

	result = pg_do_encoding_conversion(str, len, src_encoding, dest_encoding);
	if (result == NULL)
		elog(ERROR, "encoding conversion failed");

	/*
	 * build text data type structure. we cannot use textin() here, since
	 * textin assumes that input string encoding is same as database
	 * encoding.
	 */
	len = strlen(result) + VARHDRSZ;
	retval = palloc(len);
	VARATT_SIZEP(retval) = len;
	memcpy(VARDATA(retval), result, len - VARHDRSZ);

	if (result != str)
		pfree(result);
	pfree(str);

	/* free memory if allocated by the toaster */
	PG_FREE_IF_COPY(string, 0);

	PG_RETURN_TEXT_P(retval);
}

/*
 * convert client encoding to server encoding.
 */
unsigned char *
pg_client_to_server(unsigned char *s, int len)
{
	Assert(DatabaseEncoding);
	Assert(ClientEncoding);

	if (len <= 0)
		return s;

	if (ClientEncoding->encoding == DatabaseEncoding->encoding ||
		ClientEncoding->encoding == PG_SQL_ASCII)
	{
		/*
		 * No conversion is needed, but we must still validate the data.
		 */
		(void) pg_verify_mbstr(DatabaseEncoding->encoding, s, len, false);
		return s;
	}

	if (DatabaseEncoding->encoding == PG_SQL_ASCII)
	{
		/*
		 * No conversion is possible, but we must still validate the data,
		 * because the client-side code might have done string escaping
		 * using the selected client_encoding.  If the client encoding is
		 * ASCII-safe then we just do a straight validation under that
		 * encoding.  For an ASCII-unsafe encoding we have a problem:
		 * we dare not pass such data to the parser but we have no way
		 * to convert it.  We compromise by rejecting the data if it
		 * contains any non-ASCII characters.
		 */
		if (PG_VALID_BE_ENCODING(ClientEncoding->encoding))
			(void) pg_verify_mbstr(ClientEncoding->encoding, s, len, false);
		else
		{
			int		i;

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
		return s;
	}

	return perform_default_encoding_conversion(s, len, true);
}

/*
 * convert server encoding to client encoding.
 */
unsigned char *
pg_server_to_client(unsigned char *s, int len)
{
	Assert(DatabaseEncoding);
	Assert(ClientEncoding);

	if (len <= 0)
		return s;

	if (ClientEncoding->encoding == DatabaseEncoding->encoding ||
		ClientEncoding->encoding == PG_SQL_ASCII ||
		DatabaseEncoding->encoding == PG_SQL_ASCII)
		return s;		/* assume data is valid */

	return perform_default_encoding_conversion(s, len, false);
}

/*
 *	Perform default encoding conversion using cached FmgrInfo. Since
 *	this function does not access database at all, it is safe to call
 *	outside transactions. Explicit setting client encoding required
 *	before calling this function. Otherwise no conversion is
 *	performed.
*/
static unsigned char *
perform_default_encoding_conversion(unsigned char *src, int len, bool is_client_to_server)
{
	unsigned char *result;
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
		return src;

	result = palloc(len * 4 + 1);

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
pg_mb2wchar(const unsigned char *from, pg_wchar *to)
{
	return (*pg_wchar_table[DatabaseEncoding->encoding].mb2wchar_with_len) (from, to, strlen(from));
}

/* convert a multibyte string to a wchar with a limited length */
int
pg_mb2wchar_with_len(const unsigned char *from, pg_wchar *to, int len)
{
	return (*pg_wchar_table[DatabaseEncoding->encoding].mb2wchar_with_len) (from, to, len);
}

/* returns the byte length of a multibyte word */
int
pg_mblen(const unsigned char *mbstr)
{
	return ((*pg_wchar_table[DatabaseEncoding->encoding].mblen) (mbstr));
}

/* returns the length (counted as a wchar) of a multibyte string */
int
pg_mbstrlen(const unsigned char *mbstr)
{
	int			len = 0;

	/* optimization for single byte encoding */
	if (pg_database_encoding_max_length() == 1)
		return strlen((char *) mbstr);

	while (*mbstr)
	{
		mbstr += pg_mblen(mbstr);
		len++;
	}
	return (len);
}

/* returns the length (counted as a wchar) of a multibyte string
   (not necessarily  NULL terminated) */
int
pg_mbstrlen_with_len(const unsigned char *mbstr, int limit)
{
	int			len = 0;
	int			l;

	while (limit > 0 && *mbstr)
	{
		l = pg_mblen(mbstr);
		limit -= l;
		mbstr += l;
		len++;
	}
	return (len);
}

/*
 * returns the byte length of a multibyte string
 * (not necessarily  NULL terminated)
 * that is no longer than limit.
 * this function does not break multibyte word boundary.
 */
int
pg_mbcliplen(const unsigned char *mbstr, int len, int limit)
{
	int			clen = 0;
	int			l;

	/* optimization for single byte encoding */
	if (pg_database_encoding_max_length() == 1)
		return cliplen(mbstr, len, limit);

	while (len > 0 && *mbstr)
	{
		l = pg_mblen(mbstr);
		if ((clen + l) > limit)
			break;
		clen += l;
		if (clen == limit)
			break;
		len -= l;
		mbstr += l;
	}
	return (clen);
}

/*
 * Similar to pg_mbcliplen except the limit parameter specifies the
 * character length, not the byte length.  */
int
pg_mbcharcliplen(const unsigned char *mbstr, int len, int limit)
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
	return (clen);
}

void
SetDatabaseEncoding(int encoding)
{
	if (!PG_VALID_BE_ENCODING(encoding))
		elog(ERROR, "invalid database encoding");

	DatabaseEncoding = &pg_enc2name_tbl[encoding];
	Assert(DatabaseEncoding->encoding == encoding);
}

void
SetDefaultClientEncoding()
{
	ClientEncoding = &pg_enc2name_tbl[GetDatabaseEncoding()];
}

int
GetDatabaseEncoding(void)
{
	Assert(DatabaseEncoding);
	return (DatabaseEncoding->encoding);
}

const char *
GetDatabaseEncodingName(void)
{
	Assert(DatabaseEncoding);
	return (DatabaseEncoding->name);
}

Datum
getdatabaseencoding(PG_FUNCTION_ARGS)
{
	Assert(DatabaseEncoding);
	return DirectFunctionCall1(namein, CStringGetDatum(DatabaseEncoding->name));
}

Datum
pg_client_encoding(PG_FUNCTION_ARGS)
{
	Assert(ClientEncoding);
	return DirectFunctionCall1(namein, CStringGetDatum(ClientEncoding->name));
}

static int
cliplen(const unsigned char *str, int len, int limit)
{
	int			l = 0;
	const unsigned char *s;

	for (s = str; *s; s++, l++)
	{
		if (l >= len || l >= limit)
			return l;
	}
	return (s - str);
}
