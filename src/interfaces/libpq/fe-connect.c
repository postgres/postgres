/*-------------------------------------------------------------------------
 *
 * fe-connect.c
 *	  functions related to setting up a connection to the backend
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/interfaces/libpq/fe-connect.c,v 1.116 2000/01/26 05:58:45 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include <sys/types.h>
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
#include <sys/socket.h>
#include <unistd.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
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

#ifdef WIN32
static int inet_aton(const char *cp, struct in_addr *inp) {
	unsigned long a = inet_addr(cp);
	if (a == -1)
		return 0;
	inp->s_addr = a;	
	return 1;
}
#endif

/* ----------
 *     pg_setenv_state
 * A struct used when polling a setenv request. This is referred to externally
 * using a PGsetenvHandle.
 * ----------
 */
struct pg_setenv_state
{
	enum
	{
		SETENV_STATE_OPTION_SEND,    /* About to send an Environment Option */
		SETENV_STATE_OPTION_WAIT,    /* Waiting for above send to complete  */
#ifdef MULTIBYTE
		SETENV_STATE_ENCODINGS_SEND, /* About to send an "encodings" query  */
		SETENV_STATE_ENCODINGS_WAIT, /* Waiting for query to complete       */
#endif
		SETENV_STATE_OK,
		SETENV_STATE_FAILED
	} state;
	PGconn *conn;
	PGresult *res;
	struct EnvironmentOptions *eo;
} ;

static int connectDBStart(PGconn *conn);
static int connectDBComplete(PGconn *conn);

#ifdef USE_SSL
static SSL_CTX *SSL_context = NULL;
#endif

static PGconn *makeEmptyPGconn(void);
static void freePGconn(PGconn *conn);
static void closePGconn(PGconn *conn);
static int	conninfo_parse(const char *conninfo, PQExpBuffer errorMessage);
static char *conninfo_getval(char *keyword);
static void conninfo_free(void);
static void defaultNoticeProcessor(void *arg, const char *message);

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

	{"hostaddr", "PGHOSTADDR", NULL, NULL,
	 "Database-Host-IPv4-Address", "", 15}, /* Room for abc.def.ghi.jkl */

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
 *      Connecting to a Database
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
 * 
 * ----------------
 */

/* ----------------
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
 *
 * ----------------
 */
PGconn *
PQconnectdb(const char *conninfo)
{
	PGconn *conn = PQconnectStart(conninfo);

	if (conn && conn->status != CONNECTION_BAD)
		(void) connectDBComplete(conn);

	return conn;
}

/* ----------------
 *		PQconnectStart
 *
 * Begins the establishment of a connection to a postgres backend through the
 * postmaster using connection information in a string.
 *
 * See comment for PQconnectdb for the definition of the string format.
 *
 * Returns a PGconn*.  If NULL is returned, a malloc error has occurred, and
 * you should not attempt to proceed with this connection.  If the status
 * field of the connection returned is CONNECTION_BAD, an error has
 * occurred. In this case you should call PQfinish on the result, (perhaps
 * inspecting the error message first).  Other fields of the structure may not
 * be valid if that occurs.  If the status field is not CONNECTION_BAD, then
 * this stage has succeeded - call PQconnectPoll, using select(2) to see when
 * this is necessary.
 *
 * See PQconnectPoll for more info.
 *
 * ----------------
 */
PGconn *
PQconnectStart(const char *conninfo)
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
	tmp = conninfo_getval("hostaddr");
	conn->pghostaddr = tmp ? strdup(tmp) : NULL;
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
	if (!connectDBStart(conn))
	{
		/* Just in case we failed to set it in connectDBStart */
		conn->status = CONNECTION_BAD;
	}

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
PQsetdbLogin(const char *pghost, const char *pgport, const char *pgoptions,
			 const char *pgtty, const char *dbName, const char *login,
			 const char *pwd)
{
	PGconn	   *conn;
	char	   *tmp;	/* An error message from some service we call. */
	bool		error = FALSE;	/* We encountered an error. */

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

	if (error)
	{
		conn->status = CONNECTION_BAD;
	}
	else
	{
		if (connectDBStart(conn))
			(void) connectDBComplete(conn);
	}
		
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
									  "connectDBStart() -- "
									  "non-tcp access only possible on "
									  "localhost\n");
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

