#include "ecpgtype.h"
#include "ecpglib.h"
#include "ecpgerrno.h"
#include "extern.h"
#include "sqlca.h"

static struct connection *all_connections = NULL,
		   *actual_connection = NULL;

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
		struct ECPGtype_information_cache *cache,
				   *ptr;

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

		for (cache = act->cache_head; cache; ptr = cache, cache = cache->next, free(ptr));
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
		return (false);

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
		return (false);

	actual_connection = con;
	return true;
}

static void
ECPGnoticeProcessor_raise(int code, const char *message)
{
	sqlca.sqlcode = code;
	strncpy(sqlca.sqlerrm.sqlerrmc, message, sizeof(sqlca.sqlerrm.sqlerrmc));
	sqlca.sqlerrm.sqlerrmc[sizeof(sqlca.sqlerrm.sqlerrmc) - 1] = 0;
	sqlca.sqlerrm.sqlerrml = strlen(sqlca.sqlerrm.sqlerrmc);

	/* remove trailing newline */
	if (sqlca.sqlerrm.sqlerrml
		&& sqlca.sqlerrm.sqlerrmc[sqlca.sqlerrm.sqlerrml - 1] == '\n')
	{
		sqlca.sqlerrm.sqlerrmc[sqlca.sqlerrm.sqlerrml - 1] = 0;
		sqlca.sqlerrm.sqlerrml--;
	}

	ECPGlog("raising sqlcode %d\n", code);
}

/*
 * I know this is a mess, but we can't redesign the backend
 */

static void
ECPGnoticeProcessor(void *arg, const char *message)
{
	/* these notices raise an error */
	if (strncmp(message, "NOTICE: ", 8))
	{
		ECPGlog("ECPGnoticeProcessor: strange notice '%s'\n", message);
		ECPGnoticeProcessor_raise(ECPG_NOTICE_UNRECOGNIZED, message);
		return;
	}

	message += 8;
	while (*message == ' ')
		message++;
	ECPGlog("NOTICE: %s", message);

	/* NOTICE:	(transaction aborted): queries ignored until END */

	/*
	 * NOTICE:	current transaction is aborted, queries ignored until end
	 * of transaction block
	 */
	if (strstr(message, "queries ignored") && strstr(message, "transaction")
		&& strstr(message, "aborted"))
	{
		ECPGnoticeProcessor_raise(ECPG_NOTICE_QUERY_IGNORED, message);
		return;
	}

	/* NOTICE:	PerformPortalClose: portal "*" not found */
	if ((!strncmp(message, "PerformPortalClose: portal", 26)
		 || !strncmp(message, "PerformPortalFetch: portal", 26))
		&& strstr(message + 26, "not found"))
	{
		ECPGnoticeProcessor_raise(ECPG_NOTICE_UNKNOWN_PORTAL, message);
		return;
	}

	/* NOTICE:	BEGIN: already a transaction in progress */
	if (!strncmp(message, "BEGIN: already a transaction in progress", 40))
	{
		ECPGnoticeProcessor_raise(ECPG_NOTICE_IN_TRANSACTION, message);
		return;
	}

	/* NOTICE:	AbortTransaction and not in in-progress state */
	/* NOTICE:	COMMIT: no transaction in progress */
	/* NOTICE:	ROLLBACK: no transaction in progress */
	if (!strncmp(message, "AbortTransaction and not in in-progress state", 45)
		|| !strncmp(message, "COMMIT: no transaction in progress", 34)
		|| !strncmp(message, "ROLLBACK: no transaction in progress", 36))
	{
		ECPGnoticeProcessor_raise(ECPG_NOTICE_NO_TRANSACTION, message);
		return;
	}

	/* NOTICE:	BlankPortalAssignName: portal * already exists */
	if (!strncmp(message, "BlankPortalAssignName: portal", 29)
		&& strstr(message + 29, "already exists"))
	{
		ECPGnoticeProcessor_raise(ECPG_NOTICE_PORTAL_EXISTS, message);
		return;
	}

	/* these are harmless - do nothing */

	/*
	 * NOTICE:	CREATE TABLE/PRIMARY KEY will create implicit index '*'
	 * for table '*'
	 */

	/*
	 * NOTICE:	ALTER TABLE ... ADD CONSTRAINT will create implicit
	 * trigger(s) for FOREIGN KEY check(s)
	 */

	/*
	 * NOTICE:	CREATE TABLE will create implicit sequence '*' for SERIAL
	 * column '*.*'
	 */

	/*
	 * NOTICE:	CREATE TABLE will create implicit trigger(s) for FOREIGN
	 * KEY check(s)
	 */
	if ((!strncmp(message, "CREATE TABLE", 12) || !strncmp(message, "ALTER TABLE", 11))
		&& strstr(message + 11, "will create implicit"))
		return;

	/* NOTICE:	QUERY PLAN: */
	if (!strncmp(message, "QUERY PLAN:", 11))	/* do we really see these? */
		return;

	/*
	 * NOTICE:	DROP TABLE implicitly drops referential integrity trigger
	 * from table "*"
	 */
	if (!strncmp(message, "DROP TABLE implicitly drops", 27))
		return;

	/*
	 * NOTICE:	Caution: DROP INDEX cannot be rolled back, so don't abort
	 * now
	 */
	if (strstr(message, "cannot be rolled back"))
		return;

	/* these and other unmentioned should set sqlca.sqlwarn[2] */
	/* NOTICE:	The ':' operator is deprecated.  Use exp(x) instead. */
	/* NOTICE:	Rel *: Uninitialized page 0 - fixing */
	/* NOTICE:	PortalHeapMemoryFree: * not in alloc set! */
	/* NOTICE:	Too old parent tuple found - can't continue vc_repair_frag */
	/* NOTICE:	identifier "*" will be truncated to "*" */
	/* NOTICE:	InvalidateSharedInvalid: cache state reset */
	/* NOTICE:	RegisterSharedInvalid: SI buffer overflow */
	sqlca.sqlwarn[2] = 'W';
	sqlca.sqlwarn[0] = 'W';
}

