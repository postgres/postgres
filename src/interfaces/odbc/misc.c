/*-------
 * Module:			misc.c
 *
 * Description:		This module contains miscellaneous routines
 *					such as for debugging/logging and string functions.
 *
 * Classes:			n/a
 *
 * API functions:	none
 *
 * Comments:		See "notice.txt" for copyright and license information.
 *-------
 */

#include "psqlodbc.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#ifndef WIN32
#if HAVE_PWD_H
#include <pwd.h>
#endif
#include <sys/types.h>
#include <unistd.h>
#else
#include <process.h>			/* Byron: is this where Windows keeps def.
								 * of getpid ? */
#endif

extern GLOBAL_VALUES globals;
void		generate_filename(const char *, const char *, char *);


void
generate_filename(const char *dirname, const char *prefix, char *filename)
{
	int			pid = 0;

#ifndef WIN32
	struct passwd *ptr = 0;

	ptr = getpwuid(getuid());
#endif
	pid = getpid();
	if (dirname == 0 || filename == 0)
		return;

	strcpy(filename, dirname);
	strcat(filename, DIRSEPARATOR);
	if (prefix != 0)
		strcat(filename, prefix);
#ifndef WIN32
	strcat(filename, ptr->pw_name);
#endif
	sprintf(filename, "%s%u%s", filename, pid, ".log");
	return;
}

static int	mylog_on = 0,
			qlog_on = 0;
void
logs_on_off(int cnopen, int mylog_onoff, int qlog_onoff)
{
	static int	mylog_on_count = 0,
				mylog_off_count = 0,
				qlog_on_count = 0,
				qlog_off_count = 0;

	if (mylog_onoff)
		mylog_on_count += cnopen;
	else
		mylog_off_count += cnopen;
	if (mylog_on_count > 0)
		mylog_on = 1;
	else if (mylog_off_count > 0)
		mylog_on = 0;
	else
		mylog_on = globals.debug;
	if (qlog_onoff)
		qlog_on_count += cnopen;
	else
		qlog_off_count += cnopen;
	if (qlog_on_count > 0)
		qlog_on = 1;
	else if (qlog_off_count > 0)
		qlog_on = 0;
	else
		qlog_on = globals.commlog;
}

#ifdef MY_LOG
void
mylog(char *fmt,...)
{
	va_list		args;
	char		filebuf[80];
	static FILE *LOGFP = NULL;

	if (mylog_on)
	{
		va_start(args, fmt);

		if (!LOGFP)
		{
			generate_filename(MYLOGDIR, MYLOGFILE, filebuf);
			LOGFP = fopen(filebuf, PG_BINARY_W);
			setbuf(LOGFP, NULL);
		}

		if (LOGFP)
			vfprintf(LOGFP, fmt, args);

		va_end(args);
	}
}
#endif


#ifdef Q_LOG
void
qlog(char *fmt,...)
{
	va_list		args;
	char		filebuf[80];
	static FILE *LOGFP = NULL;

	if (qlog_on)
	{
		va_start(args, fmt);

		if (!LOGFP)
		{
			generate_filename(QLOGDIR, QLOGFILE, filebuf);
			LOGFP = fopen(filebuf, PG_BINARY_W);
			setbuf(LOGFP, NULL);
		}

		if (LOGFP)
			vfprintf(LOGFP, fmt, args);

		va_end(args);
	}
}
#endif


/*
 *	returns STRCPY_FAIL, STRCPY_TRUNCATED, or #bytes copied
 *	(not including null term)
 */
int
my_strcpy(char *dst, int dst_len, const char *src, int src_len)
{
	if (dst_len <= 0)
		return STRCPY_FAIL;

	if (src_len == SQL_NULL_DATA)
	{
		dst[0] = '\0';
		return STRCPY_NULL;
	}
	else if (src_len == SQL_NTS)
		src_len = strlen(src);

	if (src_len <= 0)
		return STRCPY_FAIL;
	else
	{
		if (src_len < dst_len)
		{
			memcpy(dst, src, src_len);
			dst[src_len] = '\0';
		}
		else
		{
			memcpy(dst, src, dst_len - 1);
			dst[dst_len - 1] = '\0';	/* truncated */
			return STRCPY_TRUNCATED;
		}
	}

	return strlen(dst);
}


/*
 * strncpy copies up to len characters, and doesn't terminate
 * the destination string if src has len characters or more.
 * instead, I want it to copy up to len-1 characters and always
 * terminate the destination string.
 */
char *
strncpy_null(char *dst, const char *src, int len)
{
	int			i;


	if (NULL != dst)
	{
		/* Just in case, check for special lengths */
		if (len == SQL_NULL_DATA)
		{
			dst[0] = '\0';
			return NULL;
		}
		else if (len == SQL_NTS)
			len = strlen(src) + 1;

		for (i = 0; src[i] && i < len - 1; i++)
			dst[i] = src[i];

		if (len > 0)
			dst[i] = '\0';
	}
	return dst;
}


/*------
 *	Create a null terminated string (handling the SQL_NTS thing):
 *		1. If buf is supplied, place the string in there
 *		   (assumes enough space) and return buf.
 *		2. If buf is not supplied, malloc space and return this string
 *------
 */
char *
make_string(const char *s, int len, char *buf)
{
	int			length;
	char	   *str;

	if (s && (len > 0 || (len == SQL_NTS && strlen(s) > 0)))
	{
		length = (len > 0) ? len : strlen(s);

		if (buf)
		{
			strncpy_null(buf, s, length + 1);
			return buf;
		}

		str = malloc(length + 1);
		if (!str)
			return NULL;

		strncpy_null(str, s, length + 1);
		return str;
	}

	return NULL;
}


/*
 *	Concatenate a single formatted argument to a given buffer handling the SQL_NTS thing.
 *	"fmt" must contain somewhere in it the single form '%.*s'.
 *	This is heavily used in creating queries for info routines (SQLTables, SQLColumns).
 *	This routine could be modified to use vsprintf() to handle multiple arguments.
 */
char *
my_strcat(char *buf, const char *fmt, const char *s, int len)
{
	if (s && (len > 0 || (len == SQL_NTS && strlen(s) > 0)))
	{
		int			length = (len > 0) ? len : strlen(s);

		int			pos = strlen(buf);

		sprintf(&buf[pos], fmt, length, s);
		return buf;
	}
	return NULL;
}


void
remove_newlines(char *string)
{
	unsigned int i;

	for (i = 0; i < strlen(string); i++)
	{
		if ((string[i] == '\n') ||
			(string[i] == '\r'))
			string[i] = ' ';
	}
}


char *
trim(char *s)
{
	int			i;

	for (i = strlen(s) - 1; i >= 0; i--)
	{
		if (s[i] == ' ')
			s[i] = '\0';
		else
			break;
	}

	return s;
}
