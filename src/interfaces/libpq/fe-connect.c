/*-------------------------------------------------------------------------
 *
 * fe-connect.c
 *	  functions related to setting up a connection to the backend
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/interfaces/libpq/fe-connect.c,v 1.106 1999/11/11 00:10:13 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include <fcntl.h>
#include <errno.h>
#include <ctype.h>

#include "postgres.h"
#include "libpq-fe.h"
#include "libpq-int.h"
#include "fe-auth.h"

#ifdef WIN32
#include "win32.h"
#else
#include <unistd.h>
#include <netdb.h>
#include <netinet/tcp.h>
#endif

#ifndef HAVE_STRDUP
#include "strdup.h"
#endif
#ifdef HAVE_CRYPT_H
#include <crypt.h>
#endif

#ifdef MULTIBYTE
#include "mb/pg_wchar.h"
#endif

#ifdef USE_SSL
static SSL_CTX *SSL_context = NULL;
#endif

static ConnStatusType connectDB(PGconn *conn);
static PGconn *makeEmptyPGconn(void);
static void freePGconn(PGconn *conn);
static void closePGconn(PGconn *conn);
static int	conninfo_parse(const char *conninfo, PQExpBuffer errorMessage);
static char *conninfo_getval(char *keyword);
static void conninfo_free(void);
static void defaultNoticeProcessor(void *arg, const char *message);

/* XXX Why is this not static? */
void		PQsetenv(PGconn *conn);

#define NOTIFYLIST_INITIAL_SIZE 10
#define NOTIFYLIST_GROWBY 10


/* ----------
 * Definition of the conninfo parameters and their fallback resources.
 * If Environment-Var and Compiled-in are specified as NULL, no
 * fallback is available. If after all no value can be determined
 * for an option, an error is returned.
 *
 * The values for dbname and user are treated special in conninfo_parse.
 * If the Compiled-in resource is specified as a NULL value, the
 * user is determined by fe_getauthname() and for dbname the user
 * name is copied.
 *
 * The Label and Disp-Char entries are provided for applications that
 * want to use PQconndefaults() to create a generic database connection
 * dialog. Disp-Char is defined as follows:
 *	   ""		Normal input field
 * ----------
 */
static PQconninfoOption PQconninfoOptions[] = {
/* ----------------------------------------------------------------- */
/*	  Option-name		Environment-Var Compiled-in		Current value	*/
/*						Label							Disp-Char		*/
/* ----------------- --------------- --------------- --------------- */
	/* "authtype" is ignored as it is no longer used. */
	{"authtype", "PGAUTHTYPE", DefaultAuthtype, NULL,
	"Database-Authtype", "", 20},

	{"user", "PGUSER", NULL, NULL,
	"Database-User", "", 20},

	{"password", "PGPASSWORD", DefaultPassword, NULL,
	"Database-Password", "", 20},

	{"dbname", "PGDATABASE", NULL, NULL,
	"Database-Name", "", 20},

	{"host", "PGHOST", NULL, NULL,
	"Database-Host", "", 40},

	{"port", "PGPORT", DEF_PGPORT, NULL,
	"Database-Port", "", 6},

	{"tty", "PGTTY", DefaultTty, NULL,
	"Backend-Debug-TTY", "D", 40},

	{"options", "PGOPTIONS", DefaultOption, NULL,
	"Backend-Debug-Options", "D", 40},
/* ----------------- --------------- --------------- --------------- */
	{NULL, NULL, NULL, NULL,
	NULL, NULL, 0}
};

static struct EnvironmentOptions
{
	const char *envName,
			   *pgName;
}			EnvironmentOptions[] =

{
	/* common user-interface settings */
	{
		"PGDATESTYLE", "datestyle"
	},
	{
		"PGTZ", "timezone"
	},
#ifdef MULTIBYTE
	{
		"PGCLIENTENCODING", "client_encoding"
	},
#endif
	/* internal performance-related settings */
	{
		"PGCOSTHEAP", "cost_heap"
	},
	{
		"PGCOSTINDEX", "cost_index"
	},
	{
		"PGGEQO", "geqo"
	},
	{
		NULL, NULL
	}
};

