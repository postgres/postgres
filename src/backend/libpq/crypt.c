/*-------------------------------------------------------------------------
 *
 * crypt.c
 *	  Functions for dealing with encrypted passwords stored in
 *	  pg_authid.rolpassword.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/libpq/crypt.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>

#include "catalog/pg_authid.h"
#include "common/md5.h"
#include "common/scram-common.h"
#include "libpq/crypt.h"
#include "libpq/scram.h"
#include "utils/builtins.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"

/* Enables deprecation warnings for MD5 passwords. */
bool		md5_password_warnings = true;

/*
 * Fetch stored password for a user, for authentication.
 *
 * On error, returns NULL, and stores a palloc'd string describing the reason,
 * for the postmaster log, in *logdetail.  The error reason should *not* be
 * sent to the client, to avoid giving away user information!
 */
char *
get_role_password(const char *role, const char **logdetail)
{
	TimestampTz vuntil = 0;
	HeapTuple	roleTup;
	Datum		datum;
	bool		isnull;
	char	   *shadow_pass;

	/* Get role info from pg_authid */
	roleTup = SearchSysCache1(AUTHNAME, PointerGetDatum(role));
	if (!HeapTupleIsValid(roleTup))
	{
		*logdetail = psprintf(_("Role \"%s\" does not exist."),
							  role);
		return NULL;			/* no such user */
	}

	datum = SysCacheGetAttr(AUTHNAME, roleTup,
							Anum_pg_authid_rolpassword, &isnull);
	if (isnull)
	{
		ReleaseSysCache(roleTup);
		*logdetail = psprintf(_("User \"%s\" has no password assigned."),
							  role);
		return NULL;			/* user has no password */
	}
	shadow_pass = TextDatumGetCString(datum);

	datum = SysCacheGetAttr(AUTHNAME, roleTup,
							Anum_pg_authid_rolvaliduntil, &isnull);
	if (!isnull)
		vuntil = DatumGetTimestampTz(datum);

	ReleaseSysCache(roleTup);

	/*
	 * Password OK, but check to be sure we are not past rolvaliduntil
	 */
	if (!isnull && vuntil < GetCurrentTimestamp())
	{
		*logdetail = psprintf(_("User \"%s\" has an expired password."),
							  role);
		return NULL;
	}

	return shadow_pass;
}

/*
 * What kind of a password type is 'shadow_pass'?
 */
PasswordType
get_password_type(const char *shadow_pass)
{
	char	   *encoded_salt;
	int			iterations;
	int			key_length = 0;
	pg_cryptohash_type hash_type;
	uint8		stored_key[SCRAM_MAX_KEY_LEN];
	uint8		server_key[SCRAM_MAX_KEY_LEN];

	if (strncmp(shadow_pass, "md5", 3) == 0 &&
		strlen(shadow_pass) == MD5_PASSWD_LEN &&
		strspn(shadow_pass + 3, MD5_PASSWD_CHARSET) == MD5_PASSWD_LEN - 3)
		return PASSWORD_TYPE_MD5;
	if (parse_scram_secret(shadow_pass, &iterations, &hash_type, &key_length,
						   &encoded_salt, stored_key, server_key))
		return PASSWORD_TYPE_SCRAM_SHA_256;
	return PASSWORD_TYPE_PLAINTEXT;
}

/*
 * Given a user-supplied password, convert it into a secret of
 * 'target_type' kind.
 *
 * If the password is already in encrypted form, we cannot reverse the
 * hash, so it is stored as it is regardless of the requested type.
 */
char *
encrypt_password(PasswordType target_type, const char *role,
				 const char *password)
{
	PasswordType guessed_type = get_password_type(password);
	char	   *encrypted_password = NULL;
	const char *errstr = NULL;

	if (guessed_type != PASSWORD_TYPE_PLAINTEXT)
	{
		/*
		 * Cannot convert an already-encrypted password from one format to
		 * another, so return it as it is.
		 */
		encrypted_password = pstrdup(password);
	}
	else
	{
		switch (target_type)
		{
			case PASSWORD_TYPE_MD5:
				encrypted_password = palloc(MD5_PASSWD_LEN + 1);

				if (!pg_md5_encrypt(password, (uint8 *) role, strlen(role),
									encrypted_password, &errstr))
					elog(ERROR, "password encryption failed: %s", errstr);
				break;

			case PASSWORD_TYPE_SCRAM_SHA_256:
				encrypted_password = pg_be_scram_build_secret(password);
				break;

			case PASSWORD_TYPE_PLAINTEXT:
				elog(ERROR, "cannot encrypt password with 'plaintext'");
				break;
		}
	}

	Assert(encrypted_password);

	/*
	 * Valid password hashes may be very long, but we don't want to store
	 * anything that might need out-of-line storage, since de-TOASTing won't
	 * work during authentication because we haven't selected a database yet
	 * and cannot read pg_class. 512 bytes should be more than enough for all
	 * practical use, so fail for anything longer.
	 */
	if (encrypted_password &&	/* keep compiler quiet */
		strlen(encrypted_password) > MAX_ENCRYPTED_PASSWORD_LEN)
	{
		/*
		 * We don't expect any of our own hashing routines to produce hashes
		 * that are too long.
		 */
		Assert(guessed_type != PASSWORD_TYPE_PLAINTEXT);

		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("encrypted password is too long"),
				 errdetail("Encrypted passwords must be no longer than %d bytes.",
						   MAX_ENCRYPTED_PASSWORD_LEN)));
	}

	if (md5_password_warnings &&
		get_password_type(encrypted_password) == PASSWORD_TYPE_MD5)
		ereport(WARNING,
				(errcode(ERRCODE_WARNING_DEPRECATED_FEATURE),
				 errmsg("setting an MD5-encrypted password"),
				 errdetail("MD5 password support is deprecated and will be removed in a future release of PostgreSQL."),
				 errhint("Refer to the PostgreSQL documentation for details about migrating to another password type.")));

	return encrypted_password;
}

