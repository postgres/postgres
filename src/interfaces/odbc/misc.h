
/* File:            misc.h
 *
 * Description:     See "misc.c"
 *
 * Comments:        See "notice.txt" for copyright and license information.
 *
 */

#ifndef __MISC_H__
#define __MISC_H__

#include <stdio.h>

/*	Uncomment MY_LOG define to compile in the mylog() statements.
	Then, debug logging will occur if 'Debug' is set to 1 in the ODBCINST.INI
	portion of the registry.  You may have to manually add this key.
	This logfile is intended for development use, not for an end user!
*/
// #define MY_LOG


/*	Uncomment Q_LOG to compile in the qlog() statements (Communications log, i.e. CommLog).
	This logfile contains serious log statements that are intended for an
	end user to be able to read and understand.  It is controlled by the
	'CommLog' flag in the ODBCINST.INI portion of the registry (see above),
	which is manipulated on the setup/connection dialog boxes.
*/
#define Q_LOG


#ifdef MY_LOG
void mylog();	/* prototype */
#else
#define mylog    // mylog
#endif

#ifdef Q_LOG
void qlog();	/* prototype */
#else
#define qlog    // qlog
#endif

void remove_newlines(char *string);
char *strncpy_null(char *dst, const char *src, size_t len);
char *trim(char *string);
char *make_string(char *s, int len, char *buf);
char *my_strcat(char *buf, char *fmt, char *s, int len);

/* defines for return value of my_strcpy */
#define STRCPY_SUCCESS		1
#define STRCPY_FAIL			0
#define STRCPY_TRUNCATED	-1
#define STRCPY_NULL			-2

int my_strcpy(char *dst, size_t dst_len, char *src, size_t src_len);

#endif