/* this contains some quick hacks, needs to be cleaned up, but it works */
bool
ECPGconnect(int lineno, const char *name, const char *user, const char *passwd, const char *connection_name, int autocommit)
{
	struct connection *this;
	char	   *dbname = strdup(name),
			   *host = NULL,
			   *tmp,
			   *port = NULL,
			   *realname = NULL,
			   *options = NULL;

	init_sqlca();

	if ((this = (struct connection *) ecpg_alloc(sizeof(struct connection), lineno)) == NULL)
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
			 * 	<tcp|unix>:postgresql://server[:port|:/unixsocket/path:]
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

			tmp = strrchr(dbname + offset, '/');
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
						ECPGraise(lineno, ECPG_CONNECT, realname ? realname : "<DEFAULT>");
						if (host)
							free(host);
						if (port)
							free(port);
						if (options)
							free(options);
						if (realname)
							free(realname);
						if (dbname)
							free(dbname);
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
					ECPGraise(lineno, ECPG_CONNECT, realname ? realname : "<DEFAULT>");
					if (host)
						free(host);
					if (port)
						free(port);
					if (options)
						free(options);
					if (realname)
						free(realname);
					if (dbname)
						free(dbname);
					return false;
				}
			}
			else
				host = strdup(dbname + offset);

		}
	}
	else
		realname = strdup(dbname);

	/* add connection to our list */
	if (connection_name != NULL)
		this->name = ecpg_strdup(connection_name, lineno);
	else
		this->name = ecpg_strdup(realname, lineno);

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

	if (host)
		free(host);
	if (port)
		free(port);
	if (options)
		free(options);
	if (realname)
		free(realname);
	if (dbname)
		free(dbname);

	if (PQstatus(this->connection) == CONNECTION_BAD)
	{
		ecpg_finish(this);
		ECPGlog("connect: could not open database %s on %s port %s %s%s%s%s in line %d\n",
				realname ? realname : "<DEFAULT>",
				host ? host : "<DEFAULT>",
				port ? port : "<DEFAULT>",
				options ? "with options " : "", options ? options : "",
				user ? "for user " : "", user ? user : "",
				lineno);
		ECPGraise(lineno, ECPG_CONNECT, realname ? realname : "<DEFAULT>");
		return false;
	}

	this->committed = true;
	this->autocommit = autocommit;

	PQsetNoticeProcessor(this->connection, &ECPGnoticeProcessor, (void *) this);

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
			return (false);
		else
			ecpg_finish(con);
	}

	return true;
}