/* ----------------
 *		PQconnectdb
 *
 * establishes a connection to a postgres backend through the postmaster
 * using connection information in a string.
 *
 * The conninfo string is a list of
 *
 *	   option = value
 *
 * definitions. Value might be a single value containing no whitespaces
 * or a single quoted string. If a single quote should appear everywhere
 * in the value, it must be escaped with a backslash like \'
 *
 * Returns a PGconn* which is needed for all subsequent libpq calls
 * if the status field of the connection returned is CONNECTION_BAD,
 * then some fields may be null'ed out instead of having valid values
 * ----------------
 */
PGconn *
PQconnectdb(const char *conninfo)
{
	PGconn	   *conn;
	char	   *tmp;

	/* ----------
	 * Allocate memory for the conn structure
	 * ----------
	 */
	conn = makeEmptyPGconn();
	if (conn == NULL)
		return (PGconn *) NULL;

	/* ----------
	 * Parse the conninfo string and save settings in conn structure
	 * ----------
	 */
	if (conninfo_parse(conninfo, &conn->errorMessage) < 0)
	{
		conn->status = CONNECTION_BAD;
		conninfo_free();
		return conn;
	}
	tmp = conninfo_getval("host");
	conn->pghost = tmp ? strdup(tmp) : NULL;
	tmp = conninfo_getval("port");
	conn->pgport = tmp ? strdup(tmp) : NULL;
	tmp = conninfo_getval("tty");
	conn->pgtty = tmp ? strdup(tmp) : NULL;
	tmp = conninfo_getval("options");
	conn->pgoptions = tmp ? strdup(tmp) : NULL;
	tmp = conninfo_getval("dbname");
	conn->dbName = tmp ? strdup(tmp) : NULL;
	tmp = conninfo_getval("user");
	conn->pguser = tmp ? strdup(tmp) : NULL;
	tmp = conninfo_getval("password");
	conn->pgpass = tmp ? strdup(tmp) : NULL;

	/* ----------
	 * Free the connection info - all is in conn now
	 * ----------
	 */
	conninfo_free();

	/* ----------
	 * Connect to the database
	 * ----------
	 */
	conn->status = connectDB(conn);

	return conn;
}

/* ----------------
 *		PQconndefaults
 *
 * Parse an empty string like PQconnectdb() would do and return the
 * address of the connection options structure. Using this function
 * an application might determine all possible options and their
 * current default values.
 * ----------------
 */
PQconninfoOption *
PQconndefaults(void)
{
	PQExpBufferData  errorBuf;

	initPQExpBuffer(&errorBuf);
	conninfo_parse("", &errorBuf);
	termPQExpBuffer(&errorBuf);
	return PQconninfoOptions;
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
 *
 * ----------------
 */
PGconn *
PQsetdbLogin(const char *pghost, const char *pgport, const char *pgoptions, const char *pgtty, const char *dbName, const char *login, const char *pwd)
{
	PGconn	   *conn;
	char	   *tmp;

	/* An error message from some service we call. */
	bool		error = FALSE;

	/* We encountered an error that prevents successful completion */
	int			i;

	conn = makeEmptyPGconn();
	if (conn == NULL)
		return (PGconn *) NULL;

	if ((pghost == NULL) || pghost[0] == '\0')
	{
		if ((tmp = getenv("PGHOST")) != NULL)
			conn->pghost = strdup(tmp);
	}
	else
		conn->pghost = strdup(pghost);

	if ((pgport == NULL) || pgport[0] == '\0')
	{
		if ((tmp = getenv("PGPORT")) == NULL)
			tmp = DEF_PGPORT;
		conn->pgport = strdup(tmp);
	}
	else
		conn->pgport = strdup(pgport);

	if ((pgtty == NULL) || pgtty[0] == '\0')
	{
		if ((tmp = getenv("PGTTY")) == NULL)
			tmp = DefaultTty;
		conn->pgtty = strdup(tmp);
	}
	else
		conn->pgtty = strdup(pgtty);

	if ((pgoptions == NULL) || pgoptions[0] == '\0')
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
						  "FATAL: PQsetdbLogin(): Unable to determine a Postgres username!\n");
	}

	if (pwd)
		conn->pgpass = strdup(pwd);
	else if ((tmp = getenv("PGPASSWORD")) != NULL)
		conn->pgpass = strdup(tmp);
	else
		conn->pgpass = strdup(DefaultPassword);

	if ((dbName == NULL) || dbName[0] == '\0')
	{
		if ((tmp = getenv("PGDATABASE")) != NULL)
			conn->dbName = strdup(tmp);
		else if (conn->pguser)
			conn->dbName = strdup(conn->pguser);
	}
	else
		conn->dbName = strdup(dbName);

	if (conn->dbName)
	{

		/*
		 * if the database name is surrounded by double-quotes, then don't
		 * convert case
		 */
		if (*conn->dbName == '"')
		{
			strcpy(conn->dbName, conn->dbName + 1);
			conn->dbName[strlen(conn->dbName) - 1] = '\0';
		}
		else
			for (i = 0; conn->dbName[i]; i++)
				if (isascii((unsigned char) conn->dbName[i]) &&
					isupper(conn->dbName[i]))
					conn->dbName[i] = tolower(conn->dbName[i]);
	}

	if (error)
		conn->status = CONNECTION_BAD;
	else
		conn->status = connectDB(conn);

	return conn;
}


