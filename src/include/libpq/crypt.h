/*-------------------------------------------------------------------------
 *
 * crypt.h
 *	  Interface to libpq/crypt.c
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: crypt.h,v 1.20 2002/04/04 04:25:53 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_CRYPT_H
#define PG_CRYPT_H

#include "libpq/libpq-be.h"

/* Also defined in interfaces/odbc/md5.h */
#define MD5_PASSWD_LEN	35

#define isMD5(passwd)	(strncmp((passwd),"md5",3) == 0 && \
						 strlen(passwd) == MD5_PASSWD_LEN)


extern int md5_crypt_verify(const Port *port, const char *user,
				 const char *pgpass);
extern bool md5_hash(const void *buff, size_t len, char *hexsum);
extern bool CheckMD5Pwd(char *passwd, char *storedpwd, char *seed);
/* Also defined in interfaces/odbc/md5.h */
extern bool EncryptMD5(const char *passwd, const char *salt,
		   size_t salt_len, char *buf);

#endif
