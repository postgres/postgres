/*-------------------------------------------------------------------------
 *
 * streamutil.c - utility functions for pg_basebackup and pg_receivelog
 *
 * Author: Magnus Hagander <magnus@hagander.net>
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  src/bin/pg_basebackup/streamutil.c
 *-------------------------------------------------------------------------
 */

/*
 * We have to use postgres.h not postgres_fe.h here, because there's so much
 * backend-only stuff in the XLOG include files we need.  But we need a
 * frontend-ish environment otherwise.  Hence this ugly hack.
 */
#define FRONTEND 1
#include "postgres.h"
#include "streamutil.h"

#include <stdio.h>
#include <string.h>

const char *progname;
char	   *dbhost = NULL;
char	   *dbuser = NULL;
char	   *dbport = NULL;
int			dbgetpassword = 0;	/* 0=auto, -1=never, 1=always */
static char *dbpassword = NULL;
PGconn	   *conn = NULL;

/*
 * strdup() and malloc() replacements that prints an error and exits
 * if something goes wrong. Can never return NULL.
 */
char *
xstrdup(const char *s)
{
	char	   *result;

	result = strdup(s);
	if (!result)
	{
		fprintf(stderr, _("%s: out of memory\n"), progname);
		exit(1);
	}
	return result;
}

void *
xmalloc0(int size)
{
	void	   *result;

	/* Avoid unportable behavior of malloc(0) */
	if (size == 0)
		size = 1;
	result = malloc(size);
	if (!result)
	{
		fprintf(stderr, _("%s: out of memory\n"), progname);
		exit(1);
	}
	MemSet(result, 0, size);
	return result;
}


/*
 * Connect to the server. Returns a valid PGconn pointer if connected,
 * or NULL on non-permanent error. On permanent error, the function will
 * call exit(1) directly.
 */
PGconn *
GetConnection(void)
{
	PGconn	   *tmpconn;
	int			argcount = 4;	/* dbname, replication, fallback_app_name,
								 * password */
	int			i;
	const char **keywords;
	const char **values;
	char	   *password = NULL;
	const char *tmpparam;

	if (dbhost)
		argcount++;
	if (dbuser)
		argcount++;
	if (dbport)
		argcount++;

	keywords = xmalloc0((argcount + 1) * sizeof(*keywords));
	values = xmalloc0((argcount + 1) * sizeof(*values));

	keywords[0] = "dbname";
	values[0] = "replication";
	keywords[1] = "replication";
	values[1] = "true";
	keywords[2] = "fallback_application_name";
	values[2] = progname;
	i = 3;
	if (dbhost)
	{
		keywords[i] = "host";
		values[i] = dbhost;
		i++;
	}
	if (dbuser)
	{
		keywords[i] = "user";
		values[i] = dbuser;
		i++;
	}
	if (dbport)
	{
		keywords[i] = "port";
		values[i] = dbport;
		i++;
	}

	while (true)
	{
		if (password)
			free(password);

		if (dbpassword)
		{
			/*
			 * We've saved a password when a previous connection succeeded,
			 * meaning this is the call for a second session to the same
			 * database, so just forcibly reuse that password.
			 */
			keywords[argcount - 1] = "password";
			values[argcount - 1] = dbpassword;
			dbgetpassword = -1; /* Don't try again if this fails */
		}
		else if (dbgetpassword == 1)
		{
			password = simple_prompt(_("Password: "), 100, false);
			keywords[argcount - 1] = "password";
			values[argcount - 1] = password;
		}

		tmpconn = PQconnectdbParams(keywords, values, true);

		/*
		 * If there is too little memory even to allocate the PGconn object
		 * and PQconnectdbParams returns NULL, we call exit(1) directly.
		 */
		if (!tmpconn)
		{
			fprintf(stderr, _("%s: could not connect to server\n"),
					progname);
			exit(1);
		}

		if (PQstatus(tmpconn) == CONNECTION_BAD &&
			PQconnectionNeedsPassword(tmpconn) &&
			dbgetpassword != -1)
		{
			dbgetpassword = 1;	/* ask for password next time */
			PQfinish(tmpconn);
			continue;
		}

		if (PQstatus(tmpconn) != CONNECTION_OK)
		{
			fprintf(stderr, _("%s: could not connect to server: %s\n"),
					progname, PQerrorMessage(tmpconn));
			PQfinish(tmpconn);
			free(values);
			free(keywords);
			return NULL;
		}

		/* Connection ok! */
		free(values);
		free(keywords);

		/*
		 * Ensure we have the same value of integer timestamps as the server
		 * we are connecting to.
		 */
		tmpparam = PQparameterStatus(tmpconn, "integer_datetimes");
		if (!tmpparam)
		{
			fprintf(stderr,
					_("%s: could not determine server setting for integer_datetimes\n"),
					progname);
			PQfinish(tmpconn);
			exit(1);
		}

#ifdef HAVE_INT64_TIMESTAMP
		if (strcmp(tmpparam, "on") != 0)
#else
		if (strcmp(tmpparam, "off") != 0)
#endif
		{
			fprintf(stderr,
			 _("%s: integer_datetimes compile flag does not match server\n"),
					progname);
			PQfinish(tmpconn);
			exit(1);
		}

		/* Store the password for next run */
		if (password)
			dbpassword = password;
		return tmpconn;
	}
}
