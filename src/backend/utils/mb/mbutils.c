/*
 * This file contains public functions for conversion between
 * client encoding and server internal encoding.
 * (currently mule internal code (mic) is used)
 * Tatsuo Ishii
 * $Id: mbutils.c,v 1.10 2000/06/13 07:35:12 tgl Exp $ */


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
static pg_encoding_conv_tbl *
get_enc_ent(int encoding)
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
 * set the client encoding. if client/server encoding is
 * not supported, returns -1
 */
int
pg_set_client_encoding(int encoding)
{
	int			current_server_encoding = GetDatabaseEncoding();

	client_encoding = encoding;

	if (client_encoding == current_server_encoding)
	{							/* server == client? */
		client_to_mic = client_from_mic = 0;
		server_to_mic = server_from_mic = 0;
	}
	else if (current_server_encoding == MULE_INTERNAL)
	{							/* server == MULE_INETRNAL? */
		client_to_mic = get_enc_ent(encoding)->to_mic;
		client_from_mic = get_enc_ent(encoding)->from_mic;
		server_to_mic = server_from_mic = 0;
		if (client_to_mic == 0 || client_from_mic == 0)
			return (-1);
	}
	else if (encoding == MULE_INTERNAL)
	{							/* client == MULE_INETRNAL? */
		client_to_mic = client_from_mic = 0;
		server_to_mic = get_enc_ent(current_server_encoding)->to_mic;
		server_from_mic = get_enc_ent(current_server_encoding)->from_mic;
		if (server_to_mic == 0 || server_from_mic == 0)
			return (-1);
	}
	else
	{
		client_to_mic = get_enc_ent(encoding)->to_mic;
		client_from_mic = get_enc_ent(encoding)->from_mic;
		server_to_mic = get_enc_ent(current_server_encoding)->to_mic;
		server_from_mic = get_enc_ent(current_server_encoding)->from_mic;
		if (client_to_mic == 0 || client_from_mic == 0)
			return (-1);
		if (server_to_mic == 0 || server_from_mic == 0)
			return (-1);
	}
	return (0);
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
	unsigned char *result = s;
	unsigned char *buf;

	if (client_encoding == GetDatabaseEncoding())
		return result;
	if (client_to_mic)
	{
		buf = (unsigned char *) palloc(len * 4 + 1);
		(*client_to_mic) (result, buf, len);
		result = buf;
		len = strlen(result);
	}
	if (server_from_mic)
	{
		buf = (unsigned char *) palloc(len * 4 + 1);
		(*server_from_mic) (result, buf, len);
		if (result != s)
			pfree(result);		/* release first buffer */
		result = buf;
	}
	return result;
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
	unsigned char *result = s;
	unsigned char *buf;

	if (client_encoding == GetDatabaseEncoding())
		return result;
	if (server_to_mic)
	{
		buf = (unsigned char *) palloc(len * 4 + 1);
		(*server_to_mic) (result, buf, len);
		result = buf;
		len = strlen(result);
	}
	if (client_from_mic)
	{
		buf = (unsigned char *) palloc(len * 4 + 1);
		(*client_from_mic) (result, buf, len);
		if (result != s)
			pfree(result);		/* release first buffer */
		result = buf;
	}
	return result;
}

/* convert a multi-byte string to a wchar */
void
pg_mb2wchar(const unsigned char *from, pg_wchar * to)
{
	(*pg_wchar_table[GetDatabaseEncoding()].mb2wchar_with_len) (from, to, strlen(from));
}

/* convert a multi-byte string to a wchar with a limited length */
void
pg_mb2wchar_with_len(const unsigned char *from, pg_wchar * to, int len)
{
	(*pg_wchar_table[GetDatabaseEncoding()].mb2wchar_with_len) (from, to, len);
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

	while (*mbstr && limit > 0)
	{
		l = pg_mblen(mbstr);
		limit -= l;
		mbstr += l;
		len++;
	}
	return (len);
}

/*
 * returns the length of a multi-byte string
 * (not necessarily  NULL terminated)
 * that is not longer than limit.
 * this function does not break multi-byte word boundary.
 */
int
pg_mbcliplen(const unsigned char *mbstr, int len, int limit)
{
	int			clen = 0;
	int			l;

	while (*mbstr && len > 0)
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
 * fuctions for utils/init
 */
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
	PG_RETURN_NAME(pg_encoding_to_char(DatabaseEncoding));
}

/* set and get template1 database encoding */
static int	templateEncoding;
void
SetTemplateEncoding(int encoding)
{
	templateEncoding = encoding;
}

int
GetTemplateEncoding()
{
	return (templateEncoding);
}
