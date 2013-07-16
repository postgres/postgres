/*-------------------------------------------------------------------------
 *
 * streamutil.c - utility functions for pg_basebackup and pg_receivelog
 *
 * Author: Magnus Hagander <magnus@hagander.net>
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  src/bin/pg_basebackup/streamutil.c
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"
#include "streamutil.h"

#include <stdio.h>
#include <string.h>

const char *progname;
char	   *connection_string = NULL;
char	   *dbhost = NULL;
char	   *dbuser = NULL;
char	   *dbport = NULL;
int			dbgetpassword = 0;	/* 0=auto, -1=never, 1=always */
static char *dbpassword = NULL;
PGconn	   *conn = NULL;

/*
 * Connect to the server. Returns a valid PGconn pointer if connected,
 * or NULL on non-permanent error. On permanent error, the function will
 * call exit(1) directly.
 */
PGconn *
GetConnection(void)
{
	PGconn	   *tmpconn;
	int			argcount = 7;	/* dbname, replication, fallback_app_name,
								 * host, user, port, password */
	int			i;
	const char **keywords;
	const char **values;
	char	   *password = NULL;
	const char *tmpparam;
	PQconninfoOption *conn_opts = NULL;
	PQconninfoOption *conn_opt;
	char	   *err_msg = NULL;

	/*
	 * Merge the connection info inputs given in form of connection string,
	 * options and default values (dbname=replication, replication=true, etc.)
	 */
	i = 0;
	if (connection_string)
	{
		conn_opts = PQconninfoParse(connection_string, &err_msg);
		if (conn_opts == NULL)
		{
			fprintf(stderr, "%s: %s", progname, err_msg);
			exit(1);
		}

		for (conn_opt = conn_opts; conn_opt->keyword != NULL; conn_opt++)
		{
			if (conn_opt->val != NULL && conn_opt->val[0] != '\0')
				argcount++;
		}

		keywords = pg_malloc0((argcount + 1) * sizeof(*keywords));
		values = pg_malloc0((argcount + 1) * sizeof(*values));

		for (conn_opt = conn_opts; conn_opt->keyword != NULL; conn_opt++)
		{
			if (conn_opt->val != NULL && conn_opt->val[0] != '\0')
			{
				keywords[i] = conn_opt->keyword;
				values[i] = conn_opt->val;
				i++;
			}
		}
	}
	else
	{
		keywords = pg_malloc0((argcount + 1) * sizeof(*keywords));
		values = pg_malloc0((argcount + 1) * sizeof(*values));
	}

	keywords[i] = "dbname";
	values[i] = "replication";
	i++;
	keywords[i] = "replication";
	values[i] = "true";
	i++;
	keywords[i] = "fallback_application_name";
	values[i] = progname;
	i++;

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
			keywords[i] = "password";
			values[i] = dbpassword;
			dbgetpassword = -1; /* Don't try again if this fails */
		}
		else if (dbgetpassword == 1)
		{
			password = simple_prompt(_("Password: "), 100, false);
			keywords[i] = "password";
			values[i] = password;
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
			if (conn_opts)
				PQconninfoFree(conn_opts);
			return NULL;
		}

		/* Connection ok! */
		free(values);
		free(keywords);
		if (conn_opts)
			PQconninfoFree(conn_opts);

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
