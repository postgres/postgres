
/* File:            convert.h
 *
 * Description:     See "convert.c"
 *
 * Comments:        See "notice.txt" for copyright and license information.
 *
 */

#ifndef __CONVERT_H__
#define __CONVERT_H__

#include "psqlodbc.h"

/* copy_and_convert results */
#define COPY_OK                      0
#define COPY_UNSUPPORTED_TYPE        1
#define COPY_UNSUPPORTED_CONVERSION  2
#define COPY_RESULT_TRUNCATED        3
#define COPY_GENERAL_ERROR           4
#define COPY_NO_DATA_FOUND           5

typedef struct {
	int m;
	int d;
	int y;
	int hh;
	int mm;
	int ss;
} SIMPLE_TIME;

int copy_and_convert_field_bindinfo(StatementClass *stmt, Int4 field_type, void *value, int col);
int copy_and_convert_field(StatementClass *stmt, Int4 field_type, void *value, Int2 fCType, 
						   PTR rgbValue, SDWORD cbValueMax, SDWORD *pcbValue);

int copy_statement_with_parameters(StatementClass *stmt);
char *convert_escape(char *value);
char *convert_money(char *s);
char parse_datetime(char *buf, SIMPLE_TIME *st);
int convert_linefeeds(char *s, char *dst, size_t max);
char *convert_special_chars(char *si, char *dst, int used);

int convert_pgbinary_to_char(char *value, char *rgbValue, int cbValueMax);
int convert_from_pgbinary(unsigned char *value, unsigned char *rgbValue, int cbValueMax);
int convert_to_pgbinary(unsigned char *in, char *out, int len);
void encode(char *in, char *out);
void decode(char *in, char *out);
int convert_lo(StatementClass *stmt, void *value, Int2 fCType, PTR rgbValue, 
		   SDWORD cbValueMax, SDWORD *pcbValue);

#endif
