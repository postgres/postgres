/*-------------------------------------------------------------------------
 *
 * md5.h
 *	  Interface to hba.c
 *
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_MD5_H
#define PG_MD5_H

extern bool md5_hash(const void *buff, size_t len, char *hexsum);
extern bool CheckMD5Pwd(char *passwd, char *storedpwd, char *seed);
extern bool EncryptMD5(const char *passwd, const char *salt, char *buf);

#define MD5_PASSWD_LEN	35

#define isMD5(passwd)	(strncmp((passwd),"md5",3) == 0 && \
						 strlen(passwd) == MD5_PASSWD_LEN)

#endif
