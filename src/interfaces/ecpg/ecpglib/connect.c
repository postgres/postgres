/* $Header: /cvsroot/pgsql/src/interfaces/ecpg/ecpglib/connect.c,v 1.17.2.1 2003/11/24 13:11:27 petere Exp $ */

#define POSTGRES_ECPG_INTERNAL
#include "postgres_fe.h"

#ifdef ENABLE_THREAD_SAFETY
#include <pthread.h>
#endif
#include "ecpgtype.h"
#include "ecpglib.h"
#include "ecpgerrno.h"
#include "extern.h"
#include "sqlca.h"

#ifdef ENABLE_THREAD_SAFETY
static pthread_mutex_t connections_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif
static struct connection *all_connections = NULL;
static struct connection *actual_connection = NULL;

static struct connection *
ecpg_get_connection_nr(const char *connection_name)
{
	struct connection *ret = NULL;

	if ((connection_name == NULL) || (strcmp(connection_name, "CURRENT") == 0))
		ret = actual_connection;
	else
	{
		struct connection *con;

		for (con = all_connections; con != NULL; con = con->next)
		{
			if (strcmp(connection_name, con->name) == 0)
				break;
		}
		ret = con;
	}

	return (ret);
}

struct connection *
ECPGget_connection(const char *connection_name)
{
	struct connection *ret = NULL;

#ifdef ENABLE_THREAD_SAFETY
	pthread_mutex_lock(&connections_mutex);
#endif

	ret = ecpg_get_connection_nr(connection_name);

#ifdef ENABLE_THREAD_SAFETY
	pthread_mutex_unlock(&connections_mutex);
#endif

	return (ret);

}

static void
ecpg_finish(struct connection * act)
{
	if (act != NULL)
	{
		struct ECPGtype_information_cache *cache,
				   *ptr;

		PQfinish(act->connection);

		/*
		 * no need to lock connections_mutex - we're always called by
		 * ECPGdisconnect or ECPGconnect, which are holding the lock
		 */

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

		ECPGlog("ecpg_finish: Connection %s closed.\n", act->name);

		for (cache = act->cache_head; cache; ptr = cache, cache = cache->next, ECPGfree(ptr));
		ECPGfree(act->name);
		ECPGfree(act);
	}
	else
		ECPGlog("ecpg_finish: called an extra time.\n");
}

