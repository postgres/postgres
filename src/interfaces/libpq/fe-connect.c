/*-------------------------------------------------------------------------
 *
 * fe-connect.c
 *	  functions related to setting up a connection to the backend
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/interfaces/libpq/fe-connect.c,v 1.223 2003/02/14 01:24:26 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>

#include "libpq-fe.h"
#include "libpq-int.h"
#include "fe-auth.h"

#ifdef WIN32
#include "win32.h"
#else
#include <sys/socket.h>
#include <unistd.h>
#include <netdb.h>
#include <netinet/in.h>
#ifdef HAVE_NETINET_TCP_H
#include <netinet/tcp.h>
#endif
#include <arpa/inet.h>
#endif

#include "libpq/ip.h"


#ifndef HAVE_STRDUP
#include "strdup.h"
#endif
#ifdef HAVE_CRYPT_H
#include <crypt.h>
#endif

#include "mb/pg_wchar.h"


#ifdef HAVE_IPV6
#define FREEADDRINFO2(family, addrs)	freeaddrinfo2((family), (addrs))
#else
/* do nothing */
#define FREEADDRINFO2(family, addrs)	do {} while (0)
#endif

#ifdef WIN32
static int
inet_aton(const char *cp, struct in_addr * inp)
{
	unsigned long a = inet_addr(cp);

	if (a == -1)
		return 0;
	inp->s_addr = a;
	return 1;
}
#endif


#define NOTIFYLIST_INITIAL_SIZE 10
#define NOTIFYLIST_GROWBY 10

#define PGPASSFILE ".pgpass"

/* ----------
 * Definition of the conninfo parameters and their fallback resources.
 *
 * If Environment-Var and Compiled-in are specified as NULL, no
 * fallback is available. If after all no value can be determined
 * for an option, an error is returned.
 *
 * The values for dbname and user are treated specially in conninfo_parse.
 * If the Compiled-in resource is specified as a NULL value, the
 * user is determined by fe_getauthname() and for dbname the user
 * name is copied.
 *
 * The Label and Disp-Char entries are provided for applications that
 * want to use PQconndefaults() to create a generic database connection
 * dialog. Disp-Char is defined as follows:
 *		""		Normal input field
 *		"*"		Password field - hide value
 *		"D"		Debug option - don't show by default
 *
 * PQconninfoOptions[] is a constant static array that we use to initialize
 * a dynamically allocated working copy.  All the "val" fields in
 * PQconninfoOptions[] *must* be NULL.	In a working copy, non-null "val"
 * fields point to malloc'd strings that should be freed when the working
 * array is freed (see PQconninfoFree).
 * ----------
 */
static const PQconninfoOption PQconninfoOptions[] = {
	/*
	 * "authtype" is no longer used, so mark it "don't show".  We keep it
	 * in the array so as not to reject conninfo strings from old apps
	 * that might still try to set it.
	 */
	{"authtype", "PGAUTHTYPE", DefaultAuthtype, NULL,
	"Database-Authtype", "D", 20},

	{"service", "PGSERVICE", NULL, NULL,
	"Database-Service", "", 20},

	{"user", "PGUSER", NULL, NULL,
	"Database-User", "", 20},

	{"password", "PGPASSWORD", DefaultPassword, NULL,
	"Database-Password", "*", 20},

	{"connect_timeout", "PGCONNECT_TIMEOUT", NULL, NULL,
	"Connect-timeout", "", 10}, /* strlen(INT32_MAX) == 10 */

	{"dbname", "PGDATABASE", NULL, NULL,
	"Database-Name", "", 20},

	{"host", "PGHOST", NULL, NULL,
	"Database-Host", "", 40},

	{"hostaddr", "PGHOSTADDR", NULL, NULL,
	"Database-Host-IP-Address", "", 45},

	{"port", "PGPORT", DEF_PGPORT_STR, NULL,
	"Database-Port", "", 6},

	{"tty", "PGTTY", DefaultTty, NULL,
	"Backend-Debug-TTY", "D", 40},

	{"options", "PGOPTIONS", DefaultOption, NULL,
	"Backend-Debug-Options", "D", 40},

#ifdef USE_SSL
	{"requiressl", "PGREQUIRESSL", "0", NULL,
	"Require-SSL", "", 1},
#endif

	/* Terminating entry --- MUST BE LAST */
	{NULL, NULL, NULL, NULL,
	NULL, NULL, 0}
};

static const struct EnvironmentOptions
{
	const char *envName,
			   *pgName;
}	EnvironmentOptions[] =

{
	/* common user-interface settings */
	{
		"PGDATESTYLE", "datestyle"
	},
	{
		"PGTZ", "timezone"
	},
	{
		"PGCLIENTENCODING", "client_encoding"
	},
	/* internal performance-related settings */
	{
		"PGGEQO", "geqo"
	},
	{
		NULL, NULL
	}
};


static int	connectDBStart(PGconn *conn);
static int	connectDBComplete(PGconn *conn);
static bool PQsetenvStart(PGconn *conn);
static PostgresPollingStatusType PQsetenvPoll(PGconn *conn);
static PGconn *makeEmptyPGconn(void);
static void freePGconn(PGconn *conn);
static void closePGconn(PGconn *conn);
static PQconninfoOption *conninfo_parse(const char *conninfo,
			   PQExpBuffer errorMessage);
static char *conninfo_getval(PQconninfoOption *connOptions,
				const char *keyword);
static void defaultNoticeProcessor(void *arg, const char *message);
static int parseServiceInfo(PQconninfoOption *options,
				 PQExpBuffer errorMessage);
char	   *pwdfMatchesString(char *buf, char *token);
char *PasswordFromFile(char *hostname, char *port, char *dbname,
				 char *username);

/*
 *		Connecting to a Database
 *
 * There are now four different ways a user of this API can connect to the
 * database.  Two are not recommended for use in new code, because of their
 * lack of extensibility with respect to the passing of options to the
 * backend.  These are PQsetdb and PQsetdbLogin (the former now being a macro
 * to the latter).
 *
 * If it is desired to connect in a synchronous (blocking) manner, use the
 * function PQconnectdb.
 *
 * To connect in an asychronous (non-blocking) manner, use the functions
 * PQconnectStart, and PQconnectPoll.
 *
 * Internally, the static functions connectDBStart, connectDBComplete
 * are part of the connection procedure.
 */

/*
 *		PQconnectdb
 *
 * establishes a connection to a postgres backend through the postmaster
 * using connection information in a string.
 *
 * The conninfo string is a white-separated list of
 *
 *	   option = value
 *
 * definitions. Value might be a single value containing no whitespaces or
 * a single quoted string. If a single quote should appear anywhere in
 * the value, it must be escaped with a backslash like \'
 *
 * Returns a PGconn* which is needed for all subsequent libpq calls, or NULL
 * if a memory allocation failed.
 * If the status field of the connection returned is CONNECTION_BAD,
 * then some fields may be null'ed out instead of having valid values.
 *
 * You should call PQfinish (if conn is not NULL) regardless of whether this
 * call succeeded.
 */
PGconn *
PQconnectdb(const char *conninfo)
{
	PGconn	   *conn = PQconnectStart(conninfo);

	if (conn && conn->status != CONNECTION_BAD)
		(void) connectDBComplete(conn);

	return conn;
}

/*
 *		PQconnectStart
 *
 * Begins the establishment of a connection to a postgres backend through the
 * postmaster using connection information in a string.
 *
 * See comment for PQconnectdb for the definition of the string format.
 *
 * Returns a PGconn*.  If NULL is returned, a malloc error has occurred, and
 * you should not attempt to proceed with this connection.	If the status
 * field of the connection returned is CONNECTION_BAD, an error has
 * occurred. In this case you should call PQfinish on the result, (perhaps
 * inspecting the error message first).  Other fields of the structure may not
 * be valid if that occurs.  If the status field is not CONNECTION_BAD, then
 * this stage has succeeded - call PQconnectPoll, using select(2) to see when
 * this is necessary.
 *
 * See PQconnectPoll for more info.
 */
PGconn *
PQconnectStart(const char *conninfo)
{
	PGconn	   *conn;
	PQconninfoOption *connOptions;
	char	   *tmp;

	/*
	 * Allocate memory for the conn structure
	 */

	conn = makeEmptyPGconn();
	if (conn == NULL)
		return (PGconn *) NULL;

	/*
	 * Parse the conninfo string
	 */
	connOptions = conninfo_parse(conninfo, &conn->errorMessage);
	if (connOptions == NULL)
	{
		conn->status = CONNECTION_BAD;
		/* errorMessage is already set */
		return conn;
	}

	/*
	 * Move option values into conn structure
	 */
	tmp = conninfo_getval(connOptions, "hostaddr");
	conn->pghostaddr = tmp ? strdup(tmp) : NULL;
	tmp = conninfo_getval(connOptions, "host");
	conn->pghost = tmp ? strdup(tmp) : NULL;
	tmp = conninfo_getval(connOptions, "port");
	conn->pgport = tmp ? strdup(tmp) : NULL;
	tmp = conninfo_getval(connOptions, "tty");
	conn->pgtty = tmp ? strdup(tmp) : NULL;
	tmp = conninfo_getval(connOptions, "options");
	conn->pgoptions = tmp ? strdup(tmp) : NULL;
	tmp = conninfo_getval(connOptions, "dbname");
	conn->dbName = tmp ? strdup(tmp) : NULL;
	tmp = conninfo_getval(connOptions, "user");
	conn->pguser = tmp ? strdup(tmp) : NULL;
	tmp = conninfo_getval(connOptions, "password");
	conn->pgpass = tmp ? strdup(tmp) : NULL;
	if (conn->pgpass == NULL || conn->pgpass[0] == '\0')
	{
		if (conn->pgpass)
			free(conn->pgpass);
		conn->pgpass = PasswordFromFile(conn->pghost, conn->pgport,
										conn->dbName, conn->pguser);
		if (conn->pgpass == NULL)
			conn->pgpass = strdup(DefaultPassword);
	}
	tmp = conninfo_getval(connOptions, "connect_timeout");
	conn->connect_timeout = tmp ? strdup(tmp) : NULL;
#ifdef USE_SSL
	tmp = conninfo_getval(connOptions, "requiressl");
	if (tmp && tmp[0] == '1')
		conn->require_ssl = true;
#endif

	/*
	 * Free the option info - all is in conn now
	 */
	PQconninfoFree(connOptions);

	/*
	 * Allow unix socket specification in the host name
	 */
	if (conn->pghost && conn->pghost[0] == '/')
	{
		if (conn->pgunixsocket)
			free(conn->pgunixsocket);
		conn->pgunixsocket = conn->pghost;
		conn->pghost = NULL;
	}

	/*
	 * Connect to the database
	 */
	if (!connectDBStart(conn))
	{
		/* Just in case we failed to set it in connectDBStart */
		conn->status = CONNECTION_BAD;
	}

	return conn;
}

