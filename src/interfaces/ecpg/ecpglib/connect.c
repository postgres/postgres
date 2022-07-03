/* src/interfaces/ecpg/ecpglib/connect.c */

#define POSTGRES_ECPG_INTERNAL
#include "postgres_fe.h"

#include "ecpg-pthread-win32.h"
#include "ecpgerrno.h"
#include "ecpglib.h"
#include "ecpglib_extern.h"
#include "ecpgtype.h"
#include "sqlca.h"

#ifdef HAVE_USELOCALE
locale_t	ecpg_clocale = (locale_t) 0;
#endif

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
		ecpg_pthreads_init();	/* ensure actual_connection_key is valid */

		ret = pthread_getspecific(actual_connection_key);

		/*
		 * if no connection in TSD for this thread, get the global default
		 * connection and hope the user knows what they're doing (i.e. using
		 * their own mutex to protect that connection from concurrent accesses
		 */
		if (ret == NULL)
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

	return ret;
}

struct connection *
ecpg_get_connection(const char *connection_name)
{
	struct connection *ret = NULL;

	if ((connection_name == NULL) || (strcmp(connection_name, "CURRENT") == 0))
	{
#ifdef ENABLE_THREAD_SAFETY
		ecpg_pthreads_init();	/* ensure actual_connection_key is valid */

		ret = pthread_getspecific(actual_connection_key);

		/*
		 * if no connection in TSD for this thread, get the global default
		 * connection and hope the user knows what they're doing (i.e. using
		 * their own mutex to protect that connection from concurrent accesses
		 */
		if (ret == NULL)
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

	return ret;
}

static void
ecpg_finish(struct connection *act)
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

		ecpg_log("ecpg_finish: connection %s closed\n", act->name ? act->name : "(null)");

		for (cache = act->cache_head; cache; ptr = cache, cache = cache->next, ecpg_free(ptr));
		ecpg_free(act->name);
		ecpg_free(act);
		/* delete cursor variables when last connection gets closed */
		if (all_connections == NULL)
		{
			struct var_list *iv_ptr;

			for (; ivlist; iv_ptr = ivlist, ivlist = ivlist->next, ecpg_free(iv_ptr));
		}
	}
	else
		ecpg_log("ecpg_finish: called an extra time\n");
}

