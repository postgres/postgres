/*-------------------------------------------------------------------------
 *
 * crypt.h
 *	  Interface to libpq/crypt.c
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: crypt.h,v 1.17 2001/11/02 18:39:57 tgl Exp $
 * 
 *-------------------------------------------------------------------------
 */
#ifndef PG_CRYPT_H
#define PG_CRYPT_H

#include "libpq/libpq-be.h"

#define CRYPT_PWD_FILE_SEPSTR	"\t"

#define MD5_PASSWD_LEN	35

#define isMD5(passwd)	(strncmp((passwd),"md5",3) == 0 && \
						 strlen(passwd) == MD5_PASSWD_LEN)


extern char *crypt_getpwdfilename(void);
extern void load_password_cache(void);

extern int	md5_crypt_verify(const Port *port, const char *user,
							 const char *pgpass);
extern bool md5_hash(const void *buff, size_t len, char *hexsum);
extern bool CheckMD5Pwd(char *passwd, char *storedpwd, char *seed);
extern bool EncryptMD5(const char *passwd, const char *salt,
		   size_t salt_len, char *buf);

#endif