/*
 *		PQconndefaults
 *
 * Parse an empty string like PQconnectdb() would do and return the
 * working connection options array.
 *
 * Using this function, an application may determine all possible options
 * and their current default values.
 *
 * NOTE: as of PostgreSQL 7.0, the returned array is dynamically allocated
 * and should be freed when no longer needed via PQconninfoFree().	(In prior
 * versions, the returned array was static, but that's not thread-safe.)
 * Pre-7.0 applications that use this function will see a small memory leak
 * until they are updated to call PQconninfoFree.
 */
PQconninfoOption *
PQconndefaults(void)
{
	PQExpBufferData errorBuf;
	PQconninfoOption *connOptions;

	initPQExpBuffer(&errorBuf);
	connOptions = conninfo_parse("", &errorBuf);
	termPQExpBuffer(&errorBuf);
	return connOptions;
}

/* ----------------
 *		PQsetdbLogin
 *
 * establishes a connection to a postgres backend through the postmaster
 * at the specified host and port.
 *
 * returns a PGconn* which is needed for all subsequent libpq calls
 * if the status field of the connection returned is CONNECTION_BAD,
 * then some fields may be null'ed out instead of having valid values
 *
 *	Uses these environment variables:
 *
 *	  PGHOST	   identifies host to which to connect if <pghost> argument
 *				   is NULL or a null string.
 *
 *	  PGPORT	   identifies TCP port to which to connect if <pgport> argument
 *				   is NULL or a null string.
 *
 *	  PGTTY		   identifies tty to which to send messages if <pgtty> argument
 *				   is NULL or a null string.
 *
 *	  PGOPTIONS    identifies connection options if <pgoptions> argument is
 *				   NULL or a null string.
 *
 *	  PGUSER	   Postgres username to associate with the connection.
 *
 *	  PGPASSWORD   The user's password.
 *
 *	  PGDATABASE   name of database to which to connect if <pgdatabase>
 *				   argument is NULL or a null string
 *
 *	  None of the above need be defined.  There are defaults for all of them.
 *
 * To support "delimited identifiers" for database names, only convert
 * the database name to lower case if it is not surrounded by double quotes.
 * Otherwise, strip the double quotes but leave the reset of the string intact.
 * - thomas 1997-11-08
 * ----------------
 */
PGconn *
PQsetdbLogin(const char *pghost, const char *pgport, const char *pgoptions,
			 const char *pgtty, const char *dbName, const char *login,
			 const char *pwd)
{
	PGconn	   *conn;
	char	   *tmp;			/* An error message from some service we
								 * call. */
	bool		error = FALSE;	/* We encountered an error. */

	conn = makeEmptyPGconn();
	if (conn == NULL)
		return (PGconn *) NULL;

	if (pghost)
		conn->pghost = strdup(pghost);
	else if ((tmp = getenv("PGHOST")) != NULL)
		conn->pghost = strdup(tmp);

	if (pgport == NULL || pgport[0] == '\0')
	{
		tmp = getenv("PGPORT");
		if (tmp == NULL || tmp[0] == '\0')
			tmp = DEF_PGPORT_STR;
		conn->pgport = strdup(tmp);
	}
	else
		conn->pgport = strdup(pgport);

	/*
	 * We don't allow unix socket path as a function parameter. This
	 * allows unix socket specification in the host name.
	 */
	if (conn->pghost && conn->pghost[0] == '/')
	{
		if (conn->pgunixsocket)
			free(conn->pgunixsocket);
		conn->pgunixsocket = conn->pghost;
		conn->pghost = NULL;
	}

	if (pgtty == NULL)
	{
		if ((tmp = getenv("PGTTY")) == NULL)
			tmp = DefaultTty;
		conn->pgtty = strdup(tmp);
	}
	else
		conn->pgtty = strdup(pgtty);

	if (pgoptions == NULL)
	{
		if ((tmp = getenv("PGOPTIONS")) == NULL)
			tmp = DefaultOption;
		conn->pgoptions = strdup(tmp);
	}
	else
		conn->pgoptions = strdup(pgoptions);

	if (login)
		conn->pguser = strdup(login);
	else if ((tmp = getenv("PGUSER")) != NULL)
		conn->pguser = strdup(tmp);
	else
	{
		/* fe-auth.c has not been fixed to support PQExpBuffers, so: */
		conn->pguser = fe_getauthname(conn->errorMessage.data);
		conn->errorMessage.len = strlen(conn->errorMessage.data);
	}

	if (conn->pguser == NULL)
	{
		error = TRUE;
		printfPQExpBuffer(&conn->errorMessage,
						  libpq_gettext("could not determine the PostgreSQL user name to use\n"));
	}

	if (dbName == NULL)
	{
		if ((tmp = getenv("PGDATABASE")) != NULL)
			conn->dbName = strdup(tmp);
		else if (conn->pguser)
			conn->dbName = strdup(conn->pguser);
	}
	else
		conn->dbName = strdup(dbName);

	if (pwd)
		conn->pgpass = strdup(pwd);
	else if ((tmp = getenv("PGPASSWORD")) != NULL)
		conn->pgpass = strdup(tmp);
	else if ((tmp = PasswordFromFile(conn->pghost, conn->pgport,
									 conn->dbName, conn->pguser)) != NULL)
		conn->pgpass = tmp;
	else
		conn->pgpass = strdup(DefaultPassword);

	if ((tmp = getenv("PGCONNECT_TIMEOUT")) != NULL)
		conn->connect_timeout = strdup(tmp);

#ifdef USE_SSL
	if ((tmp = getenv("PGREQUIRESSL")) != NULL)
		conn->require_ssl = (tmp[0] == '1') ? true : false;
#endif

	if (error)
		conn->status = CONNECTION_BAD;
	else
	{
		if (connectDBStart(conn))
			(void) connectDBComplete(conn);
	}

	return conn;
}


#ifdef NOT_USED					/* because it's broken */
/*
 * update_db_info -
 * get all additional info out of dbName
 *
 */
static int
update_db_info(PGconn *conn)
{
	char	   *tmp,
			   *tmp2,
			   *old = conn->dbName;

	if (strchr(conn->dbName, '@') != NULL)
	{
		/* old style: dbname[@server][:port] */
		tmp = strrchr(conn->dbName, ':');
		if (tmp != NULL)		/* port number given */
		{
			if (conn->pgport)
				free(conn->pgport);
			conn->pgport = strdup(tmp + 1);
			*tmp = '\0';
		}

		tmp = strrchr(conn->dbName, '@');
		if (tmp != NULL)		/* host name given */
		{
			if (conn->pghost)
				free(conn->pghost);
			conn->pghost = strdup(tmp + 1);
			*tmp = '\0';
		}

		conn->dbName = strdup(old);
		free(old);
	}
	else
	{
		int			offset;

		/*
		 * only allow protocols tcp and unix
		 */
		if (strncmp(conn->dbName, "tcp:", 4) == 0)
			offset = 4;
		else if (strncmp(conn->dbName, "unix:", 5) == 0)
			offset = 5;
		else
			return 0;

		if (strncmp(conn->dbName + offset, "postgresql://", strlen("postgresql://")) == 0)
		{

			/*-------
			 * new style:
			 *	<tcp|unix>:postgresql://server[:port|:/unixsocket/path:]
			 *	[/db name][?options]
			 *-------
			 */
			offset += strlen("postgresql://");

			tmp = strrchr(conn->dbName + offset, '?');
			if (tmp != NULL)	/* options given */
			{
				if (conn->pgoptions)
					free(conn->pgoptions);
				conn->pgoptions = strdup(tmp + 1);
				*tmp = '\0';
			}

			tmp = strrchr(conn->dbName + offset, '/');
			if (tmp != NULL)	/* database name given */
			{
				if (conn->dbName)
					free(conn->dbName);
				conn->dbName = strdup(tmp + 1);
				*tmp = '\0';
			}
			else
			{
				/*
				 * Why do we default only this value from the environment
				 * again?
				 */
				if ((tmp = getenv("PGDATABASE")) != NULL)
				{
					if (conn->dbName)
						free(conn->dbName);
					conn->dbName = strdup(tmp);
				}
				else if (conn->pguser)
				{
					if (conn->dbName)
						free(conn->dbName);
					conn->dbName = strdup(conn->pguser);
				}
			}

			tmp = strrchr(old + offset, ':');
			if (tmp != NULL)	/* port number or Unix socket path given */
			{
				*tmp = '\0';
				if ((tmp2 = strchr(tmp + 1, ':')) != NULL)
				{
					if (strncmp(old, "unix:", 5) != 0)
					{
						printfPQExpBuffer(&conn->errorMessage,
										  "connectDBStart() -- "
								"socket name can only be specified with "
										  "non-TCP\n");
						return 1;
					}
					*tmp2 = '\0';
					if (conn->pgunixsocket)
						free(conn->pgunixsocket);
					conn->pgunixsocket = strdup(tmp + 1);
				}
				else
				{
					if (conn->pgport)
						free(conn->pgport);
					conn->pgport = strdup(tmp + 1);
					if (conn->pgunixsocket)
						free(conn->pgunixsocket);
					conn->pgunixsocket = NULL;
				}
			}

			if (strncmp(old, "unix:", 5) == 0)
			{
				if (conn->pghost)
					free(conn->pghost);
				conn->pghost = NULL;
				if (strcmp(old + offset, "localhost") != 0)
				{
					printfPQExpBuffer(&conn->errorMessage,
									  "connectDBStart() -- "
									  "non-TCP access only possible on "
									  "localhost\n");
					return 1;
				}
			}
			else
			{
				if (conn->pghost)
					free(conn->pghost);
				conn->pghost = strdup(old + offset);
			}
			free(old);
		}
	}

	return 0;
}
#endif   /* NOT_USED */


