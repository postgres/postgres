/* File:			connection.h
 *
 * Description:		See "md.h"
 *
 * Comments:		See "notice.txt" for copyright and license information.
 *
 */

#ifndef __MD5_H__
#define __MD5_H__

#include "psqlodbc.h"

#include <stdlib.h>
#include <string.h>

#ifdef	WIN32
#define	MD5_ODBC
#define	FRONTEND
#endif
#define MD5_PASSWD_LEN	35

/* From c.h */
#ifndef __BEOS__

#ifndef __cplusplus

#ifndef bool
typedef char bool;
#endif

#ifndef true
#define true	((bool) 1)
#endif

#ifndef false
#define false	((bool) 0)
#endif
#endif   /* not C++ */
#endif   /* __BEOS__ */

/* #if SIZEOF_UINT8 == 0  Can't get this from configure */
typedef unsigned char uint8;	/* == 8 bits */
typedef unsigned short uint16;	/* == 16 bits */
typedef unsigned int uint32;	/* == 32 bits */
/* #endif */

extern bool EncryptMD5(const char *passwd, const char *salt,
		   size_t salt_len, char *buf);


#endif
