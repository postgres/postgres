/*-------------------------------------------------------------------------
 *
 * passwordcheck.c
 *
 *
 * Copyright (c) 2009-2017, PostgreSQL Global Development Group
 *
 * Author: Laurenz Albe <laurenz.albe@wien.gv.at>
 *
 * IDENTIFICATION
 *	  contrib/passwordcheck/passwordcheck.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>

#ifdef USE_CRACKLIB
#include <crack.h>
#endif

#include "commands/user.h"
#include "libpq/crypt.h"
#include "fmgr.h"

PG_MODULE_MAGIC;

/* passwords shorter than this will be rejected */
#define MIN_PWD_LENGTH 8

extern void _PG_init(void);

/*
 * check_password
 *
 * performs checks on an encrypted or unencrypted password
 * ereport's if not acceptable
 *
 * username: name of role being created or changed
 * password: new password (possibly already encrypted)
 * password_type: PASSWORD_TYPE_PLAINTEXT or PASSWORD_TYPE_MD5 (there
 *			could be other encryption schemes in future)
 * validuntil_time: password expiration time, as a timestamptz Datum
 * validuntil_null: true if password expiration time is NULL
 *
 * This sample implementation doesn't pay any attention to the password
 * expiration time, but you might wish to insist that it be non-null and
 * not too far in the future.
 */
static void
check_password(const char *username,
			   const char *shadow_pass,
			   PasswordType password_type,
			   Datum validuntil_time,
			   bool validuntil_null)
{
	if (password_type != PASSWORD_TYPE_PLAINTEXT)
	{
		/*
		 * Unfortunately we cannot perform exhaustive checks on encrypted
		 * passwords - we are restricted to guessing. (Alternatively, we could
		 * insist on the password being presented non-encrypted, but that has
		 * its own security disadvantages.)
		 *
		 * We only check for username = password.
		 */
		char	   *logdetail;

		if (plain_crypt_verify(username, shadow_pass, username, &logdetail) == STATUS_OK)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("password must not contain user name")));
	}
	else
	{
		/*
		 * For unencrypted passwords we can perform better checks
		 */
		const char *password = shadow_pass;
		int			pwdlen = strlen(password);
		int			i;
		bool		pwd_has_letter,
					pwd_has_nonletter;

		/* enforce minimum length */
		if (pwdlen < MIN_PWD_LENGTH)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("password is too short")));

		/* check if the password contains the username */
		if (strstr(password, username))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("password must not contain user name")));

		/* check if the password contains both letters and non-letters */
		pwd_has_letter = false;
		pwd_has_nonletter = false;
		for (i = 0; i < pwdlen; i++)
		{
			/*
			 * isalpha() does not work for multibyte encodings but let's
			 * consider non-ASCII characters non-letters
			 */
			if (isalpha((unsigned char) password[i]))
				pwd_has_letter = true;
			else
				pwd_has_nonletter = true;
		}
		if (!pwd_has_letter || !pwd_has_nonletter)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			   errmsg("password must contain both letters and nonletters")));

#ifdef USE_CRACKLIB
		/* call cracklib to check password */
		if (FascistCheck(password, CRACKLIB_DICTPATH))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("password is easily cracked")));
#endif
	}

	/* all checks passed, password is ok */
}

/*
 * Module initialization function
 */
void
_PG_init(void)
{
	/* activate password checks when the module is loaded */
	check_password_hook = check_password;
}