/* ----------
 * connectMakeNonblocking -
 * Make a connection non-blocking.
 * Returns 1 if successful, 0 if not.
 * ----------
 */
static int
connectMakeNonblocking(PGconn *conn)
{
#if defined(WIN32) || defined(__BEOS__)
	int			on = 1;
#endif

#if defined(WIN32)
	if (ioctlsocket(conn->sock, FIONBIO, &on) != 0)
#elif defined(__BEOS__)
		if (ioctl(conn->sock, FIONBIO, &on) != 0)
#else
	if (fcntl(conn->sock, F_SETFL, O_NONBLOCK) < 0)
#endif
	{
		printfPQExpBuffer(&conn->errorMessage,
		libpq_gettext("could not set socket to non-blocking mode: %s\n"),
						  SOCK_STRERROR(SOCK_ERRNO));
		return 0;
	}

	return 1;
}

/* ----------
 * connectNoDelay -
 * Sets the TCP_NODELAY socket option.
 * Returns 1 if successful, 0 if not.
 * ----------
 */
static int
connectNoDelay(PGconn *conn)
{
	int			on = 1;

	if (setsockopt(conn->sock, IPPROTO_TCP, TCP_NODELAY,
				   (char *) &on,
				   sizeof(on)) < 0)
	{
		printfPQExpBuffer(&conn->errorMessage,
		libpq_gettext("could not set socket to TCP no delay mode: %s\n"),
						  SOCK_STRERROR(SOCK_ERRNO));
		return 0;
	}

	return 1;
}


/* ----------
 * connectFailureMessage -
 * create a friendly error message on connection failure.
 * ----------
 */
static void
connectFailureMessage(PGconn *conn, int errorno)
{
	if (conn->raddr.sa.sa_family == AF_UNIX)
		printfPQExpBuffer(&conn->errorMessage,
						  libpq_gettext(
									  "could not connect to server: %s\n"
						"\tIs the server running locally and accepting\n"
						  "\tconnections on Unix domain socket \"%s\"?\n"
										),
						  SOCK_STRERROR(errorno),
						  conn->raddr.un.sun_path);
	else
		printfPQExpBuffer(&conn->errorMessage,
						  libpq_gettext(
									  "could not connect to server: %s\n"
					 "\tIs the server running on host %s and accepting\n"
									 "\tTCP/IP connections on port %s?\n"
										),
						  SOCK_STRERROR(errorno),
						  conn->pghost
						  ? conn->pghost
						  : (conn->pghostaddr
							 ? conn->pghostaddr
							 : "???"),
						  conn->pgport);
}


/* ----------
 * connectDBStart -
 * Start to make a connection to the backend so it is ready to receive
 * queries.
 * Returns 1 if successful, 0 if not.
 * ----------
 */
static int
connectDBStart(PGconn *conn)
{
	int			portnum;
	char		portstr[64];
#ifdef USE_SSL
	StartupPacket np;			/* Used to negotiate SSL connection */
	char		SSLok;
#endif
#ifdef HAVE_IPV6
	struct addrinfo *addrs = NULL;
	struct addrinfo *addr_cur = NULL;
	struct addrinfo hint;
	const char *node = NULL;
	const char *unix_node = "unix";
	int			ret;

	/* Initialize hint structure */
	MemSet(&hint, 0, sizeof(hint));
	hint.ai_socktype = SOCK_STREAM;
#else
	int			family = -1;
#endif

	if (!conn)
		return 0;

#ifdef NOT_USED
	/*
	 * parse dbName to get all additional info in it, if any
	 */
	if (update_db_info(conn) != 0)
		goto connect_errReturn;
#endif

	/* Ensure our buffers are empty */
	conn->inStart = conn->inCursor = conn->inEnd = 0;
	conn->outCount = 0;

	/*
	 * Set up the connection to postmaster/backend.
	 *
	 *	This code is confusing because IPv6 creates a hint structure
	 *	that is passed to getaddrinfo2(), which returns a list of address
	 *	structures that are looped through, while IPv4 creates an address
	 *	structure directly.
	 */

	MemSet((char *) &conn->raddr, 0, sizeof(conn->raddr));

	/* Set port number */
	if (conn->pgport != NULL && conn->pgport[0] != '\0')
		portnum = atoi(conn->pgport);
	else
		portnum = DEF_PGPORT;
	snprintf(portstr, sizeof(portstr), "%d", portnum);

	if (conn->pghostaddr != NULL && conn->pghostaddr[0] != '\0')
	{
#ifdef HAVE_IPV6
		node = conn->pghostaddr;
		hint.ai_family = AF_UNSPEC;
#else
		/* Using pghostaddr avoids a hostname lookup */
		struct in_addr addr;

		if (!inet_aton(conn->pghostaddr, &addr))
		{
			printfPQExpBuffer(&conn->errorMessage,
							  libpq_gettext("invalid host address: %s\n"),
							  conn->pghostaddr);
			goto connect_errReturn;
		}

		family = AF_INET;

		memcpy((char *) &(conn->raddr.in.sin_addr),
			   (char *) &addr, sizeof(addr));
#endif
	}
	else if (conn->pghost != NULL && conn->pghost[0] != '\0')
	{
#ifdef HAVE_IPV6
		node = conn->pghost;
		hint.ai_family = AF_UNSPEC;
#else
		/* Using pghost, so we have to look-up the hostname */
		if (getaddrinfo2(conn->pghost, portstr, family, &conn->raddr) != 0)
			goto connect_errReturn;

		family = AF_INET;
#endif
	}
	else
	{
#ifdef HAVE_UNIX_SOCKETS
#ifdef HAVE_IPV6
		node = unix_node;
		hint.ai_family = AF_UNIX;
#else
		/* pghostaddr and pghost are NULL, so use Unix domain socket */
		family = AF_UNIX;
#endif
#endif   /* HAVE_UNIX_SOCKETS */
	}

#ifndef HAVE_IPV6
	/* Set family */
	conn->raddr.sa.sa_family = family;
#endif

#ifdef HAVE_IPV6
	if (hint.ai_family == AF_UNSPEC)
	{
		/* do nothing */
	}
#else
	if (family == AF_INET)
	{
		conn->raddr.in.sin_port = htons((unsigned short) (portnum));
		conn->raddr_len = sizeof(struct sockaddr_in);
	}
#endif
	else
	{
#ifdef HAVE_UNIX_SOCKETS
		UNIXSOCK_PATH(conn->raddr.un, portnum, conn->pgunixsocket);
		conn->raddr_len = UNIXSOCK_LEN(conn->raddr.un);
		StrNCpy(portstr, conn->raddr.un.sun_path, sizeof(portstr));
#ifdef USE_SSL
		/* Don't bother requesting SSL over a Unix socket */
		conn->allow_ssl_try = false;
		conn->require_ssl = false;
#endif
#endif   /* HAVE_UNIX_SOCKETS */
	}

#ifdef HAVE_IPV6
	/* Use getaddrinfo2() to resolve the address */
	ret = getaddrinfo2(node, portstr, &hint, &addrs);
	if (ret || addrs == NULL)
	{
		printfPQExpBuffer(&conn->errorMessage,
						  libpq_gettext("failed to getaddrinfo(): %s\n"),
						  gai_strerror(ret));
		goto connect_errReturn;
	}
#endif

	/*
	 * For IPV6 we loop over the possible addresses returned by
	 * getaddrinfo2(), and fail only when they all fail (reporting the
	 * error returned for the *last* alternative, which may not be what
	 * users expect :-().  Otherwise, there is no true loop here.
	 *
	 * In either case, we never actually fall out of the loop; the
	 * only exits are via "break" or "goto connect_errReturn".  Thus,
	 * there is no exit test in the for().
	 */
	for (
#ifdef HAVE_IPV6
		addr_cur = addrs; ; addr_cur = addr_cur->ai_next
#else
			;;
#endif
		)
	{
		/* Open a socket */
#ifdef HAVE_IPV6
		conn->sock = socket(addr_cur->ai_family, SOCK_STREAM,
							addr_cur->ai_protocol);
#else
		conn->sock = socket(family, SOCK_STREAM, 0);
#endif
		if (conn->sock < 0)
		{
#ifdef HAVE_IPV6
			/* ignore socket() failure if we have more addrs to try */
			if (addr_cur->ai_next != NULL)
				continue;
#endif
			printfPQExpBuffer(&conn->errorMessage,
							  libpq_gettext("could not create socket: %s\n"),
							  SOCK_STRERROR(SOCK_ERRNO));
			goto connect_errReturn;
		}

		/*
		 * Set the right options. Normally, we need nonblocking I/O, and we
		 * don't want delay of outgoing data for AF_INET sockets.  If we are
		 * using SSL, then we need the blocking I/O (XXX Can this be fixed?).
		 */

#ifdef HAVE_IPV6
		if (isAF_INETx(addr_cur->ai_family))
#else
		if (isAF_INETx(family))
#endif
		{
			if (!connectNoDelay(conn))
				goto connect_errReturn;
		}

#if !defined(USE_SSL)
		if (connectMakeNonblocking(conn) == 0)
			goto connect_errReturn;
#endif

		/* ----------
		 * Start / make connection.  We are hopefully in non-blocking mode
		 * now, but it is possible that:
		 *	 1. Older systems will still block on connect, despite the
		 *		non-blocking flag. (Anyone know if this is true?)
		 *	 2. We are using SSL.
		 * Thus, we have to make arrangements for all eventualities.
		 * ----------
		 */
retry1:
#ifdef HAVE_IPV6
		if (connect(conn->sock, addr_cur->ai_addr, addr_cur->ai_addrlen) < 0)
#else
		if (connect(conn->sock, &conn->raddr.sa, conn->raddr_len) < 0)
#endif
		{
			if (SOCK_ERRNO == EINTR)
				/* Interrupted system call - we'll just try again */
				goto retry1;

			if (SOCK_ERRNO == EINPROGRESS || SOCK_ERRNO == EWOULDBLOCK || SOCK_ERRNO == 0)
			{
				/*
				 * This is fine - we're in non-blocking mode, and the
				 * connection is in progress.
				 */
				conn->status = CONNECTION_STARTED;
				break;
			}
			/* otherwise, trouble */
		}
		else
		{
			/* We're connected already */
			conn->status = CONNECTION_MADE;
			break;
		}
		/*
		 * This connection failed.  We need to close the socket,
		 * and either loop to try the next address or report an error.
		 */
#ifdef HAVE_IPV6
		/* ignore connect() failure if we have more addrs to try */
		if (addr_cur->ai_next != NULL)
		{
			close(conn->sock);
			conn->sock = -1;
			continue;
		}
#endif
		connectFailureMessage(conn, SOCK_ERRNO);
		goto connect_errReturn;
	} /* loop over addrs */

#ifdef HAVE_IPV6
	/* Remember the successfully opened address alternative */
	memcpy(&conn->raddr, addr_cur->ai_addr, addr_cur->ai_addrlen);
	conn->raddr_len = addr_cur->ai_addrlen;
	/* and release the address list */
	FREEADDRINFO2(hint.ai_family, addrs);
	addrs = NULL;
#endif

#ifdef USE_SSL
	/* Attempt to negotiate SSL usage */
	if (conn->allow_ssl_try)
	{
		memset((char *) &np, 0, sizeof(np));
		np.protoVersion = htonl(NEGOTIATE_SSL_CODE);
		if (pqPacketSend(conn, (char *) &np, sizeof(StartupPacket)) != STATUS_OK)
		{
			printfPQExpBuffer(&conn->errorMessage,
			libpq_gettext("could not send SSL negotiation packet: %s\n"),
							  SOCK_STRERROR(SOCK_ERRNO));
			goto connect_errReturn;
		}
retry2:
		/* Now receive the postmasters response */
		if (recv(conn->sock, &SSLok, 1, 0) != 1)
		{
			if (SOCK_ERRNO == EINTR)
				/* Interrupted system call - we'll just try again */
				goto retry2;

			printfPQExpBuffer(&conn->errorMessage,
							  libpq_gettext("could not receive server response to SSL negotiation packet: %s\n"),
							  SOCK_STRERROR(SOCK_ERRNO));
			goto connect_errReturn;
		}
		if (SSLok == 'S')
		{
			if (pqsecure_initialize(conn) == -1 ||
				pqsecure_open_client(conn) == -1)
				goto connect_errReturn;
			/* SSL connection finished. Continue to send startup packet */
		}
		else if (SSLok == 'E')
		{
			/* Received error - probably protocol mismatch */
			if (conn->Pfdebug)
				fprintf(conn->Pfdebug, "Postmaster reports error, attempting fallback to pre-7.0.\n");
			pqsecure_close(conn);
#ifdef WIN32
			closesocket(conn->sock);
#else
			close(conn->sock);
#endif
			conn->sock = -1;
			conn->allow_ssl_try = FALSE;
			return connectDBStart(conn);
		}
		else if (SSLok != 'N')
		{
			printfPQExpBuffer(&conn->errorMessage,
							  libpq_gettext("received invalid response to SSL negotiation: %c\n"),
							  SSLok);
			goto connect_errReturn;
		}
	}
	if (conn->require_ssl && !conn->ssl)
	{
		/* Require SSL, but server does not support/want it */
		printfPQExpBuffer(&conn->errorMessage,
						  libpq_gettext("server does not support SSL, but SSL was required\n"));
		goto connect_errReturn;
	}
#endif

	/*
	 * This makes the connection non-blocking, for all those cases which
	 * forced us not to do it above.
	 */
#if defined(USE_SSL)
	if (connectMakeNonblocking(conn) == 0)
		goto connect_errReturn;
#endif

	return 1;

connect_errReturn:
	if (conn->sock >= 0)
	{
		pqsecure_close(conn);
#ifdef WIN32
		closesocket(conn->sock);
#else
		close(conn->sock);
#endif
		conn->sock = -1;
	}
	conn->status = CONNECTION_BAD;
#ifdef HAVE_IPV6
	if (addrs != NULL)
		FREEADDRINFO2(hint.ai_family, addrs);
#endif
	return 0;
}


