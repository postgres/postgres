/*-------------------------------------------------------------------------
 *
 * crypt.c
 *	  Look into the password file and check the encrypted password with
 *	  the one passed in from the frontend.
 *
 * Original coding by Todd A. Brandys
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Header: /cvsroot/pgsql/src/backend/libpq/crypt.c,v 1.46 2002/04/25 00:56:36 momjian Exp $
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
md5_crypt_verify(const Port *port, const char *user, const char *pgpass)
{
	char	   *passwd = NULL,
			   *valuntil = NULL,
			   *crypt_pwd;
	int			retval = STATUS_ERROR;
	List	   **line;
	List		*token;
	
	if ((line = get_user_line(user)) == NULL)
		return STATUS_ERROR;

	/* Skip over line number and username */
	token = lnext(lnext(*line));
	if (token)
	{
		passwd = lfirst(token);
		token = lnext(token);
		if (token)
			valuntil = lfirst(token);
	}
	
	if (passwd == NULL || *passwd == '\0')
	{
		if (passwd)
			pfree(passwd);
		if (valuntil)
			pfree(valuntil);
		return STATUS_ERROR;
	}

	/* If they encrypt their password, force MD5 */
	if (isMD5(passwd) && port->auth_method != uaMD5)
	{
		elog(LOG, "Password is stored MD5 encrypted.  "
			 "'password' and 'crypt' auth methods cannot be used.");
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
			if (isMD5(passwd))
			{
				if (!EncryptMD5(passwd + strlen("md5"),
								(char *) port->md5Salt,
								sizeof(port->md5Salt), crypt_pwd))
				{
					pfree(crypt_pwd);
					return STATUS_ERROR;
				}
			}
			else
			{
				char	   *crypt_pwd2 = palloc(MD5_PASSWD_LEN + 1);

				if (!EncryptMD5(passwd, port->user, strlen(port->user),
								crypt_pwd2))
				{
					pfree(crypt_pwd);
					pfree(crypt_pwd2);
					return STATUS_ERROR;
				}
				if (!EncryptMD5(crypt_pwd2 + strlen("md5"), port->md5Salt,
								sizeof(port->md5Salt), crypt_pwd))
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
				crypt_pwd = crypt(passwd, salt);
				break;
			}
		default:
			crypt_pwd = passwd;
			break;
	}

	if (strcmp(pgpass, crypt_pwd) == 0)
	{
		/*
		 * Password OK, now check to be sure we are not past valuntil
		 */
		AbsoluteTime vuntil,
					current;

		if (!valuntil)
			vuntil = INVALID_ABSTIME;
		else
			vuntil = DatumGetAbsoluteTime(DirectFunctionCall1(nabstimein,
											 CStringGetDatum(valuntil)));
		current = GetCurrentAbsoluteTime();
		if (vuntil != INVALID_ABSTIME && vuntil < current)
			retval = STATUS_ERROR;
		else
			retval = STATUS_OK;
	}

	pfree(passwd);
	if (valuntil)
		pfree(valuntil);
	if (port->auth_method == uaMD5)
		pfree(crypt_pwd);

	return retval;
}