/*
 * update_db_info -
 * get all additional infos out of dbName
 *
 */
static int
update_db_info(PGconn *conn)
{
	char	   *tmp,
			   *old = conn->dbName;

	if (strchr(conn->dbName, '@') != NULL)
	{
		/* old style: dbname[@server][:port] */
		tmp = strrchr(conn->dbName, ':');
		if (tmp != NULL)		/* port number given */
		{
			conn->pgport = strdup(tmp + 1);
			*tmp = '\0';
		}

		tmp = strrchr(conn->dbName, '@');
		if (tmp != NULL)		/* host name given */
		{
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

			/*
			 * new style:
			 * <tcp|unix>:postgresql://server[:port][/dbname][?options]
			 */
			offset += strlen("postgresql://");

			tmp = strrchr(conn->dbName + offset, '?');
			if (tmp != NULL)	/* options given */
			{
				conn->pgoptions = strdup(tmp + 1);
				*tmp = '\0';
			}

			tmp = strrchr(conn->dbName + offset, '/');
			if (tmp != NULL)	/* database name given */
			{
				conn->dbName = strdup(tmp + 1);
				*tmp = '\0';
			}
			else
			{
				if ((tmp = getenv("PGDATABASE")) != NULL)
					conn->dbName = strdup(tmp);
				else if (conn->pguser)
					conn->dbName = strdup(conn->pguser);
			}

			tmp = strrchr(old + offset, ':');
			if (tmp != NULL)	/* port number given */
			{
				conn->pgport = strdup(tmp + 1);
				*tmp = '\0';
			}

			if (strncmp(old, "unix:", 5) == 0)
			{
				conn->pghost = NULL;
				if (strcmp(old + offset, "localhost") != 0)
				{
					printfPQExpBuffer(&conn->errorMessage,
									  "connectDB() -- non-tcp access only possible on localhost\n");
					return 1;
				}
			}
			else
				conn->pghost = strdup(old + offset);

			free(old);
		}
	}

	return 0;
}

/*
 * connectDB -
 * make a connection to the backend so it is ready to receive queries.
 * return CONNECTION_OK if successful, CONNECTION_BAD if not.
 *
 */