/*
 *		connectDBComplete
 *
 * Block and complete a connection.
 *
 * Returns 1 on success, 0 on failure.
 */
static int
connectDBComplete(PGconn *conn)
{
	PostgresPollingStatusType flag = PGRES_POLLING_WRITING;
	time_t		finish_time = ((time_t) -1);

	if (conn == NULL || conn->status == CONNECTION_BAD)
		return 0;

	/*
	 * Set up a time limit, if connect_timeout isn't zero.
	 */
	if (conn->connect_timeout != NULL)
	{
		int			timeout = atoi(conn->connect_timeout);

		if (timeout > 0)
		{
			/*
			 * Rounding could cause connection to fail; need at least 2
			 * secs
			 */
			if (timeout < 2)
				timeout = 2;
			/* calculate the finish time based on start + timeout */
			finish_time = time(NULL) + timeout;
		}
	}

	for (;;)
	{
		/*
		 * Wait, if necessary.	Note that the initial state (just after
		 * PQconnectStart) is to wait for the socket to select for
		 * writing.
		 */
		switch (flag)
		{
			case PGRES_POLLING_ACTIVE:
				break;

			case PGRES_POLLING_OK:
				return 1;		/* success! */

			case PGRES_POLLING_READING:
				if (pqWaitTimed(1, 0, conn, finish_time))
				{
					conn->status = CONNECTION_BAD;
					return 0;
				}
				break;

			case PGRES_POLLING_WRITING:
				if (pqWaitTimed(0, 1, conn, finish_time))
				{
					conn->status = CONNECTION_BAD;
					return 0;
				}
				break;

			default:
				/* Just in case we failed to set it in PQconnectPoll */
				conn->status = CONNECTION_BAD;
				return 0;
		}

		/*
		 * Now try to advance the state machine.
		 */
		flag = PQconnectPoll(conn);
	}
}

/* ----------------
 *		PQconnectPoll
 *
 * Poll an asynchronous connection.
 *
 * Returns a PostgresPollingStatusType.
 * Before calling this function, use select(2) to determine when data
 * has arrived..
 *
 * You must call PQfinish whether or not this fails.
 *
 * This function and PQconnectStart are intended to allow connections to be
 * made without blocking the execution of your program on remote I/O. However,
 * there are a number of caveats:
 *
 *	 o	If you call PQtrace, ensure that the stream object into which you trace
 *		will not block.
 *	 o	If you do not supply an IP address for the remote host (i.e. you
 *		supply a host name instead) then this function will block on
 *		gethostbyname.	You will be fine if using Unix sockets (i.e. by
 *		supplying neither a host name nor a host address).
 *	 o	If your backend wants to use Kerberos authentication then you must
 *		supply both a host name and a host address, otherwise this function
 *		may block on gethostname.
 *	 o	This function will block if compiled with USE_SSL.
 *
 * ----------------
 */
