/* src/interfaces/ecpg/ecpglib/cursor.c */

#define POSTGRES_ECPG_INTERNAL
#include "postgres_fe.h"

#include <ctype.h>
#include <locale.h>
#include <string.h>

#include "ecpgtype.h"
#include "ecpglib.h"
#include "ecpgerrno.h"
#include "ecpglib_extern.h"
#include "sqlca.h"

static void add_cursor(const int, const char *, const char *);
static void remove_cursor(const char *, struct connection *);
static bool find_cursor(const char *, const struct connection *);

/*
 *	Function: Handle the EXEC SQL OPEN cursor statement:
 *	Input:
 *		cursor_name --- cursor name
 *		prepared_name --- prepared name
 *		others --- keep same as the parameters in ECPGdo() function
 */
bool
ECPGopen(const char *cursor_name,const char *prepared_name,
		const int lineno, const int compat,const int force_indicator,
		const char *connection_name, const bool questionmarks,
		const int st, const char *query,...)
{
	va_list		args;
	bool		status;
	const char	*real_connection_name = NULL;

	if (!query)
	{
		ecpg_raise(lineno, ECPG_EMPTY, ECPG_SQLSTATE_ECPG_INTERNAL_ERROR, NULL);
		return false;
	}

	/*
	 * If the declared name is referred by the PREPARE statement then the
	 * prepared_name is same as declared name
	 */
	real_connection_name = ecpg_get_con_name_by_declared_name(prepared_name);
	if (real_connection_name)
	{
		/* Add the cursor name into the declared node */
		ecpg_update_declare_statement(prepared_name, cursor_name, lineno);
	}
	else
	{
		/*
		 * If can't get the connection name by declared name then using connection name
		 * coming from the parameter connection_name
		 */
		real_connection_name = connection_name;
	}


	/* Add the cursor into the connection */
	add_cursor(lineno, cursor_name, real_connection_name);

	va_start(args, query);

	status = ecpg_do(lineno, compat, force_indicator, real_connection_name, questionmarks, st, query, args);

	va_end(args);

	return status;
}


/*
 *	Function: Handle the EXEC SQL FETCH/MOVE CURSOR statements:
 *	Input:
 *		cursor_name --- cursor name
 *		others --- keep same as the parameters in ECPGdo() function
 */
bool
ECPGfetch(const char *cursor_name,
		const int lineno, const int compat,const int force_indicator,
		const char *connection_name, const bool questionmarks,
		const int st, const char *query,...)
{
	va_list		args;
	bool		status;
	const char	*real_connection_name = NULL;

	if (!query)
	{
		ecpg_raise(lineno, ECPG_EMPTY, ECPG_SQLSTATE_ECPG_INTERNAL_ERROR, NULL);
		return (false);
	}

	real_connection_name = ecpg_get_con_name_by_cursor_name(cursor_name);
	if (real_connection_name == NULL)
	{
		/*
		 * If can't get the connection name by cursor name then using connection name
		 * coming from the parameter connection_name
		 */
		real_connection_name = connection_name;
	}

	va_start(args, query);

	status = ecpg_do(lineno, compat, force_indicator, real_connection_name, questionmarks, st, query, args);

	va_end(args);

	return status;
}


/*
 *	Function: Handle the EXEC SQL CLOSE CURSOR statements:
 *	Input:
 *		cursor_name --- cursor name
 *		others --- keep same as the parameters in ECPGdo() function
 */
bool
ECPGclose(const char *cursor_name,
		const int lineno, const int compat,const int force_indicator,
		const char *connection_name, const bool questionmarks,
		const int st, const char *query,...)
{
	va_list		args;
	bool		status;
	const char	*real_connection_name = NULL;
	struct connection *con = NULL;

	if (!query)
	{
		ecpg_raise(lineno, ECPG_EMPTY, ECPG_SQLSTATE_ECPG_INTERNAL_ERROR, NULL);
		return false;
	}

	real_connection_name = ecpg_get_con_name_by_cursor_name(cursor_name);
	if (real_connection_name == NULL)
	{
		/*
		 * If can't get the connection name by cursor name then using connection name
		 * coming from the parameter connection_name
		 */
		real_connection_name = connection_name;
	}

	con = ecpg_get_connection(real_connection_name);

	/* check the existence of the cursor in the connection */
	if (find_cursor(cursor_name, con) == false)
	{
		ecpg_raise(lineno, ECPG_INVALID_CURSOR, ECPG_SQLSTATE_ECPG_INTERNAL_ERROR, NULL);
		return false;
	}

	va_start(args, query);

	status = ecpg_do(lineno, compat, force_indicator, real_connection_name, questionmarks, st, query, args);

	va_end(args);

	remove_cursor(cursor_name, con);

	return status;
}

/*
 * Function: Add a cursor into the connection
 * The duplication of cursor_name is checked at ecpg.trailer,
 * so we don't check here.
 */
static void
add_cursor(const int lineno, const char *cursor_name, const char *connection_name)
{
	struct connection *con;
	struct cursor_statement *new = NULL;

	if (!cursor_name)
	{
		ecpg_raise(lineno, ECPG_INVALID_CURSOR, ECPG_SQLSTATE_ECPG_INTERNAL_ERROR, NULL);
		return;
	}

	con = ecpg_get_connection(connection_name);
	if (!con)
	{
		ecpg_raise(lineno, ECPG_NO_CONN, ECPG_SQLSTATE_CONNECTION_DOES_NOT_EXIST,
			   connection_name ? connection_name : ecpg_gettext("NULL"));
		return;
	}

	/* allocate a node to store the new cursor */
	new = (struct cursor_statement *)ecpg_alloc(sizeof(struct cursor_statement), lineno);
	if (new)
	{
		new->name = ecpg_strdup(cursor_name, lineno);
		new->next = con->cursor_stmts;
		con->cursor_stmts = new;
	}
}

/*
 * Function: Remove the cursor from the connection
 */
static void
remove_cursor(const char *cursor_name, struct connection *connection)
{
	struct cursor_statement *cur = NULL;
	struct cursor_statement *prev = NULL;

	if (!connection || !cursor_name)
		return;

	cur = connection->cursor_stmts;
	while (cur)
	{
		if (strcmp(cur->name, cursor_name) == 0)
		{
			if (!prev)
				connection->cursor_stmts = cur->next;
			else
				prev->next = cur->next;

			ecpg_free(cur->name);
			ecpg_free(cur);

			break;
		}
		prev = cur;
		cur = cur->next;
	}
}

/*
 * Function: check the existence of the cursor in the connection
 * Return: true ---Found
 *		   false --- Not found
 */
static bool
find_cursor(const char *cursor_name, const struct connection *connection)
{
	struct cursor_statement *cur = NULL;

	if (!connection || !cursor_name)
		return false;

	for (cur = connection->cursor_stmts; cur != NULL; cur = cur->next)
	{
		if (strcmp(cur->name, cursor_name) == 0)
			return true;
	}

	return false;
}
