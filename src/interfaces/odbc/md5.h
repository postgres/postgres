/* File:			md5.h
 *
 * Description:		See "md5.h"
 *
 * Comments:		See "notice.txt" for copyright and license information.
 *
 */

#ifndef __MD5_H__
#define __MD5_H__

#include "psqlodbc.h"

#include <stdlib.h>
#include <string.h>

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

/* Also defined in include/c.h */
#ifndef HAVE_UINT8
typedef unsigned char uint8;	/* == 8 bits */
typedef unsigned short uint16;	/* == 16 bits */
typedef unsigned int uint32;	/* == 32 bits */
#endif /* not HAVE_UINT8 */

extern bool md5_hash(const void *buff, size_t len, char *hexsum);
extern bool EncryptMD5(const char *passwd, const char *salt,
		   size_t salt_len, char *buf);

#endif
