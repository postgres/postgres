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
 * $Header: /cvsroot/pgsql/src/backend/libpq/crypt.c,v 1.40 2001/11/01 18:10:48 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <errno.h>
#include <unistd.h>

#include "libpq/crypt.h"
#include "libpq/libpq.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "utils/nabstime.h"

#ifdef HAVE_CRYPT_H
#include <crypt.h>
#endif

char	  **pwd_cache = NULL;
int			pwd_cache_count = 0;

/*
 * crypt_getpwdfilename --- get name of password file
 *
 * Note that result string is palloc'd, and should be freed by the caller.
 */
char *
crypt_getpwdfilename(void)
{
	int			bufsize;
	char	   *pfnam;

	bufsize = strlen(DataDir) + 8 + strlen(CRYPT_PWD_FILE) + 1;
	pfnam = (char *) palloc(bufsize);
	snprintf(pfnam, bufsize, "%s/global/%s", DataDir, CRYPT_PWD_FILE);

	return pfnam;
}

/*
 * crypt_getpwdreloadfilename --- get name of password-reload-needed flag file
 *
 * Note that result string is palloc'd, and should be freed by the caller.
 */
char *
crypt_getpwdreloadfilename(void)
{
	char	   *pwdfilename;
	int			bufsize;
	char	   *rpfnam;

	pwdfilename = crypt_getpwdfilename();
	bufsize = strlen(pwdfilename) + strlen(CRYPT_PWD_RELOAD_SUFX) + 1;
	rpfnam = (char *) palloc(bufsize);
	snprintf(rpfnam, bufsize, "%s%s", pwdfilename, CRYPT_PWD_RELOAD_SUFX);
	pfree(pwdfilename);

	return rpfnam;
}

/*-------------------------------------------------------------------------*/

static FILE *
crypt_openpwdfile(void)
{
	char	   *filename;
	FILE	   *pwdfile;

	filename = crypt_getpwdfilename();
	pwdfile = AllocateFile(filename, "r");

	if (pwdfile == NULL && errno != ENOENT)
		elog(DEBUG, "could not open %s: %m", filename);

	pfree(filename);

	return pwdfile;
}

/*
 * Compare two password-file lines on the basis of their usernames.
 *
 * Can also be used to compare just a username against a password-file
 * line (for bsearch).
 */
static int
compar_user(const void *user_a, const void *user_b)
{
	char	   *login_a;
	char	   *login_b;
	int			len_a,
				len_b,
				result;

	login_a = *((char **) user_a);
	login_b = *((char **) user_b);

	/*
	 * We only really want to compare the user logins which are first
	 * and are terminated by CRYPT_PWD_FILE_SEPSTR.  (NB: this code
	 * effectively assumes that CRYPT_PWD_FILE_SEPSTR is just one char.)
	 */
	len_a = strcspn(login_a, CRYPT_PWD_FILE_SEPSTR);
	len_b = strcspn(login_b, CRYPT_PWD_FILE_SEPSTR);

	result = strncmp(login_a, login_b, Min(len_a, len_b));

	if (result == 0)			/* one could be a prefix of the other */
		result = (len_a - len_b);

	return result;
}

/*-------------------------------------------------------------------------*/

static void
crypt_loadpwdfile(void)
{
	char	   *filename;
	int			result;
	FILE	   *pwd_file;
	char		buffer[1024];

	filename = crypt_getpwdreloadfilename();
	result = unlink(filename);
	pfree(filename);

	/*
	 * We want to delete the flag file before reading the contents of the
	 * pg_pwd file.  If result == 0 then the unlink of the reload file was
	 * successful. This means that a backend performed a COPY of the
	 * pg_shadow file to pg_pwd.  Therefore we must now do a reload.
	 */
	if (!pwd_cache || result == 0)
	{
		/* free the old data only if this is a reload */
		if (pwd_cache)
		{
			while (pwd_cache_count--)
				free((void *) pwd_cache[pwd_cache_count]);
			free((void *) pwd_cache);
			pwd_cache = NULL;
			pwd_cache_count = 0;
		}

		if (!(pwd_file = crypt_openpwdfile()))
			return;

		/*
		 * Here is where we load the data from pg_pwd.
		 */
		while (fgets(buffer, sizeof(buffer), pwd_file) != NULL)
		{
			/*
			 * We must remove the return char at the end of the string, as
			 * this will affect the correct parsing of the password entry.
			 */
			if (buffer[(result = strlen(buffer) - 1)] == '\n')
				buffer[result] = '\0';

			pwd_cache = (char **)
				realloc((void *) pwd_cache,
						sizeof(char *) * (pwd_cache_count + 1));
			pwd_cache[pwd_cache_count++] = strdup(buffer);
		}
		FreeFile(pwd_file);

		/*
		 * Now sort the entries in the cache for faster searching later.
		 */
		qsort((void *) pwd_cache, pwd_cache_count, sizeof(char *), compar_user);
	}
}

/*-------------------------------------------------------------------------*/

static void
crypt_parsepwdentry(char *buffer, char **pwd, char **valdate)
{
	char	   *parse = buffer;
	int			count,
				i;

	/*
	 * skip to the password field
	 */
	for (i = 0; i < 6; i++)
		parse += (strcspn(parse, CRYPT_PWD_FILE_SEPSTR) + 1);

	/*
	 * store a copy of user password to return
	 */
	count = strcspn(parse, CRYPT_PWD_FILE_SEPSTR);
	*pwd = (char *) palloc(count + 1);
	strncpy(*pwd, parse, count);
	(*pwd)[count] = '\0';
	parse += (count + 1);

	/*
	 * store a copy of the date login becomes invalid
	 */
	count = strcspn(parse, CRYPT_PWD_FILE_SEPSTR);
	*valdate = (char *) palloc(count + 1);
	strncpy(*valdate, parse, count);
	(*valdate)[count] = '\0';
	parse += (count + 1);
}

/*-------------------------------------------------------------------------*/

static int
crypt_getloginfo(const char *user, char **passwd, char **valuntil)
{
	crypt_loadpwdfile();

	if (pwd_cache)
	{
		char	  **pwd_entry;

		pwd_entry = (char **) bsearch((void *) &user,
									  (void *) pwd_cache,
									  pwd_cache_count,
									  sizeof(char *),
									  compar_user);
		if (pwd_entry)
		{
			crypt_parsepwdentry(*pwd_entry, passwd, valuntil);
			return STATUS_OK;
		}
	}

	*passwd = NULL;
	*valuntil = NULL;
	return STATUS_ERROR;
}

/*-------------------------------------------------------------------------*/

int
md5_crypt_verify(const Port *port, const char *user, const char *pgpass)
{
	char	   *passwd,
			   *valuntil,
			   *crypt_pwd;
	int			retval = STATUS_ERROR;

	if (crypt_getloginfo(user, &passwd, &valuntil) == STATUS_ERROR)
		return STATUS_ERROR;

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
		snprintf(PQerrormsg, PQERRORMSG_LENGTH,
				 "Password is stored MD5 encrypted.  "
				 "'password' and 'crypt' auth methods cannot be used.\n");
		fputs(PQerrormsg, stderr);
		pqdebug("%s", PQerrormsg);
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

		if (!valuntil || strcmp(valuntil, "\\N") == 0)
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