static ConnStatusType
connectDB(PGconn *conn)
{
	PGresult   *res;
	struct hostent *hp;
	StartupPacket sp;
	AuthRequest areq;
	SOCKET_SIZE_TYPE laddrlen;
	int			portno,
				family;
	char		beresp;
	int			on = 1;
#ifdef USE_SSL
	StartupPacket           np; /* Used to negotiate SSL connection */
	char                    SSLok;
	static int              allow_ssl_try = 1;  /* Allowed to do SSL negotiation */
	int                     tried_ssl = 0;      /* Set if SSL negotiation was tried */
#endif

	/*
	 * parse dbName to get all additional info in it, if any
	 */
	if (update_db_info(conn) != 0)
		goto connect_errReturn;

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
	 * Open a connection to postmaster/backend.
	 */

	if (conn->pghost != NULL)
	{
		hp = gethostbyname(conn->pghost);
		if ((hp == NULL) || (hp->h_addrtype != AF_INET))
		{
			printfPQExpBuffer(&conn->errorMessage,
							  "connectDB() --  unknown hostname: %s\n",
							  conn->pghost);
			goto connect_errReturn;
		}
		family = AF_INET;
	}
	else
	{
		hp = NULL;
		family = AF_UNIX;
	}

	MemSet((char *) &conn->raddr, 0, sizeof(conn->raddr));
	conn->raddr.sa.sa_family = family;

	portno = atoi(conn->pgport);
	if (family == AF_INET)
	{
		memmove((char *) &(conn->raddr.in.sin_addr),
				(char *) hp->h_addr,
				hp->h_length);
		conn->raddr.in.sin_port = htons((unsigned short) (portno));
		conn->raddr_len = sizeof(struct sockaddr_in);
	}
#if !defined(WIN32) && !defined(__CYGWIN32__)
	else
		conn->raddr_len = UNIXSOCK_PATH(conn->raddr.un, portno);
#endif


	/* Connect to the server  */
	if ((conn->sock = socket(family, SOCK_STREAM, 0)) < 0)
	{
		printfPQExpBuffer(&conn->errorMessage,
						  "connectDB() -- socket() failed: errno=%d\n%s\n",
						  errno, strerror(errno));
		goto connect_errReturn;
	}
	if (connect(conn->sock, &conn->raddr.sa, conn->raddr_len) < 0)
	{
		printfPQExpBuffer(&conn->errorMessage,
						  "connectDB() -- connect() failed: %s\n"
						  "Is the postmaster running%s at '%s' and accepting connections on %s '%s'?\n",
						  strerror(errno),
						  (family == AF_INET) ? " (with -i)" : "",
						  conn->pghost ? conn->pghost : "localhost",
						  (family == AF_INET) ? "TCP/IP port" : "Unix socket",
						  conn->pgport);
		goto connect_errReturn;
	}

	/* This needs to be done before we set into nonblocking, since SSL negotiation
	 * does not like that mode */

#ifdef USE_SSL
	/* Attempt to negotiate SSL usage */
	if (allow_ssl_try) {
	  tried_ssl = 1;
	  memset((char *)&np, 0, sizeof(np));
	  np.protoVersion = htonl(NEGOTIATE_SSL_CODE);
	  if (pqPacketSend(conn, (char *) &np, sizeof(StartupPacket)) != STATUS_OK)
	    {
	      sprintf(conn->errorMessage,
		      "connectDB() -- couldn't send SSL negotiation packet: errno=%d\n%s\n",
		      errno, strerror(errno));
	      goto connect_errReturn;
	    }
	  /* Now receive the backends response */
	  if (recv(conn->sock, &SSLok, 1, 0) != 1) {
	    sprintf(conn->errorMessage, "PQconnectDB() -- couldn't read backend response: errno=%d\n%s\n",
		    errno, strerror(errno));
	    goto connect_errReturn;
	  }
	  if (SSLok == 'S') {
	    if (!SSL_context) 
	      {
		SSL_load_error_strings();
		SSL_library_init();
		SSL_context = SSL_CTX_new(SSLv23_method());
		if (!SSL_context) {
		  sprintf(conn->errorMessage,
			  "connectDB() -- couldn't create SSL context: %s\n",
			  ERR_reason_error_string(ERR_get_error()));
		  goto connect_errReturn;
		}
	      }
	    if (!(conn->ssl = SSL_new(SSL_context)) ||
		!SSL_set_fd(conn->ssl, conn->sock) ||
		SSL_connect(conn->ssl) <= 0) 
	      {
		sprintf(conn->errorMessage,
			"connectDB() -- couldn't establish SSL connection: %s\n",
			ERR_reason_error_string(ERR_get_error()));
		goto connect_errReturn;
	      }
	    /* SSL connection finished. Continue to send startup packet */
	  }
	  else if (SSLok == 'E') {
	    /* Received error - probably protocol mismatch */
	    if (conn->Pfdebug)
	      fprintf(conn->Pfdebug, "Backend reports error, attempting fallback to pre-6.6.\n");
	    close(conn->sock);
	    allow_ssl_try = 0;
	    return connectDB(conn);
	  }
	  else if (SSLok != 'N') {
	    strcpy(conn->errorMessage,
		   "Received invalid negotiation response.\n");
	    goto connect_errReturn;
	  }
	}
	else
	  allow_ssl_try = 1; /* We'll allow an attempt to use SSL next time */
#endif

	/*
	 * Set the right options. We need nonblocking I/O, and we don't want
	 * delay of outgoing data.
	 */

#ifndef WIN32
	if (fcntl(conn->sock, F_SETFL, O_NONBLOCK) < 0)
#else
	if (ioctlsocket(conn->sock, FIONBIO, &on) != 0)
#endif
	{
		printfPQExpBuffer(&conn->errorMessage,
						  "connectDB() -- fcntl() failed: errno=%d\n%s\n",
						  errno, strerror(errno));
		goto connect_errReturn;
	}

	if (family == AF_INET)
	{
		struct protoent *pe;

		pe = getprotobyname("TCP");
		if (pe == NULL)
		{
			printfPQExpBuffer(&conn->errorMessage,
							  "connectDB(): getprotobyname failed\n");
			goto connect_errReturn;
		}
		if (setsockopt(conn->sock, pe->p_proto, TCP_NODELAY,
#ifdef WIN32
					   (char *)
#endif
					   &on,
					   sizeof(on)) < 0)
		{
			printfPQExpBuffer(&conn->errorMessage,
							  "connectDB() -- setsockopt failed: errno=%d\n%s\n",
							  errno, strerror(errno));
#ifdef WIN32
			printf("Winsock error: %i\n", WSAGetLastError());
#endif
			goto connect_errReturn;
		}
	}

	/* Fill in the client address */
	laddrlen = sizeof(conn->laddr);
	if (getsockname(conn->sock, &conn->laddr.sa, &laddrlen) < 0)
	{
		printfPQExpBuffer(&conn->errorMessage,
						  "connectDB() -- getsockname() failed: errno=%d\n%s\n",
						  errno, strerror(errno));
		goto connect_errReturn;
	}

	/* Ensure our buffers are empty */
	conn->inStart = conn->inCursor = conn->inEnd = 0;
	conn->outCount = 0;

	/* Send the startup packet. */

	if (pqPacketSend(conn, (char *) &sp, sizeof(StartupPacket)) != STATUS_OK)
	{
		printfPQExpBuffer(&conn->errorMessage,
						  "connectDB() --  couldn't send startup packet: errno=%d\n%s\n",
						  errno, strerror(errno));
		goto connect_errReturn;
	}

	/*
	 * Perform the authentication exchange: wait for backend messages and
	 * respond as necessary. We fall out of this loop when done talking to
	 * the postmaster.
	 */

	for (;;)
	{
		/* Wait for some data to arrive (or for the channel to close) */
		if (pqWait(TRUE, FALSE, conn))
			goto connect_errReturn;
		/* Load data, or detect EOF */
		if (pqReadData(conn) < 0)
			goto connect_errReturn;

		/*
		 * Scan the message. If we run out of data, loop around to try
		 * again.
		 */
		conn->inCursor = conn->inStart;

		if (pqGetc(&beresp, conn))
			continue;			/* no data yet */

		/* Handle errors. */
		if (beresp == 'E')
		{
			if (pqGets(&conn->errorMessage, conn))
				continue;
			goto connect_errReturn;
		}

		/* Otherwise it should be an authentication request. */
		if (beresp != 'R')
		{
			printfPQExpBuffer(&conn->errorMessage,
							  "connectDB() -- expected authentication request\n");
			goto connect_errReturn;
		}

		/* Get the type of request. */
		if (pqGetInt((int *) &areq, 4, conn))
			continue;

		/* Get the password salt if there is one. */
		if (areq == AUTH_REQ_CRYPT)
		{
			if (pqGetnchar(conn->salt, sizeof(conn->salt), conn))
				continue;
		}

		/* OK, we successfully read the message; mark data consumed */
		conn->inStart = conn->inCursor;

		/* Respond to the request if necessary. */
		/* fe-auth.c has not been fixed to support PQExpBuffers, so: */
		if (fe_sendauth(areq, conn, conn->pghost, conn->pgpass,
						conn->errorMessage.data) != STATUS_OK)
		{
			conn->errorMessage.len = strlen(conn->errorMessage.data);
			goto connect_errReturn;
		}

		if (pqFlush(conn))
			goto connect_errReturn;

		/* Are we done? */
		if (areq == AUTH_REQ_OK)
			break;
	}

	/*
	 * Now we expect to hear from the backend. A ReadyForQuery message
	 * indicates that startup is successful, but we might also get an
	 * Error message indicating failure. (Notice messages indicating
	 * nonfatal warnings are also allowed by the protocol, as is a
	 * BackendKeyData message.) Easiest way to handle this is to let
	 * PQgetResult() read the messages. We just have to fake it out about
	 * the state of the connection.
	 */

	conn->status = CONNECTION_OK;
	conn->asyncStatus = PGASYNC_BUSY;
	res = PQgetResult(conn);
	/* NULL return indicating we have gone to IDLE state is expected */
	if (res)
	{
		if (res->resultStatus != PGRES_FATAL_ERROR)
			printfPQExpBuffer(&conn->errorMessage,
							  "connectDB() -- unexpected message during startup\n");
		PQclear(res);
		goto connect_errReturn;
	}

	/*
	 * Post-connection housekeeping. Send environment variables to server
	 */

	PQsetenv(conn);

	return CONNECTION_OK;

connect_errReturn:
	if (conn->sock >= 0)
	{
#ifdef WIN32
		closesocket(conn->sock);
#else
		close(conn->sock);
#endif
		conn->sock = -1;
	}
	return CONNECTION_BAD;

}