/* ----------
 * connectMakeNonblocking -
 * Make a connection non-blocking.
 * Returns 1 if successful, 0 if not.
 * ----------
 */
static int
connectMakeNonblocking(PGconn *conn)
{
#ifndef WIN32
	if (fcntl(conn->sock, F_SETFL, O_NONBLOCK) < 0)
#else
	if (ioctlsocket(conn->sock, FIONBIO, &on) != 0)
#endif
	{
		printfPQExpBuffer(&conn->errorMessage,
						  "connectMakeNonblocking -- fcntl() failed: errno=%d\n%s\n",
						  errno, strerror(errno));
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
	struct protoent *pe;
	int			on = 1;

	pe = getprotobyname("TCP");
	if (pe == NULL)
	{
		printfPQExpBuffer(&conn->errorMessage,
						  "connectNoDelay() -- "
						  "getprotobyname failed: errno=%d\n%s\n",
						  errno, strerror(errno));
		return 0;
	}
	if (setsockopt(conn->sock, pe->p_proto, TCP_NODELAY,
#ifdef WIN32
				   (char *)
#endif
				   &on,
				   sizeof(on)) < 0)
	{
		printfPQExpBuffer(&conn->errorMessage,
						  "connectNoDelay() -- setsockopt failed: errno=%d\n%s\n",
						  errno, strerror(errno));
#ifdef WIN32
		printf("Winsock error: %i\n", WSAGetLastError());
#endif
		return 0;
	}

	return 1;
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
	int			portno,
				family;
#ifdef USE_SSL
	StartupPacket           np; /* Used to negotiate SSL connection */
	char                    SSLok;
#endif

	if (!conn)
		return 0;
	/*
	 * parse dbName to get all additional info in it, if any
	 */
	if (update_db_info(conn) != 0)
		goto connect_errReturn;

	/* Ensure our buffers are empty */
	conn->inStart = conn->inCursor = conn->inEnd = 0;
	conn->outCount = 0;

	/*
	 * Set up the connection to postmaster/backend.
	 * Note that this supports IPv4 and UDP only.
	 */

	MemSet((char *) &conn->raddr, 0, sizeof(conn->raddr));

	if (conn->pghostaddr != NULL)
	{
		/* Using pghostaddr avoids a hostname lookup */
		/* Note that this supports IPv4 only */
		struct in_addr addr;

		if(!inet_aton(conn->pghostaddr, &addr))
		{
			printfPQExpBuffer(&conn->errorMessage,
							  "connectDBStart() -- "
							  "invalid host address: %s\n", conn->pghostaddr);
			goto connect_errReturn;
		}

		family = AF_INET;

		memmove((char *) &(conn->raddr.in.sin_addr),
				(char *) &addr, sizeof(addr));
	}
	else if (conn->pghost != NULL)
	{
		/* Using pghost, so we have to look-up the hostname */
		struct hostent *hp;

		hp = gethostbyname(conn->pghost);
		if ((hp == NULL) || (hp->h_addrtype != AF_INET))
		{
			printfPQExpBuffer(&conn->errorMessage,
							  "connectDBStart() --  unknown hostname: %s\n",
							  conn->pghost);
			goto connect_errReturn;
		}
		family = AF_INET;

		memmove((char *) &(conn->raddr.in.sin_addr),
				(char *) hp->h_addr,
				hp->h_length);
	}
	else
	{
		/* pghostaddr and pghost are NULL, so use UDP */
		family = AF_UNIX;
	}

	/* Set family */
	conn->raddr.sa.sa_family = family;

	/* Set port number */
	portno = atoi(conn->pgport);
	if (family == AF_INET)
	{
		conn->raddr.in.sin_port = htons((unsigned short) (portno));
		conn->raddr_len = sizeof(struct sockaddr_in);
	}
#if !defined(WIN32) && !defined(__CYGWIN32__)
		else
			conn->raddr_len = UNIXSOCK_PATH(conn->raddr.un, portno);
#endif


	/* Open a socket */
	if ((conn->sock = socket(family, SOCK_STREAM, 0)) < 0)
	{
		printfPQExpBuffer(&conn->errorMessage,
						  "connectDBStart() -- "
						  "socket() failed: errno=%d\n%s\n",
						  errno, strerror(errno));
		goto connect_errReturn;
	}

	/* ----------
	 * Set the right options. Normally, we need nonblocking I/O, and we don't
	 * want delay of outgoing data for AF_INET sockets.  If we are using SSL,
	 * then we need the blocking I/O (XXX Can this be fixed?).
	 * ---------- */

	if (family == AF_INET)
	{
		if (!connectNoDelay(conn))
			goto connect_errReturn;
	}

	/* ----------
	 * Since I have no idea whether this is a valid thing to do under Windows
	 * before a connection is made, and since I have no way of testing it, I
	 * leave the code looking as below.  When someone decides that they want
	 * non-blocking connections under Windows, they can define
	 * WIN32_NON_BLOCKING_CONNECTIONS before compilation.  If it works, then
	 * this code can be cleaned up.
	 *
	 *   Ewan Mellor <eem21@cam.ac.uk>.
	 * ---------- */
#if (!defined(WIN32) || defined(WIN32_NON_BLOCKING_CONNECTIONS)) && !defined(USE_SSL)
	if (connectMakeNonblocking(conn) == 0)
		goto connect_errReturn;
#endif	

#ifdef USE_SSL
	/* This needs to be done before we set into nonblocking, since SSL
	 * negotiation does not like that mode */

	/* Attempt to negotiate SSL usage */
	if (conn->allow_ssl_try) {
	  memset((char *)&np, 0, sizeof(np));
	  np.protoVersion = htonl(NEGOTIATE_SSL_CODE);
	  if (pqPacketSend(conn, (char *) &np, sizeof(StartupPacket)) != STATUS_OK)
	    {
	      sprintf(conn->errorMessage,
		      "connectDB() -- couldn't send SSL negotiation packet: errno=%d\n%s\n",
		      errno, strerror(errno));
	      goto connect_errReturn;
	    }
	  /* Now receive the postmasters response */
	  if (recv(conn->sock, &SSLok, 1, 0) != 1) {
	    sprintf(conn->errorMessage, "PQconnectDB() -- couldn't read postmaster response: errno=%d\n%s\n",
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
	      fprintf(conn->Pfdebug, "Postmaster reports error, attempting fallback to pre-6.6.\n");
	    close(conn->sock);
	    conn->allow_ssl_try = FALSE;
	    return connectDBStart(conn);
	  }
	  else if (SSLok != 'N') {
	    strcpy(conn->errorMessage,
		   "Received invalid negotiation response.\n");
	    goto connect_errReturn;
	  }
	}
#endif

	/* ----------
     * Start / make connection.  We are hopefully in non-blocking mode
	 * now, but it is possible that:
	 *   1. Older systems will still block on connect, despite the
	 *      non-blocking flag. (Anyone know if this is true?)
	 *   2. We are running under Windows, and aren't even trying
	 *      to be non-blocking (see above).
	 *   3. We are using SSL.
	 * Thus, we have make arrangements for all eventualities.
	 * ----------
	 */
	if (connect(conn->sock, &conn->raddr.sa, conn->raddr_len) < 0)
	{
#ifndef WIN32
		if (errno == EINPROGRESS)
#else
		if (WSAGetLastError() == WSAEINPROGRESS)
#endif
		{
			/* This is fine - we're in non-blocking mode, and the
			 * connection is in progress. */
			conn->status = CONNECTION_STARTED;
		}
		else
		{
			/* Something's gone wrong */
			printfPQExpBuffer(&conn->errorMessage,
							  "connectDBStart() -- connect() failed: %s\n"
							  "\tIs the postmaster running%s at '%s'\n"
							  "\tand accepting connections on %s '%s'?\n",
							  strerror(errno),
							  (family == AF_INET) ? " (with -i)" : "",
							  conn->pghost ? conn->pghost : "localhost",
							  (family == AF_INET) ?
							  "TCP/IP port" : "Unix socket",
							  conn->pgport);
			goto connect_errReturn;
		}
	}
	else
	{
		/* We're connected already */
		conn->status = CONNECTION_MADE;
	}

	/* This makes the connection non-blocking, for all those cases which forced us
	   not to do it above. */
#if (defined(WIN32) && !defined(WIN32_NON_BLOCKING_CONNECTIONS)) || defined(USE_SSL)
	if (connectMakeNonblocking(conn) == 0)
		goto connect_errReturn;
#endif	

	return 1;

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
	conn->status = CONNECTION_BAD;

	return 0;
}


/* ----------------
 *		connectDBComplete
 *
 * Block and complete a connection.
 *
 * Returns 1 on success, 0 on failure.
 * ----------------
 */
static int
connectDBComplete(PGconn *conn)
{
	PostgresPollingStatusType flag = PGRES_POLLING_WRITING;

	if (conn == NULL || conn->status == CONNECTION_BAD)
		return 0;

	for (;;) {
		/*
		 * Wait, if necessary.  Note that the initial state (just after
		 * PQconnectStart) is to wait for the socket to select for writing.
		 */
		switch (flag)
		{
			case PGRES_POLLING_ACTIVE:
				break;

			case PGRES_POLLING_OK:
				return 1;		/* success! */
				
			case PGRES_POLLING_READING:
				if (pqWait(1, 0, conn))
				{
					conn->status = CONNECTION_BAD;
					return 0;
				}
				break;

			case PGRES_POLLING_WRITING:
				if (pqWait(0, 1, conn))
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
 * Before calling this function, use select(2) to determine when data arrive.
 * 
 * You must call PQfinish whether or not this fails.
 *
 * This function and PQconnectStart are intended to allow connections to be
 * made without blocking the execution of your program on remote I/O. However,
 * there are a number of caveats:
 *
 *   o  If you call PQtrace, ensure that the stream object into which you trace
 *      will not block.
 *   o  If you do not supply an IP address for the remote host (i.e. you 
 *      supply a host name instead) then this function will block on
 *      gethostbyname.  You will be fine if using Unix sockets (i.e. by
 *      supplying neither a host name nor a host address).
 *   o  If your backend wants to use Kerberos authentication then you must
 *      supply both a host name and a host address, otherwise this function
 *      may block on gethostname.
 *   o  This function will block if compiled with USE_SSL.
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
		/* We really shouldn't have been polled in these two cases, but
		   we can handle it. */
		case CONNECTION_BAD:
			return PGRES_POLLING_FAILED;
		case CONNECTION_OK:
			return PGRES_POLLING_OK;

		/* These are reading states */
		case CONNECTION_AWAITING_RESPONSE:
		case CONNECTION_AUTH_OK:
		{
			/* Load waiting data */
			int n = pqReadData(conn);
				
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
							  "PQconnectPoll() -- unknown connection state - "
							  "probably indicative of memory corruption!\n");
			goto error_return;
	}


 keep_going: /* We will come back to here until there is nothing left to
				parse. */
	switch(conn->status)
	{
		case CONNECTION_STARTED:
		{
			SOCKET_SIZE_TYPE laddrlen;
#ifndef WIN32
			int optval;
#else
			char optval;
#endif
			int optlen = sizeof(optval);

			/* Write ready, since we've made it here, so the connection
			 * has been made. */

			/* Now check (using getsockopt) that there is not an error
			   state waiting for us on the socket. */

			if (getsockopt(conn->sock, SOL_SOCKET, SO_ERROR,
						   &optval, &optlen) == -1)
			{
				printfPQExpBuffer(&conn->errorMessage,
								  "PQconnectPoll() -- getsockopt() failed: "
								  "errno=%d\n%s\n",
								  errno, strerror(errno));
				goto error_return;
			}
			else if (optval != 0)
			{
				/*
				 * When using a nonblocking connect, we will typically see
				 * connect failures at this point, so provide a friendly
				 * error message.
				 */
				printfPQExpBuffer(&conn->errorMessage,
								  "PQconnectPoll() -- connect() failed: %s\n"
								  "\tIs the postmaster running%s at '%s'\n"
								  "\tand accepting connections on %s '%s'?\n",
								  strerror(optval),
								  (conn->raddr.sa.sa_family == AF_INET) ? " (with -i)" : "",
								  conn->pghost ? conn->pghost : "localhost",
								  (conn->raddr.sa.sa_family == AF_INET) ?
								  "TCP/IP port" : "Unix socket",
								  conn->pgport);
				goto error_return;
			}

			/* Fill in the client address */
			laddrlen = sizeof(conn->laddr);
			if (getsockname(conn->sock, &conn->laddr.sa, &laddrlen) < 0)
			{
				printfPQExpBuffer(&conn->errorMessage,
								  "PQconnectPoll() -- getsockname() failed: "
								  "errno=%d\n%s\n",
								  errno, strerror(errno));
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

			/* Send the startup packet.
			 *
			 * Theoretically, this could block, but it really shouldn't
			 * since we only got here if the socket is write-ready.
			 */

			if (pqPacketSend(conn, (char *) &sp,
							 sizeof(StartupPacket)) != STATUS_OK)
			{
				printfPQExpBuffer(&conn->errorMessage,
								  "PQconnectPoll() --  "
								  "couldn't send startup packet: "
								  "errno=%d\n%s\n",
								  errno, strerror(errno));
				goto error_return;
			}

			conn->status = CONNECTION_AWAITING_RESPONSE;
			return PGRES_POLLING_READING;
		}

		/*
		 * Handle the authentication exchange: wait for postmaster messages
		 * and respond as necessary.
		 */
		case CONNECTION_AWAITING_RESPONSE:
		{
			char		beresp;
			AuthRequest areq;

			/* Scan the message from current point (note that if we find
			 * the message is incomplete, we will return without advancing
			 * inStart, and resume here next time).
			 */
			conn->inCursor = conn->inStart;

			if (pqGetc(&beresp, conn))
			{
				/* We'll come back when there are more data */
				return PGRES_POLLING_READING;
			}

			/* Handle errors. */
			if (beresp == 'E')
			{
				if (pqGets(&conn->errorMessage, conn))
				{
					/* We'll come back when there are more data */
					return PGRES_POLLING_READING;
				}
				/* OK, we read the message; mark data consumed */
				conn->inStart = conn->inCursor;
				/* The postmaster typically won't end its message with a
				 * newline, so add one to conform to libpq conventions.
				 */
				appendPQExpBufferChar(&conn->errorMessage, '\n');
				goto error_return;
			}

			/* Otherwise it should be an authentication request. */
			if (beresp != 'R')
			{
				printfPQExpBuffer(&conn->errorMessage,
								  "PQconnectPoll() -- expected "
								  "authentication request\n");
				goto error_return;
			}

			/* Get the type of request. */
			if (pqGetInt((int *) &areq, 4, conn))
			{
				/* We'll come back when there are more data */
				return PGRES_POLLING_READING;
			}

			/* Get the password salt if there is one. */
			if (areq == AUTH_REQ_CRYPT)
			{
				if (pqGetnchar(conn->salt, sizeof(conn->salt), conn))
				{
					/* We'll come back when there are more data */
					return PGRES_POLLING_READING;
				}
			}

			/* OK, we successfully read the message; mark data consumed */
			conn->inStart = conn->inCursor;

			/* Respond to the request if necessary. */
			/* Note that conn->pghost must be non-NULL if we are going to
			 * avoid the Kerberos code doing a hostname look-up. */
			/* XXX fe-auth.c has not been fixed to support PQExpBuffers, so: */
			if (fe_sendauth(areq, conn, conn->pghost, conn->pgpass,
							conn->errorMessage.data) != STATUS_OK)
			{
				conn->errorMessage.len = strlen(conn->errorMessage.data);
				goto error_return;
			}
			conn->errorMessage.len = strlen(conn->errorMessage.data);

			/* Just make sure that any data sent by fe_sendauth is flushed
			 * out.  Although this theoretically could block, it really
			 * shouldn't since we don't send large auth responses.
			 */
			if (pqFlush(conn))
				goto error_return;

			if (areq == AUTH_REQ_OK)
			{
				/* We are done with authentication exchange */
				conn->status = CONNECTION_AUTH_OK;
				/* Set asyncStatus so that PQsetResult will think that what
				 * comes back next is the result of a query.  See below.  */
				conn->asyncStatus = PGASYNC_BUSY;
			}

			/* Look to see if we have more data yet. */
			goto keep_going;
		}

		case CONNECTION_AUTH_OK:
		{
			/* ----------
			 * Now we expect to hear from the backend. A ReadyForQuery
			 * message indicates that startup is successful, but we might
			 * also get an Error message indicating failure. (Notice
			 * messages indicating nonfatal warnings are also allowed by
			 * the protocol, as is a BackendKeyData message.) Easiest way
			 * to handle this is to let PQgetResult() read the messages. We
			 * just have to fake it out about the state of the connection,
			 * by setting asyncStatus = PGASYNC_BUSY (done above).
			 *----------
			 */

			if (PQisBusy(conn))
				return PGRES_POLLING_READING;
			
			res = PQgetResult(conn);
			/* NULL return indicating we have gone to
			   IDLE state is expected */
			if (res)
			{
				if (res->resultStatus != PGRES_FATAL_ERROR)
					printfPQExpBuffer(&conn->errorMessage,
									  "PQconnectPoll() -- unexpected message "
									  "during startup\n");
				/* if the resultStatus is FATAL, then conn->errorMessage
				 * already has a copy of the error; needn't copy it back.
				 * But add a newline if it's not there already, since
				 * postmaster error messages may not have one.
				 */
				if (conn->errorMessage.len <= 0 ||
					conn->errorMessage.data[conn->errorMessage.len-1] != '\n')
					appendPQExpBufferChar(&conn->errorMessage, '\n');
				PQclear(res);
				goto error_return;
			}

			/*
			 * Post-connection housekeeping. Prepare to send environment
			 * variables to server.
			 */

			if ((conn->setenv_handle = PQsetenvStart(conn)) == NULL)
				goto error_return;

			conn->status = CONNECTION_SETENV;

			goto keep_going;
		}

		case CONNECTION_SETENV:
			/* We pretend that the connection is OK for the duration of
			   these queries. */
			conn->status = CONNECTION_OK;

			switch (PQsetenvPoll(conn))
			{
				case PGRES_POLLING_OK: /* Success */
					conn->status = CONNECTION_OK;
					return PGRES_POLLING_OK;

				case PGRES_POLLING_READING: /* Still going */
					conn->status = CONNECTION_SETENV;
					return PGRES_POLLING_READING;

				case PGRES_POLLING_WRITING: /* Still going */
					conn->status = CONNECTION_SETENV;
					return PGRES_POLLING_WRITING;

				default:
					conn->status = CONNECTION_SETENV;
					goto error_return;
			}
			/* Unreachable */

		default:
			printfPQExpBuffer(&conn->errorMessage,
							  "PQconnectPoll() -- unknown connection state - "
							  "probably indicative of memory corruption!\n");
			goto error_return;
	}

	/* Unreachable */

error_return:
	/* ----------
	 * We used to close the socket at this point, but that makes it awkward
	 * for those above us if they wish to remove this socket from their
	 * own records (an fd_set for example).  We'll just have this socket
	 * closed when PQfinish is called (which is compulsory even after an
	 * error, since the connection structure must be freed).
	 * ----------
	 */
	return PGRES_POLLING_FAILED;
}


/* ----------------
 *		PQsetenvStart
 *
 * Starts the process of passing the values of a standard set of environment
 * variables to the backend.
 *
 * ----------------
 */
PGsetenvHandle
PQsetenvStart(PGconn *conn)
{
	struct pg_setenv_state *handle;

	if (conn == NULL || conn->status == CONNECTION_BAD)
		return NULL;

	if ((handle = malloc(sizeof(struct pg_setenv_state))) == NULL)
	{
		printfPQExpBuffer(&conn->errorMessage,
						  "PQsetenvStart() -- malloc error - %s\n",
						  strerror(errno));
		return NULL;
	}

	handle->conn = conn;
	handle->res = NULL;
	handle->eo = EnvironmentOptions;

#ifdef MULTIBYTE
	handle->state = SETENV_STATE_ENCODINGS_SEND;
#else
	handle->state = SETENV_STATE_OPTION_SEND;
#endif
	
	return handle; /* Note that a struct pg_setenv_state * is the same as a
					  PGsetenvHandle */
}

/* ----------------
 *		PQsetenvPoll
 *
 * Polls the process of passing the values of a standard set of environment
 * variables to the backend.
 *
 * ----------------
 */
PostgresPollingStatusType
PQsetenvPoll(PGconn *conn)
{
	PGsetenvHandle handle = conn->setenv_handle;
#ifdef MULTIBYTE
	static const char envname[] = "PGCLIENTENCODING";
#endif

	if (!handle || handle->state == SETENV_STATE_FAILED)
		return PGRES_POLLING_FAILED;

	/* Check whether there are any data for us */
	switch (handle->state)
	{
		/* These are reading states */
#ifdef MULTIBYTE
		case SETENV_STATE_ENCODINGS_WAIT:
#endif
		case SETENV_STATE_OPTION_WAIT:
		{
			/* Load waiting data */
			int n = pqReadData(handle->conn);
				
			if (n < 0)
				goto error_return;
			if (n == 0)
				return PGRES_POLLING_READING;

			break;
		}

		/* These are writing states, so we just proceed. */
#ifdef MULTIBYTE
		case SETENV_STATE_ENCODINGS_SEND:
#endif
		case SETENV_STATE_OPTION_SEND:
			break;
		   
		default:
			printfPQExpBuffer(&handle->conn->errorMessage,
							  "PQsetenvPoll() -- unknown state - "
							  "probably indicative of memory corruption!\n");
			goto error_return;
	}


 keep_going: /* We will come back to here until there is nothing left to
				parse. */
	switch(handle->state)
	{

#ifdef MULTIBYTE
		case SETENV_STATE_ENCODINGS_SEND:
		{
			const char *env;
			
			/* query server encoding */
			env = getenv(envname);
			if (!env || *env == '\0')
			{
				if (!PQsendQuery(handle->conn,
								 "select getdatabaseencoding()"))
					goto error_return;

				handle->state = SETENV_STATE_ENCODINGS_WAIT;
				return PGRES_POLLING_READING;
			}
		}

		case SETENV_STATE_ENCODINGS_WAIT:
		{
			const char *encoding = 0;

			if (PQisBusy(handle->conn))
				return PGRES_POLLING_READING;
			
			handle->res = PQgetResult(handle->conn);

			if (handle->res)
			{
				if (PQresultStatus(handle->res) != PGRES_TUPLES_OK)
				{
					PQclear(handle->res);
					goto error_return;
				}

				encoding = PQgetvalue(handle->res, 0, 0);
				if (!encoding)			/* this should not happen */
					encoding = SQL_ASCII;

				if (encoding)
				{
					/* set client encoding to pg_conn struct */
					conn->client_encoding = atoi(encoding);
				}
				PQclear(handle->res);
				/* We have to keep going in order to clear up the query */
				goto keep_going;
			}

			/* NULL result indicates that the query is finished */

			/* Move on to setting the environment options */
			handle->state = SETENV_STATE_OPTION_SEND;
			goto keep_going;
		}
#endif

		case SETENV_STATE_OPTION_SEND:
		{
			/* Send an Environment Option */
			char		setQuery[100];	/* note length limits in sprintf's below */

			if (handle->eo->envName)
			{
				const char *val;

				if ((val = getenv(handle->eo->envName)))
				{
					if (strcasecmp(val, "default") == 0)
						sprintf(setQuery, "SET %s = %.60s",
								handle->eo->pgName, val);
					else
						sprintf(setQuery, "SET %s = '%.60s'",
								handle->eo->pgName, val);
#ifdef CONNECTDEBUG
					printf("Use environment variable %s to send %s\n",
						   handle->eo->envName, setQuery);
#endif
					if (!PQsendQuery(handle->conn, setQuery))
						goto error_return;

					handle->state = SETENV_STATE_OPTION_WAIT;
				}
				else
				{
					handle->eo++;
				}
			}
			else
			{
				/* No option to send, so we are done. */
				handle->state = SETENV_STATE_OK;
			}

			goto keep_going;
		}

		case SETENV_STATE_OPTION_WAIT:
		{
			if (PQisBusy(handle->conn))
				return PGRES_POLLING_READING;
			
			handle->res = PQgetResult(handle->conn);

			if (handle->res)
			{
				if (PQresultStatus(handle->res) != PGRES_COMMAND_OK)
				{
					PQclear(handle->res);
					goto error_return;
				}
				/* Don't need the result */
				PQclear(handle->res);
				/* We have to keep going in order to clear up the query */
				goto keep_going;
			}

			/* NULL result indicates that the query is finished */

			/* Send the next option */
			handle->eo++;
			handle->state = SETENV_STATE_OPTION_SEND;
			goto keep_going;
		}

		case SETENV_STATE_OK:
			/* Tidy up */
			free(handle);
			return PGRES_POLLING_OK;

		default:
			printfPQExpBuffer(&handle->conn->errorMessage,
							  "PQsetenvPoll() -- unknown state - "
							  "probably indicative of memory corruption!\n");
			goto error_return;
	}

	/* Unreachable */

 error_return:
	handle->state = SETENV_STATE_FAILED; /* This may protect us even if we
										  * are called after the handle
										  * has been freed.             */
	free(handle);
	return PGRES_POLLING_FAILED;
}


/* ----------------
 *		PQsetenvAbort
 *
 * Aborts the process of passing the values of a standard set of environment
 * variables to the backend.
 *
 * ----------------
 */
void
PQsetenvAbort(PGsetenvHandle handle)
{
	/* We should not have been called in the FAILED state, but we can cope by
	 * not freeing the handle (it has probably been freed by now anyway). */
	if (handle->state != SETENV_STATE_FAILED)
	{
		handle->state = SETENV_STATE_FAILED;
		free(handle);
	}
}


/* ----------------
 *		PQsetenv
 *
 * Passes the values of a standard set of environment variables to the
 * backend.
 *
 * Returns 1 on success, 0 on failure.
 *
 * This function used to return void.  I don't think that there should be
 * compatibility problems caused by giving it a return value, especially as
 * this function has not been documented previously.
 *
 * ----------------
 */
int
PQsetenv(PGconn *conn)
{
	PGsetenvHandle handle;
	PostgresPollingStatusType flag = PGRES_POLLING_WRITING;

	if ((handle = PQsetenvStart(conn)) == NULL)
		return 0;

	for (;;) {
		/*
		 * Wait, if necessary.  Note that the initial state (just after
		 * PQsetenvStart) is to wait for the socket to select for writing.
		 */
		switch (flag)
		{
			case PGRES_POLLING_ACTIVE:
				break;

			case PGRES_POLLING_OK:
				return 1;		/* success! */
				
			case PGRES_POLLING_READING:
				if (pqWait(1, 0, conn))
				{
					conn->status = CONNECTION_BAD;
					return 0;
				}
				break;

			case PGRES_POLLING_WRITING:
				if (pqWait(0, 1, conn))
				{
					conn->status = CONNECTION_BAD;
					return 0;
				}
				break;

			default:
				/* Just in case we failed to set it in PQsetenvPoll */
				conn->status = CONNECTION_BAD;
				return 0;
		}
		/*
		 * Now try to advance the state machine.
		 */
		flag = PQsetenvPoll(conn);
	}
}

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
	conn->notifyList = DLNewList();
	conn->sock = -1;
#ifdef USE_SSL
	conn->allow_ssl_try = TRUE;
#endif

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
	if (conn->pghostaddr)
		free(conn->pghostaddr);
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
	if (conn->status == CONNECTION_SETENV)
	{
		/* We have to abort the setenv process as well */
		PQsetenvAbort(conn->setenv_handle);
	}

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
	 * must reset the blocking status so a possible reconnect will work
	 * don't call PQsetnonblocking() because it will fail if it's unable
	 * to flush the connection.
	 */
	conn->nonblocking = FALSE;

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
	{
		return PQconnectPoll(conn);
	}

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

int
PQclientencoding(const PGconn *conn)
{
	if (!conn || conn->status != CONNECTION_OK)
		return -1;
	return conn->client_encoding;
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
