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

#ifdef NOT_USED
extern MsgType crypt_salt(const char *user);

#endif
extern int	crypt_verify(const Port *port, const char *user, const char *pgpass);

#endif
