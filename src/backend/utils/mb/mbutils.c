/*
 * This file contains public functions for conversion between
 * client encoding and server internal encoding.
 * (currently mule internal code (mic) is used)
 * Tatsuo Ishii
 * $Id: mbutils.c,v 1.19 2001/08/15 07:07:40 ishii Exp $
 */
#include "postgres.h"

#include "miscadmin.h"
#include "mb/pg_wchar.h"
#include "utils/builtins.h"

static int	client_encoding = -1;
static void (*client_to_mic) ();/* something to MIC */
static void (*client_from_mic) ();		/* MIC to something */
static void (*server_to_mic) ();/* something to MIC */
static void (*server_from_mic) ();		/* MIC to something */

/*
 * find encoding table entry by encoding
 */
pg_encoding_conv_tbl *
pg_get_enc_ent(int encoding)
{
	pg_encoding_conv_tbl *p = pg_conv_tbl;

	for (; p->encoding >= 0; p++)
	{
		if (p->encoding == encoding)
			return (p);
	}
	return (0);
}

/*
 * Find appropriate encoding conversion functions. If no such
 * functions found, returns -1.
 *
 * Arguments:
 *
 * src, dest (in): source and destination encoding ids
 *
 * src_to_mic (out): pointer to a function which converts src to
 * mic/unicode according to dest. if src == mic/unicode or no
 * appropriate function found, set to 0.
 *
 * dest_from_mic (out): pointer to a function which converts
 * mic/unicode to dest according to src. if dest == mic/unicode or no
 * appropriate function found, set to 0.
 */
int
pg_find_encoding_converters(int src, int dest, void (**src_to_mic)(), void (**dest_from_mic)())
{
	if (src == dest)
	{							/* src == dest? */
		*src_to_mic = *dest_from_mic = 0;
	}
	else if (src == MULE_INTERNAL)
	{							/* src == MULE_INETRNAL? */
		*dest_from_mic = pg_get_enc_ent(dest)->from_mic;
		if (*dest_from_mic == 0)
			return (-1);
		*src_to_mic = 0;
	}
	else if (dest == MULE_INTERNAL)
	{							/* dest == MULE_INETRNAL? */
		*src_to_mic = pg_get_enc_ent(src)->to_mic;
		if (*src_to_mic == 0)
			return (-1);
		*dest_from_mic = 0;
	}
	else if (src == UNICODE)
	{							/* src == UNICODE? */
		*dest_from_mic = pg_get_enc_ent(dest)->from_unicode;
		if (*dest_from_mic == 0)
			return (-1);
		*src_to_mic = 0;
	}
	else if (dest == UNICODE)
	{							/* dest == UNICODE? */
		*src_to_mic = pg_get_enc_ent(src)->to_unicode;
		if (*src_to_mic == 0)
			return (-1);
		*dest_from_mic = 0;
	}
	else
	{
		*src_to_mic = pg_get_enc_ent(src)->to_mic;
		*dest_from_mic = pg_get_enc_ent(dest)->from_mic;
		if (*src_to_mic == 0 || *dest_from_mic == 0)
			return (-1);
	}
	return (0);
}

/*
 * set the client encoding. if encoding conversion between
 * client/server encoding is not supported, returns -1
 */
int
pg_set_client_encoding(int encoding)
{
	int current_server_encoding = GetDatabaseEncoding();

	if (pg_find_encoding_converters(encoding, current_server_encoding, &client_to_mic, &server_from_mic) < 0)
		return (-1);
	client_encoding = encoding;

	if (pg_find_encoding_converters(current_server_encoding, encoding, &server_to_mic, &client_from_mic) < 0)
		return (-1);
	return 0;
}

/*
 * returns the current client encoding
 */
int
pg_get_client_encoding()
{
	if (client_encoding == -1)
	{
		/* this is the first time */
		client_encoding = GetDatabaseEncoding();
	}
	return (client_encoding);
}

