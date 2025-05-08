/*-------------------------------------------------------------------------
 *
 * crypt.h
 *	  Interface to libpq/crypt.c
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
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
 * Valid password hashes may be very long, but we don't want to store anything
 * that might need out-of-line storage, since de-TOASTing won't work during
 * authentication because we haven't selected a database yet and cannot read
 * pg_class.  512 bytes should be more than enough for all practical use, and
 * our own password encryption routines should never produce hashes longer than
 * this.
 */
#define MAX_ENCRYPTED_PASSWORD_LEN (512)

/* Enables deprecation warnings for MD5 passwords. */
extern PGDLLIMPORT bool md5_password_warnings;

/*
 * Types of password hashes or secrets.
 *
 * Plaintext passwords can be passed in by the user, in a CREATE/ALTER USER
 * command. They will be encrypted to MD5 or SCRAM-SHA-256 format, before
 * storing on-disk, so only MD5 and SCRAM-SHA-256 passwords should appear
 * in pg_authid.rolpassword. They are also the allowed values for the
 * password_encryption GUC.
 */
typedef enum PasswordType
{
	PASSWORD_TYPE_PLAINTEXT = 0,
	PASSWORD_TYPE_MD5,
	PASSWORD_TYPE_SCRAM_SHA_256,
} PasswordType;

extern PasswordType get_password_type(const char *shadow_pass);
extern char *encrypt_password(PasswordType target_type, const char *role,
							  const char *password);

extern char *get_role_password(const char *role, const char **logdetail);

extern int	md5_crypt_verify(const char *role, const char *shadow_pass,
							 const char *client_pass, const uint8 *md5_salt,
							 int md5_salt_len, const char **logdetail);
extern int	plain_crypt_verify(const char *role, const char *shadow_pass,
							   const char *client_pass,
							   const char **logdetail);

#endif