bool
ECPGsetcommit(int lineno, const char *mode, const char *connection_name)
{
	struct connection *con = ECPGget_connection(connection_name);
	PGresult   *results;

	if (!ECPGinit(con, connection_name, lineno))
		return (false);

	ECPGlog("ECPGsetcommit line %d action = %s connection = %s\n", lineno, mode, con->name);

	if (con->autocommit == true && strncmp(mode, "off", strlen("off")) == 0)
	{
		if (con->committed)
		{
			if ((results = PQexec(con->connection, "begin transaction")) == NULL)
			{
				ECPGraise(lineno, ECPG_TRANS, ECPG_SQLSTATE_TRANSACTION_RESOLUTION_UNKNOWN, NULL);
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
				ECPGraise(lineno, ECPG_TRANS, ECPG_SQLSTATE_TRANSACTION_RESOLUTION_UNKNOWN, NULL);
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
	struct connection *con = ECPGget_connection(connection_name);

	if (!ECPGinit(con, connection_name, lineno))
		return (false);

	actual_connection = con;
	return true;
}


static void
ECPGnoticeReceiver(void *arg, const PGresult *result)
{
	char	   *sqlstate = PQresultErrorField(result, PG_DIAG_SQLSTATE);
	char	   *message = PQresultErrorField(result, PG_DIAG_MESSAGE_PRIMARY);
	struct sqlca_t *sqlca = ECPGget_sqlca();

	int			sqlcode;

	if (sqlstate == NULL)
		sqlstate = ECPG_SQLSTATE_ECPG_INTERNAL_ERROR;

	/* these are not warnings */
	if (strncmp(sqlstate, "00", 2) == 0)
		return;

	ECPGlog("%s", message);

	/* map to SQLCODE for backward compatibility */
	if (strcmp(sqlstate, ECPG_SQLSTATE_INVALID_CURSOR_NAME) == 0)
		sqlcode = ECPG_WARNING_UNKNOWN_PORTAL;
	else if (strcmp(sqlstate, ECPG_SQLSTATE_ACTIVE_SQL_TRANSACTION) == 0)
		sqlcode = ECPG_WARNING_IN_TRANSACTION;
	else if (strcmp(sqlstate, ECPG_SQLSTATE_NO_ACTIVE_SQL_TRANSACTION) == 0)
		sqlcode = ECPG_WARNING_NO_TRANSACTION;
	else if (strcmp(sqlstate, ECPG_SQLSTATE_DUPLICATE_CURSOR) == 0)
		sqlcode = ECPG_WARNING_PORTAL_EXISTS;
	else
		sqlcode = 0;

	strncpy(sqlca->sqlstate, sqlstate, sizeof(sqlca->sqlstate));
	sqlca->sqlcode = sqlcode;
	sqlca->sqlwarn[2] = 'W';
	sqlca->sqlwarn[0] = 'W';

	strncpy(sqlca->sqlerrm.sqlerrmc, message, sizeof(sqlca->sqlerrm.sqlerrmc));
	sqlca->sqlerrm.sqlerrmc[sizeof(sqlca->sqlerrm.sqlerrmc) - 1] = 0;
	sqlca->sqlerrm.sqlerrml = strlen(sqlca->sqlerrm.sqlerrmc);

	ECPGlog("raising sqlcode %d\n", sqlcode);
}


/* this contains some quick hacks, needs to be cleaned up, but it works */
bool
ECPGconnect(int lineno, int c, const char *name, const char *user, const char *passwd, const char *connection_name, int autocommit)
{
	struct sqlca_t *sqlca = ECPGget_sqlca();
	enum COMPAT_MODE compat = c;
	struct connection *this;
	char	   *dbname = strdup(name),
			   *host = NULL,
			   *tmp,
			   *port = NULL,
			   *realname = NULL,
			   *options = NULL;

	ECPGinit_sqlca(sqlca);

	if (INFORMIX_MODE(compat))
	{
		char	   *envname;

		/*
		 * Informix uses an environment variable DBPATH that overrides the
		 * connection parameters given here. We do the same with PG_DBPATH
		 * as the syntax is different.
		 */
		envname = getenv("PG_DBPATH");
		if (envname)
		{
			ECPGfree(dbname);
			dbname = strdup(envname);
		}

	}

	if ((this = (struct connection *) ECPGalloc(sizeof(struct connection), lineno)) == NULL)
		return false;

	if (dbname == NULL && connection_name == NULL)
		connection_name = "DEFAULT";

	/* get the detail information out of dbname */
	if (strchr(dbname, '@') != NULL)
	{
		/* old style: dbname[@server][:port] */
		tmp = strrchr(dbname, ':');
		if (tmp != NULL)		/* port number given */
		{
			port = strdup(tmp + 1);
			*tmp = '\0';
		}

		tmp = strrchr(dbname, '@');
		if (tmp != NULL)		/* host name given */
		{
			host = strdup(tmp + 1);
			*tmp = '\0';
		}
		realname = strdup(dbname);
	}
	else if (strncmp(dbname, "tcp:", 4) == 0 || strncmp(dbname, "unix:", 5) == 0)
	{
		int			offset = 0;

		/*
		 * only allow protocols tcp and unix
		 */
		if (strncmp(dbname, "tcp:", 4) == 0)
			offset = 4;
		else if (strncmp(dbname, "unix:", 5) == 0)
			offset = 5;

		if (strncmp(dbname + offset, "postgresql://", strlen("postgresql://")) == 0)
		{

			/*------
			 * new style:
			 *	<tcp|unix>:postgresql://server[:port|:/unixsocket/path:]
			 *	[/db name][?options]
			 *------
			 */
			offset += strlen("postgresql://");

			tmp = strrchr(dbname + offset, '?');
			if (tmp != NULL)	/* options given */
			{
				options = strdup(tmp + 1);
				*tmp = '\0';
			}

			tmp = last_path_separator(dbname + offset);
			if (tmp != NULL)	/* database name given */
			{
				realname = strdup(tmp + 1);
				*tmp = '\0';
			}

			tmp = strrchr(dbname + offset, ':');
			if (tmp != NULL)	/* port number or Unix socket path given */
			{
				char	   *tmp2;

				*tmp = '\0';
				if ((tmp2 = strchr(tmp + 1, ':')) != NULL)
				{
					*tmp2 = '\0';
					host = strdup(tmp + 1);
					if (strncmp(dbname, "unix:", 5) != 0)
					{
						ECPGlog("connect: socketname %s given for TCP connection in line %d\n", host, lineno);
						ECPGraise(lineno, ECPG_CONNECT, ECPG_SQLSTATE_SQLCLIENT_UNABLE_TO_ESTABLISH_SQLCONNECTION, realname ? realname : "<DEFAULT>");
						if (host)
							ECPGfree(host);
						if (port)
							ECPGfree(port);
						if (options)
							ECPGfree(options);
						if (realname)
							ECPGfree(realname);
						if (dbname)
							ECPGfree(dbname);
						return false;
					}
				}
				else
					port = strdup(tmp + 1);
			}

			if (strncmp(dbname, "unix:", 5) == 0)
			{
				if (strcmp(dbname + offset, "localhost") != 0 && strcmp(dbname + offset, "127.0.0.1") != 0)
				{
					ECPGlog("connect: non-localhost access via sockets in line %d\n", lineno);
					ECPGraise(lineno, ECPG_CONNECT, ECPG_SQLSTATE_SQLCLIENT_UNABLE_TO_ESTABLISH_SQLCONNECTION, realname ? realname : "<DEFAULT>");
					if (host)
						ECPGfree(host);
					if (port)
						ECPGfree(port);
					if (options)
						ECPGfree(options);
					if (realname)
						ECPGfree(realname);
					if (dbname)
						ECPGfree(dbname);
					return false;
				}
			}
			else
				host = strdup(dbname + offset);

		}
		else
			realname = strdup(dbname);
	}
	else
		realname = strdup(dbname);

	/* add connection to our list */
#ifdef ENABLE_THREAD_SAFETY
	pthread_mutex_lock(&connections_mutex);
#endif
	if (connection_name != NULL)
		this->name = ECPGstrdup(connection_name, lineno);
	else
		this->name = ECPGstrdup(realname, lineno);

	this->cache_head = NULL;

	if (all_connections == NULL)
		this->next = NULL;
	else
		this->next = all_connections;

	actual_connection = all_connections = this;

	ECPGlog("ECPGconnect: opening database %s on %s port %s %s%s%s%s\n",
			realname ? realname : "<DEFAULT>",
			host ? host : "<DEFAULT>",
			port ? port : "<DEFAULT>",
			options ? "with options " : "", options ? options : "",
			user ? "for user " : "", user ? user : "");

	this->connection = PQsetdbLogin(host, port, options, NULL, realname, user, passwd);

	if (PQstatus(this->connection) == CONNECTION_BAD)
	{
		const char *errmsg = PQerrorMessage(this->connection);
		char	   *db = realname ? realname : "<DEFAULT>";

		ecpg_finish(this);
#ifdef ENABLE_THREAD_SAFETY
		pthread_mutex_unlock(&connections_mutex);
#endif
		ECPGlog("connect: could not open database %s on %s port %s %s%s%s%s in line %d\n\t%s\n",
				db,
				host ? host : "<DEFAULT>",
				port ? port : "<DEFAULT>",
				options ? "with options " : "", options ? options : "",
				user ? "for user " : "", user ? user : "",
				lineno, errmsg);

		ECPGraise(lineno, ECPG_CONNECT, ECPG_SQLSTATE_SQLCLIENT_UNABLE_TO_ESTABLISH_SQLCONNECTION, db);
		if (host)
			ECPGfree(host);
		if (port)
			ECPGfree(port);
		if (options)
			ECPGfree(options);
		if (realname)
			ECPGfree(realname);
		if (dbname)
			ECPGfree(dbname);
		return false;
	}
#ifdef ENABLE_THREAD_SAFETY
	pthread_mutex_unlock(&connections_mutex);
#endif

	if (host)
		ECPGfree(host);
	if (port)
		ECPGfree(port);
	if (options)
		ECPGfree(options);
	if (realname)
		ECPGfree(realname);
	if (dbname)
		ECPGfree(dbname);

	this->committed = true;
	this->autocommit = autocommit;

	PQsetNoticeReceiver(this->connection, &ECPGnoticeReceiver, (void *) this);

	return true;
}

bool
ECPGdisconnect(int lineno, const char *connection_name)
{
	struct sqlca_t *sqlca = ECPGget_sqlca();
	struct connection *con;

#ifdef ENABLE_THREAD_SAFETY
	pthread_mutex_lock(&connections_mutex);
#endif

	if (strcmp(connection_name, "ALL") == 0)
	{
		ECPGinit_sqlca(sqlca);
		for (con = all_connections; con;)
		{
			struct connection *f = con;

			con = con->next;
			ecpg_finish(f);
		}
	}
	else
	{
		con = ecpg_get_connection_nr(connection_name);

		if (!ECPGinit(con, connection_name, lineno))
		{
#ifdef ENABLE_THREAD_SAFETY
			pthread_mutex_unlock(&connections_mutex);
#endif
			return (false);
		}
		else
			ecpg_finish(con);
	}

#ifdef ENABLE_THREAD_SAFETY
	pthread_mutex_unlock(&connections_mutex);
#endif

	return true;
}
