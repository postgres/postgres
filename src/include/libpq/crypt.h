/*-------------------------------------------------------------------------
 *
 * crypt.h
 *	  Interface to libpq/crypt.c
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/libpq/crypt.h,v 1.32.2.1 2005/11/22 18:23:28 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_CRYPT_H
#define PG_CRYPT_H

#include "libpq/libpq-be.h"

#define MD5_PASSWD_LEN	35

#define isMD5(passwd)	(strncmp(passwd, "md5", 3) == 0 && \
						 strlen(passwd) == MD5_PASSWD_LEN)


/* in crypt.c */
extern int md5_crypt_verify(const Port *port, const char *user,
				 char *client_pass);

/* in md5.c --- these are also present in frontend libpq */
extern bool pg_md5_hash(const void *buff, size_t len, char *hexsum);
extern bool pg_md5_encrypt(const char *passwd, const char *salt,
			   size_t salt_len, char *buf);

#endif