PostgresPollingStatusType
PQconnectPoll(PGconn *conn)
{
	PGresult   *res;

	if (conn == NULL)
		return PGRES_POLLING_FAILED;

	/* Get the new data */
	switch (conn->status)
	{
			/*
			 * We really shouldn't have been polled in these two cases,
			 * but we can handle it.
			 */
		case CONNECTION_BAD:
			return PGRES_POLLING_FAILED;
		case CONNECTION_OK:
			return PGRES_POLLING_OK;

			/* These are reading states */
		case CONNECTION_AWAITING_RESPONSE:
		case CONNECTION_AUTH_OK:
			{
				/* Load waiting data */
				int			n = pqReadData(conn);

				if (n < 0)
					goto error_return;
				if (n == 0)
					return PGRES_POLLING_READING;

				break;
			}

			/* These are writing states, so we just proceed. */
		case CONNECTION_STARTED:
		case CONNECTION_MADE:
			break;

		case CONNECTION_SETENV:
			/* We allow PQsetenvPoll to decide whether to proceed */
			break;

		default:
			printfPQExpBuffer(&conn->errorMessage,
							  libpq_gettext(
											"invalid connection state, "
							 "probably indicative of memory corruption\n"
											));
			goto error_return;
	}


keep_going:						/* We will come back to here until there
								 * is nothing left to parse. */
	switch (conn->status)
	{
		case CONNECTION_STARTED:
			{
				ACCEPT_TYPE_ARG3 laddrlen;
				int			optval;
				ACCEPT_TYPE_ARG3 optlen = sizeof(optval);

				/*
				 * Write ready, since we've made it here, so the
				 * connection has been made.
				 */

				/*
				 * Now check (using getsockopt) that there is not an error
				 * state waiting for us on the socket.
				 */

				if (getsockopt(conn->sock, SOL_SOCKET, SO_ERROR,
							   (char *) &optval, &optlen) == -1)
				{
					printfPQExpBuffer(&conn->errorMessage,
									  libpq_gettext("could not get socket error status: %s\n"),
									  SOCK_STRERROR(SOCK_ERRNO));
					goto error_return;
				}
				else if (optval != 0)
				{
					/*
					 * When using a nonblocking connect, we will typically
					 * see connect failures at this point, so provide a
					 * friendly error message.
					 */
					connectFailureMessage(conn, optval);
					goto error_return;
				}

				/* Fill in the client address */
				laddrlen = sizeof(conn->laddr);
				if (getsockname(conn->sock, &conn->laddr.sa, &laddrlen) < 0)
				{
					printfPQExpBuffer(&conn->errorMessage,
									  libpq_gettext("could not get client address from socket: %s\n"),
									  SOCK_STRERROR(SOCK_ERRNO));
					goto error_return;
				}

				conn->status = CONNECTION_MADE;
				return PGRES_POLLING_WRITING;
			}

		case CONNECTION_MADE:
			{
				StartupPacket sp;

				/*
				 * Initialize the startup packet.
				 */

				MemSet((char *) &sp, 0, sizeof(StartupPacket));

				sp.protoVersion = (ProtocolVersion) htonl(PG_PROTOCOL_LIBPQ);

				strncpy(sp.user, conn->pguser, SM_USER);
				strncpy(sp.database, conn->dbName, SM_DATABASE);
				strncpy(sp.tty, conn->pgtty, SM_TTY);

				if (conn->pgoptions)
					strncpy(sp.options, conn->pgoptions, SM_OPTIONS);

				/*
				 * Send the startup packet.
				 *
				 * Theoretically, this could block, but it really shouldn't
				 * since we only got here if the socket is write-ready.
				 */

				if (pqPacketSend(conn, (char *) &sp,
								 sizeof(StartupPacket)) != STATUS_OK)
				{
					printfPQExpBuffer(&conn->errorMessage,
					libpq_gettext("could not send startup packet: %s\n"),
									  SOCK_STRERROR(SOCK_ERRNO));
					goto error_return;
				}

				conn->status = CONNECTION_AWAITING_RESPONSE;
				return PGRES_POLLING_READING;
			}

			/*
			 * Handle the authentication exchange: wait for postmaster
			 * messages and respond as necessary.
			 */
		case CONNECTION_AWAITING_RESPONSE:
			{
				char		beresp;
				AuthRequest areq;

				/*
				 * Scan the message from current point (note that if we
				 * find the message is incomplete, we will return without
				 * advancing inStart, and resume here next time).
				 */
				conn->inCursor = conn->inStart;

				if (pqGetc(&beresp, conn))
				{
					/* We'll come back when there is more data */
					return PGRES_POLLING_READING;
				}

				/* Handle errors. */
				if (beresp == 'E')
				{
					if (pqGets(&conn->errorMessage, conn))
					{
						/* We'll come back when there is more data */
						return PGRES_POLLING_READING;
					}
					/* OK, we read the message; mark data consumed */
					conn->inStart = conn->inCursor;

					/*
					 * The postmaster typically won't end its message with
					 * a newline, so add one to conform to libpq
					 * conventions.
					 */
					appendPQExpBufferChar(&conn->errorMessage, '\n');
					goto error_return;
				}

				/* Otherwise it should be an authentication request. */
				if (beresp != 'R')
				{
					printfPQExpBuffer(&conn->errorMessage,
									  libpq_gettext(
								  "expected authentication request from "
											  "server, but received %c\n"
													),
									  beresp);
					goto error_return;
				}

				/* Get the type of request. */
				if (pqGetInt((int *) &areq, 4, conn))
				{
					/* We'll come back when there are more data */
					return PGRES_POLLING_READING;
				}

				/* Get the password salt if there is one. */
				if (areq == AUTH_REQ_MD5)
				{
					if (pqGetnchar(conn->md5Salt,
								   sizeof(conn->md5Salt), conn))
					{
						/* We'll come back when there are more data */
						return PGRES_POLLING_READING;
					}
				}
				if (areq == AUTH_REQ_CRYPT)
				{
					if (pqGetnchar(conn->cryptSalt,
								   sizeof(conn->cryptSalt), conn))
					{
						/* We'll come back when there are more data */
						return PGRES_POLLING_READING;
					}
				}

				/*
				 * OK, we successfully read the message; mark data
				 * consumed
				 */
				conn->inStart = conn->inCursor;

				/* Respond to the request if necessary. */

				/*
				 * Note that conn->pghost must be non-NULL if we are going
				 * to avoid the Kerberos code doing a hostname look-up.
				 */

				/*
				 * XXX fe-auth.c has not been fixed to support
				 * PQExpBuffers, so:
				 */
				if (fe_sendauth(areq, conn, conn->pghost, conn->pgpass,
								conn->errorMessage.data) != STATUS_OK)
				{
					conn->errorMessage.len = strlen(conn->errorMessage.data);
					goto error_return;
				}
				conn->errorMessage.len = strlen(conn->errorMessage.data);

				/*
				 * Just make sure that any data sent by fe_sendauth is
				 * flushed out.  Although this theoretically could block,
				 * it really shouldn't since we don't send large auth
				 * responses.
				 */
				if (pqFlush(conn))
					goto error_return;

				if (areq == AUTH_REQ_OK)
				{
					/* We are done with authentication exchange */
					conn->status = CONNECTION_AUTH_OK;

					/*
					 * Set asyncStatus so that PQsetResult will think that
					 * what comes back next is the result of a query.  See
					 * below.
					 */
					conn->asyncStatus = PGASYNC_BUSY;
				}

				/* Look to see if we have more data yet. */
				goto keep_going;
			}

		case CONNECTION_AUTH_OK:
			{
				/*
				 * Now we expect to hear from the backend. A ReadyForQuery
				 * message indicates that startup is successful, but we
				 * might also get an Error message indicating failure.
				 * (Notice messages indicating nonfatal warnings are also
				 * allowed by the protocol, as is a BackendKeyData
				 * message.) Easiest way to handle this is to let
				 * PQgetResult() read the messages. We just have to fake
				 * it out about the state of the connection, by setting
				 * asyncStatus = PGASYNC_BUSY (done above).
				 */

				if (PQisBusy(conn))
					return PGRES_POLLING_READING;

				res = PQgetResult(conn);

				/*
				 * NULL return indicating we have gone to IDLE state is
				 * expected
				 */
				if (res)
				{
					if (res->resultStatus != PGRES_FATAL_ERROR)
						printfPQExpBuffer(&conn->errorMessage,
										  libpq_gettext("unexpected message from server during startup\n"));

					/*
					 * if the resultStatus is FATAL, then
					 * conn->errorMessage already has a copy of the error;
					 * needn't copy it back. But add a newline if it's not
					 * there already, since postmaster error messages may
					 * not have one.
					 */
					if (conn->errorMessage.len <= 0 ||
						conn->errorMessage.data[conn->errorMessage.len - 1] != '\n')
						appendPQExpBufferChar(&conn->errorMessage, '\n');
					PQclear(res);
					goto error_return;
				}

				/*
				 * Post-connection housekeeping. Prepare to send
				 * environment variables to server.
				 */
				if (!PQsetenvStart(conn))
					goto error_return;

				conn->status = CONNECTION_SETENV;

				goto keep_going;
			}

		case CONNECTION_SETENV:

			/*
			 * We pretend that the connection is OK for the duration of
			 * these queries.
			 */
			conn->status = CONNECTION_OK;

			switch (PQsetenvPoll(conn))
			{
				case PGRES_POLLING_OK:	/* Success */
					conn->status = CONNECTION_OK;
					return PGRES_POLLING_OK;

				case PGRES_POLLING_READING:		/* Still going */
					conn->status = CONNECTION_SETENV;
					return PGRES_POLLING_READING;

				case PGRES_POLLING_WRITING:		/* Still going */
					conn->status = CONNECTION_SETENV;
					return PGRES_POLLING_WRITING;

				default:
					conn->status = CONNECTION_SETENV;
					goto error_return;
			}
			/* Unreachable */

		default:
			printfPQExpBuffer(&conn->errorMessage,
							  libpq_gettext(
										  "invalid connection state %c, "
							 "probably indicative of memory corruption\n"
											),
							  conn->status);
			goto error_return;
	}

	/* Unreachable */

error_return:

	/*
	 * We used to close the socket at this point, but that makes it
	 * awkward for those above us if they wish to remove this socket from
	 * their own records (an fd_set for example).  We'll just have this
	 * socket closed when PQfinish is called (which is compulsory even
	 * after an error, since the connection structure must be freed).
	 */
	return PGRES_POLLING_FAILED;
}


/*
 *		PQsetenvStart
 *
 * Starts the process of passing the values of a standard set of environment
 * variables to the backend.
 */
static bool
PQsetenvStart(PGconn *conn)
{
	if (conn == NULL ||
		conn->status == CONNECTION_BAD ||
		conn->setenv_state != SETENV_STATE_IDLE)
		return false;

	conn->setenv_state = SETENV_STATE_ENCODINGS_SEND;
	conn->next_eo = EnvironmentOptions;

	return true;
}

/*
 *		PQsetenvPoll
 *
 * Polls the process of passing the values of a standard set of environment
 * variables to the backend.
 */
static PostgresPollingStatusType
PQsetenvPoll(PGconn *conn)
{
	PGresult   *res;

	if (conn == NULL || conn->status == CONNECTION_BAD)
		return PGRES_POLLING_FAILED;

	/* Check whether there are any data for us */
	switch (conn->setenv_state)
	{
			/* These are reading states */
		case SETENV_STATE_ENCODINGS_WAIT:
		case SETENV_STATE_OPTION_WAIT:
			{
				/* Load waiting data */
				int			n = pqReadData(conn);

				if (n < 0)
					goto error_return;
				if (n == 0)
					return PGRES_POLLING_READING;

				break;
			}

			/* These are writing states, so we just proceed. */
		case SETENV_STATE_ENCODINGS_SEND:
		case SETENV_STATE_OPTION_SEND:
			break;

			/* Should we raise an error if called when not active? */
		case SETENV_STATE_IDLE:
			return PGRES_POLLING_OK;

		default:
			printfPQExpBuffer(&conn->errorMessage,
							  libpq_gettext(
											"invalid setenv state %c, "
							 "probably indicative of memory corruption\n"
											),
							  conn->setenv_state);
			goto error_return;
	}

	/* We will loop here until there is nothing left to do in this call. */
	for (;;)
	{
		switch (conn->setenv_state)
		{
			case SETENV_STATE_ENCODINGS_SEND:
				{
					const char *env = getenv("PGCLIENTENCODING");

					if (!env || *env == '\0')
					{
						/*
						 * PGCLIENTENCODING is not specified, so query
						 * server for it.  We must use begin/commit in
						 * case autocommit is off by default.
						 */
						if (!PQsendQuery(conn, "begin; select getdatabaseencoding(); commit"))
							goto error_return;

						conn->setenv_state = SETENV_STATE_ENCODINGS_WAIT;
						return PGRES_POLLING_READING;
					}
					else
					{
						/* otherwise set client encoding in pg_conn struct */
						int			encoding = pg_char_to_encoding(env);

						if (encoding < 0)
						{
							printfPQExpBuffer(&conn->errorMessage,
											  libpq_gettext("invalid encoding name in PGCLIENTENCODING: %s\n"),
											  env);
							goto error_return;
						}
						conn->client_encoding = encoding;

						/* Move on to setting the environment options */
						conn->setenv_state = SETENV_STATE_OPTION_SEND;
					}
					break;
				}

			case SETENV_STATE_ENCODINGS_WAIT:
				{
					if (PQisBusy(conn))
						return PGRES_POLLING_READING;

					res = PQgetResult(conn);

					if (res)
					{
						if (PQresultStatus(res) == PGRES_TUPLES_OK)
						{
							/* set client encoding in pg_conn struct */
							char	   *encoding;

							encoding = PQgetvalue(res, 0, 0);
							if (!encoding)		/* this should not happen */
								conn->client_encoding = PG_SQL_ASCII;
							else
								conn->client_encoding = pg_char_to_encoding(encoding);
						}
						else if (PQresultStatus(res) != PGRES_COMMAND_OK)
						{
							PQclear(res);
							goto error_return;
						}
						PQclear(res);
						/* Keep reading until PQgetResult returns NULL */
					}
					else
					{
						/*
						 * NULL result indicates that the query is
						 * finished
						 */
						/* Move on to setting the environment options */
						conn->setenv_state = SETENV_STATE_OPTION_SEND;
					}
					break;
				}

			case SETENV_STATE_OPTION_SEND:
				{
					/* Send an Environment Option */
					char		setQuery[100];	/* note length limits in
												 * sprintf's below */

					if (conn->next_eo->envName)
					{
						const char *val;

						if ((val = getenv(conn->next_eo->envName)))
						{
							if (strcasecmp(val, "default") == 0)
								sprintf(setQuery, "SET %s = %.60s",
										conn->next_eo->pgName, val);
							else
								sprintf(setQuery, "SET %s = '%.60s'",
										conn->next_eo->pgName, val);
#ifdef CONNECTDEBUG
							printf("Use environment variable %s to send %s\n",
								   conn->next_eo->envName, setQuery);
#endif
							if (!PQsendQuery(conn, setQuery))
								goto error_return;

							conn->setenv_state = SETENV_STATE_OPTION_WAIT;
						}
						else
							conn->next_eo++;
					}
					else
					{
						/* No more options to send, so we are done. */
						conn->setenv_state = SETENV_STATE_IDLE;
					}
					break;
				}

			case SETENV_STATE_OPTION_WAIT:
				{
					if (PQisBusy(conn))
						return PGRES_POLLING_READING;

					res = PQgetResult(conn);

					if (res)
					{
						if (PQresultStatus(res) != PGRES_COMMAND_OK)
						{
							PQclear(res);
							goto error_return;
						}
						PQclear(res);
						/* Keep reading until PQgetResult returns NULL */
					}
					else
					{
						/*
						 * NULL result indicates that the query is
						 * finished
						 */
						/* Send the next option */
						conn->next_eo++;
						conn->setenv_state = SETENV_STATE_OPTION_SEND;
					}
					break;
				}

			case SETENV_STATE_IDLE:
				return PGRES_POLLING_OK;

			default:
				printfPQExpBuffer(&conn->errorMessage,
								  libpq_gettext("invalid state %c, "
						   "probably indicative of memory corruption\n"),
								  conn->setenv_state);
				goto error_return;
		}
	}

	/* Unreachable */

error_return:
	conn->setenv_state = SETENV_STATE_IDLE;
	return PGRES_POLLING_FAILED;
}


