/*-------------------------------------------------------------------------
 *
 * crypt.h
 *	  Interface to libpq/crypt.c
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/libpq/crypt.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_CRYPT_H
#define PG_CRYPT_H

#include "datatype/timestamp.h"

/*
 * Types of password hashes or verifiers that can be stored in
 * pg_authid.rolpassword.
 *
 * This is also used for the password_encryption GUC.
 */
typedef enum PasswordType
{
	PASSWORD_TYPE_PLAINTEXT = 0,
	PASSWORD_TYPE_MD5
} PasswordType;

extern PasswordType get_password_type(const char *shadow_pass);
extern char *encrypt_password(PasswordType target_type, const char *role,
				 const char *password);

extern int get_role_password(const char *role, char **shadow_pass,
				  char **logdetail);

extern int md5_crypt_verify(const char *role, const char *shadow_pass,
				 const char *client_pass, const char *md5_salt,
				 int md5_salt_len, char **logdetail);
extern int plain_crypt_verify(const char *role, const char *shadow_pass,
				   const char *client_pass, char **logdetail);

#endif