void
PQsetenv(PGconn *conn)
{
	struct EnvironmentOptions *eo;
	char		setQuery[100];	/* note length limits in sprintf's below */
	const char *val;
	PGresult   *res;
#ifdef MULTIBYTE
	char	   *envname = "PGCLIENTENCODING";

	/* Set env. variable PGCLIENTENCODING if it's not set already */
	val = getenv(envname);
	if (!val || *val == '\0')
	{
		const char *encoding = NULL;

		/* query server encoding */
		res = PQexec(conn, "select getdatabaseencoding()");
		if (res && PQresultStatus(res) == PGRES_TUPLES_OK)
			encoding = PQgetvalue(res, 0, 0);
		if (!encoding)			/* this should not happen */
			encoding = pg_encoding_to_char(MULTIBYTE);
		if (encoding)
		{
			/* set client encoding via environment variable */
			char	   *envbuf;

			envbuf = (char *) malloc(strlen(envname) + strlen(encoding) + 2);
			sprintf(envbuf, "%s=%s", envname, encoding);
			putenv(envbuf);
		}
		PQclear(res);
	}
#endif

	for (eo = EnvironmentOptions; eo->envName; eo++)
	{
		if ((val = getenv(eo->envName)))
		{
			if (strcasecmp(val, "default") == 0)
				sprintf(setQuery, "SET %s = %.60s", eo->pgName, val);
			else
				sprintf(setQuery, "SET %s = '%.60s'", eo->pgName, val);
#ifdef CONNECTDEBUG
			printf("Use environment variable %s to send %s\n", eo->envName, setQuery);
#endif
			res = PQexec(conn, setQuery);
			PQclear(res);		/* Don't care? */
		}
	}
}	/* PQsetenv() */

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

	/* Zero all pointers */
	MemSet((char *) conn, 0, sizeof(PGconn));

	conn->noticeHook = defaultNoticeProcessor;
	conn->status = CONNECTION_BAD;
	conn->asyncStatus = PGASYNC_IDLE;
	conn->notifyList = DLNewList();
	conn->sock = -1;
	/*
	 * The output buffer size is set to 8K, which is the usual size of pipe
	 * buffers on Unix systems.  That way, when we are sending a large
	 * amount of data, we avoid incurring extra kernel context swaps for
	 * partial bufferloads.  Note that we currently don't ever enlarge
	 * the output buffer.
	 *
	 * With the same goal of minimizing context swaps, the input buffer will
	 * be enlarged anytime it has less than 8K free, so we initially allocate
	 * twice that.
	 */
	conn->inBufSize = 16 * 1024;
	conn->inBuffer = (char *) malloc(conn->inBufSize);
	conn->outBufSize = 8 * 1024;
	conn->outBuffer = (char *) malloc(conn->outBufSize);
	initPQExpBuffer(&conn->errorMessage);
	initPQExpBuffer(&conn->workBuffer);
	if (conn->inBuffer == NULL ||
		conn->outBuffer == NULL ||
		conn->errorMessage.data == NULL ||
		conn->workBuffer.data == NULL)
	{
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
#ifdef USE_SSL
	if (conn->ssl)
	  SSL_free(conn->ssl);
#endif
	if (conn->sock >= 0)
#ifdef WIN32
		closesocket(conn->sock);
#else
		close(conn->sock);
#endif
	if (conn->pghost)
		free(conn->pghost);
	if (conn->pgport)
		free(conn->pgport);
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
	if (conn->sock >= 0)
	{

		/*
		 * Try to send "close connection" message to backend. Ignore any
		 * error. Note: this routine used to go to substantial lengths to
		 * avoid getting SIGPIPE'd if the connection were already closed.
		 * Now we rely on pqFlush to avoid the signal.
		 */
		(void) pqPuts("X", conn);
		(void) pqFlush(conn);
	}

	/*
	 * Close the connection, reset all transient state, flush I/O buffers.
	 */
	if (conn->sock >= 0)
#ifdef WIN32
		closesocket(conn->sock);
#else
		close(conn->sock);
#endif
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
		conn->status = connectDB(conn);
	}
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
 *
 * NOTE: this routine must not generate any error message longer than
 * INITIAL_EXPBUFFER_SIZE (currently 256), since we dare not try to
 * expand conn->errorMessage!
 */

