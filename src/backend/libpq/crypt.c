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
 * $Header: /cvsroot/pgsql/src/backend/libpq/crypt.c,v 1.44 2002/03/04 01:46:03 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <errno.h>
#include <unistd.h>
#ifdef HAVE_CRYPT_H
#include <crypt.h>
#endif

#include "libpq/crypt.h"
#include "libpq/libpq.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "utils/nabstime.h"


#define CRYPT_PWD_FILE	"pg_pwd"


static char **pwd_cache = NULL;
static int	pwd_cache_count = 0;

/*
 * crypt_getpwdfilename --- get full pathname of password file
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
 * Open the password file if possible (return NULL if not)
 */
static FILE *
crypt_openpwdfile(void)
{
	char	   *filename;
	FILE	   *pwdfile;

	filename = crypt_getpwdfilename();
	pwdfile = AllocateFile(filename, "r");

	if (pwdfile == NULL && errno != ENOENT)
		elog(LOG, "could not open %s: %m", filename);

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
	 * We only really want to compare the user logins which are first and
	 * are terminated by CRYPT_PWD_FILE_SEPSTR.  (NB: this code
	 * effectively assumes that CRYPT_PWD_FILE_SEPSTR is just one char.)
	 */
	len_a = strcspn(login_a, CRYPT_PWD_FILE_SEPSTR);
	len_b = strcspn(login_b, CRYPT_PWD_FILE_SEPSTR);

	result = strncmp(login_a, login_b, Min(len_a, len_b));

	if (result == 0)			/* one could be a prefix of the other */
		result = (len_a - len_b);

	return result;
}

/*
 * Load or reload the password-file cache
 */
void
load_password_cache(void)
{
	FILE	   *pwd_file;
	char		buffer[1024];

	/*
	 * If for some reason we fail to open the password file, preserve the
	 * old cache contents; this seems better than dropping the cache if,
	 * say, we are temporarily out of filetable slots.
	 */
	if (!(pwd_file = crypt_openpwdfile()))
		return;

	/* free any old data */
	if (pwd_cache)
	{
		while (--pwd_cache_count >= 0)
			pfree(pwd_cache[pwd_cache_count]);
		pfree(pwd_cache);
		pwd_cache = NULL;
		pwd_cache_count = 0;
	}

	/*
	 * Read the file and store its lines in current memory context, which
	 * we expect will be PostmasterContext.  That context will live as
	 * long as we need the cache to live, ie, until just after each
	 * postmaster child has completed client authentication.
	 */
	while (fgets(buffer, sizeof(buffer), pwd_file) != NULL)
	{
		int			blen;

		/*
		 * We must remove the return char at the end of the string, as
		 * this will affect the correct parsing of the password entry.
		 */
		if (buffer[(blen = strlen(buffer) - 1)] == '\n')
			buffer[blen] = '\0';

		if (pwd_cache == NULL)
			pwd_cache = (char **)
				palloc(sizeof(char *) * (pwd_cache_count + 1));
		else
			pwd_cache = (char **)
				repalloc((void *) pwd_cache,
						 sizeof(char *) * (pwd_cache_count + 1));
		pwd_cache[pwd_cache_count++] = pstrdup(buffer);
	}

	FreeFile(pwd_file);

	/*
	 * Now sort the entries in the cache for faster searching later.
	 */
	qsort((void *) pwd_cache, pwd_cache_count, sizeof(char *), compar_user);
}

/*
 * Parse a line of the password file to extract password and valid-until date.
 */
static bool
crypt_parsepwdentry(char *buffer, char **pwd, char **valdate)
{
	char	   *parse = buffer;
	int			count,
				i;

	*pwd = NULL;
	*valdate = NULL;

	/*
	 * skip to the password field
	 */
	for (i = 0; i < 6; i++)
	{
		parse += strcspn(parse, CRYPT_PWD_FILE_SEPSTR);
		if (*parse == '\0')
			return false;
		parse++;
	}

	/*
	 * store a copy of user password to return
	 */
	count = strcspn(parse, CRYPT_PWD_FILE_SEPSTR);
	*pwd = (char *) palloc(count + 1);
	memcpy(*pwd, parse, count);
	(*pwd)[count] = '\0';
	parse += count;
	if (*parse == '\0')
	{
		pfree(*pwd);
		*pwd = NULL;
		return false;
	}
	parse++;

	/*
	 * store a copy of the date login becomes invalid
	 */
	count = strcspn(parse, CRYPT_PWD_FILE_SEPSTR);
	*valdate = (char *) palloc(count + 1);
	memcpy(*valdate, parse, count);
	(*valdate)[count] = '\0';

	return true;
}

/*
 * Lookup a username in the password-file cache,
 * return his password and valid-until date.
 */
static bool
crypt_getloginfo(const char *user, char **passwd, char **valuntil)
{
	*passwd = NULL;
	*valuntil = NULL;

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
			if (crypt_parsepwdentry(*pwd_entry, passwd, valuntil))
				return true;
		}
	}

	return false;
}

/*-------------------------------------------------------------------------*/

int
md5_crypt_verify(const Port *port, const char *user, const char *pgpass)
{
	char	   *passwd,
			   *valuntil,
			   *crypt_pwd;
	int			retval = STATUS_ERROR;

	if (!crypt_getloginfo(user, &passwd, &valuntil))
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
