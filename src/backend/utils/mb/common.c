/*
 * This file contains some public functions
 * usable for both the backend and the frontend.
 * Tatsuo Ishii
 * $Id: common.c,v 1.2 1998/09/01 04:33:19 momjian Exp $ */

#include <stdio.h>
#include <string.h>

#include "mb/pg_wchar.h"

/*
 * convert encoding char to encoding symbol value.
 * case is ignored.
 * if there's no valid encoding, returns -1
 */
int
pg_char_to_encoding(const char *s)
{
	pg_encoding_conv_tbl *p = pg_conv_tbl;

	for (; p->encoding >= 0; p++)
	{
		if (!strcasecmp(s, p->name))
			break;
	}
	return (p->encoding);
}

/*
 * check to see if encoding name is valid
 */
int
pg_valid_client_encoding(const char *name)
{
	return (pg_char_to_encoding(name));
}

/*
 * find encoding table entry by encoding
 */
pg_encoding_conv_tbl *
pg_get_encent_by_encoding(int encoding)
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
 * convert encoding symbol to encoding char.
 * if there's no valid encoding symbol, returns ""
 */
const char *
pg_encoding_to_char(int encoding)
{
	pg_encoding_conv_tbl *p = pg_get_encent_by_encoding(encoding);

	if (!p)
		return ("");
	return (p->name);
}

/* returns the byte length of a multi-byte word for an encoding */
int
pg_encoding_mblen(int encoding, const unsigned char *mbstr)
{
	return ((*pg_wchar_table[encoding].mblen) (mbstr));
}
