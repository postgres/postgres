/* $PostgreSQL: pgsql/src/interfaces/ecpg/ecpglib/connect.c,v 1.48 2007/11/15 21:14:45 momjian Exp $ */

#define POSTGRES_ECPG_INTERNAL
#include "postgres_fe.h"

#include "ecpg-pthread-win32.h"
#include "ecpgtype.h"
#include "ecpglib.h"
#include "ecpgerrno.h"
#include "extern.h"
#include "sqlca.h"

#ifdef ENABLE_THREAD_SAFETY
static pthread_mutex_t connections_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_key_t actual_connection_key;
static pthread_once_t actual_connection_key_once = PTHREAD_ONCE_INIT;
#endif
static struct connection *actual_connection = NULL;
static struct connection *all_connections = NULL;

#ifdef ENABLE_THREAD_SAFETY
static void
ecpg_actual_connection_init(void)
{
	pthread_key_create(&actual_connection_key, NULL);
}

void
ecpg_pthreads_init(void)
{
	pthread_once(&actual_connection_key_once, ecpg_actual_connection_init);
}
#endif

static struct connection *
ecpg_get_connection_nr(const char *connection_name)
{
	struct connection *ret = NULL;

	if ((connection_name == NULL) || (strcmp(connection_name, "CURRENT") == 0))
	{
#ifdef ENABLE_THREAD_SAFETY
		ret = pthread_getspecific(actual_connection_key);

		/*
		 * if no connection in TSD for this thread, get the global default
		 * connection and hope the user knows what they're doing (i.e. using
		 * their own mutex to protect that connection from concurrent accesses
		 */
		/* if !ret then  we  got the connection from TSD */
		if (NULL == ret)
			/* no TSD connection, going for global */
			ret = actual_connection;
#else
		ret = actual_connection;
#endif
	}
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
ecpg_get_connection(const char *connection_name)
{
	struct connection *ret = NULL;

	if ((connection_name == NULL) || (strcmp(connection_name, "CURRENT") == 0))
	{
#ifdef ENABLE_THREAD_SAFETY
		ret = pthread_getspecific(actual_connection_key);

		/*
		 * if no connection in TSD for this thread, get the global default
		 * connection and hope the user knows what they're doing (i.e. using
		 * their own mutex to protect that connection from concurrent accesses
		 */
		/* if !ret then  we  got the connection from TSD */
		if (NULL == ret)
			/* no TSD connection here either, using global */
			ret = actual_connection;
#else
		ret = actual_connection;
#endif
	}
	else
	{
#ifdef ENABLE_THREAD_SAFETY
		pthread_mutex_lock(&connections_mutex);
#endif

		ret = ecpg_get_connection_nr(connection_name);

#ifdef ENABLE_THREAD_SAFETY
		pthread_mutex_unlock(&connections_mutex);
#endif
	}

	return (ret);
}

static void
ecpg_finish(struct connection * act)
{
	if (act != NULL)
	{
		struct ECPGtype_information_cache *cache,
				   *ptr;

		ecpg_deallocate_all_conn(0, ECPG_COMPAT_PGSQL, act);
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

#ifdef ENABLE_THREAD_SAFETY
		if (pthread_getspecific(actual_connection_key) == act)
			pthread_setspecific(actual_connection_key, all_connections);
#endif
		if (actual_connection == act)
			actual_connection = all_connections;

		ecpg_log("ecpg_finish: Connection %s closed.\n", act->name);

		for (cache = act->cache_head; cache; ptr = cache, cache = cache->next, ecpg_free(ptr));
		ecpg_free(act->name);
		ecpg_free(act);
	}
	else
		ecpg_log("ecpg_finish: called an extra time.\n");
}

bool
ECPGsetcommit(int lineno, const char *mode, const char *connection_name)
{
	struct connection *con = ecpg_get_connection(connection_name);
	PGresult   *results;

	if (!ecpg_init(con, connection_name, lineno))
		return (false);

	ecpg_log("ECPGsetcommit line %d action = %s connection = %s\n", lineno, mode, con->name);

	if (con->autocommit == true && strncmp(mode, "off", strlen("off")) == 0)
	{
		if (con->committed)
		{
			results = PQexec(con->connection, "begin transaction");
			if (!ecpg_check_PQresult(results, lineno, con->connection, ECPG_COMPAT_PGSQL))
				return false;
			PQclear(results);
			con->committed = false;
		}
		con->autocommit = false;
	}
	else if (con->autocommit == false && strncmp(mode, "on", strlen("on")) == 0)
	{
		if (!con->committed)
		{
			results = PQexec(con->connection, "commit");
			if (!ecpg_check_PQresult(results, lineno, con->connection, ECPG_COMPAT_PGSQL))
				return false;
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
	struct connection *con = ecpg_get_connection(connection_name);

	if (!ecpg_init(con, connection_name, lineno))
		return (false);

#ifdef ENABLE_THREAD_SAFETY
	pthread_setspecific(actual_connection_key, con);
#else
	actual_connection = con;
#endif
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

	if (message == NULL)		/* Shouldn't happen, but need to be sure */
		message = "No message received";

	/* these are not warnings */
	if (strncmp(sqlstate, "00", 2) == 0)
		return;

	ecpg_log("ECPGnoticeReceiver %s\n", message);

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

	ecpg_log("raising sqlcode %d\n", sqlcode);
}


/* this contains some quick hacks, needs to be cleaned up, but it works */
bool
ECPGconnect(int lineno, int c, const char *name, const char *user, const char *passwd, const char *connection_name, int autocommit)
{
	struct sqlca_t *sqlca = ECPGget_sqlca();
	enum COMPAT_MODE compat = c;
	struct connection *this;
	char	   *dbname = name ? ecpg_strdup(name, lineno) : NULL,
			   *host = NULL,
			   *tmp,
			   *port = NULL,
			   *realname = NULL,
			   *options = NULL;

	ecpg_init_sqlca(sqlca);

	/*
	 * clear auto_mem structure because some error handling functions might
	 * access it
	 */
	ecpg_clear_auto_mem();

	if (INFORMIX_MODE(compat))
	{
		char	   *envname;

		/*
		 * Informix uses an environment variable DBPATH that overrides the
		 * connection parameters given here. We do the same with PG_DBPATH as
		 * the syntax is different.
		 */
		envname = getenv("PG_DBPATH");
		if (envname)
		{
			ecpg_free(dbname);
			dbname = ecpg_strdup(envname, lineno);
		}

	}

	if (dbname == NULL && connection_name == NULL)
		connection_name = "DEFAULT";

#if ENABLE_THREAD_SAFETY
	ecpg_pthreads_init();
#endif

	/* check if the identifier is unique */
	if (ecpg_get_connection(connection_name))
	{
		ecpg_free(dbname);
		ecpg_log("ECPGconnect: connection identifier %s is already in use\n",
				 connection_name);
		return false;
	}

	if ((this = (struct connection *) ecpg_alloc(sizeof(struct connection), lineno)) == NULL)
		return false;

	if (dbname != NULL)
	{
		/* get the detail information out of dbname */
		if (strncmp(dbname, "tcp:", 4) == 0 || strncmp(dbname, "unix:", 5) == 0)
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
					options = ecpg_strdup(tmp + 1, lineno);
					*tmp = '\0';
				}

				tmp = last_dir_separator(dbname + offset);
				if (tmp != NULL)	/* database name given */
				{
					realname = ecpg_strdup(tmp + 1, lineno);
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
						host = ecpg_strdup(tmp + 1, lineno);
						if (strncmp(dbname, "unix:", 5) != 0)
						{
							ecpg_log("ECPGconnect: socketname %s given for TCP connection in line %d\n", host, lineno);
							ecpg_raise(lineno, ECPG_CONNECT, ECPG_SQLSTATE_SQLCLIENT_UNABLE_TO_ESTABLISH_SQLCONNECTION, realname ? realname : "<DEFAULT>");
							if (host)
								ecpg_free(host);

							/*
							 * port not set yet if (port) ecpg_free(port);
							 */
							if (options)
								ecpg_free(options);
							if (realname)
								ecpg_free(realname);
							if (dbname)
								ecpg_free(dbname);
							free(this);
							return false;
						}
					}
					else
						port = ecpg_strdup(tmp + 1, lineno);
				}

				if (strncmp(dbname, "unix:", 5) == 0)
				{
					if (strcmp(dbname + offset, "localhost") != 0 && strcmp(dbname + offset, "127.0.0.1") != 0)
					{
						ecpg_log("ECPGconnect: non-localhost access via sockets in line %d\n", lineno);
						ecpg_raise(lineno, ECPG_CONNECT, ECPG_SQLSTATE_SQLCLIENT_UNABLE_TO_ESTABLISH_SQLCONNECTION, realname ? realname : "<DEFAULT>");
						if (host)
							ecpg_free(host);
						if (port)
							ecpg_free(port);
						if (options)
							ecpg_free(options);
						if (realname)
							ecpg_free(realname);
						if (dbname)
							ecpg_free(dbname);
						free(this);
						return false;
					}
				}
				else
					host = ecpg_strdup(dbname + offset, lineno);

			}
		}
		else
		{
			/* old style: dbname[@server][:port] */
			tmp = strrchr(dbname, ':');
			if (tmp != NULL)	/* port number given */
			{
				port = ecpg_strdup(tmp + 1, lineno);
				*tmp = '\0';
			}

			tmp = strrchr(dbname, '@');
			if (tmp != NULL)	/* host name given */
			{
				host = ecpg_strdup(tmp + 1, lineno);
				*tmp = '\0';
			}

			realname = (strlen(dbname) > 0) ? ecpg_strdup(dbname, lineno) : NULL;
		}
	}
	else
		realname = NULL;

	/* add connection to our list */
