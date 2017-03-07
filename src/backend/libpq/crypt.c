/*-------------------------------------------------------------------------
 *
 * crypt.c
 *	  Functions for dealing with encrypted passwords stored in
 *	  pg_authid.rolpassword.
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/libpq/crypt.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>
#ifdef HAVE_CRYPT_H
#include <crypt.h>
#endif

#include "catalog/pg_authid.h"
#include "common/md5.h"
#include "libpq/crypt.h"
#include "libpq/scram.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"


/*
 * Fetch stored password for a user, for authentication.
 *
 * Returns STATUS_OK on success.  On error, returns STATUS_ERROR, and stores
 * a palloc'd string describing the reason, for the postmaster log, in
 * *logdetail.  The error reason should *not* be sent to the client, to avoid
 * giving away user information!
 *
 * If the password is expired, it is still returned in *shadow_pass, but the
 * return code is STATUS_ERROR.  On other errors, *shadow_pass is set to
 * NULL.
 */
int
get_role_password(const char *role, char **shadow_pass, char **logdetail)
{
	int			retval = STATUS_ERROR;
	TimestampTz vuntil = 0;
	HeapTuple	roleTup;
	Datum		datum;
	bool		isnull;

	*shadow_pass = NULL;

	/* Get role info from pg_authid */
	roleTup = SearchSysCache1(AUTHNAME, PointerGetDatum(role));
	if (!HeapTupleIsValid(roleTup))
	{
		*logdetail = psprintf(_("Role \"%s\" does not exist."),
							  role);
		return STATUS_ERROR;	/* no such user */
	}

	datum = SysCacheGetAttr(AUTHNAME, roleTup,
							Anum_pg_authid_rolpassword, &isnull);
	if (isnull)
	{
		ReleaseSysCache(roleTup);
		*logdetail = psprintf(_("User \"%s\" has no password assigned."),
							  role);
		return STATUS_ERROR;	/* user has no password */
	}
	*shadow_pass = TextDatumGetCString(datum);

	datum = SysCacheGetAttr(AUTHNAME, roleTup,
							Anum_pg_authid_rolvaliduntil, &isnull);
	if (!isnull)
		vuntil = DatumGetTimestampTz(datum);

	ReleaseSysCache(roleTup);

	if (**shadow_pass == '\0')
	{
		*logdetail = psprintf(_("User \"%s\" has an empty password."),
							  role);
		pfree(*shadow_pass);
		*shadow_pass = NULL;
		return STATUS_ERROR;	/* empty password */
	}

	/*
	 * Password OK, now check to be sure we are not past rolvaliduntil
	 */
	if (isnull)
		retval = STATUS_OK;
	else if (vuntil < GetCurrentTimestamp())
	{
		*logdetail = psprintf(_("User \"%s\" has an expired password."),
							  role);
		retval = STATUS_ERROR;
	}
	else
		retval = STATUS_OK;

	return retval;
}

/*
 * What kind of a password verifier is 'shadow_pass'?
 */
PasswordType
get_password_type(const char *shadow_pass)
{
	if (strncmp(shadow_pass, "md5", 3) == 0 && strlen(shadow_pass) == MD5_PASSWD_LEN)
		return PASSWORD_TYPE_MD5;
	if (strncmp(shadow_pass, "scram-sha-256:", strlen("scram-sha-256:")) == 0)
		return PASSWORD_TYPE_SCRAM;
	return PASSWORD_TYPE_PLAINTEXT;
}

/*
 * Given a user-supplied password, convert it into a verifier of
 * 'target_type' kind.
 *
 * If the password looks like a valid MD5 hash, it is stored as it is.
 * We cannot reverse the hash, so even if the caller requested a plaintext
 * plaintext password, the MD5 hash is returned.
 */
char *
encrypt_password(PasswordType target_type, const char *role,
				 const char *password)
{
	PasswordType guessed_type = get_password_type(password);
	char	   *encrypted_password;

	switch (target_type)
	{
		case PASSWORD_TYPE_PLAINTEXT:

			/*
			 * We cannot convert a hashed password back to plaintext, so just
			 * store the password as it was, whether it was hashed or not.
			 */
			return pstrdup(password);

		case PASSWORD_TYPE_MD5:
			switch (guessed_type)
			{
				case PASSWORD_TYPE_PLAINTEXT:
					encrypted_password = palloc(MD5_PASSWD_LEN + 1);

					if (!pg_md5_encrypt(password, role, strlen(role),
										encrypted_password))
						elog(ERROR, "password encryption failed");
					return encrypted_password;

				case PASSWORD_TYPE_SCRAM:

					/*
					 * cannot convert a SCRAM verifier to an MD5 hash, so fall
					 * through to save the SCRAM verifier instead.
					 */
				case PASSWORD_TYPE_MD5:
					return pstrdup(password);
			}

		case PASSWORD_TYPE_SCRAM:
			switch (guessed_type)
			{
				case PASSWORD_TYPE_PLAINTEXT:
					return scram_build_verifier(role, password, 0);

				case PASSWORD_TYPE_MD5:

					/*
					 * cannot convert an MD5 hash to a SCRAM verifier, so fall
					 * through to save the MD5 hash instead.
					 */
				case PASSWORD_TYPE_SCRAM:
					return pstrdup(password);
			}
	}

	/*
	 * This shouldn't happen, because the above switch statements should
	 * handle every combination of source and target password types.
	 */
	elog(ERROR, "cannot encrypt password to requested type");
	return NULL;				/* keep compiler quiet */
}