/*
 * Check MD5 authentication response, and return STATUS_OK or STATUS_ERROR.
 *
 * 'shadow_pass' is the user's correct password or password hash, as stored
 * in pg_authid.rolpassword.
 * 'client_pass' is the response given by the remote user to the MD5 challenge.
 * 'md5_salt' is the salt used in the MD5 authentication challenge.
 *
 * In the error case, save a string at *logdetail that will be sent to the
 * postmaster log (but not the client).
 */
int
md5_crypt_verify(const char *role, const char *shadow_pass,
				 const char *client_pass,
				 const uint8 *md5_salt, int md5_salt_len,
				 const char **logdetail)
{
	int			retval;
	char		crypt_pwd[MD5_PASSWD_LEN + 1];
	const char *errstr = NULL;

	Assert(md5_salt_len > 0);

	if (get_password_type(shadow_pass) != PASSWORD_TYPE_MD5)
	{
		/* incompatible password hash format. */
		*logdetail = psprintf(_("User \"%s\" has a password that cannot be used with MD5 authentication."),
							  role);
		return STATUS_ERROR;
	}

	/*
	 * Compute the correct answer for the MD5 challenge.
	 */
	/* stored password already encrypted, only do salt */
	if (!pg_md5_encrypt(shadow_pass + strlen("md5"),
						md5_salt, md5_salt_len,
						crypt_pwd, &errstr))
	{
		*logdetail = errstr;
		return STATUS_ERROR;
	}

	if (strcmp(client_pass, crypt_pwd) == 0)
		retval = STATUS_OK;
	else
	{
		*logdetail = psprintf(_("Password does not match for user \"%s\"."),
							  role);
		retval = STATUS_ERROR;
	}

	return retval;
}

/*
 * Check given password for given user, and return STATUS_OK or STATUS_ERROR.
 *
 * 'shadow_pass' is the user's correct password hash, as stored in
 * pg_authid.rolpassword.
 * 'client_pass' is the password given by the remote user.
 *
 * In the error case, store a string at *logdetail that will be sent to the
 * postmaster log (but not the client).
 */
int
plain_crypt_verify(const char *role, const char *shadow_pass,
				   const char *client_pass,
				   const char **logdetail)
{
	char		crypt_client_pass[MD5_PASSWD_LEN + 1];
	const char *errstr = NULL;

	/*
	 * Client sent password in plaintext.  If we have an MD5 hash stored, hash
	 * the password the client sent, and compare the hashes.  Otherwise
	 * compare the plaintext passwords directly.
	 */
	switch (get_password_type(shadow_pass))
	{
		case PASSWORD_TYPE_SCRAM_SHA_256:
			if (scram_verify_plain_password(role,
											client_pass,
											shadow_pass))
			{
				return STATUS_OK;
			}
			else
			{
				*logdetail = psprintf(_("Password does not match for user \"%s\"."),
									  role);
				return STATUS_ERROR;
			}
			break;

		case PASSWORD_TYPE_MD5:
			if (!pg_md5_encrypt(client_pass,
								(uint8 *) role,
								strlen(role),
								crypt_client_pass,
								&errstr))
			{
				*logdetail = errstr;
				return STATUS_ERROR;
			}
			if (strcmp(crypt_client_pass, shadow_pass) == 0)
				return STATUS_OK;
			else
			{
				*logdetail = psprintf(_("Password does not match for user \"%s\"."),
									  role);
				return STATUS_ERROR;
			}
			break;

		case PASSWORD_TYPE_PLAINTEXT:

			/*
			 * We never store passwords in plaintext, so this shouldn't
			 * happen.
			 */
			break;
	}

	/*
	 * This shouldn't happen.  Plain "password" authentication is possible
	 * with any kind of stored password hash.
	 */
	*logdetail = psprintf(_("Password of user \"%s\" is in unrecognized format."),
						  role);
	return STATUS_ERROR;
}