#ifdef NOT_USED

/*
 *		PQsetenv
 *
 * Passes the values of a standard set of environment variables to the
 * backend.
 *
 * Returns true on success, false on failure.
 *
 * This function used to be exported for no particularly good reason.
 * Since it's no longer used by libpq itself, let's try #ifdef'ing it out
 * and see if anyone complains.
 */
static bool
PQsetenv(PGconn *conn)
{
	PostgresPollingStatusType flag = PGRES_POLLING_WRITING;

	if (!PQsetenvStart(conn))
		return false;

	for (;;)
	{
		/*
		 * Wait, if necessary.	Note that the initial state (just after
		 * PQsetenvStart) is to wait for the socket to select for writing.
		 */
		switch (flag)
		{
			case PGRES_POLLING_ACTIVE:
				break;

			case PGRES_POLLING_OK:
				return true;	/* success! */

			case PGRES_POLLING_READING:
				if (pqWait(1, 0, conn))
				{
					conn->status = CONNECTION_BAD;
					return false;
				}
				break;

			case PGRES_POLLING_WRITING:
				if (pqWait(0, 1, conn))
				{
					conn->status = CONNECTION_BAD;
					return false;
				}
				break;

			default:
				/* Just in case we failed to set it in PQsetenvPoll */
				conn->status = CONNECTION_BAD;
				return false;
		}

		/*
		 * Now try to advance the state machine.
		 */
		flag = PQsetenvPoll(conn);
	}
}
#endif   /* NOT_USED */


/*
 * makeEmptyPGconn
 *	 - create a PGconn data structure with (as yet) no interesting data
 */
static PGconn *
makeEmptyPGconn(void)
{
	PGconn	   *conn = (PGconn *) malloc(sizeof(PGconn));

	if (conn == NULL)
		return conn;

	/* Zero all pointers and booleans */
	MemSet((char *) conn, 0, sizeof(PGconn));

	conn->noticeHook = defaultNoticeProcessor;
	conn->status = CONNECTION_BAD;
	conn->asyncStatus = PGASYNC_IDLE;
	conn->setenv_state = SETENV_STATE_IDLE;
	conn->notifyList = DLNewList();
	conn->sock = -1;
#ifdef USE_SSL
	conn->allow_ssl_try = TRUE;
#endif

	/*
	 * The output buffer size is set to 8K, which is the usual size of
	 * pipe buffers on Unix systems.  That way, when we are sending a
	 * large amount of data, we avoid incurring extra kernel context swaps
	 * for partial bufferloads.  Note that we currently don't ever enlarge
	 * the output buffer.
	 *
	 * With the same goal of minimizing context swaps, the input buffer will
	 * be enlarged anytime it has less than 8K free, so we initially
	 * allocate twice that.
	 */
	conn->inBufSize = 16 * 1024;
	conn->inBuffer = (char *) malloc(conn->inBufSize);
	conn->outBufSize = 8 * 1024;
	conn->outBuffer = (char *) malloc(conn->outBufSize);
	conn->nonblocking = FALSE;
	initPQExpBuffer(&conn->errorMessage);
	initPQExpBuffer(&conn->workBuffer);
	if (conn->inBuffer == NULL ||
		conn->outBuffer == NULL ||
		conn->errorMessage.data == NULL ||
		conn->workBuffer.data == NULL)
	{
		/* out of memory already :-( */
		freePGconn(conn);
		conn = NULL;
	}
	return conn;
}

/*
 * freePGconn
 *	 - free the PGconn data structure
 *
 */
static void
freePGconn(PGconn *conn)
{
	if (!conn)
		return;
	pqClearAsyncResult(conn);	/* deallocate result and curTuple */
	if (conn->sock >= 0)
	{
		pqsecure_close(conn);
#ifdef WIN32
		closesocket(conn->sock);
#else
		close(conn->sock);
#endif
	}
	if (conn->pghost)
		free(conn->pghost);
	if (conn->pghostaddr)
		free(conn->pghostaddr);
	if (conn->pgport)
		free(conn->pgport);
	if (conn->pgunixsocket)
		free(conn->pgunixsocket);
	if (conn->pgtty)
		free(conn->pgtty);
	if (conn->pgoptions)
		free(conn->pgoptions);
	if (conn->dbName)
		free(conn->dbName);
	if (conn->pguser)
		free(conn->pguser);
	if (conn->pgpass)
		free(conn->pgpass);
	if (conn->connect_timeout)
		free(conn->connect_timeout);
	/* Note that conn->Pfdebug is not ours to close or free */
	if (conn->notifyList)
		DLFreeList(conn->notifyList);
	if (conn->lobjfuncs)
		free(conn->lobjfuncs);
	if (conn->inBuffer)
		free(conn->inBuffer);
	if (conn->outBuffer)
		free(conn->outBuffer);
	termPQExpBuffer(&conn->errorMessage);
	termPQExpBuffer(&conn->workBuffer);
	free(conn);
}

/*
   closePGconn
	 - properly close a connection to the backend
*/
static void
closePGconn(PGconn *conn)
{
	/*
	 * Note that the protocol doesn't allow us to send Terminate messages
	 * during the startup phase.
	 */
	if (conn->sock >= 0 && conn->status == CONNECTION_OK)
	{
		/*
		 * Try to send "close connection" message to backend. Ignore any
		 * error. Note: this routine used to go to substantial lengths to
		 * avoid getting SIGPIPE'd if the connection were already closed.
		 * Now we rely on pqFlush to avoid the signal.
		 */
		pqPutc('X', conn);
		pqFlush(conn);
	}

	/*
	 * must reset the blocking status so a possible reconnect will work
	 * don't call PQsetnonblocking() because it will fail if it's unable
	 * to flush the connection.
	 */
	conn->nonblocking = FALSE;

	/*
	 * Close the connection, reset all transient state, flush I/O buffers.
	 */
	if (conn->sock >= 0)
	{
		pqsecure_close(conn);
#ifdef WIN32
		closesocket(conn->sock);
#else
		close(conn->sock);
#endif
	}
	conn->sock = -1;
	conn->status = CONNECTION_BAD;		/* Well, not really _bad_ - just
										 * absent */
	conn->asyncStatus = PGASYNC_IDLE;
	pqClearAsyncResult(conn);	/* deallocate result and curTuple */
	if (conn->lobjfuncs)
		free(conn->lobjfuncs);
	conn->lobjfuncs = NULL;
	conn->inStart = conn->inCursor = conn->inEnd = 0;
	conn->outCount = 0;
	conn->nonblocking = FALSE;

}

/*
   PQfinish:
	  properly close a connection to the backend
	  also frees the PGconn data structure so it shouldn't be re-used
	  after this
*/
void
PQfinish(PGconn *conn)
{
	if (conn)
	{
		closePGconn(conn);
		freePGconn(conn);
	}
}

/* PQreset :
   resets the connection to the backend
   closes the existing connection and makes a new one
*/
void
PQreset(PGconn *conn)
{
	if (conn)
	{
		closePGconn(conn);

		if (connectDBStart(conn))
			(void) connectDBComplete(conn);
	}
}


/* PQresetStart :
   resets the connection to the backend
   closes the existing connection and makes a new one
   Returns 1 on success, 0 on failure.
*/
int
PQresetStart(PGconn *conn)
{
	if (conn)
	{
		closePGconn(conn);

		return connectDBStart(conn);
	}

	return 0;
}


/* PQresetPoll :
   resets the connection to the backend
   closes the existing connection and makes a new one
*/

PostgresPollingStatusType
PQresetPoll(PGconn *conn)
{
	if (conn)
		return PQconnectPoll(conn);

	return PGRES_POLLING_FAILED;
}