/*
 * Convert src encoding and returns it. Actual conversion is done by
 * src_to_mic and dest_from_mic, which can be obtained by
 * pg_find_encoding_converters(). The reason we require two conversion
 * functions is that we have an intermediate encoding: MULE_INTERNAL
 * Using intermediate encodings will reduce the number of functions
 * doing encoding conversions. Special case is either src or dest is
 * the intermediate encoding itself. In this case, you don't need src
 * or dest (setting 0 will indicate there's no conversion
 * function). Another case is you have direct-conversion function from
 * src to dest. In this case either src_to_mic or dest_from_mic could
 * be set to 0 also.
 * 
 * Note that If src or dest is UNICODE, we have to do
 * direct-conversion, since we don't support conversion bwteen UNICODE
 * and MULE_INTERNAL, we cannot go through MULE_INTERNAL.
 *
 * CASE 1: if no conversion is required, then the given pointer s is returned.
 *
 * CASE 2: if conversion is required, a palloc'd string is returned.
 *
 * Callers must check whether return value differs from passed value
 * to determine whether to pfree the result or not!
 *
 * Note: we assume that conversion cannot cause more than a 4-to-1 growth
 * in the length of the string --- is this enough?  */

unsigned char *
pg_do_encoding_conversion(unsigned char *src, int len, void (*src_to_mic)(), void (*dest_from_mic)())
{
	unsigned char *result = src;
	unsigned char *buf;

	if (src_to_mic)
	{
		buf = (unsigned char *) palloc(len * 4 + 1);
		(*src_to_mic) (result, buf, len);
		result = buf;
		len = strlen(result);
	}
	if (dest_from_mic)
	{
		buf = (unsigned char *) palloc(len * 4 + 1);
		(*dest_from_mic) (result, buf, len);
		if (result != src)
			pfree(result);		/* release first buffer */
		result = buf;
	}
	return result;
}

/*
 * Convert string using encoding_nanme. We assume that string's
 * encoding is same as DB encoding.
 *
 * TEXT convert(TEXT string, NAME encoding_name)
 */
Datum
pg_convert(PG_FUNCTION_ARGS)
{
	text	*string = PG_GETARG_TEXT_P(0);
	Name	s = PG_GETARG_NAME(1);
	int encoding = pg_char_to_encoding(NameStr(*s));
	int db_encoding = GetDatabaseEncoding();
	void (*src)(), (*dest)();
	unsigned char	*result;
	text	*retval;

	if (encoding < 0)
	    elog(ERROR, "Invalid encoding name %s", NameStr(*s));

	if (pg_find_encoding_converters(db_encoding, encoding, &src, &dest) < 0)
	{
	    char *encoding_name = (char *)pg_encoding_to_char(db_encoding);
	    elog(ERROR, "Conversion from %s to %s is not possible", NameStr(*s), encoding_name);
	}

	result = pg_do_encoding_conversion(VARDATA(string), VARSIZE(string)-VARHDRSZ,
					   src, dest);
	if (result == NULL)
	    elog(ERROR, "Encoding conversion failed");

	retval = DatumGetTextP(DirectFunctionCall1(textin, CStringGetDatum(result)));
	if (result != (unsigned char *)VARDATA(string))
	    pfree(result);

	/* free memory if allocated by the toaster */
	PG_FREE_IF_COPY(string, 0);

	PG_RETURN_TEXT_P(retval);
}

/*
 * Convert string using encoding_nanme.
 *
 * TEXT convert(TEXT string, NAME src_encoding_name, NAME dest_encoding_name)
 */
Datum
pg_convert2(PG_FUNCTION_ARGS)
{
	text	*string = PG_GETARG_TEXT_P(0);
	char *src_encoding_name = NameStr(*PG_GETARG_NAME(1));
	int src_encoding = pg_char_to_encoding(src_encoding_name);
	char *dest_encoding_name = NameStr(*PG_GETARG_NAME(2));
	int dest_encoding = pg_char_to_encoding(dest_encoding_name);
	void (*src)(), (*dest)();
	unsigned char	*result;
	text	*retval;

	if (src_encoding < 0)
	    elog(ERROR, "Invalid source encoding name %s", src_encoding_name);
	if (dest_encoding < 0)
	    elog(ERROR, "Invalid destination encoding name %s", dest_encoding_name);

	if (pg_find_encoding_converters(src_encoding, dest_encoding, &src, &dest) < 0)
	{
	    elog(ERROR, "Conversion from %s to %s is not possible",
		 src_encoding_name, dest_encoding_name);
	}

	result = pg_do_encoding_conversion(VARDATA(string), VARSIZE(string)-VARHDRSZ,
					   src, dest);
	if (result == NULL)
	    elog(ERROR, "Encoding conversion failed");

	retval = DatumGetTextP(DirectFunctionCall1(textin, CStringGetDatum(result)));
	if (result != (unsigned char *)VARDATA(string))
	    pfree(result);

	/* free memory if allocated by the toaster */
	PG_FREE_IF_COPY(string, 0);

	PG_RETURN_TEXT_P(retval);
}