int
PQrequestCancel(PGconn *conn)
{
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
	if (connect(tmpsock, &conn->raddr.sa, conn->raddr_len) < 0)
	{
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

	if (send(tmpsock, (char *) &crp, sizeof(crp), 0) != (int) sizeof(crp))
	{
		strcpy(conn->errorMessage.data,
			   "PQrequestCancel() -- send() failed: ");
		goto cancel_errReturn;
	}

	/* Sent it, done */
#ifdef WIN32
	closesocket(tmpsock);
#else
	close(tmpsock);
#endif

	return TRUE;

cancel_errReturn:
	strcat(conn->errorMessage.data, strerror(errno));
	strcat(conn->errorMessage.data, "\n");
	conn->errorMessage.len = strlen(conn->errorMessage.data);
	if (tmpsock >= 0)
	{
#ifdef WIN32
		closesocket(tmpsock);
#else
		close(tmpsock);
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


/* ----------------
 * Conninfo parser routine
 * ----------------
 */
static int
conninfo_parse(const char *conninfo, PQExpBuffer errorMessage)
{
	char	   *pname;
	char	   *pval;
	char	   *buf;
	char	   *tmp;
	char	   *cp;
	char	   *cp2;
	PQconninfoOption *option;
	char		errortmp[INITIAL_EXPBUFFER_SIZE];

	conninfo_free();

	if ((buf = strdup(conninfo)) == NULL)
	{
		printfPQExpBuffer(errorMessage,
		  "FATAL: cannot allocate memory for copy of conninfo string\n");
		return -1;
	}
	cp = buf;

	while (*cp)
	{
		/* Skip blanks before the parameter name */
		if (isspace(*cp))
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
			if (isspace(*cp))
			{
				*cp++ = '\0';
				while (*cp)
				{
					if (!isspace(*cp))
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
				"ERROR: PQconnectdb() - Missing '=' after '%s' in conninfo\n",
							  pname);
			free(buf);
			return -1;
		}
		*cp++ = '\0';

		/* Skip blanks after the '=' */
		while (*cp)
		{
			if (!isspace(*cp))
				break;
			cp++;
		}

		pval = cp;

		if (*cp != '\'')
		{
			cp2 = pval;
			while (*cp)
			{
				if (isspace(*cp))
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
							"ERROR: PQconnectdb() - unterminated quoted string in conninfo\n");
					free(buf);
					return -1;
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

		/* ----------
		 * Now we have the name and the value. Search
		 * for the param record.
		 * ----------
		 */
		for (option = PQconninfoOptions; option->keyword != NULL; option++)
		{
			if (!strcmp(option->keyword, pname))
				break;
		}
		if (option->keyword == NULL)
		{
			printfPQExpBuffer(errorMessage,
							  "ERROR: PQconnectdb() - unknown option '%s'\n",
							  pname);
			free(buf);
			return -1;
		}

		/* ----------
		 * Store the value
		 * ----------
		 */
		option->val = strdup(pval);
	}

	free(buf);

	/* ----------
	 * Get the fallback resources for parameters not specified
	 * in the conninfo string.
	 * ----------
	 */
	for (option = PQconninfoOptions; option->keyword != NULL; option++)
	{
		if (option->val != NULL)
			continue;			/* Value was in conninfo */

		/* ----------
		 * Try to get the environment variable fallback
		 * ----------
		 */
		if (option->envvar != NULL)
		{
			if ((tmp = getenv(option->envvar)) != NULL)
			{
				option->val = strdup(tmp);
				continue;
			}
		}

		/* ----------
		 * No environment variable specified or this one isn't set -
		 * try compiled in
		 * ----------
		 */
		if (option->compiled != NULL)
		{
			option->val = strdup(option->compiled);
			continue;
		}

		/* ----------
		 * Special handling for user
		 * ----------
		 */
		if (!strcmp(option->keyword, "user"))
		{
			option->val = fe_getauthname(errortmp);
			/* note any error message is thrown away */
			continue;
		}

		/* ----------
		 * Special handling for dbname
		 * ----------
		 */
		if (!strcmp(option->keyword, "dbname"))
		{
			tmp = conninfo_getval("user");
			if (tmp)
				option->val = strdup(tmp);
			continue;
		}
	}

	return 0;
}


static char *
conninfo_getval(char *keyword)
{
	PQconninfoOption *option;

	for (option = PQconninfoOptions; option->keyword != NULL; option++)
	{
		if (!strcmp(option->keyword, keyword))
			return option->val;
	}

	return NULL;
}


static void
conninfo_free()
{
	PQconninfoOption *option;

	for (option = PQconninfoOptions; option->keyword != NULL; option++)
	{
		if (option->val != NULL)
		{
			free(option->val);
			option->val = NULL;
		}
	}
}

/* =========== accessor functions for PGconn ========= */
const char *
PQdb(const PGconn *conn)
{
	if (!conn)
		return (char *) NULL;
	return conn->dbName;
}

const char *
PQuser(const PGconn *conn)
{
	if (!conn)
		return (char *) NULL;
	return conn->pguser;
}

const char *
PQpass(const PGconn *conn)
{
	if (!conn)
		return (char *) NULL;
	return conn->pgpass;
}

const char *
PQhost(const PGconn *conn)
{
	if (!conn)
		return (char *) NULL;
	return conn->pghost;
}

const char *
PQport(const PGconn *conn)
{
	if (!conn)
		return (char *) NULL;
	return conn->pgport;
}

const char *
PQtty(const PGconn *conn)
{
	if (!conn)
		return (char *) NULL;
	return conn->pgtty;
}

const char *
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

const char *
PQerrorMessage(const PGconn *conn)
{
	static char noConn[] = "PQerrorMessage: conn pointer is NULL\n";

	if (!conn)
		return noConn;
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
	if (proc) {
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
	/* Note: we expect the supplied string to end with a newline already. */
	fprintf(stderr, "%s", message);
}