/*
 * PQrequestCancel: attempt to request cancellation of the current operation.
 *
 * The return value is TRUE if the cancel request was successfully
 * dispatched, FALSE if not (in which case conn->errorMessage is set).
 * Note: successful dispatch is no guarantee that there will be any effect at
 * the backend.  The application must read the operation result as usual.
 *
 * XXX it was a bad idea to have the error message returned in
 * conn->errorMessage, since it could overwrite a message already there.
 * Would be better to return it in a char array passed by the caller.
 *
 * CAUTION: we want this routine to be safely callable from a signal handler
 * (for example, an application might want to call it in a SIGINT handler).
 * This means we cannot use any C library routine that might be non-reentrant.
 * malloc/free are often non-reentrant, and anything that might call them is
 * just as dangerous.  We avoid sprintf here for that reason.  Building up
 * error messages with strcpy/strcat is tedious but should be quite safe.
 * We also save/restore errno in case the signal handler support doesn't.
 *
 * NOTE: this routine must not generate any error message longer than
 * INITIAL_EXPBUFFER_SIZE (currently 256), since we dare not try to
 * expand conn->errorMessage!
 */

int
PQrequestCancel(PGconn *conn)
{
	int			save_errno = SOCK_ERRNO;
	int			tmpsock = -1;
	struct
	{
		uint32		packetlen;
		CancelRequestPacket cp;
	}			crp;

	/* Check we have an open connection */
	if (!conn)
		return FALSE;

	if (conn->sock < 0)
	{
		strcpy(conn->errorMessage.data,
			   "PQrequestCancel() -- connection is not open\n");
		conn->errorMessage.len = strlen(conn->errorMessage.data);
#ifdef WIN32
		WSASetLastError(save_errno);
#else
		errno = save_errno;
#endif
		return FALSE;
	}

	/*
	 * We need to open a temporary connection to the postmaster. Use the
	 * information saved by connectDB to do this with only kernel calls.
	 */
	if ((tmpsock = socket(conn->raddr.sa.sa_family, SOCK_STREAM, 0)) < 0)
	{
		strcpy(conn->errorMessage.data,
			   "PQrequestCancel() -- socket() failed: ");
		goto cancel_errReturn;
	}
retry3:
	if (connect(tmpsock, &conn->raddr.sa, conn->raddr_len) < 0)
	{
		if (SOCK_ERRNO == EINTR)
			/* Interrupted system call - we'll just try again */
			goto retry3;
		strcpy(conn->errorMessage.data,
			   "PQrequestCancel() -- connect() failed: ");
		goto cancel_errReturn;
	}

	/*
	 * We needn't set nonblocking I/O or NODELAY options here.
	 */

	/* Create and send the cancel request packet. */

	crp.packetlen = htonl((uint32) sizeof(crp));
	crp.cp.cancelRequestCode = (MsgType) htonl(CANCEL_REQUEST_CODE);
	crp.cp.backendPID = htonl(conn->be_pid);
	crp.cp.cancelAuthCode = htonl(conn->be_key);

retry4:
	if (send(tmpsock, (char *) &crp, sizeof(crp), 0) != (int) sizeof(crp))
	{
		if (SOCK_ERRNO == EINTR)
			/* Interrupted system call - we'll just try again */
			goto retry4;
		strcpy(conn->errorMessage.data,
			   "PQrequestCancel() -- send() failed: ");
		goto cancel_errReturn;
	}

	/* Sent it, done */
#ifdef WIN32
	closesocket(tmpsock);
	WSASetLastError(save_errno);
#else
	close(tmpsock);
	errno = save_errno;
#endif

	return TRUE;

cancel_errReturn:
	strcat(conn->errorMessage.data, SOCK_STRERROR(SOCK_ERRNO));
	strcat(conn->errorMessage.data, "\n");
	conn->errorMessage.len = strlen(conn->errorMessage.data);
	if (tmpsock >= 0)
	{
#ifdef WIN32
		closesocket(tmpsock);
		WSASetLastError(save_errno);
#else
		close(tmpsock);
		errno = save_errno;
#endif
	}
	return FALSE;
}


/*
 * pqPacketSend() -- send a single-packet message.
 * this is like PacketSend(), defined in backend/libpq/pqpacket.c
 *
 * RETURNS: STATUS_ERROR if the write fails, STATUS_OK otherwise.
 * SIDE_EFFECTS: may block.
*/
int
pqPacketSend(PGconn *conn, const char *buf, size_t len)
{
	/* Send the total packet size. */

	if (pqPutInt(4 + len, 4, conn))
		return STATUS_ERROR;

	/* Send the packet itself. */

	if (pqPutnchar(buf, len, conn))
		return STATUS_ERROR;

	if (pqFlush(conn))
		return STATUS_ERROR;

	return STATUS_OK;
}



#ifndef SYSCONFDIR
#error "You must compile this file with SYSCONFDIR defined."
#endif

#define MAXBUFSIZE 256

static int
parseServiceInfo(PQconninfoOption *options, PQExpBuffer errorMessage)
{
	char	   *service = conninfo_getval(options, "service");
	char	   *serviceFile = SYSCONFDIR "/pg_service.conf";
	int			group_found = 0;
	int			linenr = 0,
				i;

	if (service != NULL)
	{
		FILE	   *f;
		char		buf[MAXBUFSIZE],
				   *line;

		f = fopen(serviceFile, "r");
		if (f == NULL)
		{
			printfPQExpBuffer(errorMessage, "ERROR: Service file '%s' not found\n",
							  serviceFile);
			return 1;
		}

		while ((line = fgets(buf, MAXBUFSIZE - 1, f)) != NULL)
		{
			linenr++;

			if (strlen(line) >= MAXBUFSIZE - 2)
			{
				fclose(f);
				printfPQExpBuffer(errorMessage,
						"ERROR: line %d too long in service file '%s'\n",
								  linenr,
								  serviceFile);
				return 2;
			}

			/* ignore EOL at end of line */
			if (strlen(line) && line[strlen(line) - 1] == '\n')
				line[strlen(line) - 1] = 0;

			/* ignore leading blanks */
			while (*line && isspace((unsigned char) line[0]))
				line++;

			/* ignore comments and empty lines */
			if (strlen(line) == 0 || line[0] == '#')
				continue;

			/* Check for right groupname */
			if (line[0] == '[')
			{
				if (group_found)
				{
					/* group info already read */
					fclose(f);
					return 0;
				}

				if (strncmp(line + 1, service, strlen(service)) == 0 &&
					line[strlen(service) + 1] == ']')
					group_found = 1;
				else
					group_found = 0;
			}
			else
			{
				if (group_found)
				{
					/*
					 * Finally, we are in the right group and can parse
					 * the line
					 */
					char	   *key,
							   *val;
					int			found_keyword;

					key = strtok(line, "=");
					if (key == NULL)
					{
						printfPQExpBuffer(errorMessage,
										  "ERROR: syntax error in service file '%s', line %d\n",
										  serviceFile,
										  linenr);
						fclose(f);
						return 3;
					}

					/*
					 *	If not already set, set the database name to the
					 *	name of the service
					 */
					for (i = 0; options[i].keyword; i++)
						if (strcmp(options[i].keyword, "dbname") == 0)
							if (options[i].val == NULL)
								options[i].val = strdup(service);

					val = line + strlen(line) + 1;

					found_keyword = 0;
					for (i = 0; options[i].keyword; i++)
					{
						if (strcmp(options[i].keyword, key) == 0)
						{
							if (options[i].val != NULL)
								free(options[i].val);
							options[i].val = strdup(val);
							found_keyword = 1;
						}
					}

					if (!found_keyword)
					{
						printfPQExpBuffer(errorMessage,
										  "ERROR: syntax error in service file '%s', line %d\n",
										  serviceFile,
										  linenr);
						fclose(f);
						return 3;
					}
				}
			}
		}

		fclose(f);
	}

	return 0;
}


/*
 * Conninfo parser routine
 *
 * If successful, a malloc'd PQconninfoOption array is returned.
 * If not successful, NULL is returned and an error message is
 * left in errorMessage.
 */
