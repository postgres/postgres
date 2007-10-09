/*-------------------------------------------------------------------------
 *
 * fe-connect.c
 *	  functions related to setting up a connection to the backend
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/interfaces/libpq/fe-connect.c,v 1.339.2.3 2007/10/09 15:03:31 mha Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>

#include "libpq-fe.h"
#include "libpq-int.h"
#include "fe-auth.h"
#include "pg_config_paths.h"

#ifdef WIN32
#include "win32.h"
#ifdef _WIN32_IE
#undef _WIN32_IE
#endif
#define _WIN32_IE 0x0500
#ifdef near
#undef near
#endif
#define near
#include <shlobj.h>
#else
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#ifdef HAVE_NETINET_TCP_H
#include <netinet/tcp.h>
#endif
#include <arpa/inet.h>
#endif

#ifdef ENABLE_THREAD_SAFETY
#ifdef WIN32
#include "pthread-win32.h"
#else
#include <pthread.h>
#endif
#endif

#ifdef USE_LDAP
#ifdef WIN32
#include <winldap.h>
#else
/* OpenLDAP deprecates RFC 1823, but we want standard conformance */
#define LDAP_DEPRECATED 1
#include <ldap.h>
typedef struct timeval LDAP_TIMEVAL;
#endif
static int ldapServiceLookup(const char *purl, PQconninfoOption *options,
				  PQExpBuffer errorMessage);
#endif

#include "libpq/ip.h"
#include "mb/pg_wchar.h"

#ifndef FD_CLOEXEC
#define FD_CLOEXEC 1
#endif


#ifndef WIN32
#define PGPASSFILE ".pgpass"
#else
#define PGPASSFILE "pgpass.conf"
#endif

/* fall back options if they are not specified by arguments or defined
   by environment variables */
#define DefaultHost		"localhost"
#define DefaultTty		""
#define DefaultOption	""
#define DefaultAuthtype		  ""
#define DefaultPassword		  ""
#ifdef USE_SSL
#define DefaultSSLMode	"prefer"
#else
#define DefaultSSLMode	"disable"
#endif

/* ----------
 * Definition of the conninfo parameters and their fallback resources.
 *
 * If Environment-Var and Compiled-in are specified as NULL, no
 * fallback is available. If after all no value can be determined
 * for an option, an error is returned.
 *
 * The value for the username is treated specially in conninfo_parse.
 * If the Compiled-in resource is specified as a NULL value, the
 * user is determined by pg_fe_getauthname().
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
	 * "authtype" is no longer used, so mark it "don't show".  We keep it in
	 * the array so as not to reject conninfo strings from old apps that might
	 * still try to set it.
	 */
	{"authtype", "PGAUTHTYPE", DefaultAuthtype, NULL,
	"Database-Authtype", "D", 20},

	{"service", "PGSERVICE", NULL, NULL,
	"Database-Service", "", 20},

	{"user", "PGUSER", NULL, NULL,
	"Database-User", "", 20},

	{"password", "PGPASSWORD", NULL, NULL,
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

	/*
	 * "tty" is no longer used either, but keep it present for backwards
	 * compatibility.
	 */
	{"tty", "PGTTY", DefaultTty, NULL,
	"Backend-Debug-TTY", "D", 40},

	{"options", "PGOPTIONS", DefaultOption, NULL,
	"Backend-Debug-Options", "D", 40},

#ifdef USE_SSL

	/*
	 * "requiressl" is deprecated, its purpose having been taken over by
	 * "sslmode". It remains for backwards compatibility.
	 */
	{"requiressl", "PGREQUIRESSL", "0", NULL,
	"Require-SSL", "D", 1},
#endif

	/*
	 * "sslmode" option is allowed even without client SSL support because the
	 * client can still handle SSL modes "disable" and "allow".
	 */
	{"sslmode", "PGSSLMODE", DefaultSSLMode, NULL,
	"SSL-Mode", "", 8},			/* sizeof("disable") == 8 */

#ifdef KRB5
	/* Kerberos authentication supports specifying the service name */
	{"krbsrvname", "PGKRBSRVNAME", PG_KRB_SRVNAM, NULL,
	"Kerberos-service-name", "", 20},
#endif

	/* Terminating entry --- MUST BE LAST */
	{NULL, NULL, NULL, NULL,
	NULL, NULL, 0}
};

static const PQEnvironmentOption EnvironmentOptions[] =
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


static bool connectOptions1(PGconn *conn, const char *conninfo);
static bool connectOptions2(PGconn *conn);
static int	connectDBStart(PGconn *conn);
static int	connectDBComplete(PGconn *conn);
static PGconn *makeEmptyPGconn(void);
static void freePGconn(PGconn *conn);
static void closePGconn(PGconn *conn);
static PQconninfoOption *conninfo_parse(const char *conninfo,
			   PQExpBuffer errorMessage);
static char *conninfo_getval(PQconninfoOption *connOptions,
				const char *keyword);
static void defaultNoticeReceiver(void *arg, const PGresult *res);
static void defaultNoticeProcessor(void *arg, const char *message);
static int parseServiceInfo(PQconninfoOption *options,
				 PQExpBuffer errorMessage);
static char *pwdfMatchesString(char *buf, char *token);
static char *PasswordFromFile(char *hostname, char *port, char *dbname,
				 char *username);
static void default_threadlock(int acquire);


/* global variable because fe-auth.c needs to access it */
pgthreadlock_t pg_g_threadlock = default_threadlock;


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
 * To connect in an asynchronous (non-blocking) manner, use the functions
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

	/*
	 * Allocate memory for the conn structure
	 */
	conn = makeEmptyPGconn();
	if (conn == NULL)
		return NULL;

	/*
	 * Parse the conninfo string
	 */
	if (!connectOptions1(conn, conninfo))
		return conn;

	/*
	 * Compute derived options
	 */
	if (!connectOptions2(conn))
		return conn;

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
 *		connectOptions1
 *
 * Internal subroutine to set up connection parameters given an already-
 * created PGconn and a conninfo string.  Derived settings should be
 * processed by calling connectOptions2 next.  (We split them because
 * PQsetdbLogin overrides defaults in between.)
 *
 * Returns true if OK, false if trouble (in which case errorMessage is set
 * and so is conn->status).
 */
static bool
connectOptions1(PGconn *conn, const char *conninfo)
{
	PQconninfoOption *connOptions;
	char	   *tmp;

	/*
	 * Parse the conninfo string
	 */
	connOptions = conninfo_parse(conninfo, &conn->errorMessage);
	if (connOptions == NULL)
	{
		conn->status = CONNECTION_BAD;
		/* errorMessage is already set */
		return false;
	}

	/*
	 * Move option values into conn structure
	 *
	 * Don't put anything cute here --- intelligence should be in
	 * connectOptions2 ...
	 *
	 * XXX: probably worth checking strdup() return value here...
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
	tmp = conninfo_getval(connOptions, "connect_timeout");
	conn->connect_timeout = tmp ? strdup(tmp) : NULL;
	tmp = conninfo_getval(connOptions, "sslmode");
	conn->sslmode = tmp ? strdup(tmp) : NULL;
#ifdef USE_SSL
	tmp = conninfo_getval(connOptions, "requiressl");
	if (tmp && tmp[0] == '1')
	{
		/* here warn that the requiressl option is deprecated? */
		if (conn->sslmode)
			free(conn->sslmode);
		conn->sslmode = strdup("require");
	}
#endif
#ifdef KRB5
	tmp = conninfo_getval(connOptions, "krbsrvname");
	conn->krbsrvname = tmp ? strdup(tmp) : NULL;
#endif

	/*
	 * Free the option info - all is in conn now
	 */
	PQconninfoFree(connOptions);

	return true;
}

/*
 *		connectOptions2
 *
 * Compute derived connection options after absorbing all user-supplied info.
 *
 * Returns true if OK, false if trouble (in which case errorMessage is set
 * and so is conn->status).
 */