#ifdef ENABLE_THREAD_SAFETY
	pthread_mutex_lock(&connections_mutex);
#endif
	if (connection_name != NULL)
		this->name = ecpg_strdup(connection_name, lineno);
	else
		this->name = ecpg_strdup(realname, lineno);

	this->cache_head = NULL;
	this->prep_stmts = NULL;

	if (all_connections == NULL)
		this->next = NULL;
	else
		this->next = all_connections;

	all_connections = this;
#ifdef ENABLE_THREAD_SAFETY
	pthread_setspecific(actual_connection_key, all_connections);
#endif
	actual_connection = all_connections;

	ecpg_log("ECPGconnect: opening database %s on %s port %s %s%s%s%s\n",
			 realname ? realname : "<DEFAULT>",
			 host ? host : "<DEFAULT>",
			 port ? (ecpg_internal_regression_mode ? "<REGRESSION_PORT>" : port) : "<DEFAULT>",
			 options ? "with options " : "", options ? options : "",
			 user ? "for user " : "", user ? user : "");

	this->connection = PQsetdbLogin(host, port, options, NULL, realname, user, passwd);

	if (PQstatus(this->connection) == CONNECTION_BAD)
	{
		const char *errmsg = PQerrorMessage(this->connection);
		const char *db = realname ? realname : "<DEFAULT>";

		ecpg_log("ECPGconnect: could not open database %s on %s port %s %s%s%s%s in line %d\n\t%s\n",
				 db,
				 host ? host : "<DEFAULT>",
				 port ? (ecpg_internal_regression_mode ? "<REGRESSION_PORT>" : port) : "<DEFAULT>",
				 options ? "with options " : "", options ? options : "",
				 user ? "for user " : "", user ? user : "",
				 lineno, errmsg);

		ecpg_finish(this);
#ifdef ENABLE_THREAD_SAFETY
		pthread_mutex_unlock(&connections_mutex);
#endif

		ecpg_raise(lineno, ECPG_CONNECT, ECPG_SQLSTATE_SQLCLIENT_UNABLE_TO_ESTABLISH_SQLCONNECTION, db);
		if (host)
			ecpg_free(host);
		if (port)
			ecpg_free(port);
		if (options)
			ecpg_free(options);
		if (realname)
			ecpg_free(realname);
		if (dbname)
			ecpg_free(dbname);
		return false;
	}
#ifdef ENABLE_THREAD_SAFETY
	pthread_mutex_unlock(&connections_mutex);
#endif

	if (host)
		ecpg_free(host);
	if (port)
		ecpg_free(port);
	if (options)
		ecpg_free(options);
	if (realname)
		ecpg_free(realname);
	if (dbname)
		ecpg_free(dbname);

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
		ecpg_init_sqlca(sqlca);
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

		if (!ecpg_init(con, connection_name, lineno))
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
