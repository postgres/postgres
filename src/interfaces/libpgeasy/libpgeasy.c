/*
 * pgeasy.c
 *
 */

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>

#include "libpq-fe.h"
#include "halt.h"
#include "libpgeasy.h"

#ifndef NUL
#define NUL '\0'
#endif

#ifndef TRUE
#define TRUE	1
#endif

#ifndef FALSE
#define FALSE	0
#endif

/* GLOBAL VARIABLES */
static PGconn *conn;
static PGresult *res = NULL;

static int	tuple;				/* stores fetch location */

#define ON_ERROR_STOP		0
#define ON_ERROR_CONTINUE	1

static int	on_error_state = ON_ERROR_STOP;		/* halt on errors? */

static int	user_has_res = FALSE;

static void add_res_tuple(void);
static void get_res_tuple(void);
static void del_res_tuple(void);


/*
 *	connectdb - returns PGconn structure
 */
PGconn *
connectdb(char *options)
{
	/* make a connection to the database */
	conn = PQconnectdb(options);
	if (PQstatus(conn) == CONNECTION_BAD)
		halt("Connection to database using '%s' failed.\n%s\n", options,
			 PQerrorMessage(conn));
	return conn;
}


/*
 *		disconnectdb
 */
void
disconnectdb()
{
	if (res != NULL && user_has_res == FALSE)
	{
		PQclear(res);
		res = NULL;
	}

	PQfinish(conn);
}


/*
 *	doquery - returns PGresult structure
 */
PGresult *
doquery(char *query)
{
	if (res != NULL && user_has_res == FALSE)
		PQclear(res);

	user_has_res = FALSE;
	res = PQexec(conn, query);

	if (on_error_state == ON_ERROR_STOP &&
		(res == NULL ||
		 PQresultStatus(res) == PGRES_BAD_RESPONSE ||
		 PQresultStatus(res) == PGRES_NONFATAL_ERROR ||
		 PQresultStatus(res) == PGRES_FATAL_ERROR))
	{
		if (res != NULL)
			fprintf(stderr, "query error:  %s\n", PQresultErrorMessage(res));
		else
			fprintf(stderr, "connection error:  %s\n", PQerrorMessage(conn));
		PQfinish(conn);
		halt("failed query:  %s\n", query);
	}
	tuple = 0;
	return res;
}


/*
 *	fetch - returns tuple number (starts at 0), or the value END_OF_TUPLES
 *			NULL pointers are skipped
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
 *		fetchwithnulls - returns tuple number (starts at 0),
 *																						or the value END_OF_TUPLES
 *		Returns TRUE or FALSE into null indicator variables
 *		NULL pointers are skipped
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
 *	reset_fetch
 */
void
reset_fetch()
{
	tuple = 0;
}


/*
 *	on_error_stop
 */
void
on_error_stop()
{
	on_error_state = ON_ERROR_STOP;
}


/*
 *	on_error_continue
 */
void
on_error_continue()
{
	on_error_state = ON_ERROR_CONTINUE;
}


/*
 *	get_result
 */
PGresult *
get_result()
{
	if (res == NULL)
		halt("get_result called with no result pointer used\n");

	/* delete it if it is already there; we are about to re-add it */
	del_res_tuple();

	/* we have to store the fetch location */
	add_res_tuple();

	user_has_res = TRUE;

	return res;
}


/*
 *	set_result
 */
void
set_result(PGresult *newres)
{
	if (newres == NULL)
		halt("set_result called with null result pointer\n");

	if (res != NULL && user_has_res == FALSE)
	{
		/*
		 * Basically, throw away res. We can't return to it because the
		 * user doesn't have the res pointer.
		 */
		del_res_tuple();
		PQclear(res);
	}

	user_has_res = FALSE;

	res = newres;

	get_res_tuple();
}


/*
 *	Routines to store res/tuple mapping
 *	This is used to keep track of fetch locations while using get/set on
 *	result sets.
 *	Auto-growing array is used, with empty slots marked by res == NULL
 */

static struct res_tuple
{
	PGresult   *res;
	int			tuple;
}	*res_tuple = NULL;

static int	res_tuple_len = 0;


/*
 * add_res_tuple
 */
static void
add_res_tuple(void)
{
	int			i,
				new_res_tuple_len = res_tuple_len ? res_tuple_len * 2 : 1;

	for (i = 0; i < res_tuple_len; i++)
		/* Put it in an empty slot */
		if (res_tuple[i].res == NULL)
		{
			res_tuple[i].res = res;
			res_tuple[i].tuple = tuple;
		}

	/* Need to grow array */
	res_tuple = realloc(res_tuple, new_res_tuple_len * sizeof(struct res_tuple));

	/* clear new elements */
	for (i = res_tuple_len; i < new_res_tuple_len; i++)
	{
		res_tuple[i].res = NULL;
		res_tuple[i].tuple = 0;
	}

	/* recursion to add entry */
	add_res_tuple();
}


/*
 * get_res_tuple
 */
static void
get_res_tuple(void)
{
	int			i;

	for (i = 0; i < res_tuple_len; i++)
		if (res_tuple[i].res == res)
		{
			tuple = res_tuple[i].tuple;
			return;
		}
	halt("get_res_tuple called with invalid result pointer\n");
}


/*
 * del_res_tuple
 */
static void
del_res_tuple(void)
{
	int			i;

	for (i = 0; i < res_tuple_len; i++)
		if (res_tuple[i].res == res)
		{
			res_tuple[i].res = NULL;
			return;
		}
	return;
}
