/*
 * pginterface.c
 *
*/

#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include <libpq-fe.h>
#include "halt.h"
#include "pginterface.h"

#define NUL '\0'

#ifndef TRUE
#define TRUE 1
#endif

#ifndef FALSE
#define FALSE 0
#endif

/* GLOBAL VARIABLES */
static PGconn *conn;
static PGresult *res = NULL;

#define ON_ERROR_STOP	0
#define ON_ERROR_CONTINUE		1

static int	on_error_state = ON_ERROR_STOP;

static in_result_block = FALSE;
static was_get_unset_result = FALSE;

/* LOCAL VARIABLES */
static int	tuple;

/*
**
**		connectdb - returns PGconn structure
**
*/
PGconn *
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
	if (res != NULL && in_result_block == FALSE && was_get_unset_result == FALSE)
		PQclear(res);

	was_get_unset_result = FALSE;
	res = PQexec(conn, query);

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
**								Returns TRUE or FALSE into null indicator variables
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
**		get_result
**
*/
PGresult   *
get_result()
{
	char	   *cmdstatus = PQcmdStatus(res);

	was_get_unset_result = TRUE;

	/* we have to store the fetch location somewhere */
	cmdstatus[0] = NUL;
	memcpy(&cmdstatus[1], &tuple, sizeof(tuple));

	return res;
}

/*
**
**		set_result
**
*/
void
set_result(PGresult *newres)
{

	char	   *cmdstatus = PQcmdStatus(res);

	if (newres == NULL)
		halt("set_result called with null result pointer\n");

	if (res != NULL && was_get_unset_result == FALSE)
		if (in_result_block == FALSE)
			PQclear(res);
		else
		{
			cmdstatus[0] = NUL;
			memcpy(&cmdstatus[1], &tuple, sizeof(tuple));
		}

	in_result_block = TRUE;
	was_get_unset_result = FALSE;

	cmdstatus = PQcmdStatus(newres);
	memcpy(&tuple, &cmdstatus[1], sizeof(tuple));

	res = newres;
}


/*
**
**		unset_result
**
*/
void
unset_result(PGresult *oldres)
{
	char	   *cmdstatus = PQcmdStatus(oldres);

	if (oldres == NULL)
		halt("unset_result called with null result pointer\n");

	if (in_result_block == FALSE)
		halt("Unset of result without being set.\n");

	was_get_unset_result = TRUE;
	cmdstatus[0] = NUL;
	memcpy(&cmdstatus[1], &tuple, sizeof(tuple));
	in_result_block = FALSE;
}

/*
**
**		reset_fetch
**
*/
void
reset_fetch()
{
	tuple = 0;
}