bool
ECPGsetcommit(int lineno, const char *mode, const char *connection_name)
{
	struct connection *con = ecpg_get_connection(connection_name);
	PGresult   *results;

	if (!ecpg_init(con, connection_name, lineno))
		return false;

	ecpg_log("ECPGsetcommit on line %d: action \"%s\"; connection \"%s\"\n", lineno, mode, con->name);

	if (con->autocommit && strncmp(mode, "off", strlen("off")) == 0)
	{
		if (PQtransactionStatus(con->connection) == PQTRANS_IDLE)
		{
			results = PQexec(con->connection, "begin transaction");
			if (!ecpg_check_PQresult(results, lineno, con->connection, ECPG_COMPAT_PGSQL))
				return false;
			PQclear(results);
		}
		con->autocommit = false;
	}
	else if (!con->autocommit && strncmp(mode, "on", strlen("on")) == 0)
	{
		if (PQtransactionStatus(con->connection) != PQTRANS_IDLE)
		{
			results = PQexec(con->connection, "commit");
			if (!ecpg_check_PQresult(results, lineno, con->connection, ECPG_COMPAT_PGSQL))
				return false;
			PQclear(results);
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
		return false;

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

	if (sqlca == NULL)
	{
		ecpg_log("out of memory");
		return;
	}

	(void) arg;					/* keep the compiler quiet */
	if (sqlstate == NULL)
		sqlstate = ECPG_SQLSTATE_ECPG_INTERNAL_ERROR;

	if (message == NULL)		/* Shouldn't happen, but need to be sure */
		message = ecpg_gettext("empty message text");

	/* these are not warnings */
	if (strncmp(sqlstate, "00", 2) == 0)
		return;

	ecpg_log("ECPGnoticeReceiver: %s\n", message);

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
	int			i,
				connect_params = 0;
	char	   *dbname = name ? ecpg_strdup(name, lineno) : NULL,
			   *host = NULL,
			   *tmp,
			   *port = NULL,
			   *realname = NULL,
			   *options = NULL;
	const char **conn_keywords;
	const char **conn_values;

	if (sqlca == NULL)
	{
		ecpg_raise(lineno, ECPG_OUT_OF_MEMORY,
				   ECPG_SQLSTATE_ECPG_OUT_OF_MEMORY, NULL);
		ecpg_free(dbname);
		return false;
	}

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
	{
		ecpg_free(dbname);
		return false;
	}

	if (dbname != NULL)
	{
		/* get the detail information from dbname */
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
				 *	[/db-name][?options]
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
					if (tmp[1] != '\0') /* non-empty database name */
					{
						realname = ecpg_strdup(tmp + 1, lineno);
						connect_params++;
					}
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
						connect_params++;
						if (strncmp(dbname, "unix:", 5) != 0)
						{
							ecpg_log("ECPGconnect: socketname %s given for TCP connection on line %d\n", host, lineno);
							ecpg_raise(lineno, ECPG_CONNECT, ECPG_SQLSTATE_SQLCLIENT_UNABLE_TO_ESTABLISH_SQLCONNECTION, realname ? realname : ecpg_gettext("<DEFAULT>"));
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
					{
						port = ecpg_strdup(tmp + 1, lineno);
						connect_params++;
					}
				}

				if (strncmp(dbname, "unix:", 5) == 0)
				{
					if (strcmp(dbname + offset, "localhost") != 0 && strcmp(dbname + offset, "127.0.0.1") != 0)
					{
						ecpg_log("ECPGconnect: non-localhost access via sockets on line %d\n", lineno);
						ecpg_raise(lineno, ECPG_CONNECT, ECPG_SQLSTATE_SQLCLIENT_UNABLE_TO_ESTABLISH_SQLCONNECTION, realname ? realname : ecpg_gettext("<DEFAULT>"));
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
				{
					if (*(dbname + offset) != '\0')
					{
						host = ecpg_strdup(dbname + offset, lineno);
						connect_params++;
					}
				}

			}
		}
		else
		{
			/* old style: dbname[@server][:port] */
			tmp = strrchr(dbname, ':');
			if (tmp != NULL)	/* port number given */
			{
				port = ecpg_strdup(tmp + 1, lineno);
				connect_params++;
				*tmp = '\0';
			}

			tmp = strrchr(dbname, '@');
			if (tmp != NULL)	/* host name given */
			{
				host = ecpg_strdup(tmp + 1, lineno);
				connect_params++;
				*tmp = '\0';
			}

			if (strlen(dbname) > 0)
			{
				realname = ecpg_strdup(dbname, lineno);
				connect_params++;
			}
			else
				realname = NULL;
		}
	}
	else
		realname = NULL;

	/*
	 * Count options for the allocation done below (this may produce an
	 * overestimate, it's ok).
	 */
	if (options)
		for (i = 0; options[i]; i++)
			if (options[i] == '=')
				connect_params++;

	if (user && strlen(user) > 0)
		connect_params++;
	if (passwd && strlen(passwd) > 0)
		connect_params++;

	/*
	 * Allocate enough space for all connection parameters.  These allocations
	 * are done before manipulating the list of connections to ease the error
	 * handling on failure.
	 */
	conn_keywords = (const char **) ecpg_alloc((connect_params + 1) * sizeof(char *), lineno);
	conn_values = (const char **) ecpg_alloc(connect_params * sizeof(char *), lineno);
	if (conn_keywords == NULL || conn_values == NULL)
	{
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
		if (conn_keywords)
			ecpg_free(conn_keywords);
		if (conn_values)
			ecpg_free(conn_values);
		free(this);
		return false;
	}

	/* add connection to our list */
#ifdef ENABLE_THREAD_SAFETY
	pthread_mutex_lock(&connections_mutex);
#endif

	/*
	 * ... but first, make certain we have created ecpg_clocale.  Rely on
	 * holding connections_mutex to ensure this is done by only one thread.
	 */
#ifdef HAVE_USELOCALE
	if (!ecpg_clocale)
	{
		ecpg_clocale = newlocale(LC_NUMERIC_MASK, "C", (locale_t) 0);
		if (!ecpg_clocale)
		{
#ifdef ENABLE_THREAD_SAFETY
			pthread_mutex_unlock(&connections_mutex);
#endif
			ecpg_raise(lineno, ECPG_OUT_OF_MEMORY,
					   ECPG_SQLSTATE_ECPG_OUT_OF_MEMORY, NULL);
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
			if (conn_keywords)
				ecpg_free(conn_keywords);
			if (conn_values)
				ecpg_free(conn_values);
			free(this);
			return false;
		}
	}
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

	ecpg_log("ECPGconnect: opening database %s on %s port %s %s%s %s%s\n",
			 realname ? realname : "<DEFAULT>",
			 host ? host : "<DEFAULT>",
			 port ? (ecpg_internal_regression_mode ? "<REGRESSION_PORT>" : port) : "<DEFAULT>",
			 options ? "with options " : "", options ? options : "",
			 (user && strlen(user) > 0) ? "for user " : "", user ? user : "");

	i = 0;
	if (realname)
	{
		conn_keywords[i] = "dbname";
		conn_values[i] = realname;
		i++;
	}
	if (host)
	{
		conn_keywords[i] = "host";
		conn_values[i] = host;
		i++;
	}
	if (port)
	{
		conn_keywords[i] = "port";
		conn_values[i] = port;
		i++;
	}
	if (user && strlen(user) > 0)
	{
		conn_keywords[i] = "user";
		conn_values[i] = user;
		i++;
	}
	if (passwd && strlen(passwd) > 0)
	{
		conn_keywords[i] = "password";
		conn_values[i] = passwd;
		i++;
	}
	if (options)
	{
		char	   *str;

		/*
		 * The options string contains "keyword=value" pairs separated by
		 * '&'s.  We must break this up into keywords and values to pass to
		 * libpq (it's okay to scribble on the options string).  We ignore
		 * spaces just before each keyword or value.
		 */
		for (str = options; *str;)
		{
			int			e,
						a;
			char	   *token1,
					   *token2;

			/* Skip spaces before keyword */
			for (token1 = str; *token1 == ' '; token1++)
				 /* skip */ ;
			/* Find end of keyword */
			for (e = 0; token1[e] && token1[e] != '='; e++)
				 /* skip */ ;
			if (token1[e])		/* found "=" */
			{
				token1[e] = '\0';
				/* Skip spaces before value */
				for (token2 = token1 + e + 1; *token2 == ' '; token2++)
					 /* skip */ ;
				/* Find end of value */
				for (a = 0; token2[a] && token2[a] != '&'; a++)
					 /* skip */ ;
				if (token2[a])	/* found "&" => another option follows */
				{
					token2[a] = '\0';
					str = token2 + a + 1;
				}
				else
					str = token2 + a;

				conn_keywords[i] = token1;
				conn_values[i] = token2;
				i++;
			}
			else
			{
				/* Bogus options syntax ... ignore trailing garbage */
				str = token1 + e;
			}
		}
	}

	Assert(i <= connect_params);
	conn_keywords[i] = NULL;	/* terminator */

	this->connection = PQconnectdbParams(conn_keywords, conn_values, 0);

	if (host)
		ecpg_free(host);
	if (port)
		ecpg_free(port);
	if (options)
		ecpg_free(options);
	if (dbname)
		ecpg_free(dbname);
	ecpg_free(conn_values);
	ecpg_free(conn_keywords);

	if (PQstatus(this->connection) == CONNECTION_BAD)
	{
		const char *errmsg = PQerrorMessage(this->connection);
		const char *db = realname ? realname : ecpg_gettext("<DEFAULT>");

		ecpg_log("ECPGconnect: could not open database: %s\n", errmsg);

		ecpg_finish(this);
#ifdef ENABLE_THREAD_SAFETY
		pthread_mutex_unlock(&connections_mutex);
#endif

		ecpg_raise(lineno, ECPG_CONNECT, ECPG_SQLSTATE_SQLCLIENT_UNABLE_TO_ESTABLISH_SQLCONNECTION, db);
		if (realname)
			ecpg_free(realname);

		return false;
	}

	if (realname)
		ecpg_free(realname);

#ifdef ENABLE_THREAD_SAFETY
	pthread_mutex_unlock(&connections_mutex);
#endif

	this->autocommit = autocommit;

	PQsetNoticeReceiver(this->connection, &ECPGnoticeReceiver, (void *) this);

	return true;
}

bool
ECPGdisconnect(int lineno, const char *connection_name)
{
	struct sqlca_t *sqlca = ECPGget_sqlca();
	struct connection *con;

	if (sqlca == NULL)
	{
		ecpg_raise(lineno, ECPG_OUT_OF_MEMORY,
				   ECPG_SQLSTATE_ECPG_OUT_OF_MEMORY, NULL);
		return false;
	}

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
			return false;
		}
		else
			ecpg_finish(con);
	}

#ifdef ENABLE_THREAD_SAFETY
	pthread_mutex_unlock(&connections_mutex);
#endif

	return true;
}

PGconn *
ECPGget_PGconn(const char *connection_name)
{
	struct connection *con;

	con = ecpg_get_connection(connection_name);
	if (con == NULL)
		return NULL;

	return con->connection;
}
