/*-------------------------------------------------------------------------
 *
 * crypt.h
 *	  Interface to hba.c
 *
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_CRYPT_H
#define PG_CRYPT_H

#include "libpq/libpq-be.h"

#define CRYPT_PWD_FILE	"pg_pwd"
#define CRYPT_PWD_FILE_SEPCHAR	"'\\t'"
#define CRYPT_PWD_FILE_SEPSTR	"\t"
#define CRYPT_PWD_RELOAD_SUFX	".reload"

extern char **pwd_cache;
extern int	pwd_cache_count;

extern char *crypt_getpwdfilename(void);
extern char *crypt_getpwdreloadfilename(void);

extern int	md5_crypt_verify(const Port *port, const char *user, const char *pgpass);

extern bool md5_hash(const void *buff, size_t len, char *hexsum);
extern bool CheckMD5Pwd(char *passwd, char *storedpwd, char *seed);
extern bool EncryptMD5(const char *passwd, const char *salt,
		   size_t salt_len, char *buf);

#define MD5_PASSWD_LEN	35

#define isMD5(passwd)	(strncmp((passwd),"md5",3) == 0 && \
						 strlen(passwd) == MD5_PASSWD_LEN)
#endif
