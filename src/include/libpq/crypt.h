/*-------------------------------------------------------------------------
 *
 * crypt.h--
 *	  Interface to hba.c
 *
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_CRYPT_H
#define PG_CRYPT_H

#include <libpq/pqcomm.h>

#define CRYPT_PWD_FILE	"pg_pwd"

extern char* crypt_getpwdfilename(void);
extern MsgType crypt_salt(const char* user);
extern int crypt_verify(Port* port, const char* user, const char* pgpass);

#endif