static bool
connectOptions2(PGconn *conn)
{
	/*
	 * If database name was not given, default it to equal user name
	 */
	if ((conn->dbName == NULL || conn->dbName[0] == '\0')
		&& conn->pguser != NULL)
	{
		if (conn->dbName)
			free(conn->dbName);
		conn->dbName = strdup(conn->pguser);
	}

	/*
	 * Supply default password if none given
	 */
	if (conn->pgpass == NULL || conn->pgpass[0] == '\0')
	{
		if (conn->pgpass)
			free(conn->pgpass);
		conn->pgpass = PasswordFromFile(conn->pghost, conn->pgport,
										conn->dbName, conn->pguser);
		if (conn->pgpass == NULL)
			conn->pgpass = strdup(DefaultPassword);
	}

	/*
	 * Allow unix socket specification in the host name
	 */
	if (conn->pghost && is_absolute_path(conn->pghost))
	{
		if (conn->pgunixsocket)
			free(conn->pgunixsocket);
		conn->pgunixsocket = conn->pghost;
		conn->pghost = NULL;
	}

	/*
	 * validate sslmode option
	 */
	if (conn->sslmode)
	{
		if (strcmp(conn->sslmode, "disable") != 0
			&& strcmp(conn->sslmode, "allow") != 0
			&& strcmp(conn->sslmode, "prefer") != 0
			&& strcmp(conn->sslmode, "require") != 0)
		{
			conn->status = CONNECTION_BAD;
			printfPQExpBuffer(&conn->errorMessage,
							libpq_gettext("invalid sslmode value: \"%s\"\n"),
							  conn->sslmode);
			return false;
		}

#ifndef USE_SSL
		switch (conn->sslmode[0])
		{
			case 'a':			/* "allow" */
			case 'p':			/* "prefer" */

				/*
				 * warn user that an SSL connection will never be negotiated
				 * since SSL was not compiled in?
				 */
				break;

			case 'r':			/* "require" */
				conn->status = CONNECTION_BAD;
				printfPQExpBuffer(&conn->errorMessage,
								  libpq_gettext("sslmode value \"%s\" invalid when SSL support is not compiled in\n"),
								  conn->sslmode);
				return false;
		}
#endif
	}
	else
		conn->sslmode = strdup(DefaultSSLMode);

	/*
	 * Only if we get this far is it appropriate to try to connect. (We need a
	 * state flag, rather than just the boolean result of this function, in
	 * case someone tries to PQreset() the PGconn.)
	 */
	conn->options_valid = true;

	return true;
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
 *
 * if the status field of the connection returned is CONNECTION_BAD,
 * then only the errorMessage is likely to be useful.
 * ----------------
 */
PGconn *
PQsetdbLogin(const char *pghost, const char *pgport, const char *pgoptions,
			 const char *pgtty, const char *dbName, const char *login,
			 const char *pwd)
{
	PGconn	   *conn;

	/*
	 * Allocate memory for the conn structure
	 */
	conn = makeEmptyPGconn();
	if (conn == NULL)
		return NULL;

	/*
	 * Parse an empty conninfo string in order to set up the same defaults
	 * that PQconnectdb() would use.
	 */
	if (!connectOptions1(conn, ""))
		return conn;

	/*
	 * Absorb specified options into conn structure, overriding defaults
	 */
	if (pghost && pghost[0] != '\0')
	{
		if (conn->pghost)
			free(conn->pghost);
		conn->pghost = strdup(pghost);
	}

	if (pgport && pgport[0] != '\0')
	{
		if (conn->pgport)
			free(conn->pgport);
		conn->pgport = strdup(pgport);
	}

	if (pgoptions && pgoptions[0] != '\0')
	{
		if (conn->pgoptions)
			free(conn->pgoptions);
		conn->pgoptions = strdup(pgoptions);
	}

	if (pgtty && pgtty[0] != '\0')
	{
		if (conn->pgtty)
			free(conn->pgtty);
		conn->pgtty = strdup(pgtty);
	}

	if (dbName && dbName[0] != '\0')
	{
		if (conn->dbName)
			free(conn->dbName);
		conn->dbName = strdup(dbName);
	}

	if (login && login[0] != '\0')
	{
		if (conn->pguser)
			free(conn->pguser);
		conn->pguser = strdup(login);
	}

	if (pwd && pwd[0] != '\0')
	{
		if (conn->pgpass)
			free(conn->pgpass);
		conn->pgpass = strdup(pwd);
	}

	/*
	 * Compute derived options
	 */
	if (!connectOptions2(conn))
		return conn;

	/*
	 * Connect to the database
	 */
	if (connectDBStart(conn))
		(void) connectDBComplete(conn);

	return conn;
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
#ifdef	TCP_NODELAY
	int			on = 1;

	if (setsockopt(conn->sock, IPPROTO_TCP, TCP_NODELAY,
				   (char *) &on,
				   sizeof(on)) < 0)
	{
		char		sebuf[256];

		printfPQExpBuffer(&conn->errorMessage,
			libpq_gettext("could not set socket to TCP no delay mode: %s\n"),
						  SOCK_STRERROR(SOCK_ERRNO, sebuf, sizeof(sebuf)));
		return 0;
	}
#endif

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
	char		sebuf[256];

#ifdef HAVE_UNIX_SOCKETS
	if (IS_AF_UNIX(conn->raddr.addr.ss_family))
	{
		char		service[NI_MAXHOST];

		pg_getnameinfo_all(&conn->raddr.addr, conn->raddr.salen,
						   NULL, 0,
						   service, sizeof(service),
						   NI_NUMERICSERV);
		printfPQExpBuffer(&conn->errorMessage,
						  libpq_gettext("could not connect to server: %s\n"
							"\tIs the server running locally and accepting\n"
							"\tconnections on Unix domain socket \"%s\"?\n"),
						  SOCK_STRERROR(errorno, sebuf, sizeof(sebuf)),
						  service);
	}
	else
#endif   /* HAVE_UNIX_SOCKETS */
	{
		printfPQExpBuffer(&conn->errorMessage,
						  libpq_gettext("could not connect to server: %s\n"
					 "\tIs the server running on host \"%s\" and accepting\n"
										"\tTCP/IP connections on port %s?\n"),
						  SOCK_STRERROR(errorno, sebuf, sizeof(sebuf)),
						  conn->pghostaddr
						  ? conn->pghostaddr
						  : (conn->pghost
							 ? conn->pghost
							 : "???"),
						  conn->pgport);
	}
}


/* ----------
 * connectDBStart -
 *		Begin the process of making a connection to the backend.
 *
 * Returns 1 if successful, 0 if not.
 * ----------
 */
static int
connectDBStart(PGconn *conn)
{
	int			portnum;
	char		portstr[128];
	struct addrinfo *addrs = NULL;
	struct addrinfo hint;
	const char *node;
	int			ret;

	if (!conn)
		return 0;

	if (!conn->options_valid)
		goto connect_errReturn;

	/* Ensure our buffers are empty */
	conn->inStart = conn->inCursor = conn->inEnd = 0;
	conn->outCount = 0;

	/*
	 * Determine the parameters to pass to pg_getaddrinfo_all.
	 */

	/* Initialize hint structure */
	MemSet(&hint, 0, sizeof(hint));
	hint.ai_socktype = SOCK_STREAM;
	hint.ai_family = AF_UNSPEC;

	/* Set up port number as a string */
	if (conn->pgport != NULL && conn->pgport[0] != '\0')
		portnum = atoi(conn->pgport);
	else
		portnum = DEF_PGPORT;
	snprintf(portstr, sizeof(portstr), "%d", portnum);

	if (conn->pghostaddr != NULL && conn->pghostaddr[0] != '\0')
	{
		/* Using pghostaddr avoids a hostname lookup */
		node = conn->pghostaddr;
		hint.ai_family = AF_UNSPEC;
		hint.ai_flags = AI_NUMERICHOST;
	}
	else if (conn->pghost != NULL && conn->pghost[0] != '\0')
	{
		/* Using pghost, so we have to look-up the hostname */
		node = conn->pghost;
		hint.ai_family = AF_UNSPEC;
	}
	else
	{
#ifdef HAVE_UNIX_SOCKETS
		/* pghostaddr and pghost are NULL, so use Unix domain socket */
		node = NULL;
		hint.ai_family = AF_UNIX;
		UNIXSOCK_PATH(portstr, portnum, conn->pgunixsocket);
#else
		/* Without Unix sockets, default to localhost instead */
		node = "localhost";
		hint.ai_family = AF_UNSPEC;
#endif   /* HAVE_UNIX_SOCKETS */
	}

	/* Use pg_getaddrinfo_all() to resolve the address */
	ret = pg_getaddrinfo_all(node, portstr, &hint, &addrs);
	if (ret || !addrs)
	{
		if (node)
			printfPQExpBuffer(&conn->errorMessage,
							  libpq_gettext("could not translate host name \"%s\" to address: %s\n"),
							  node, gai_strerror(ret));
		else
			printfPQExpBuffer(&conn->errorMessage,
							  libpq_gettext("could not translate Unix-domain socket path \"%s\" to address: %s\n"),
							  portstr, gai_strerror(ret));
		if (addrs)
			pg_freeaddrinfo_all(hint.ai_family, addrs);
		goto connect_errReturn;
	}

#ifdef USE_SSL
	/* setup values based on SSL mode */
	if (conn->sslmode[0] == 'd')	/* "disable" */
		conn->allow_ssl_try = false;
	else if (conn->sslmode[0] == 'a')	/* "allow" */
		conn->wait_ssl_try = true;
#endif

	/*
	 * Set up to try to connect, with protocol 3.0 as the first attempt.
	 */
	conn->addrlist = addrs;
	conn->addr_cur = addrs;
	conn->addrlist_family = hint.ai_family;
	conn->pversion = PG_PROTOCOL(3, 0);
	conn->status = CONNECTION_NEEDED;

	/*
	 * The code for processing CONNECTION_NEEDED state is in PQconnectPoll(),
	 * so that it can easily be re-executed if needed again during the
	 * asynchronous startup process.  However, we must run it once here,
	 * because callers expect a success return from this routine to mean that
	 * we are in PGRES_POLLING_WRITING connection state.
	 */
	if (PQconnectPoll(conn) == PGRES_POLLING_WRITING)
		return 1;

connect_errReturn:
	if (conn->sock >= 0)
	{
		pqsecure_close(conn);
		closesocket(conn->sock);
		conn->sock = -1;
	}
	conn->status = CONNECTION_BAD;
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
			 * Rounding could cause connection to fail; need at least 2 secs
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
		 * PQconnectStart) is to wait for the socket to select for writing.
		 */
		switch (flag)
		{
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
 *		supply a host name instead) then PQconnectStart will block on
 *		gethostbyname.	You will be fine if using Unix sockets (i.e. by
 *		supplying neither a host name nor a host address).
 *	 o	If your backend wants to use Kerberos authentication then you must
 *		supply both a host name and a host address, otherwise this function
 *		may block on gethostname.
 *
 * ----------------
 */
PostgresPollingStatusType
PQconnectPoll(PGconn *conn)
{
	PGresult   *res;
	char		sebuf[256];

	if (conn == NULL)
		return PGRES_POLLING_FAILED;

	/* Get the new data */
	switch (conn->status)
	{
			/*
			 * We really shouldn't have been polled in these two cases, but we
			 * can handle it.
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

			/* We allow pqSetenvPoll to decide whether to proceed. */
		case CONNECTION_SETENV:
			break;

			/* Special cases: proceed without waiting. */
		case CONNECTION_SSL_STARTUP:
		case CONNECTION_NEEDED:
			break;

		default:
			printfPQExpBuffer(&conn->errorMessage,
							  libpq_gettext(
											"invalid connection state, "
								 "probably indicative of memory corruption\n"
											));
			goto error_return;
	}


keep_going:						/* We will come back to here until there is
								 * nothing left to do. */
	switch (conn->status)
	{
		case CONNECTION_NEEDED:
			{
				/*
				 * Try to initiate a connection to one of the addresses
				 * returned by pg_getaddrinfo_all().  conn->addr_cur is the
				 * next one to try. We fail when we run out of addresses
				 * (reporting the error returned for the *last* alternative,
				 * which may not be what users expect :-().
				 */
				while (conn->addr_cur != NULL)
				{
					struct addrinfo *addr_cur = conn->addr_cur;

					/* Remember current address for possible error msg */
					memcpy(&conn->raddr.addr, addr_cur->ai_addr,
						   addr_cur->ai_addrlen);
					conn->raddr.salen = addr_cur->ai_addrlen;

					/* Open a socket */
					conn->sock = socket(addr_cur->ai_family, SOCK_STREAM, 0);
					if (conn->sock < 0)
					{
						/*
						 * ignore socket() failure if we have more addresses
						 * to try
						 */
						if (addr_cur->ai_next != NULL)
						{
							conn->addr_cur = addr_cur->ai_next;
							continue;
						}
						printfPQExpBuffer(&conn->errorMessage,
							  libpq_gettext("could not create socket: %s\n"),
							SOCK_STRERROR(SOCK_ERRNO, sebuf, sizeof(sebuf)));
						break;
					}

					/*
					 * Select socket options: no delay of outgoing data for
					 * TCP sockets, nonblock mode, close-on-exec. Fail if any
					 * of this fails.
					 */
					if (!IS_AF_UNIX(addr_cur->ai_family))
					{
						if (!connectNoDelay(conn))
						{
							closesocket(conn->sock);
							conn->sock = -1;
							conn->addr_cur = addr_cur->ai_next;
							continue;
						}
					}
					if (!pg_set_noblock(conn->sock))
					{
						printfPQExpBuffer(&conn->errorMessage,
										  libpq_gettext("could not set socket to non-blocking mode: %s\n"),
							SOCK_STRERROR(SOCK_ERRNO, sebuf, sizeof(sebuf)));
						closesocket(conn->sock);
						conn->sock = -1;
						conn->addr_cur = addr_cur->ai_next;
						continue;
					}

#ifdef F_SETFD
					if (fcntl(conn->sock, F_SETFD, FD_CLOEXEC) == -1)
					{
						printfPQExpBuffer(&conn->errorMessage,
										  libpq_gettext("could not set socket to close-on-exec mode: %s\n"),
							SOCK_STRERROR(SOCK_ERRNO, sebuf, sizeof(sebuf)));
						closesocket(conn->sock);
						conn->sock = -1;
						conn->addr_cur = addr_cur->ai_next;
						continue;
					}
#endif   /* F_SETFD */

					/*
					 * Start/make connection.  This should not block, since we
					 * are in nonblock mode.  If it does, well, too bad.
					 */
					if (connect(conn->sock, addr_cur->ai_addr,
								addr_cur->ai_addrlen) < 0)
					{
						if (SOCK_ERRNO == EINPROGRESS ||
							SOCK_ERRNO == EWOULDBLOCK ||
							SOCK_ERRNO == EINTR ||
							SOCK_ERRNO == 0)
						{
							/*
							 * This is fine - we're in non-blocking mode, and
							 * the connection is in progress.  Tell caller to
							 * wait for write-ready on socket.
							 */
							conn->status = CONNECTION_STARTED;
							return PGRES_POLLING_WRITING;
						}
						/* otherwise, trouble */
					}
					else
					{
						/*
						 * Hm, we're connected already --- seems the "nonblock
						 * connection" wasn't.  Advance the state machine and
						 * go do the next stuff.
						 */
						conn->status = CONNECTION_STARTED;
						goto keep_going;
					}

					/*
					 * This connection failed --- set up error report, then
					 * close socket (do it this way in case close() affects
					 * the value of errno...).	We will ignore the connect()
					 * failure and keep going if there are more addresses.
					 */
					connectFailureMessage(conn, SOCK_ERRNO);
					if (conn->sock >= 0)
					{
						closesocket(conn->sock);
						conn->sock = -1;
					}

					/*
					 * Try the next address, if any.
					 */
					conn->addr_cur = addr_cur->ai_next;
				}				/* loop over addresses */

				/*
				 * Ooops, no more addresses.  An appropriate error message is
				 * already set up, so just set the right status.
				 */
				goto error_return;
			}

		case CONNECTION_STARTED:
			{
				int			optval;
				ACCEPT_TYPE_ARG3 optlen = sizeof(optval);

				/*
				 * Write ready, since we've made it here, so the connection
				 * has been made ... or has failed.
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
							SOCK_STRERROR(SOCK_ERRNO, sebuf, sizeof(sebuf)));
					goto error_return;
				}
				else if (optval != 0)
				{
					/*
					 * When using a nonblocking connect, we will typically see
					 * connect failures at this point, so provide a friendly
					 * error message.
					 */
					connectFailureMessage(conn, optval);

					/*
					 * If more addresses remain, keep trying, just as in the
					 * case where connect() returned failure immediately.
					 */
					if (conn->addr_cur->ai_next != NULL)
					{
						if (conn->sock >= 0)
						{
							closesocket(conn->sock);
							conn->sock = -1;
						}
						conn->addr_cur = conn->addr_cur->ai_next;
						conn->status = CONNECTION_NEEDED;
						goto keep_going;
					}
					goto error_return;
				}

				/* Fill in the client address */
				conn->laddr.salen = sizeof(conn->laddr.addr);
				if (getsockname(conn->sock,
								(struct sockaddr *) & conn->laddr.addr,
								&conn->laddr.salen) < 0)
				{
					printfPQExpBuffer(&conn->errorMessage,
									  libpq_gettext("could not get client address from socket: %s\n"),
							SOCK_STRERROR(SOCK_ERRNO, sebuf, sizeof(sebuf)));
					goto error_return;
				}

				/*
				 * Make sure we can write before advancing to next step.
				 */
				conn->status = CONNECTION_MADE;
				return PGRES_POLLING_WRITING;
			}

		case CONNECTION_MADE:
			{
				char	   *startpacket;
				int			packetlen;

#ifdef USE_SSL

				/*
				 * If SSL is enabled and we haven't already got it running,
				 * request it instead of sending the startup message.
				 */
				if (IS_AF_UNIX(conn->raddr.addr.ss_family))
				{
					/* Don't bother requesting SSL over a Unix socket */
					conn->allow_ssl_try = false;
				}
				if (conn->allow_ssl_try && !conn->wait_ssl_try &&
					conn->ssl == NULL)
				{
					ProtocolVersion pv;

					/*
					 * Send the SSL request packet.
					 *
					 * Theoretically, this could block, but it really
					 * shouldn't since we only got here if the socket is
					 * write-ready.
					 */
					pv = htonl(NEGOTIATE_SSL_CODE);
					if (pqPacketSend(conn, 0, &pv, sizeof(pv)) != STATUS_OK)
					{
						printfPQExpBuffer(&conn->errorMessage,
										  libpq_gettext("could not send SSL negotiation packet: %s\n"),
							SOCK_STRERROR(SOCK_ERRNO, sebuf, sizeof(sebuf)));
						goto error_return;
					}
					/* Ok, wait for response */
					conn->status = CONNECTION_SSL_STARTUP;
					return PGRES_POLLING_READING;
				}
#endif   /* USE_SSL */

				/*
				 * Build the startup packet.
				 */
				if (PG_PROTOCOL_MAJOR(conn->pversion) >= 3)
					startpacket = pqBuildStartupPacket3(conn, &packetlen,
														EnvironmentOptions);
				else
					startpacket = pqBuildStartupPacket2(conn, &packetlen,
														EnvironmentOptions);
				if (!startpacket)
				{
					printfPQExpBuffer(&conn->errorMessage,
									  libpq_gettext("out of memory\n"));
					goto error_return;
				}

				/*
				 * Send the startup packet.
				 *
				 * Theoretically, this could block, but it really shouldn't
				 * since we only got here if the socket is write-ready.
				 */
				if (pqPacketSend(conn, 0, startpacket, packetlen) != STATUS_OK)
				{
					printfPQExpBuffer(&conn->errorMessage,
						libpq_gettext("could not send startup packet: %s\n"),
							SOCK_STRERROR(SOCK_ERRNO, sebuf, sizeof(sebuf)));
					free(startpacket);
					goto error_return;
				}

				free(startpacket);

				conn->status = CONNECTION_AWAITING_RESPONSE;
				return PGRES_POLLING_READING;
			}

			/*
			 * Handle SSL negotiation: wait for postmaster messages and
			 * respond as necessary.
			 */
		case CONNECTION_SSL_STARTUP:
			{
#ifdef USE_SSL
				PostgresPollingStatusType pollres;

				/*
				 * On first time through, get the postmaster's response to our
				 * SSL negotiation packet.
				 */
				if (conn->ssl == NULL)
				{
					/*
					 * We use pqReadData here since it has the logic to
					 * distinguish no-data-yet from connection closure. Since
					 * conn->ssl isn't set, a plain recv() will occur.
					 */
					char		SSLok;
					int			rdresult;

					rdresult = pqReadData(conn);
					if (rdresult < 0)
					{
						/* errorMessage is already filled in */
						goto error_return;
					}
					if (rdresult == 0)
					{
						/* caller failed to wait for data */
						return PGRES_POLLING_READING;
					}
					if (pqGetc(&SSLok, conn) < 0)
					{
						/* should not happen really */
						return PGRES_POLLING_READING;
					}
					/* mark byte consumed */
					conn->inStart = conn->inCursor;
					if (SSLok == 'S')
					{
						/* Do one-time setup; this creates conn->ssl */
						if (pqsecure_initialize(conn) == -1)
							goto error_return;
					}
					else if (SSLok == 'N')
					{
						if (conn->sslmode[0] == 'r')	/* "require" */
						{
							/* Require SSL, but server does not want it */
							printfPQExpBuffer(&conn->errorMessage,
											  libpq_gettext("server does not support SSL, but SSL was required\n"));
							goto error_return;
						}
						/* Otherwise, proceed with normal startup */
						conn->allow_ssl_try = false;
						conn->status = CONNECTION_MADE;
						return PGRES_POLLING_WRITING;
					}
					else if (SSLok == 'E')
					{
						/* Received error - probably protocol mismatch */
						if (conn->Pfdebug)
							fprintf(conn->Pfdebug, "received error from server, attempting fallback to pre-7.0\n");
						if (conn->sslmode[0] == 'r')	/* "require" */
						{
							/* Require SSL, but server is too old */
							printfPQExpBuffer(&conn->errorMessage,
											  libpq_gettext("server does not support SSL, but SSL was required\n"));
							goto error_return;
						}
						/* Otherwise, try again without SSL */
						conn->allow_ssl_try = false;
						/* Assume it ain't gonna handle protocol 3, either */
						conn->pversion = PG_PROTOCOL(2, 0);
						/* Must drop the old connection */
						closesocket(conn->sock);
						conn->sock = -1;
						conn->status = CONNECTION_NEEDED;
						goto keep_going;
					}
					else
					{
						printfPQExpBuffer(&conn->errorMessage,
										  libpq_gettext("received invalid response to SSL negotiation: %c\n"),
										  SSLok);
						goto error_return;
					}
				}

				/*
				 * Begin or continue the SSL negotiation process.
				 */
				pollres = pqsecure_open_client(conn);
				if (pollres == PGRES_POLLING_OK)
				{
					/* SSL handshake done, ready to send startup packet */
					conn->status = CONNECTION_MADE;
					return PGRES_POLLING_WRITING;
				}
				if (pollres == PGRES_POLLING_FAILED)
				{
					/*
					 * Failed ... if sslmode is "prefer" then do a non-SSL
					 * retry
					 */
					if (conn->sslmode[0] == 'p' /* "prefer" */
						&& conn->allow_ssl_try	/* redundant? */
						&& !conn->wait_ssl_try) /* redundant? */
					{
						/* only retry once */
						conn->allow_ssl_try = false;
						/* Must drop the old connection */
						closesocket(conn->sock);
						conn->sock = -1;
						conn->status = CONNECTION_NEEDED;
						goto keep_going;
					}
				}
				return pollres;
#else							/* !USE_SSL */
				/* can't get here */
				goto error_return;
#endif   /* USE_SSL */
			}

			/*
			 * Handle authentication exchange: wait for postmaster messages
			 * and respond as necessary.
			 */
		case CONNECTION_AWAITING_RESPONSE:
			{
				char		beresp;
				int			msgLength;
				int			avail;
				AuthRequest areq;

				/*
				 * Scan the message from current point (note that if we find
				 * the message is incomplete, we will return without advancing
				 * inStart, and resume here next time).
				 */
				conn->inCursor = conn->inStart;

				/* Read type byte */
				if (pqGetc(&beresp, conn))
				{
					/* We'll come back when there is more data */
					return PGRES_POLLING_READING;
				}

				/*
				 * Validate message type: we expect only an authentication
				 * request or an error here.  Anything else probably means
				 * it's not Postgres on the other end at all.
				 */
				if (!(beresp == 'R' || beresp == 'E'))
				{
					printfPQExpBuffer(&conn->errorMessage,
									  libpq_gettext(
									  "expected authentication request from "
												"server, but received %c\n"),
									  beresp);
					goto error_return;
				}

				if (PG_PROTOCOL_MAJOR(conn->pversion) >= 3)
				{
					/* Read message length word */
					if (pqGetInt(&msgLength, 4, conn))
					{
						/* We'll come back when there is more data */
						return PGRES_POLLING_READING;
					}
				}
				else
				{
					/* Set phony message length to disable checks below */
					msgLength = 8;
				}

				/*
				 * Try to validate message length before using it.
				 * Authentication requests can't be very large.  Errors can be
				 * a little larger, but not huge.  If we see a large apparent
				 * length in an error, it means we're really talking to a
				 * pre-3.0-protocol server; cope.
				 */
				if (beresp == 'R' && (msgLength < 8 || msgLength > 100))
				{
					printfPQExpBuffer(&conn->errorMessage,
									  libpq_gettext(
									  "expected authentication request from "
												"server, but received %c\n"),
									  beresp);
					goto error_return;
				}

				if (beresp == 'E' && (msgLength < 8 || msgLength > 30000))
				{
					/* Handle error from a pre-3.0 server */
					conn->inCursor = conn->inStart + 1; /* reread data */
					if (pqGets(&conn->errorMessage, conn))
					{
						/* We'll come back when there is more data */
						return PGRES_POLLING_READING;
					}
					/* OK, we read the message; mark data consumed */
					conn->inStart = conn->inCursor;

					/*
					 * The postmaster typically won't end its message with a
					 * newline, so add one to conform to libpq conventions.
					 */
					appendPQExpBufferChar(&conn->errorMessage, '\n');

					/*
					 * If we tried to open the connection in 3.0 protocol,
					 * fall back to 2.0 protocol.
					 */
					if (PG_PROTOCOL_MAJOR(conn->pversion) >= 3)
					{
						conn->pversion = PG_PROTOCOL(2, 0);
						/* Must drop the old connection */
						pqsecure_close(conn);
						closesocket(conn->sock);
						conn->sock = -1;
						conn->status = CONNECTION_NEEDED;
						goto keep_going;
					}

					goto error_return;
				}

				/*
				 * Can't process if message body isn't all here yet.
				 *
				 * (In protocol 2.0 case, we are assuming messages carry at
				 * least 4 bytes of data.)
				 */
				msgLength -= 4;
				avail = conn->inEnd - conn->inCursor;
				if (avail < msgLength)
				{
					/*
					 * Before returning, try to enlarge the input buffer if
					 * needed to hold the whole message; see notes in
					 * pqParseInput3.
					 */
					if (pqCheckInBufferSpace(conn->inCursor + msgLength, conn))
						goto error_return;
					/* We'll come back when there is more data */
					return PGRES_POLLING_READING;
				}

				/* Handle errors. */
				if (beresp == 'E')
				{
					if (PG_PROTOCOL_MAJOR(conn->pversion) >= 3)
					{
						if (pqGetErrorNotice3(conn, true))
						{
							/* We'll come back when there is more data */
							return PGRES_POLLING_READING;
						}
					}
					else
					{
						if (pqGets(&conn->errorMessage, conn))
						{
							/* We'll come back when there is more data */
							return PGRES_POLLING_READING;
						}
					}
					/* OK, we read the message; mark data consumed */
					conn->inStart = conn->inCursor;

#ifdef USE_SSL

					/*
					 * if sslmode is "allow" and we haven't tried an SSL
					 * connection already, then retry with an SSL connection
					 */
					if (conn->sslmode[0] == 'a' /* "allow" */
						&& conn->ssl == NULL
						&& conn->allow_ssl_try
						&& conn->wait_ssl_try)
					{
						/* only retry once */
						conn->wait_ssl_try = false;
						/* Must drop the old connection */
						closesocket(conn->sock);
						conn->sock = -1;
						conn->status = CONNECTION_NEEDED;
						goto keep_going;
					}

					/*
					 * if sslmode is "prefer" and we're in an SSL connection,
					 * then do a non-SSL retry
					 */
					if (conn->sslmode[0] == 'p' /* "prefer" */
						&& conn->ssl
						&& conn->allow_ssl_try	/* redundant? */
						&& !conn->wait_ssl_try) /* redundant? */
					{
						/* only retry once */
						conn->allow_ssl_try = false;
						/* Must drop the old connection */
						pqsecure_close(conn);
						closesocket(conn->sock);
						conn->sock = -1;
						conn->status = CONNECTION_NEEDED;
						goto keep_going;
					}
#endif

					goto error_return;
				}

				/* It is an authentication request. */
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
				 * OK, we successfully read the message; mark data consumed
				 */
				conn->inStart = conn->inCursor;

				/* Respond to the request if necessary. */

				/*
				 * Note that conn->pghost must be non-NULL if we are going to
				 * avoid the Kerberos code doing a hostname look-up.
				 */

				/*
				 * XXX fe-auth.c has not been fixed to support PQExpBuffers,
				 * so:
				 */
				if (pg_fe_sendauth(areq, conn, conn->pghost, conn->pgpass,
								   conn->errorMessage.data) != STATUS_OK)
				{
					conn->errorMessage.len = strlen(conn->errorMessage.data);
					goto error_return;
				}
				conn->errorMessage.len = strlen(conn->errorMessage.data);

				/*
				 * Just make sure that any data sent by pg_fe_sendauth is
				 * flushed out.  Although this theoretically could block, it
				 * really shouldn't since we don't send large auth responses.
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
				 * message indicates that startup is successful, but we might
				 * also get an Error message indicating failure. (Notice
				 * messages indicating nonfatal warnings are also allowed by
				 * the protocol, as are ParameterStatus and BackendKeyData
				 * messages.) Easiest way to handle this is to let
				 * PQgetResult() read the messages. We just have to fake it
				 * out about the state of the connection, by setting
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
					 * if the resultStatus is FATAL, then conn->errorMessage
					 * already has a copy of the error; needn't copy it back.
					 * But add a newline if it's not there already, since
					 * postmaster error messages may not have one.
					 */
					if (conn->errorMessage.len <= 0 ||
						conn->errorMessage.data[conn->errorMessage.len - 1] != '\n')
						appendPQExpBufferChar(&conn->errorMessage, '\n');
					PQclear(res);
					goto error_return;
				}

				/* We can release the address list now. */
				pg_freeaddrinfo_all(conn->addrlist_family, conn->addrlist);
				conn->addrlist = NULL;
				conn->addr_cur = NULL;

				/* Fire up post-connection housekeeping if needed */
				if (PG_PROTOCOL_MAJOR(conn->pversion) < 3)
				{
					conn->status = CONNECTION_SETENV;
					conn->setenv_state = SETENV_STATE_OPTION_SEND;
					conn->next_eo = EnvironmentOptions;
					return PGRES_POLLING_WRITING;
				}

				/* Otherwise, we are open for business! */
				conn->status = CONNECTION_OK;
				return PGRES_POLLING_OK;
			}

		case CONNECTION_SETENV:

			/*
			 * Do post-connection housekeeping (only needed in protocol 2.0).
			 *
			 * We pretend that the connection is OK for the duration of these
			 * queries.
			 */
			conn->status = CONNECTION_OK;

			switch (pqSetenvPoll(conn))
			{
				case PGRES_POLLING_OK:	/* Success */
					break;

				case PGRES_POLLING_READING:		/* Still going */
					conn->status = CONNECTION_SETENV;
					return PGRES_POLLING_READING;

				case PGRES_POLLING_WRITING:		/* Still going */
					conn->status = CONNECTION_SETENV;
					return PGRES_POLLING_WRITING;

				default:
					goto error_return;
			}

			/* We are open for business! */
			conn->status = CONNECTION_OK;
			return PGRES_POLLING_OK;

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
	 * We used to close the socket at this point, but that makes it awkward
	 * for those above us if they wish to remove this socket from their own
	 * records (an fd_set for example).  We'll just have this socket closed
	 * when PQfinish is called (which is compulsory even after an error, since
	 * the connection structure must be freed).
	 */
	conn->status = CONNECTION_BAD;
	return PGRES_POLLING_FAILED;
}


