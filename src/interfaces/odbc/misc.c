
/* Module:          misc.c
 *
 * Description:     This module contains miscellaneous routines
 *                  such as for debugging/logging and string functions.
 *
 * Classes:         n/a
 *
 * API functions:   none
 *
 * Comments:        See "notice.txt" for copyright and license information.
 *
 */

#include <stdio.h>
#include <windows.h>
#include <sql.h>

#include "psqlodbc.h"

extern GLOBAL_VALUES globals;


#ifdef MY_LOG
#include <varargs.h>

void
mylog(va_alist)
va_dcl
{
char *fmt;
char *args;

static FILE *LOGFP = 0;

	if ( globals.debug) {
		va_start(args);
		fmt = va_arg(args, char *);

		if (! LOGFP) {
			LOGFP = fopen("c:\\mylog.log", "w");
			setbuf(LOGFP, NULL);
		}

		if (LOGFP)
			vfprintf(LOGFP, fmt, args);

		va_end(args);
	}
}
#endif


#ifdef Q_LOG
#include <varargs.h>

void qlog(va_alist)
va_dcl
{
char *fmt;
char *args;
static FILE *LOGFP = 0;

	if ( globals.commlog) {
		va_start(args);
		fmt = va_arg(args, char *);

		if (! LOGFP) {
			LOGFP = fopen("c:\\psqlodbc.log", "w");
			setbuf(LOGFP, NULL);
		}

		if (LOGFP)
			vfprintf(LOGFP, fmt, args);

		va_end(args);
	}
}
#endif


/*	returns STRCPY_FAIL, STRCPY_TRUNCATED, or #bytes copied (not including null term) */
int 
my_strcpy(char *dst, size_t dst_len, char *src, size_t src_len)
{
	if (dst_len <= 0)
		return STRCPY_FAIL;

	if (src_len == SQL_NULL_DATA) {
		dst[0] = '\0';
		return STRCPY_NULL;	
	}

	else if (src_len == SQL_NTS) {
		if (src_len < dst_len)
			strcpy(dst, src);
		else {
			memcpy(dst, src, dst_len-1);
			dst[dst_len-1] = '\0';	/* truncated */
			return STRCPY_TRUNCATED;
		}
	}

	else if (src_len <= 0)
		return STRCPY_FAIL;

	else {	
		if (src_len < dst_len) {
			memcpy(dst, src, src_len);
			dst[src_len] = '\0';
		}
		else { 
			memcpy(dst, src, dst_len-1);
			dst[dst_len-1] = '\0';	/* truncated */
			return STRCPY_TRUNCATED;
		}
	}

	return strlen(dst);
}

// strncpy copies up to len characters, and doesn't terminate
// the destination string if src has len characters or more.
// instead, I want it to copy up to len-1 characters and always
// terminate the destination string.
char *strncpy_null(char *dst, const char *src, size_t len)
{
unsigned int i;


	if (NULL != dst) {

		/*  Just in case, check for special lengths */
		if (len == SQL_NULL_DATA) {
			dst[0] = '\0';
			return NULL;
		}	
		else if (len == SQL_NTS)
			len = strlen(src) + 1;

		for(i = 0; src[i] && i < len - 1; i++) {
			dst[i] = src[i];
		}

		if(len > 0) {
			dst[i] = '\0';
		}
	}
	return dst;
}

//	Create a null terminated string (handling the SQL_NTS thing):
//		1. If buf is supplied, place the string in there (assumes enough space) and return buf.
//		2. If buf is not supplied, malloc space and return this string
char *
make_string(char *s, int len, char *buf)
{
int length;
char *str;

    if(s && (len > 0 || len == SQL_NTS)) {
		length = (len > 0) ? len : strlen(s);

		if (buf) {
			strncpy_null(buf, s, length+1);
			return buf;
		}

        str = malloc(length + 1);
		if ( ! str)
			return NULL;

        strncpy_null(str, s, length+1);
		return str;
	}

	return NULL;
}

//	Concatenate a single formatted argument to a given buffer handling the SQL_NTS thing.
//	"fmt" must contain somewhere in it the single form '%.*s'
//	This is heavily used in creating queries for info routines (SQLTables, SQLColumns).
//	This routine could be modified to use vsprintf() to handle multiple arguments.
char *
my_strcat(char *buf, char *fmt, char *s, int len)
{

    if (s && (len > 0 || (len == SQL_NTS && strlen(s) > 0))) {
		int length = (len > 0) ? len : strlen(s);

		int pos = strlen(buf);

		sprintf(&buf[pos], fmt, length, s);
		return buf;
	}
	return NULL;
}

void remove_newlines(char *string)
{
    unsigned int i;

    for(i=0; i < strlen(string); i++) {
        if((string[i] == '\n') ||
           (string[i] == '\r')) {
            string[i] = ' ';
        }
    }
}

char *
trim(char *s)
{
	int i;

	for (i = strlen(s) - 1; i >= 0; i--) {
		if (s[i] == ' ')
			s[i] = '\0';
		else
			break;
	}

	return s;
}
