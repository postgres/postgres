#include <ecpgtype.h>
#include <ecpglib.h>
#include <ecpgerrno.h>
#include "extern.h"
#include <sqlca.h>

static struct connection *all_connections = NULL, *actual_connection = NULL;

struct connection *
get_connection(const char *connection_name)
{
	struct connection *con = all_connections;

	if (connection_name == NULL || strcmp(connection_name, "CURRENT") == 0)
		return actual_connection;

	for (; con && strcmp(connection_name, con->name) != 0; con = con->next);
	if (con)
		return con;
	else
		return NULL;
}

static void
ecpg_finish(struct connection * act)
{
	if (act != NULL)
	{
		ECPGlog("ecpg_finish: finishing %s.\n", act->name);
		PQfinish(act->connection);

		/* remove act from the list */
		if (act == all_connections)
			all_connections = act->next;
		else
		{
			struct connection *con;

			for (con = all_connections; con->next && con->next != act; con = con->next);
			if (con->next)
				con->next = act->next;
		}

		if (actual_connection == act)
			actual_connection = all_connections;

		free(act->name);
		free(act);
	}
	else
		ECPGlog("ecpg_finish: called an extra time.\n");
}

bool
ECPGsetcommit(int lineno, const char *mode, const char *connection_name)
{
	struct connection *con = get_connection(connection_name);
	PGresult   *results;

	if (!ecpg_init(con, connection_name, lineno))
		return(false);

	ECPGlog("ECPGsetcommit line %d action = %s connection = %s\n", lineno, mode, con->name);
	
	if (con->autocommit == true && strncmp(mode, "off", strlen("off")) == 0)
	{
		if (con->committed)
		{
			if ((results = PQexec(con->connection, "begin transaction")) == NULL)
			{
				ECPGraise(lineno, ECPG_TRANS, NULL);
				return false;
			}
			PQclear(results);
			con->committed = false;
		}
		con->autocommit = false;
	}
	else if (con->autocommit == false && strncmp(mode, "on", strlen("on")) == 0)
	{
		if (!con->committed)
		{
			if ((results = PQexec(con->connection, "commit")) == NULL)
			{
				ECPGraise(lineno, ECPG_TRANS, NULL);
				return false;
			}
			PQclear(results);
			con->committed = true;
		}
		con->autocommit = true;
	}

	return true;
}

bool
ECPGsetconn(int lineno, const char *connection_name)
{
	struct connection *con = get_connection(connection_name);

	if (!ecpg_init(con, connection_name, lineno))
		return(false);

	actual_connection = con;
	return true;
}

bool
ECPGconnect(int lineno, const char *dbname, const char *user, const char *passwd, const char *connection_name, int autocommit)
{
	struct connection *this;

	init_sqlca();
	
	if ((this = (struct connection *) ecpg_alloc(sizeof(struct connection), lineno)) == NULL)
		return false;

	if (dbname == NULL && connection_name == NULL)
		connection_name = "DEFAULT";

	/* add connection to our list */
	if (connection_name != NULL)
		this->name = ecpg_strdup(connection_name, lineno);
	else
		this->name = ecpg_strdup(dbname, lineno);

	if (all_connections == NULL)
		this->next = NULL;
	else
		this->next = all_connections;

	actual_connection = all_connections = this;

	ECPGlog("ECPGconnect: opening database %s %s%s\n", dbname ? dbname : "<DEFAULT>", user ? "for user " : "", user ? user : "");

	this->connection = PQsetdbLogin(NULL, NULL, NULL, NULL, dbname, user, passwd);

	if (PQstatus(this->connection) == CONNECTION_BAD)
	{
		ecpg_finish(this);
		ECPGlog("connect: could not open database %s %s%s in line %d\n", dbname ? dbname : "<DEFAULT>", user ? "for user " : "", user ? user : "", lineno);
		ECPGraise(lineno, ECPG_CONNECT, dbname ? dbname : "<DEFAULT>");
		return false;
	}

	this->committed = true;
	this->autocommit = autocommit;

	return true;
}

bool
ECPGdisconnect(int lineno, const char *connection_name)
{
	struct connection *con;

	if (strcmp(connection_name, "ALL") == 0)
	{
		init_sqlca();
		for (con = all_connections; con;)
		{
			struct connection *f = con;

			con = con->next;
			ecpg_finish(f);
		}
	}
	else
	{
		con = get_connection(connection_name);

		if (!ecpg_init(con, connection_name, lineno))
		        return(false);
		else
			ecpg_finish(con);
	}

	return true;
}
