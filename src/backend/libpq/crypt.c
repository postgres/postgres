/*-------------------------------------------------------------------------
 *
 * crypt.c
 *	  Look into the password file and check the encrypted password with
 *	  the one passed in from the frontend.
 *
 * Original coding by Todd A. Brandys
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
	if (isMD5(shadow_pass))
	{
		/* stored password already encrypted, only do salt */
		if (!pg_md5_encrypt(shadow_pass + strlen("md5"),
							md5_salt, md5_salt_len,
							crypt_pwd))
		{
			return STATUS_ERROR;
		}
	}
	else
	{
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
	if (isMD5(shadow_pass))
	{
		if (!pg_md5_encrypt(client_pass,
							role,
							strlen(role),
							crypt_client_pass))
		{
			/*
			 * We do not bother setting logdetail for pg_md5_encrypt failure:
			 * the only possible error is out-of-memory, which is unlikely,
			 * and if it did happen adding a psprintf call would only make
			 * things worse.
			 */
			return STATUS_ERROR;
		}
		client_pass = crypt_client_pass;
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
