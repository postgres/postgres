/*
 * pginterface.c
 *
*/

#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <stdarg.h>

#include <libpq-fe.h>
#include "halt.h"
#include "pginterface.h"

static void sig_disconnect();
static void set_signals();

#define NUL '\0'

/* GLOBAL VARIABLES */
static PGconn *conn;
static PGresult *res = NULL;

#define ON_ERROR_STOP	0
#define ON_ERROR_CONTINUE		1

static int	on_error_state = ON_ERROR_STOP;

/* LOCAL VARIABLES */
static sigset_t block_sigs,
			unblock_sigs;
static int	tuple;

/*
**
**		connectdb - returns PGconn structure
**
*/
PGconn	   *
connectdb(char *dbName,
		  char *pghost,
		  char *pgport,
		  char *pgoptions,
		  char *pgtty)
{
	/* make a connection to the database */
	conn = PQsetdb(pghost, pgport, pgoptions, pgtty, dbName);
	if (PQstatus(conn) == CONNECTION_BAD)
		halt("Connection to database '%s' failed.\n%s\n", dbName,
			 PQerrorMessage(conn));
	set_signals();
	return conn;
}

/*
**
**		disconnectdb
**
*/
void
disconnectdb()
{
	PQfinish(conn);
}

/*
**
**		doquery - returns PGresult structure
**
*/
PGresult   *
doquery(char *query)
{
	if (res != NULL)
		PQclear(res);

	sigprocmask(SIG_SETMASK, &block_sigs, NULL);
	res = PQexec(conn, query);
	sigprocmask(SIG_SETMASK, &unblock_sigs, NULL);

	if (on_error_state == ON_ERROR_STOP &&
		(res == NULL ||
		 PQresultStatus(res) == PGRES_BAD_RESPONSE ||
		 PQresultStatus(res) == PGRES_NONFATAL_ERROR ||
		 PQresultStatus(res) == PGRES_FATAL_ERROR))
	{
		if (res != NULL)
			fprintf(stderr, "query error:  %s\n", PQcmdStatus(res));
		else
			fprintf(stderr, "connection error:  %s\n", PQerrorMessage(conn));
		PQfinish(conn);
		halt("failed request:  %s\n", query);
	}
	tuple = 0;
	return res;
}

/*
**
**		fetch - returns tuple number (starts at 0), or the value END_OF_TUPLES
**						NULL pointers are skipped
**
*/
int
fetch(void *param,...)
{
	va_list		ap;
	int			arg,
				num_fields;

	num_fields = PQnfields(res);

	if (tuple >= PQntuples(res))
		return END_OF_TUPLES;

	va_start(ap, param);
	for (arg = 0; arg < num_fields; arg++)
	{
		if (param != NULL)
		{
			if (PQfsize(res, arg) == -1)
			{
				memcpy(param, PQgetvalue(res, tuple, arg), PQgetlength(res, tuple, arg));
				((char *) param)[PQgetlength(res, tuple, arg)] = NUL;
			}
			else
				memcpy(param, PQgetvalue(res, tuple, arg), PQfsize(res, arg));
		}
		param = va_arg(ap, char *);
	}
	va_end(ap);
	return tuple++;
}

/*
**
**		fetchwithnulls - returns tuple number (starts at 0),
**																						or the value END_OF_TUPLES
**								Returns true or false into null indicator variables
**								NULL pointers are skipped
*/
int
fetchwithnulls(void *param,...)
{
	va_list		ap;
	int			arg,
				num_fields;

	num_fields = PQnfields(res);

	if (tuple >= PQntuples(res))
		return END_OF_TUPLES;

	va_start(ap, param);
	for (arg = 0; arg < num_fields; arg++)
	{
		if (param != NULL)
		{
			if (PQfsize(res, arg) == -1)
			{
				memcpy(param, PQgetvalue(res, tuple, arg), PQgetlength(res, tuple, arg));
				((char *) param)[PQgetlength(res, tuple, arg)] = NUL;
			}
			else
				memcpy(param, PQgetvalue(res, tuple, arg), PQfsize(res, arg));
		}
		param = va_arg(ap, char *);
		if (PQgetisnull(res, tuple, arg) != 0)
			*(int *) param = 1;
		else
			*(int *) param = 0;
		param = va_arg(ap, char *);
	}
	va_end(ap);
	return tuple++;
}

/*
**
**		on_error_stop
**
*/
void
on_error_stop()
{
	on_error_state = ON_ERROR_STOP;
}

/*
**
**		on_error_continue
**
*/
void
on_error_continue()
{
	on_error_state = ON_ERROR_CONTINUE;
}

/*
**
**		sig_disconnect
**
*/
static void
sig_disconnect()
{
	fprintf(stderr, "exiting...\n");
	PQfinish(conn);
	exit(1);
}

/*
**
**		set_signals
**
*/
static void
set_signals()
{
	sigemptyset(&block_sigs);
	sigemptyset(&unblock_sigs);
	sigaddset(&block_sigs, SIGTERM);
	sigaddset(&block_sigs, SIGHUP);
	sigaddset(&block_sigs, SIGINT);
/*		sigaddset(&block_sigs,SIGQUIT); no block */
	sigprocmask(SIG_SETMASK, &unblock_sigs, NULL);
	signal(SIGTERM, sig_disconnect);
	signal(SIGHUP, sig_disconnect);
	signal(SIGINT, sig_disconnect);
	signal(SIGQUIT, sig_disconnect);
}