/*
 * convert client encoding to server encoding.
 *
 * CASE 1: if no conversion is required, then the given pointer s is returned.
 *
 * CASE 2: if conversion is required, a palloc'd string is returned.
 *
 * Callers must check whether return value differs from passed value
 * to determine whether to pfree the result or not!
 *
 * Note: we assume that conversion cannot cause more than a 4-to-1 growth
 * in the length of the string --- is this enough?
 */
unsigned char *
pg_client_to_server(unsigned char *s, int len)
{
	if (client_encoding == GetDatabaseEncoding())
	    return s;

	return pg_do_encoding_conversion(s, len, client_to_mic, server_from_mic);
}

/*
 * convert server encoding to client encoding.
 *
 * CASE 1: if no conversion is required, then the given pointer s is returned.
 *
 * CASE 2: if conversion is required, a palloc'd string is returned.
 *
 * Callers must check whether return value differs from passed value
 * to determine whether to pfree the result or not!
 *
 * Note: we assume that conversion cannot cause more than a 4-to-1 growth
 * in the length of the string --- is this enough?
 */
unsigned char *
pg_server_to_client(unsigned char *s, int len)
{
	if (client_encoding == GetDatabaseEncoding())
		return s;

	return pg_do_encoding_conversion(s, len, server_to_mic, client_from_mic);
}

/* convert a multi-byte string to a wchar */
int
pg_mb2wchar(const unsigned char *from, pg_wchar * to)
{
	return (*pg_wchar_table[GetDatabaseEncoding()].mb2wchar_with_len) (from, to, strlen(from));
}

/* convert a multi-byte string to a wchar with a limited length */
int
pg_mb2wchar_with_len(const unsigned char *from, pg_wchar * to, int len)
{
	return (*pg_wchar_table[GetDatabaseEncoding()].mb2wchar_with_len) (from, to, len);
}

/* returns the byte length of a multi-byte word */
int
pg_mblen(const unsigned char *mbstr)
{
	return ((*pg_wchar_table[GetDatabaseEncoding()].mblen) (mbstr));
}

/* returns the length (counted as a wchar) of a multi-byte string */
int
pg_mbstrlen(const unsigned char *mbstr)
{
	int			len = 0;

	while (*mbstr)
	{
		mbstr += pg_mblen(mbstr);
		len++;
	}
	return (len);
}

/* returns the length (counted as a wchar) of a multi-byte string
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
 * returns the byte length of a multi-byte string
 * (not necessarily  NULL terminated)
 * that is no longer than limit.
 * this function does not break multi-byte word boundary.
 */
int
pg_mbcliplen(const unsigned char *mbstr, int len, int limit)
{
	int			clen = 0;
	int			l;

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
 * Similar to pg_mbcliplen but the limit parameter specifies the
 * character length, not the byte length.  */
int
pg_mbcharcliplen(const unsigned char *mbstr, int len, int limit)
{
	int			clen = 0;
	int			nch = 0;
	int			l;

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

/*
 * functions for utils/init */
static int	DatabaseEncoding = MULTIBYTE;

void
SetDatabaseEncoding(int encoding)
{
	DatabaseEncoding = encoding;
}

int
GetDatabaseEncoding()
{
	return (DatabaseEncoding);
}

/* for builtin-function */
Datum
getdatabaseencoding(PG_FUNCTION_ARGS)
{
	const char *encoding_name = pg_encoding_to_char(DatabaseEncoding);

	return DirectFunctionCall1(namein, CStringGetDatum(encoding_name));
}
