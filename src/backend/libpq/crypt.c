/*-------------------------------------------------------------------------
 *
 * crypt.c
 *	  Look into the password file and check the encrypted password with
 *	  the one passed in from the frontend.
 *
 * Original coding by Todd A. Brandys
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Header: /cvsroot/pgsql/src/backend/libpq/crypt.c,v 1.57 2003/09/25 06:57:59 petere Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>
#ifdef HAVE_CRYPT_H
#include <crypt.h>
#endif

#include "libpq/crypt.h"
#include "libpq/libpq.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "nodes/pg_list.h"
#include "utils/nabstime.h"


int
md5_crypt_verify(const Port *port, const char *user, char *client_pass)
{
	char	   *shadow_pass = NULL,
			   *valuntil = NULL,
			   *crypt_pwd;
	int			retval = STATUS_ERROR;
	List	  **line;
	List	   *token;
	char	   *crypt_client_pass = client_pass;

	if ((line = get_user_line(user)) == NULL)
		return STATUS_ERROR;

	/* Skip over line number and username */
	token = lnext(lnext(*line));
	if (token)
	{
		shadow_pass = lfirst(token);
		token = lnext(token);
		if (token)
			valuntil = lfirst(token);
	}

	if (shadow_pass == NULL || *shadow_pass == '\0')
		return STATUS_ERROR;

	/* We can't do crypt with pg_shadow MD5 passwords */
	if (isMD5(shadow_pass) && port->auth_method == uaCrypt)
	{
		ereport(LOG,
				(errmsg("cannot use authentication method \"crypt\" because password is MD5-encrypted")));
		return STATUS_ERROR;
	}

	/*
	 * Compare with the encrypted or plain password depending on the
	 * authentication method being used for this connection.
	 */
	switch (port->auth_method)
	{
		case uaMD5:
			crypt_pwd = palloc(MD5_PASSWD_LEN + 1);
			if (isMD5(shadow_pass))
			{
				/* pg_shadow already encrypted, only do salt */
				if (!EncryptMD5(shadow_pass + strlen("md5"),
								(char *) port->md5Salt,
								sizeof(port->md5Salt), crypt_pwd))
				{
					pfree(crypt_pwd);
					return STATUS_ERROR;
				}
			}
			else
			{
				/* pg_shadow plain, double-encrypt */
				char	   *crypt_pwd2 = palloc(MD5_PASSWD_LEN + 1);

				if (!EncryptMD5(shadow_pass,
								port->user_name,
								strlen(port->user_name),
								crypt_pwd2))
				{
					pfree(crypt_pwd);
					pfree(crypt_pwd2);
					return STATUS_ERROR;
				}
				if (!EncryptMD5(crypt_pwd2 + strlen("md5"),
								port->md5Salt,
								sizeof(port->md5Salt),
								crypt_pwd))
				{
					pfree(crypt_pwd);
					pfree(crypt_pwd2);
					return STATUS_ERROR;
				}
				pfree(crypt_pwd2);
			}
			break;
		case uaCrypt:
			{
				char		salt[3];

				StrNCpy(salt, port->cryptSalt, 3);
				crypt_pwd = crypt(shadow_pass, salt);
				break;
			}
		default:
			if (isMD5(shadow_pass))
			{
				/*
				 * Encrypt user-supplied password to match MD5 in
				 * pg_shadow
				 */
				crypt_client_pass = palloc(MD5_PASSWD_LEN + 1);
				if (!EncryptMD5(client_pass,
								port->user_name,
								strlen(port->user_name),
								crypt_client_pass))
				{
					pfree(crypt_client_pass);
					return STATUS_ERROR;
				}
			}
			crypt_pwd = shadow_pass;
			break;
	}

	if (strcmp(crypt_client_pass, crypt_pwd) == 0)
	{
		/*
		 * Password OK, now check to be sure we are not past valuntil
		 */
		AbsoluteTime vuntil,
					current;

		if (!valuntil)
			vuntil = INVALID_ABSTIME;
		else
			vuntil = DatumGetAbsoluteTime(DirectFunctionCall1(abstimein,
											 CStringGetDatum(valuntil)));
		current = GetCurrentAbsoluteTime();
		if (vuntil != INVALID_ABSTIME && vuntil < current)
			retval = STATUS_ERROR;
		else
			retval = STATUS_OK;
	}

	if (port->auth_method == uaMD5)
		pfree(crypt_pwd);
	if (crypt_client_pass != client_pass)
		pfree(crypt_client_pass);

	return retval;
}