/*
 * makeEmptyPGconn
 *	 - create a PGconn data structure with (as yet) no interesting data
 */
static PGconn *
makeEmptyPGconn(void)
{
	PGconn	   *conn;

#ifdef WIN32

	/*
	 * Make sure socket support is up and running.
	 */
	WSADATA		wsaData;

	if (WSAStartup(MAKEWORD(1, 1), &wsaData))
		return NULL;
	WSASetLastError(0);
#endif

	conn = (PGconn *) malloc(sizeof(PGconn));
	if (conn == NULL)
	{
#ifdef WIN32
		WSACleanup();
#endif
		return conn;
	}

	/* Zero all pointers and booleans */
	MemSet(conn, 0, sizeof(PGconn));

	conn->noticeHooks.noticeRec = defaultNoticeReceiver;
	conn->noticeHooks.noticeProc = defaultNoticeProcessor;
	conn->status = CONNECTION_BAD;
	conn->asyncStatus = PGASYNC_IDLE;
	conn->xactStatus = PQTRANS_IDLE;
	conn->options_valid = false;
	conn->nonblocking = false;
	conn->setenv_state = SETENV_STATE_IDLE;
	conn->client_encoding = PG_SQL_ASCII;
	conn->std_strings = false;	/* unless server says differently */
	conn->verbosity = PQERRORS_DEFAULT;
	conn->sock = -1;
#ifdef USE_SSL
	conn->allow_ssl_try = true;
	conn->wait_ssl_try = false;
#endif

	/*
	 * We try to send at least 8K at a time, which is the usual size of pipe
	 * buffers on Unix systems.  That way, when we are sending a large amount
	 * of data, we avoid incurring extra kernel context swaps for partial
	 * bufferloads.  The output buffer is initially made 16K in size, and we
	 * try to dump it after accumulating 8K.
	 *
	 * With the same goal of minimizing context swaps, the input buffer will
	 * be enlarged anytime it has less than 8K free, so we initially allocate
	 * twice that.
	 */
	conn->inBufSize = 16 * 1024;
	conn->inBuffer = (char *) malloc(conn->inBufSize);
	conn->outBufSize = 16 * 1024;
	conn->outBuffer = (char *) malloc(conn->outBufSize);
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
 * When changing/adding to this function, see also closePGconn!
 */
static void
freePGconn(PGconn *conn)
{
	PGnotify   *notify;
	pgParameterStatus *pstatus;

	if (!conn)
		return;

	pqClearAsyncResult(conn);	/* deallocate result and curTuple */
	if (conn->sock >= 0)
	{
		pqsecure_close(conn);
		closesocket(conn->sock);
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
	if (conn->connect_timeout)
		free(conn->connect_timeout);
	if (conn->pgoptions)
		free(conn->pgoptions);
	if (conn->dbName)
		free(conn->dbName);
	if (conn->pguser)
		free(conn->pguser);
	if (conn->pgpass)
		free(conn->pgpass);
	if (conn->sslmode)
		free(conn->sslmode);
#ifdef KRB5
	if (conn->krbsrvname)
		free(conn->krbsrvname);
#endif
	/* Note that conn->Pfdebug is not ours to close or free */
	if (conn->last_query)
		free(conn->last_query);
	pg_freeaddrinfo_all(conn->addrlist_family, conn->addrlist);
	notify = conn->notifyHead;
	while (notify != NULL)
	{
		PGnotify   *prev = notify;

		notify = notify->next;
		free(prev);
	}
	pstatus = conn->pstatus;
	while (pstatus != NULL)
	{
		pgParameterStatus *prev = pstatus;

		pstatus = pstatus->next;
		free(prev);
	}
	if (conn->lobjfuncs)
		free(conn->lobjfuncs);
	if (conn->inBuffer)
		free(conn->inBuffer);
	if (conn->outBuffer)
		free(conn->outBuffer);
	termPQExpBuffer(&conn->errorMessage);
	termPQExpBuffer(&conn->workBuffer);
	free(conn);

#ifdef WIN32
	WSACleanup();
#endif
}

/*
 * closePGconn
 *	 - properly close a connection to the backend
 *
 * Release all transient state, but NOT the connection parameters.
 */
static void
closePGconn(PGconn *conn)
{
	PGnotify   *notify;
	pgParameterStatus *pstatus;

	/*
	 * Note that the protocol doesn't allow us to send Terminate messages
	 * during the startup phase.
	 */
	if (conn->sock >= 0 && conn->status == CONNECTION_OK)
	{
		/*
		 * Try to send "close connection" message to backend. Ignore any
		 * error.
		 */
		pqPutMsgStart('X', false, conn);
		pqPutMsgEnd(conn);
		pqFlush(conn);
	}

	/*
	 * must reset the blocking status so a possible reconnect will work don't
	 * call PQsetnonblocking() because it will fail if it's unable to flush
	 * the connection.
	 */
	conn->nonblocking = FALSE;

	/*
	 * Close the connection, reset all transient state, flush I/O buffers.
	 */
	if (conn->sock >= 0)
	{
		pqsecure_close(conn);
		closesocket(conn->sock);
	}
	conn->sock = -1;
	conn->status = CONNECTION_BAD;		/* Well, not really _bad_ - just
										 * absent */
	conn->asyncStatus = PGASYNC_IDLE;
	pqClearAsyncResult(conn);	/* deallocate result and curTuple */
	pg_freeaddrinfo_all(conn->addrlist_family, conn->addrlist);
	conn->addrlist = NULL;
	conn->addr_cur = NULL;
	notify = conn->notifyHead;
	while (notify != NULL)
	{
		PGnotify   *prev = notify;

		notify = notify->next;
		free(prev);
	}
	conn->notifyHead = NULL;
	pstatus = conn->pstatus;
	while (pstatus != NULL)
	{
		pgParameterStatus *prev = pstatus;

		pstatus = pstatus->next;
		free(prev);
	}
	conn->pstatus = NULL;
	if (conn->lobjfuncs)
		free(conn->lobjfuncs);
	conn->lobjfuncs = NULL;
	conn->inStart = conn->inCursor = conn->inEnd = 0;
	conn->outCount = 0;
}

/*
 * PQfinish: properly close a connection to the backend. Also frees
 * the PGconn data structure so it shouldn't be re-used after this.
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

/*
 * PQreset: resets the connection to the backend by closing the
 * existing connection and creating a new one.
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


/*
 * PQresetStart:
 * resets the connection to the backend
 * closes the existing connection and makes a new one
 * Returns 1 on success, 0 on failure.
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


/*
 * PQresetPoll:
 * resets the connection to the backend
 * closes the existing connection and makes a new one
 */
PostgresPollingStatusType
PQresetPoll(PGconn *conn)
{
	if (conn)
		return PQconnectPoll(conn);

	return PGRES_POLLING_FAILED;
}

/*
 * PQcancelGet: get a PGcancel structure corresponding to a connection.
 *
 * A copy is needed to be able to cancel a running query from a different
 * thread. If the same structure is used all structure members would have
 * to be individually locked (if the entire structure was locked, it would
 * be impossible to cancel a synchronous query because the structure would
 * have to stay locked for the duration of the query).
 */
PGcancel *
PQgetCancel(PGconn *conn)
{
	PGcancel   *cancel;

	if (!conn)
		return NULL;

	if (conn->sock < 0)
		return NULL;

	cancel = malloc(sizeof(PGcancel));
	if (cancel == NULL)
		return NULL;

	memcpy(&cancel->raddr, &conn->raddr, sizeof(SockAddr));
	cancel->be_pid = conn->be_pid;
	cancel->be_key = conn->be_key;

	return cancel;
}

/* PQfreeCancel: free a cancel structure */
void
PQfreeCancel(PGcancel *cancel)
{
	if (cancel)
		free(cancel);
}


/*
 * PQcancel and PQrequestCancel: attempt to request cancellation of the
 * current operation.
 *
 * The return value is TRUE if the cancel request was successfully
 * dispatched, FALSE if not (in which case an error message is available).
 * Note: successful dispatch is no guarantee that there will be any effect at
 * the backend.  The application must read the operation result as usual.
 *
 * CAUTION: we want this routine to be safely callable from a signal handler
 * (for example, an application might want to call it in a SIGINT handler).
 * This means we cannot use any C library routine that might be non-reentrant.
 * malloc/free are often non-reentrant, and anything that might call them is
 * just as dangerous.  We avoid sprintf here for that reason.  Building up
 * error messages with strcpy/strcat is tedious but should be quite safe.
 * We also save/restore errno in case the signal handler support doesn't.
 *
 * internal_cancel() is an internal helper function to make code-sharing
 * between the two versions of the cancel function possible.
 */
static int
internal_cancel(SockAddr *raddr, int be_pid, int be_key,
				char *errbuf, int errbufsize)
{
	int			save_errno = SOCK_ERRNO;
	int			tmpsock = -1;
	char		sebuf[256];
	int			maxlen;
	struct
	{
		uint32		packetlen;
		CancelRequestPacket cp;
	}			crp;

	/*
	 * We need to open a temporary connection to the postmaster. Do this with
	 * only kernel calls.
	 */
	if ((tmpsock = socket(raddr->addr.ss_family, SOCK_STREAM, 0)) < 0)
	{
		StrNCpy(errbuf, "PQcancel() -- socket() failed: ", errbufsize);
		goto cancel_errReturn;
	}
retry3:
	if (connect(tmpsock, (struct sockaddr *) & raddr->addr,
				raddr->salen) < 0)
	{
		if (SOCK_ERRNO == EINTR)
			/* Interrupted system call - we'll just try again */
			goto retry3;
		StrNCpy(errbuf, "PQcancel() -- connect() failed: ", errbufsize);
		goto cancel_errReturn;
	}

	/*
	 * We needn't set nonblocking I/O or NODELAY options here.
	 */

	/* Create and send the cancel request packet. */

	crp.packetlen = htonl((uint32) sizeof(crp));
	crp.cp.cancelRequestCode = (MsgType) htonl(CANCEL_REQUEST_CODE);
	crp.cp.backendPID = htonl(be_pid);
	crp.cp.cancelAuthCode = htonl(be_key);

retry4:
	if (send(tmpsock, (char *) &crp, sizeof(crp), 0) != (int) sizeof(crp))
	{
		if (SOCK_ERRNO == EINTR)
			/* Interrupted system call - we'll just try again */
			goto retry4;
		StrNCpy(errbuf, "PQcancel() -- send() failed: ", errbufsize);
		goto cancel_errReturn;
	}

	/*
	 * Wait for the postmaster to close the connection, which indicates that
	 * it's processed the request.  Without this delay, we might issue another
	 * command only to find that our cancel zaps that command instead of the
	 * one we thought we were canceling.  Note we don't actually expect this
	 * read to obtain any data, we are just waiting for EOF to be signaled.
	 */
retry5:
	if (recv(tmpsock, (char *) &crp, 1, 0) < 0)
	{
		if (SOCK_ERRNO == EINTR)
			/* Interrupted system call - we'll just try again */
			goto retry5;
		/* we ignore other error conditions */
	}

	/* All done */
	closesocket(tmpsock);
	SOCK_ERRNO_SET(save_errno);
	return TRUE;

cancel_errReturn:

	/*
	 * Make sure we don't overflow the error buffer. Leave space for the \n at
	 * the end, and for the terminating zero.
	 */
	maxlen = errbufsize - strlen(errbuf) - 2;
	if (maxlen >= 0)
	{
		strncat(errbuf, SOCK_STRERROR(SOCK_ERRNO, sebuf, sizeof(sebuf)),
				maxlen);
		strcat(errbuf, "\n");
	}
	if (tmpsock >= 0)
		closesocket(tmpsock);
	SOCK_ERRNO_SET(save_errno);
	return FALSE;
}

/*
 * PQcancel: request query cancel
 *
 * Returns TRUE if able to send the cancel request, FALSE if not.
 *
 * On failure, an error message is stored in *errbuf, which must be of size
 * errbufsize (recommended size is 256 bytes).	*errbuf is not changed on
 * success return.
 */
int
PQcancel(PGcancel *cancel, char *errbuf, int errbufsize)
{
	if (!cancel)
	{
		StrNCpy(errbuf, "PQcancel() -- no cancel object supplied", errbufsize);
		return FALSE;
	}

	return internal_cancel(&cancel->raddr, cancel->be_pid, cancel->be_key,
						   errbuf, errbufsize);
}

/*
 * PQrequestCancel: old, not thread-safe function for requesting query cancel
 *
 * Returns TRUE if able to send the cancel request, FALSE if not.
 *
 * On failure, the error message is saved in conn->errorMessage; this means
 * that this can't be used when there might be other active operations on
 * the connection object.
 *
 * NOTE: error messages will be cut off at the current size of the
 * error message buffer, since we dare not try to expand conn->errorMessage!
 */
int
PQrequestCancel(PGconn *conn)
{
	int			r;

	/* Check we have an open connection */
	if (!conn)
		return FALSE;

	if (conn->sock < 0)
	{
		StrNCpy(conn->errorMessage.data,
				"PQrequestCancel() -- connection is not open\n",
				conn->errorMessage.maxlen);
		conn->errorMessage.len = strlen(conn->errorMessage.data);

		return FALSE;
	}

	r = internal_cancel(&conn->raddr, conn->be_pid, conn->be_key,
						conn->errorMessage.data, conn->errorMessage.maxlen);

	if (!r)
		conn->errorMessage.len = strlen(conn->errorMessage.data);

	return r;
}


/*
 * pqPacketSend() -- convenience routine to send a message to server.
 *
 * pack_type: the single-byte message type code.  (Pass zero for startup
 * packets, which have no message type code.)
 *
 * buf, buf_len: contents of message.  The given length includes only what
 * is in buf; the message type and message length fields are added here.
 *
 * RETURNS: STATUS_ERROR if the write fails, STATUS_OK otherwise.
 * SIDE_EFFECTS: may block.
 *
 * Note: all messages sent with this routine have a length word, whether
 * it's protocol 2.0 or 3.0.
 */
int
pqPacketSend(PGconn *conn, char pack_type,
			 const void *buf, size_t buf_len)
{
	/* Start the message. */
	if (pqPutMsgStart(pack_type, true, conn))
		return STATUS_ERROR;

	/* Send the message body. */
	if (pqPutnchar(buf, buf_len, conn))
		return STATUS_ERROR;

	/* Finish the message. */
	if (pqPutMsgEnd(conn))
		return STATUS_ERROR;

	/* Flush to ensure backend gets it. */
	if (pqFlush(conn))
		return STATUS_ERROR;

	return STATUS_OK;
}

#ifdef USE_LDAP

#define LDAP_URL	"ldap://"
#define LDAP_DEF_PORT	389
#define PGLDAP_TIMEOUT 2

#define ld_is_sp_tab(x) ((x) == ' ' || (x) == '\t')
#define ld_is_nl_cr(x) ((x) == '\r' || (x) == '\n')


/*
 *		ldapServiceLookup
 *
 * Search the LDAP URL passed as first argument, treat the result as a
 * string of connection options that are parsed and added to the array of
 * options passed as second argument.
 *
 * LDAP URLs must conform to RFC 1959 without escape sequences.
 *	ldap://host:port/dn?attributes?scope?filter?extensions
 *
 * Returns
 *	0 if the lookup was successful,
 *	1 if the connection to the LDAP server could be established but
 *	  the search was unsuccessful,
 *	2 if a connection could not be established, and
 *	3 if a fatal error occurred.
 *
 * An error message is returned in the third argument for return codes 1 and 3.
 */
static int
ldapServiceLookup(const char *purl, PQconninfoOption *options,
				  PQExpBuffer errorMessage)
{
	int			port = LDAP_DEF_PORT,
				scope,
				rc,
				msgid,
				size,
				state,
				oldstate,
				i;
	bool		found_keyword;
	char	   *url,
			   *hostname,
			   *portstr,
			   *endptr,
			   *dn,
			   *scopestr,
			   *filter,
			   *result,
			   *p,
			   *p1 = NULL,
			   *optname = NULL,
			   *optval = NULL;
	char	   *attrs[2] = {NULL, NULL};
	LDAP	   *ld = NULL;
	LDAPMessage *res,
			   *entry;
	struct berval **values;
	LDAP_TIMEVAL time = {PGLDAP_TIMEOUT, 0};

	if ((url = strdup(purl)) == NULL)
	{
		printfPQExpBuffer(errorMessage, libpq_gettext("out of memory\n"));
		return 3;
	}

	/*
	 * Parse URL components, check for correctness.  Basically, url has '\0'
	 * placed at component boundaries and variables are pointed at each
	 * component.
	 */

	if (pg_strncasecmp(url, LDAP_URL, strlen(LDAP_URL)) != 0)
	{
		printfPQExpBuffer(errorMessage,
		libpq_gettext("invalid LDAP URL \"%s\": scheme must be ldap://\n"), purl);
		free(url);
		return 3;
	}

	/* hostname */
	hostname = url + strlen(LDAP_URL);
	if (*hostname == '/')		/* no hostname? */
		hostname = "localhost"; /* the default */

	/* dn, "distinguished name" */
	p = strchr(url + strlen(LDAP_URL), '/');
	if (p == NULL || *(p + 1) == '\0' || *(p + 1) == '?')
	{
		printfPQExpBuffer(errorMessage, libpq_gettext(
				 "invalid LDAP URL \"%s\": missing distinguished name\n"), purl);
		free(url);
		return 3;
	}
	*p = '\0';					/* terminate hostname */
	dn = p + 1;

	/* attribute */
	if ((p = strchr(dn, '?')) == NULL || *(p + 1) == '\0' || *(p + 1) == '?')
	{
		printfPQExpBuffer(errorMessage, libpq_gettext(
			"invalid LDAP URL \"%s\": must have exactly one attribute\n"), purl);
		free(url);
		return 3;
	}
	*p = '\0';
	attrs[0] = p + 1;

	/* scope */
	if ((p = strchr(attrs[0], '?')) == NULL || *(p + 1) == '\0' || *(p + 1) == '?')
	{
		printfPQExpBuffer(errorMessage, libpq_gettext("invalid LDAP URL \"%s\": must have search scope (base/one/sub)\n"), purl);
		free(url);
		return 3;
	}
	*p = '\0';
	scopestr = p + 1;

	/* filter */
	if ((p = strchr(scopestr, '?')) == NULL || *(p + 1) == '\0' || *(p + 1) == '?')
	{
		printfPQExpBuffer(errorMessage,
					libpq_gettext("invalid LDAP URL \"%s\": no filter\n"), purl);
		free(url);
		return 3;
	}
	*p = '\0';
	filter = p + 1;
	if ((p = strchr(filter, '?')) != NULL)
		*p = '\0';

	/* port number? */
	if ((p1 = strchr(hostname, ':')) != NULL)
	{
		long		lport;

		*p1 = '\0';
		portstr = p1 + 1;
		errno = 0;
		lport = strtol(portstr, &endptr, 10);
		if (*portstr == '\0' || *endptr != '\0' || errno || lport < 0 || lport > 65535)
		{
			printfPQExpBuffer(errorMessage, libpq_gettext(
						"invalid LDAP URL \"%s\": invalid port number\n"), purl);
			free(url);
			return 3;
		}
		port = (int) lport;
	}

	/* Allow only one attribute */
	if (strchr(attrs[0], ',') != NULL)
	{
		printfPQExpBuffer(errorMessage, libpq_gettext(
			"invalid LDAP URL \"%s\": must have exactly one attribute\n"), purl);
		free(url);
		return 3;
	}

	/* set scope */
	if (pg_strcasecmp(scopestr, "base") == 0)
		scope = LDAP_SCOPE_BASE;
	else if (pg_strcasecmp(scopestr, "one") == 0)
		scope = LDAP_SCOPE_ONELEVEL;
	else if (pg_strcasecmp(scopestr, "sub") == 0)
		scope = LDAP_SCOPE_SUBTREE;
	else
	{
		printfPQExpBuffer(errorMessage, libpq_gettext("invalid LDAP URL \"%s\": must have search scope (base/one/sub)\n"), purl);
		free(url);
		return 3;
	}

	/* initialize LDAP structure */
	if ((ld = ldap_init(hostname, port)) == NULL)
	{
		printfPQExpBuffer(errorMessage,
						  libpq_gettext("could not create LDAP structure\n"));
		free(url);
		return 3;
	}

	/*
	 * Initialize connection to the server.  We do an explicit bind because we
	 * want to return 2 if the bind fails.
	 */
	if ((msgid = ldap_simple_bind(ld, NULL, NULL)) == -1)
	{
		/* error in ldap_simple_bind() */
		free(url);
		ldap_unbind(ld);
		return 2;
	}

	/* wait some time for the connection to succeed */
	res = NULL;
	if ((rc = ldap_result(ld, msgid, LDAP_MSG_ALL, &time, &res)) == -1 ||
		res == NULL)
	{
		if (res != NULL)
		{
			/* timeout */
			ldap_msgfree(res);
		}
		/* error in ldap_result() */
		free(url);
		ldap_unbind(ld);
		return 2;
	}
	ldap_msgfree(res);

	/* search */
	res = NULL;
	if ((rc = ldap_search_st(ld, dn, scope, filter, attrs, 0, &time, &res))
		!= LDAP_SUCCESS)
	{
		if (res != NULL)
			ldap_msgfree(res);
		printfPQExpBuffer(errorMessage,
						  libpq_gettext("lookup on LDAP server failed: %s\n"),
						  ldap_err2string(rc));
		ldap_unbind(ld);
		free(url);
		return 1;
	}

	/* complain if there was not exactly one result */
	if ((rc = ldap_count_entries(ld, res)) != 1)
	{
		printfPQExpBuffer(errorMessage,
			 rc ? libpq_gettext("more than one entry found on LDAP lookup\n")
						  : libpq_gettext("no entry found on LDAP lookup\n"));
		ldap_msgfree(res);
		ldap_unbind(ld);
		free(url);
		return 1;
	}

	/* get entry */
	if ((entry = ldap_first_entry(ld, res)) == NULL)
	{
		/* should never happen */
		printfPQExpBuffer(errorMessage,
						  libpq_gettext("no entry found on LDAP lookup\n"));
		ldap_msgfree(res);
		ldap_unbind(ld);
		free(url);
		return 1;
	}

	/* get values */
	if ((values = ldap_get_values_len(ld, entry, attrs[0])) == NULL)
	{
		printfPQExpBuffer(errorMessage,
				  libpq_gettext("attribute has no values on LDAP lookup\n"));
		ldap_msgfree(res);
		ldap_unbind(ld);
		free(url);
		return 1;
	}

	ldap_msgfree(res);
	free(url);

	if (values[0] == NULL)
	{
		printfPQExpBuffer(errorMessage,
				  libpq_gettext("attribute has no values on LDAP lookup\n"));
		ldap_value_free_len(values);
		ldap_unbind(ld);
		return 1;
	}

	/* concatenate values to a single string */
	for (size = 0, i = 0; values[i] != NULL; ++i)
		size += values[i]->bv_len + 1;
	if ((result = malloc(size + 1)) == NULL)
	{
		printfPQExpBuffer(errorMessage,
						  libpq_gettext("out of memory\n"));
		ldap_value_free_len(values);
		ldap_unbind(ld);
		return 3;
	}
	for (p = result, i = 0; values[i] != NULL; ++i)
	{
		strncpy(p, values[i]->bv_val, values[i]->bv_len);
		p += values[i]->bv_len;
		*(p++) = '\n';
		if (values[i + 1] == NULL)
			*(p + 1) = '\0';
	}

	ldap_value_free_len(values);
	ldap_unbind(ld);

	/* parse result string */
	oldstate = state = 0;
	for (p = result; *p != '\0'; ++p)
	{
		switch (state)
		{
			case 0:				/* between entries */
				if (!ld_is_sp_tab(*p) && !ld_is_nl_cr(*p))
				{
					optname = p;
					state = 1;
				}
				break;
			case 1:				/* in option name */
				if (ld_is_sp_tab(*p))
				{
					*p = '\0';
					state = 2;
				}
				else if (ld_is_nl_cr(*p))
				{
					printfPQExpBuffer(errorMessage, libpq_gettext(
					"missing \"=\" after \"%s\" in connection info string\n"),
									  optname);
					return 3;
				}
				else if (*p == '=')
				{
					*p = '\0';
					state = 3;
				}
				break;
			case 2:				/* after option name */
				if (*p == '=')
				{
					state = 3;
				}
				else if (!ld_is_sp_tab(*p))
				{
					printfPQExpBuffer(errorMessage, libpq_gettext(
					"missing \"=\" after \"%s\" in connection info string\n"),
									  optname);
					return 3;
				}
				break;
			case 3:				/* before option value */
				if (*p == '\'')
				{
					optval = p + 1;
					p1 = p + 1;
					state = 5;
				}
				else if (ld_is_nl_cr(*p))
				{
					optval = optname + strlen(optname); /* empty */
					state = 0;
				}
				else if (!ld_is_sp_tab(*p))
				{
					optval = p;
					state = 4;
				}
				break;
			case 4:				/* in unquoted option value */
				if (ld_is_sp_tab(*p) || ld_is_nl_cr(*p))
				{
					*p = '\0';
					state = 0;
				}
				break;
			case 5:				/* in quoted option value */
				if (*p == '\'')
				{
					*p1 = '\0';
					state = 0;
				}
				else if (*p == '\\')
					state = 6;
				else
					*(p1++) = *p;
				break;
			case 6:				/* in quoted option value after escape */
				*(p1++) = *p;
				state = 5;
				break;
		}

		if (state == 0 && oldstate != 0)
		{
			found_keyword = false;
			for (i = 0; options[i].keyword; i++)
			{
				if (strcmp(options[i].keyword, optname) == 0)
				{
					if (options[i].val == NULL)
						options[i].val = strdup(optval);
					found_keyword = true;
					break;
				}
			}
			if (!found_keyword)
			{
				printfPQExpBuffer(errorMessage,
						 libpq_gettext("invalid connection option \"%s\"\n"),
								  optname);
				return 1;
			}
			optname = NULL;
			optval = NULL;
		}
		oldstate = state;
	}

	if (state == 5 || state == 6)
	{
		printfPQExpBuffer(errorMessage, libpq_gettext(
				  "unterminated quoted string in connection info string\n"));
		return 3;
	}

	return 0;
}
#endif

#define MAXBUFSIZE 256

static int
parseServiceInfo(PQconninfoOption *options, PQExpBuffer errorMessage)
{
	char	   *service = conninfo_getval(options, "service");
	char		serviceFile[MAXPGPATH];
	bool		group_found = false;
	int			linenr = 0,
				i;

	/*
	 * We have to special-case the environment variable PGSERVICE here, since
	 * this is and should be called before inserting environment defaults for
	 * other connection options.
	 */
	if (service == NULL)
		service = getenv("PGSERVICE");

	/*
	 * This could be used by any application so we can't use the binary
	 * location to find our config files.
	 */
	snprintf(serviceFile, MAXPGPATH, "%s/pg_service.conf",
			 getenv("PGSYSCONFDIR") ? getenv("PGSYSCONFDIR") : SYSCONFDIR);

	if (service != NULL)
	{
		FILE	   *f;
		char		buf[MAXBUFSIZE],
				   *line;

		f = fopen(serviceFile, "r");
		if (f == NULL)
		{
			printfPQExpBuffer(errorMessage, libpq_gettext("ERROR: service file \"%s\" not found\n"),
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
								  libpq_gettext("ERROR: line %d too long in service file \"%s\"\n"),
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
					group_found = true;
				else
					group_found = false;
			}
			else
			{
				if (group_found)
				{
					/*
					 * Finally, we are in the right group and can parse the
					 * line
					 */
					char	   *key,
							   *val;
					bool		found_keyword;

#ifdef USE_LDAP
					if (strncmp(line, "ldap", 4) == 0)
					{
						int			rc = ldapServiceLookup(line, options, errorMessage);

						/* if rc = 2, go on reading for fallback */
						switch (rc)
						{
							case 0:
								fclose(f);
								return 0;
							case 1:
							case 3:
								fclose(f);
								return 3;
							case 2:
								continue;
						}
					}
#endif

					key = line;
					val = strchr(line, '=');
					if (val == NULL)
					{
						printfPQExpBuffer(errorMessage,
										  libpq_gettext("ERROR: syntax error in service file \"%s\", line %d\n"),
										  serviceFile,
										  linenr);
						fclose(f);
						return 3;
					}
					*val++ = '\0';

					/*
					 * Set the parameter --- but don't override any previous
					 * explicit setting.
					 */
					found_keyword = false;
					for (i = 0; options[i].keyword; i++)
					{
						if (strcmp(options[i].keyword, key) == 0)
						{
							if (options[i].val == NULL)
								options[i].val = strdup(val);
							found_keyword = true;
							break;
						}
					}

					if (!found_keyword)
					{
						printfPQExpBuffer(errorMessage,
										  libpq_gettext("ERROR: syntax error in service file \"%s\", line %d\n"),
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
		 * Now we have the name and the value. Search for the param record.
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
		if (!option->val)
		{
			printfPQExpBuffer(errorMessage,
							  libpq_gettext("out of memory\n"));
			PQconninfoFree(options);
			free(buf);
			return NULL;
		}
	}

	/* Done with the modifiable input string */
	free(buf);

	/*
	 * If there's a service spec, use it to obtain any not-explicitly-given
	 * parameters.
	 */
	if (parseServiceInfo(options, errorMessage))
	{
		PQconninfoFree(options);
		return NULL;
	}

	/*
	 * Get the fallback resources for parameters not specified in the conninfo
	 * string nor the service.
	 */
	for (option = options; option->keyword != NULL; option++)
	{
		if (option->val != NULL)
			continue;			/* Value was in conninfo or service */

		/*
		 * Try to get the environment variable fallback
		 */
		if (option->envvar != NULL)
		{
			if ((tmp = getenv(option->envvar)) != NULL)
			{
				option->val = strdup(tmp);
				if (!option->val)
				{
					printfPQExpBuffer(errorMessage,
									  libpq_gettext("out of memory\n"));
					PQconninfoFree(options);
					return NULL;
				}
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
			if (!option->val)
			{
				printfPQExpBuffer(errorMessage,
								  libpq_gettext("out of memory\n"));
				PQconninfoFree(options);
				return NULL;
			}
			continue;
		}

		/*
		 * Special handling for user
		 */
		if (strcmp(option->keyword, "user") == 0)
		{
			option->val = pg_fe_getauthname(errortmp);
			/* note any error message is thrown away */
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
		return NULL;
	return conn->dbName;
}

char *
PQuser(const PGconn *conn)
{
	if (!conn)
		return NULL;
	return conn->pguser;
}

char *
PQpass(const PGconn *conn)
{
	if (!conn)
		return NULL;
	return conn->pgpass;
}

char *
PQhost(const PGconn *conn)
{
	if (!conn)
		return NULL;
	return conn->pghost ? conn->pghost : conn->pgunixsocket;
}

char *
PQport(const PGconn *conn)
{
	if (!conn)
		return NULL;
	return conn->pgport;
}

char *
PQtty(const PGconn *conn)
{
	if (!conn)
		return NULL;
	return conn->pgtty;
}

char *
PQoptions(const PGconn *conn)
{
	if (!conn)
		return NULL;
	return conn->pgoptions;
}

ConnStatusType
PQstatus(const PGconn *conn)
{
	if (!conn)
		return CONNECTION_BAD;
	return conn->status;
}

PGTransactionStatusType
PQtransactionStatus(const PGconn *conn)
{
	if (!conn || conn->status != CONNECTION_OK)
		return PQTRANS_UNKNOWN;
	if (conn->asyncStatus != PGASYNC_IDLE)
		return PQTRANS_ACTIVE;
	return conn->xactStatus;
}

const char *
PQparameterStatus(const PGconn *conn, const char *paramName)
{
	const pgParameterStatus *pstatus;

	if (!conn || !paramName)
		return NULL;
	for (pstatus = conn->pstatus; pstatus != NULL; pstatus = pstatus->next)
	{
		if (strcmp(pstatus->name, paramName) == 0)
			return pstatus->value;
	}
	return NULL;
}

int
PQprotocolVersion(const PGconn *conn)
{
	if (!conn)
		return 0;
	if (conn->status == CONNECTION_BAD)
		return 0;
	return PG_PROTOCOL_MAJOR(conn->pversion);
}

int
PQserverVersion(const PGconn *conn)
{
	if (!conn)
		return 0;
	if (conn->status == CONNECTION_BAD)
		return 0;
	return conn->sversion;
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
	static const char query[] = "set client_encoding to '%s'";
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

	if (res == NULL)
		return -1;
	if (res->resultStatus != PGRES_COMMAND_OK)
		status = -1;
	else
	{
		/*
		 * In protocol 2 we have to assume the setting will stick, and adjust
		 * our state immediately.  In protocol 3 and up we can rely on the
		 * backend to report the parameter value, and we'll change state at
		 * that time.
		 */
		if (PG_PROTOCOL_MAJOR(conn->pversion) < 3)
			pqSaveParameterStatus(conn, "client_encoding", encoding);
		status = 0;				/* everything is ok */
	}
	PQclear(res);
	return status;
}

PGVerbosity
PQsetErrorVerbosity(PGconn *conn, PGVerbosity verbosity)
{
	PGVerbosity old;

	if (!conn)
		return PQERRORS_DEFAULT;
	old = conn->verbosity;
	conn->verbosity = verbosity;
	return old;
}

void
PQtrace(PGconn *conn, FILE *debug_port)
{
	if (conn == NULL)
		return;
	PQuntrace(conn);
	conn->Pfdebug = debug_port;
}

void
PQuntrace(PGconn *conn)
{
	if (conn == NULL)
		return;
	if (conn->Pfdebug)
	{
		fflush(conn->Pfdebug);
		conn->Pfdebug = NULL;
	}
}

PQnoticeReceiver
PQsetNoticeReceiver(PGconn *conn, PQnoticeReceiver proc, void *arg)
{
	PQnoticeReceiver old;

	if (conn == NULL)
		return NULL;

	old = conn->noticeHooks.noticeRec;
	if (proc)
	{
		conn->noticeHooks.noticeRec = proc;
		conn->noticeHooks.noticeRecArg = arg;
	}
	return old;
}

PQnoticeProcessor
PQsetNoticeProcessor(PGconn *conn, PQnoticeProcessor proc, void *arg)
{
	PQnoticeProcessor old;

	if (conn == NULL)
		return NULL;

	old = conn->noticeHooks.noticeProc;
	if (proc)
	{
		conn->noticeHooks.noticeProc = proc;
		conn->noticeHooks.noticeProcArg = arg;
	}
	return old;
}

/*
 * The default notice message receiver just gets the standard notice text
 * and sends it to the notice processor.  This two-level setup exists
 * mostly for backwards compatibility; perhaps we should deprecate use of
 * PQsetNoticeProcessor?
 */
static void
defaultNoticeReceiver(void *arg, const PGresult *res)
{
	(void) arg;					/* not used */
	if (res->noticeHooks.noticeProc != NULL)
		(*res->noticeHooks.noticeProc) (res->noticeHooks.noticeProcArg,
										PQresultErrorMessage(res));
}

/*
 * The default notice message processor just prints the
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

/*
 * returns a pointer to the next token or NULL if the current
 * token doesn't match
 */
static char *
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
static char *
PasswordFromFile(char *hostname, char *port, char *dbname, char *username)
{
	FILE	   *fp;
	char		pgpassfile[MAXPGPATH];
	struct stat stat_buf;
	char	   *passfile_env;

#define LINELEN NAMEDATALEN*5
	char		buf[LINELEN];

	if (dbname == NULL || strlen(dbname) == 0)
		return NULL;

	if (username == NULL || strlen(username) == 0)
		return NULL;

	/* 'localhost' matches pghost of '' or the default socket directory */
	if (hostname == NULL)
		hostname = DefaultHost;
	else if (is_absolute_path(hostname))

		/*
		 * We should probably use canonicalize_path(), but then we have to
		 * bring path.c into libpq, and it doesn't seem worth it.
		 */
		if (strcmp(hostname, DEFAULT_PGSOCKET_DIR) == 0)
			hostname = DefaultHost;

	if (port == NULL)
		port = DEF_PGPORT_STR;

	if ((passfile_env = getenv("PGPASSFILE")) != NULL)
		/* use the literal path from the environment, if set */
		StrNCpy(pgpassfile, passfile_env, MAXPGPATH);
	else
	{
		char		homedir[MAXPGPATH];

		if (!pqGetHomeDirectory(homedir, sizeof(homedir)))
			return NULL;
		snprintf(pgpassfile, MAXPGPATH, "%s/%s", homedir, PGPASSFILE);
	}

	/* If password file cannot be opened, ignore it. */
	if (stat(pgpassfile, &stat_buf) == -1)
		return NULL;

#ifndef WIN32

	if (!S_ISREG(stat_buf.st_mode))
	{
		fprintf(stderr,
		libpq_gettext("WARNING: password file \"%s\" is not a plain file\n"),
				pgpassfile);
		return NULL;
	}

	/* If password file is insecure, alert the user and ignore it. */
	if (stat_buf.st_mode & (S_IRWXG | S_IRWXO))
	{
		fprintf(stderr,
				libpq_gettext("WARNING: password file \"%s\" has world or group read access; permission should be u=rw (0600)\n"),
				pgpassfile);
		return NULL;
	}
#endif

	fp = fopen(pgpassfile, "r");
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

/*
 * Obtain user's home directory, return in given buffer
 *
 * On Unix, this actually returns the user's home directory.  On Windows
 * it returns the PostgreSQL-specific application data folder.
 *
 * This is essentially the same as get_home_path(), but we don't use that
 * because we don't want to pull path.c into libpq (it pollutes application
 * namespace)
 */
bool
pqGetHomeDirectory(char *buf, int bufsize)
{
#ifndef WIN32
	char		pwdbuf[BUFSIZ];
	struct passwd pwdstr;
	struct passwd *pwd = NULL;

	if (pqGetpwuid(geteuid(), &pwdstr, pwdbuf, sizeof(pwdbuf), &pwd) != 0)
		return false;
	StrNCpy(buf, pwd->pw_dir, bufsize);
	return true;
#else
	char		tmppath[MAX_PATH];

	ZeroMemory(tmppath, sizeof(tmppath));
	if (SHGetFolderPath(NULL, CSIDL_APPDATA, NULL, 0, tmppath) != S_OK)
		return false;
	snprintf(buf, bufsize, "%s/postgresql", tmppath);
	return true;
#endif
}

/*
 * To keep the API consistent, the locking stubs are always provided, even
 * if they are not required.
 */

static void
default_threadlock(int acquire)
{
#ifdef ENABLE_THREAD_SAFETY
#ifndef WIN32
	static pthread_mutex_t singlethread_lock = PTHREAD_MUTEX_INITIALIZER;
#else
	static pthread_mutex_t singlethread_lock = NULL;
	static long mutex_initlock = 0;

	if (singlethread_lock == NULL)
	{
		while (InterlockedExchange(&mutex_initlock, 1) == 1)
			 /* loop, another thread own the lock */ ;
		if (singlethread_lock == NULL)
			pthread_mutex_init(&singlethread_lock, NULL);
		InterlockedExchange(&mutex_initlock, 0);
	}
#endif
	if (acquire)
		pthread_mutex_lock(&singlethread_lock);
	else
		pthread_mutex_unlock(&singlethread_lock);
#endif
}

pgthreadlock_t
PQregisterThreadLock(pgthreadlock_t newhandler)
{
	pgthreadlock_t prev = pg_g_threadlock;

	if (newhandler)
		pg_g_threadlock = newhandler;
	else
		pg_g_threadlock = default_threadlock;

	return prev;
}