static PQconninfoOption *
conninfo_parse(const char *conninfo, PQExpBuffer errorMessage)
{
	char	   *pname;
	char	   *pval;
	char	   *buf;
	char	   *tmp;
	char	   *cp;
	char	   *cp2;
	PQconninfoOption *options;
	PQconninfoOption *option;
	char		errortmp[INITIAL_EXPBUFFER_SIZE];

	/* Make a working copy of PQconninfoOptions */
	options = malloc(sizeof(PQconninfoOptions));
	if (options == NULL)
	{
		printfPQExpBuffer(errorMessage,
						  libpq_gettext("out of memory\n"));
		return NULL;
	}
	memcpy(options, PQconninfoOptions, sizeof(PQconninfoOptions));

	/* Need a modifiable copy of the input string */
	if ((buf = strdup(conninfo)) == NULL)
	{
		printfPQExpBuffer(errorMessage,
						  libpq_gettext("out of memory\n"));
		PQconninfoFree(options);
		return NULL;
	}
	cp = buf;

	while (*cp)
	{
		/* Skip blanks before the parameter name */
		if (isspace((unsigned char) *cp))
		{
			cp++;
			continue;
		}

		/* Get the parameter name */
		pname = cp;
		while (*cp)
		{
			if (*cp == '=')
				break;
			if (isspace((unsigned char) *cp))
			{
				*cp++ = '\0';
				while (*cp)
				{
					if (!isspace((unsigned char) *cp))
						break;
					cp++;
				}
				break;
			}
			cp++;
		}

		/* Check that there is a following '=' */
		if (*cp != '=')
		{
			printfPQExpBuffer(errorMessage,
							  libpq_gettext("missing \"=\" after \"%s\" in connection info string\n"),
							  pname);
			PQconninfoFree(options);
			free(buf);
			return NULL;
		}
		*cp++ = '\0';

		/* Skip blanks after the '=' */
		while (*cp)
		{
			if (!isspace((unsigned char) *cp))
				break;
			cp++;
		}

		/* Get the parameter value */
		pval = cp;

		if (*cp != '\'')
		{
			cp2 = pval;
			while (*cp)
			{
				if (isspace((unsigned char) *cp))
				{
					*cp++ = '\0';
					break;
				}
				if (*cp == '\\')
				{
					cp++;
					if (*cp != '\0')
						*cp2++ = *cp++;
				}
				else
					*cp2++ = *cp++;
			}
			*cp2 = '\0';
		}
		else
		{
			cp2 = pval;
			cp++;
			for (;;)
			{
				if (*cp == '\0')
				{
					printfPQExpBuffer(errorMessage,
									  libpq_gettext("unterminated quoted string in connection info string\n"));
					PQconninfoFree(options);
					free(buf);
					return NULL;
				}
				if (*cp == '\\')
				{
					cp++;
					if (*cp != '\0')
						*cp2++ = *cp++;
					continue;
				}
				if (*cp == '\'')
				{
					*cp2 = '\0';
					cp++;
					break;
				}
				*cp2++ = *cp++;
			}
		}

		/*
		 * Now we have the name and the value. Search for the param
		 * record.
		 */
		for (option = options; option->keyword != NULL; option++)
		{
			if (strcmp(option->keyword, pname) == 0)
				break;
		}
		if (option->keyword == NULL)
		{
			printfPQExpBuffer(errorMessage,
					 libpq_gettext("invalid connection option \"%s\"\n"),
							  pname);
			PQconninfoFree(options);
			free(buf);
			return NULL;
		}

		/*
		 * Store the value
		 */
		if (option->val)
			free(option->val);
		option->val = strdup(pval);

	}

	/* Now check for service info */
	if (parseServiceInfo(options, errorMessage))
	{
		PQconninfoFree(options);
		free(buf);
		return NULL;
	}

	/* Done with the modifiable input string */
	free(buf);

	/*
	 * Get the fallback resources for parameters not specified in the
	 * conninfo string.
	 */
	for (option = options; option->keyword != NULL; option++)
	{
		if (option->val != NULL)
			continue;			/* Value was in conninfo */

		/*
		 * Try to get the environment variable fallback
		 */
		if (option->envvar != NULL)
		{
			if ((tmp = getenv(option->envvar)) != NULL)
			{
				option->val = strdup(tmp);
				continue;
			}
		}

		/*
		 * No environment variable specified or this one isn't set - try
		 * compiled in
		 */
		if (option->compiled != NULL)
		{
			option->val = strdup(option->compiled);
			continue;
		}

		/*
		 * Special handling for user
		 */
		if (strcmp(option->keyword, "user") == 0)
		{
			option->val = fe_getauthname(errortmp);
			/* note any error message is thrown away */
			continue;
		}

		/*
		 * Special handling for dbname
		 */
		if (strcmp(option->keyword, "dbname") == 0)
		{
			tmp = conninfo_getval(options, "user");
			if (tmp)
				option->val = strdup(tmp);
			continue;
		}
	}

	return options;
}


static char *
conninfo_getval(PQconninfoOption *connOptions,
				const char *keyword)
{
	PQconninfoOption *option;

	for (option = connOptions; option->keyword != NULL; option++)
	{
		if (strcmp(option->keyword, keyword) == 0)
			return option->val;
	}

	return NULL;
}


void
PQconninfoFree(PQconninfoOption *connOptions)
{
	PQconninfoOption *option;

	if (connOptions == NULL)
		return;

	for (option = connOptions; option->keyword != NULL; option++)
	{
		if (option->val != NULL)
			free(option->val);
	}
	free(connOptions);
}


/* =========== accessor functions for PGconn ========= */
char *
PQdb(const PGconn *conn)
{
	if (!conn)
		return (char *) NULL;
	return conn->dbName;
}

char *
PQuser(const PGconn *conn)
{
	if (!conn)
		return (char *) NULL;
	return conn->pguser;
}

char *
PQpass(const PGconn *conn)
{
	if (!conn)
		return (char *) NULL;
	return conn->pgpass;
}

char *
PQhost(const PGconn *conn)
{
	if (!conn)
		return (char *) NULL;
	return conn->pghost ? conn->pghost : conn->pgunixsocket;
}

char *
PQport(const PGconn *conn)
{
	if (!conn)
		return (char *) NULL;
	return conn->pgport;
}

char *
PQtty(const PGconn *conn)
{
	if (!conn)
		return (char *) NULL;
	return conn->pgtty;
}

char *
PQoptions(const PGconn *conn)
{
	if (!conn)
		return (char *) NULL;
	return conn->pgoptions;
}

ConnStatusType
PQstatus(const PGconn *conn)
{
	if (!conn)
		return CONNECTION_BAD;
	return conn->status;
}

char *
PQerrorMessage(const PGconn *conn)
{
	if (!conn)
		return libpq_gettext("connection pointer is NULL\n");

	return conn->errorMessage.data;
}

int
PQsocket(const PGconn *conn)
{
	if (!conn)
		return -1;
	return conn->sock;
}

int
PQbackendPID(const PGconn *conn)
{
	if (!conn || conn->status != CONNECTION_OK)
		return 0;
	return conn->be_pid;
}

int
PQclientEncoding(const PGconn *conn)
{
	if (!conn || conn->status != CONNECTION_OK)
		return -1;
	return conn->client_encoding;
}

int
PQsetClientEncoding(PGconn *conn, const char *encoding)
{
	char		qbuf[128];
	static char query[] = "set client_encoding to '%s'";
	PGresult   *res;
	int			status;

	if (!conn || conn->status != CONNECTION_OK)
		return -1;

	if (!encoding)
		return -1;

	/* check query buffer overflow */
	if (sizeof(qbuf) < (sizeof(query) + strlen(encoding)))
		return -1;

	/* ok, now send a query */
	sprintf(qbuf, query, encoding);
	res = PQexec(conn, qbuf);

	if (res == (PGresult *) NULL)
		return -1;
	if (res->resultStatus != PGRES_COMMAND_OK)
		status = -1;
	else
	{
		/* change libpq internal encoding */
		conn->client_encoding = pg_char_to_encoding(encoding);
		status = 0;				/* everything is ok */
	}
	PQclear(res);
	return (status);
}

void
PQtrace(PGconn *conn, FILE *debug_port)
{
	if (conn == NULL ||
		conn->status == CONNECTION_BAD)
		return;
	PQuntrace(conn);
	conn->Pfdebug = debug_port;
}

void
PQuntrace(PGconn *conn)
{
	/* note: better allow untrace even when connection bad */
	if (conn == NULL)
		return;
	if (conn->Pfdebug)
	{
		fflush(conn->Pfdebug);
		conn->Pfdebug = NULL;
	}
}

PQnoticeProcessor
PQsetNoticeProcessor(PGconn *conn, PQnoticeProcessor proc, void *arg)
{
	PQnoticeProcessor old;

	if (conn == NULL)
		return NULL;

	old = conn->noticeHook;
	if (proc)
	{
		conn->noticeHook = proc;
		conn->noticeArg = arg;
	}
	return old;
}

/*
 * The default notice/error message processor just prints the
 * message on stderr.  Applications can override this if they
 * want the messages to go elsewhere (a window, for example).
 * Note that simply discarding notices is probably a bad idea.
 */

static void
defaultNoticeProcessor(void *arg, const char *message)
{
	(void) arg;					/* not used */
	/* Note: we expect the supplied string to end with a newline already. */
	fprintf(stderr, "%s", message);
}

/* returns a pointer to the next token or NULL if the current
 * token doesn't match */
char *
pwdfMatchesString(char *buf, char *token)
{
	char	   *tbuf,
			   *ttok;
	bool		bslash = false;

	if (buf == NULL || token == NULL)
		return NULL;
	tbuf = buf;
	ttok = token;
	if (*tbuf == '*')
		return tbuf + 2;
	while (*tbuf != 0)
	{
		if (*tbuf == '\\' && !bslash)
		{
			tbuf++;
			bslash = true;
		}
		if (*tbuf == ':' && *ttok == 0 && !bslash)
			return tbuf + 1;
		bslash = false;
		if (*ttok == 0)
			return NULL;
		if (*tbuf == *ttok)
		{
			tbuf++;
			ttok++;
		}
		else
			return NULL;
	}
	return NULL;
}

/* Get a password from the password file. Return value is malloc'd. */
char *
PasswordFromFile(char *hostname, char *port, char *dbname, char *username)
{
	FILE	   *fp;
	char	   *pgpassfile;
	char	   *home;
	struct stat stat_buf;

#define LINELEN NAMEDATALEN*5
	char		buf[LINELEN];

	if (dbname == NULL || strlen(dbname) == 0)
		return NULL;

	if (username == NULL || strlen(username) == 0)
		return NULL;

	if (hostname == NULL)
		hostname = DefaultHost;

	if (port == NULL)
		port = DEF_PGPORT_STR;

	/* Look for it in the home dir */
	home = getenv("HOME");
	if (!home)
		return NULL;

	pgpassfile = malloc(strlen(home) + 1 + strlen(PGPASSFILE) + 1);
	if (!pgpassfile)
	{
		fprintf(stderr, libpq_gettext("out of memory\n"));
		return NULL;
	}

	sprintf(pgpassfile, "%s/%s", home, PGPASSFILE);

	/* If password file cannot be opened, ignore it. */
	if (stat(pgpassfile, &stat_buf) == -1)
	{
		free(pgpassfile);
		return NULL;
	}

#ifndef WIN32
	/* If password file is insecure, alert the user and ignore it. */
	if (stat_buf.st_mode & (S_IRWXG | S_IRWXO))
	{
		fprintf(stderr,
				libpq_gettext("WARNING: Password file %s has world or group read access; permission should be u=rw (0600)\n"),
				pgpassfile);
		free(pgpassfile);
		return NULL;
	}
#endif

	fp = fopen(pgpassfile, "r");
	free(pgpassfile);
	if (fp == NULL)
		return NULL;

	while (!feof(fp))
	{
		char	   *t = buf,
				   *ret;
		int			len;

		fgets(buf, LINELEN - 1, fp);

		len = strlen(buf);
		if (len == 0)
			continue;

		/* Remove trailing newline */
		if (buf[len - 1] == '\n')
			buf[len - 1] = 0;

		if ((t = pwdfMatchesString(t, hostname)) == NULL ||
			(t = pwdfMatchesString(t, port)) == NULL ||
			(t = pwdfMatchesString(t, dbname)) == NULL ||
			(t = pwdfMatchesString(t, username)) == NULL)
			continue;
		ret = strdup(t);
		fclose(fp);
		return ret;
	}

	fclose(fp);
	return NULL;

#undef LINELEN
}