/*
 * Check MD5 authentication response, and return STATUS_OK or STATUS_ERROR.
 *
 * 'shadow_pass' is the user's correct password or password hash, as stored
 * in pg_authid.rolpassword.
 * 'client_pass' is the response given by the remote user to the MD5 challenge.
 * 'md5_salt' is the salt used in the MD5 authentication challenge.
 *
 * In the error case, optionally store a palloc'd string at *logdetail
 * that will be sent to the postmaster log (but not the client).
 */
int
md5_crypt_verify(const char *role, const char *shadow_pass,
				 const char *client_pass,
				 const char *md5_salt, int md5_salt_len,
				 char **logdetail)
{
	int			retval;
	char		crypt_pwd[MD5_PASSWD_LEN + 1];
	char		crypt_pwd2[MD5_PASSWD_LEN + 1];

	Assert(md5_salt_len > 0);

	/*
	 * Compute the correct answer for the MD5 challenge.
	 *
	 * We do not bother setting logdetail for any pg_md5_encrypt failure
	 * below: the only possible error is out-of-memory, which is unlikely, and
	 * if it did happen adding a psprintf call would only make things worse.
	 */
	switch (get_password_type(shadow_pass))
	{
		case PASSWORD_TYPE_MD5:
			/* stored password already encrypted, only do salt */
			if (!pg_md5_encrypt(shadow_pass + strlen("md5"),
								md5_salt, md5_salt_len,
								crypt_pwd))
			{
				return STATUS_ERROR;
			}
			break;

		case PASSWORD_TYPE_PLAINTEXT:
			/* stored password is plain, double-encrypt */
			if (!pg_md5_encrypt(shadow_pass,
								role,
								strlen(role),
								crypt_pwd2))
			{
				return STATUS_ERROR;
			}
			if (!pg_md5_encrypt(crypt_pwd2 + strlen("md5"),
								md5_salt, md5_salt_len,
								crypt_pwd))
			{
				return STATUS_ERROR;
			}
			break;

		default:
			/* unknown password hash format. */
			*logdetail = psprintf(_("User \"%s\" has a password that cannot be used with MD5 authentication."),
								  role);
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
 * 'shadow_pass' is the user's correct password or password hash, as stored
 * in pg_authid.rolpassword.
 * 'client_pass' is the password given by the remote user.
 *
 * In the error case, optionally store a palloc'd string at *logdetail
 * that will be sent to the postmaster log (but not the client).
 */
int
plain_crypt_verify(const char *role, const char *shadow_pass,
				   const char *client_pass,
				   char **logdetail)
{
	int			retval;
	char		crypt_client_pass[MD5_PASSWD_LEN + 1];

	/*
	 * Client sent password in plaintext.  If we have an MD5 hash stored, hash
	 * the password the client sent, and compare the hashes.  Otherwise
	 * compare the plaintext passwords directly.
	 */
	switch (get_password_type(shadow_pass))
	{
		case PASSWORD_TYPE_MD5:
			if (!pg_md5_encrypt(client_pass,
								role,
								strlen(role),
								crypt_client_pass))
			{
				/*
				 * We do not bother setting logdetail for pg_md5_encrypt
				 * failure: the only possible error is out-of-memory, which is
				 * unlikely, and if it did happen adding a psprintf call would
				 * only make things worse.
				 */
				return STATUS_ERROR;
			}
			client_pass = crypt_client_pass;
			break;
		case PASSWORD_TYPE_PLAINTEXT:
			break;

		default:

			/*
			 * This shouldn't happen. Plain "password" authentication should
			 * be possible with any kind of stored password hash.
			 */
			*logdetail = psprintf(_("Password of user \"%s\" is in unrecognized format."),
								  role);
			return STATUS_ERROR;
	}

	if (strcmp(client_pass, shadow_pass) == 0)
		retval = STATUS_OK;
	else
	{
		*logdetail = psprintf(_("Password does not match for user \"%s\"."),
							  role);
		retval = STATUS_ERROR;
	}

	return retval;
}
