/*-------------------------------------------------------------------------
 *
 * fe-connect.c--
 *	  functions related to setting up a connection to the backend
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/interfaces/libpq/fe-connect.c,v 1.68 1998/06/16 07:29:47 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <netdb.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <errno.h>
#include <signal.h>
#include <ctype.h>				/* for isspace() */

#include "postgres.h"
#include "fe-auth.h"
#include "fe-connect.h"
#include "libpq-fe.h"

#ifndef HAVE_STRDUP
#include "strdup.h"
#endif
#ifdef HAVE_CRYPT_H
#include <crypt.h>
#endif


static ConnStatusType connectDB(PGconn *conn);
static PGconn *makeEmptyPGconn(void);
static void freePGconn(PGconn *conn);
static void closePGconn(PGconn *conn);
static int	conninfo_parse(const char *conninfo, char *errorMessage);
static char *conninfo_getval(char *keyword);
static void conninfo_free(void);
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

struct EnvironmentOptions
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
#ifdef MB
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
		"PGRPLANS", "r_plans"
	},
	{
		"PGGEQO", "geqo"
	},
	{
		NULL
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
	{
		fprintf(stderr,
		   "FATAL: PQconnectdb() -- unable to allocate memory for a PGconn");
		return (PGconn *) NULL;
	}

	/* ----------
	 * Parse the conninfo string and save settings in conn structure
	 * ----------
	 */
	if (conninfo_parse(conninfo, conn->errorMessage) < 0)
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
	char		errorMessage[ERROR_MSG_LENGTH];

	conninfo_parse("", errorMessage);
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
	{
		fprintf(stderr,
		   "FATAL: PQsetdbLogin() -- unable to allocate memory for a PGconn");
		return (PGconn *) NULL;
	}

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
		conn->pguser = fe_getauthname(conn->errorMessage);

	if (conn->pguser == NULL)
	{
		error = TRUE;
		sprintf(conn->errorMessage,
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
		 * if the database name is surrounded by double-quotes, then
		 * don't convert case
		 */
		if (*conn->dbName == '"')
		{
			strcpy(conn->dbName, conn->dbName + 1);
			conn->dbName[strlen(conn->dbName) - 1] = '\0';
		}
		else
			for (i = 0; conn->dbName[i]; i++)
				if (isascii((unsigned char)conn->dbName[i]) &&
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
	int			laddrlen = sizeof(SockAddr);
	int			portno,
				family,
				len;
	char		beresp;

	/*
	 * Initialize the startup packet.
	 */

	MemSet((char *) &sp, 0, sizeof(StartupPacket));

	sp.protoVersion = (ProtocolVersion) htonl(PG_PROTOCOL_LATEST);

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
			(void) sprintf(conn->errorMessage,
						   "connectDB() --  unknown hostname: %s\n",
						   conn->pghost);
			goto connect_errReturn;
		}
		family = AF_INET;
	}
	else {
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
		len = sizeof(struct sockaddr_in);
	}
	else
		len = UNIXSOCK_PATH(conn->raddr.un, portno);

	/* Connect to the server  */
	if ((conn->sock = socket(family, SOCK_STREAM, 0)) < 0)
	{
		(void) sprintf(conn->errorMessage,
					   "connectDB() -- socket() failed: errno=%d\n%s\n",
					   errno, strerror(errno));
		goto connect_errReturn;
	}
	if (connect(conn->sock, &conn->raddr.sa, len) < 0)
	{
		(void) sprintf(conn->errorMessage,
					   "connectDB() failed: Is the postmaster running and accepting%s connections at '%s' on port '%s'?\n",
					   conn->pghost ? " TCP/IP(with -i)" : "",
					   conn->pghost ? conn->pghost : "UNIX Socket",
					   conn->pgport);
		goto connect_errReturn;
	}

	/*
	 * Set the right options.
	 * We need nonblocking I/O, and we don't want delay of outgoing data.
	 */

	if (fcntl(conn->sock, F_SETFL, O_NONBLOCK) < 0)
	{
		(void) sprintf(conn->errorMessage,
					   "connectDB() -- fcntl() failed: errno=%d\n%s\n",
					   errno, strerror(errno));
		goto connect_errReturn;
	}

	if (family == AF_INET)
	{
		struct protoent *pe;
		int			on = 1;

		pe = getprotobyname("TCP");
		if (pe == NULL)
		{
			(void) sprintf(conn->errorMessage,
						   "connectDB(): getprotobyname failed\n");
			goto connect_errReturn;
		}
		if (setsockopt(conn->sock, pe->p_proto, TCP_NODELAY,
					   &on, sizeof(on)) < 0)
		{
			(void) sprintf(conn->errorMessage,
						   "connectDB() -- setsockopt failed: errno=%d\n%s\n",
						   errno, strerror(errno));
			goto connect_errReturn;
		}
	}

	/* Fill in the client address */
	if (getsockname(conn->sock, &conn->laddr.sa, &laddrlen) < 0)
	{
		(void) sprintf(conn->errorMessage,
					   "connectDB() -- getsockname() failed: errno=%d\n%s\n",
					   errno, strerror(errno));
		goto connect_errReturn;
	}

	/* Ensure our buffers are empty */
	conn->inStart = conn->inCursor = conn->inEnd = 0;
	conn->outCount = 0;

	/* Send the startup packet. */

	if (packetSend(conn, (char *) &sp, sizeof(StartupPacket)) != STATUS_OK)
	{
		sprintf(conn->errorMessage,
				"connectDB() --  couldn't send startup packet: errno=%d\n%s\n",
				errno, strerror(errno));
		goto connect_errReturn;
	}

	/*
	 * Perform the authentication exchange:
	 * wait for backend messages and respond as necessary.
	 * We fall out of this loop when done talking to the postmaster.
	 */

	for (;;)
	{
		/* Wait for some data to arrive (or for the channel to close) */
		if (pqWait(TRUE, FALSE, conn))
			goto connect_errReturn;
		/* Load data, or detect EOF */
		if (pqReadData(conn) < 0)
			goto connect_errReturn;
		/* Scan the message.
		 * If we run out of data, loop around to try again.
		 */
		conn->inCursor = conn->inStart;

		if (pqGetc(&beresp, conn))
			continue;			/* no data yet */

		/* Handle errors. */
		if (beresp == 'E')
		{
			if (pqGets(conn->errorMessage, sizeof(conn->errorMessage), conn))
				continue;
			goto connect_errReturn;
		}

		/* Otherwise it should be an authentication request. */
		if (beresp != 'R')
		{
			(void) sprintf(conn->errorMessage,
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
		if (fe_sendauth(areq, conn, conn->pghost, conn->pgpass,
						conn->errorMessage) != STATUS_OK)
			goto connect_errReturn;
		if (pqFlush(conn))
			goto connect_errReturn;

		/* Are we done? */
		if (areq == AUTH_REQ_OK)
			break;
	}

	/*
	 * Now we expect to hear from the backend.
	 * A ReadyForQuery message indicates that startup is successful,
	 * but we might also get an Error message indicating failure.
	 * (Notice messages indicating nonfatal warnings are also allowed
	 * by the protocol.)
	 * Easiest way to handle this is to let PQgetResult() read the messages.
	 * We just have to fake it out about the state of the connection.
	 */

	conn->status = CONNECTION_OK;
	conn->asyncStatus = PGASYNC_BUSY;
	res = PQgetResult(conn);
	/* NULL return indicating we have gone to IDLE state is expected */
	if (res) {
		if (res->resultStatus != PGRES_FATAL_ERROR)
			sprintf(conn->errorMessage,
					"connectDB() -- unexpected message during startup\n");
		PQclear(res);
		goto connect_errReturn;
	}

	/* Given the new protocol that sends a ReadyForQuery message
	 * after successful backend startup, it should no longer be
	 * necessary to send an empty query to test for startup.
	 */

#if 0

	/*
	 * Send a blank query to make sure everything works; in
	 * particular, that the database exists.
	 */
	res = PQexec(conn, " ");
	if (res == NULL || res->resultStatus != PGRES_EMPTY_QUERY)
	{
		/* PQexec has put error message in conn->errorMessage */
		closePGconn(conn);
		PQclear(res);
		goto connect_errReturn;
	}
	PQclear(res);

#endif

	/* Post-connection housekeeping.
	 * Send environment variables to server
	 */

	PQsetenv(conn);

	/* Free the password so it's not hanging out in memory forever */
	/* XXX Is this *really* a good idea?  The security gain is marginal
	 * if not totally illusory, and it breaks PQreset() for databases
	 * that use passwords.
	 */
	if (conn->pgpass != NULL)
	{
		free(conn->pgpass);
		conn->pgpass = NULL;
	}

	return CONNECTION_OK;

connect_errReturn:
	if (conn->sock >= 0)
	{
		close(conn->sock);
		conn->sock = -1;
	}
	return CONNECTION_BAD;

}

void
PQsetenv(PGconn *conn)
{
	struct EnvironmentOptions *eo;
	char		setQuery[80];	/* mjl: size okay? XXX */

	for (eo = EnvironmentOptions; eo->envName; eo++)
	{
		const char *val;

		if ((val = getenv(eo->envName)))
		{
			PGresult   *res;

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
 *   - create a PGconn data structure with (as yet) no interesting data
 */
static PGconn *
makeEmptyPGconn(void)
{
	PGconn	*conn = (PGconn *) malloc(sizeof(PGconn));
	if (conn == NULL)
		return conn;

	/* Zero all pointers */
	MemSet((char *) conn, 0, sizeof(PGconn));

	conn->status = CONNECTION_BAD;
	conn->asyncStatus = PGASYNC_IDLE;
	conn->notifyList = DLNewList();
	conn->sock = -1;
	conn->inBufSize = 8192;
	conn->inBuffer = (char *) malloc(conn->inBufSize);
	conn->outBufSize = 8192;
	conn->outBuffer = (char *) malloc(conn->outBufSize);
	if (conn->inBuffer == NULL || conn->outBuffer == NULL)
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
	PQclearAsyncResult(conn);	/* deallocate result and curTuple */
	if (conn->sock >= 0)
		close(conn->sock);		/* shouldn't happen, but... */
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
		 * Try to send close message.
		 * If connection is already gone, that's cool.  No reason for kernel
		 * to kill us when we try to write to it.  So ignore SIGPIPE signals.
		 */
#if defined(USE_POSIX_SIGNALS)
		struct sigaction ignore_action;
		struct sigaction oldaction;

		ignore_action.sa_handler = SIG_IGN;
		sigemptyset(&ignore_action.sa_mask);
		ignore_action.sa_flags = 0;
		sigaction(SIGPIPE, (struct sigaction *) & ignore_action, &oldaction);

		(void) pqPuts("X", conn);
		(void) pqFlush(conn);

		sigaction(SIGPIPE, &oldaction, NULL);
#else
		void (*oldsignal)(int);

		oldsignal = signal(SIGPIPE, SIG_IGN);

		(void) pqPuts("X", conn);
		(void) pqFlush(conn);

		signal(SIGPIPE, oldsignal);
#endif
	}

	/*
	 * Close the connection, reset all transient state, flush I/O buffers.
	 */
	if (conn->sock >= 0)
		close(conn->sock);
	conn->sock = -1;
	conn->status = CONNECTION_BAD;		/* Well, not really _bad_ - just
										 * absent */
	conn->asyncStatus = PGASYNC_IDLE;
	PQclearAsyncResult(conn);	/* deallocate result and curTuple */
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
	if (!conn)
		fprintf(stderr, "PQfinish() -- pointer to PGconn is null\n");
	else
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
	if (!conn)
		fprintf(stderr, "PQreset() -- pointer to PGconn is null\n");
	else
	{
		closePGconn(conn);
		conn->status = connectDB(conn);
	}
}

/*
 * PacketSend() -- send a single-packet message.
 * this is like PacketSend(), defined in backend/libpq/pqpacket.c
 *
 * RETURNS: STATUS_ERROR if the write fails, STATUS_OK otherwise.
 * SIDE_EFFECTS: may block.
*/
int
packetSend(PGconn *conn, const char *buf, size_t len)
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
conninfo_parse(const char *conninfo, char *errorMessage)
{
	char	   *pname;
	char	   *pval;
	char	   *buf;
	char	   *tmp;
	char	   *cp;
	char	   *cp2;
	PQconninfoOption *option;
	char		errortmp[ERROR_MSG_LENGTH];

	conninfo_free();

	if ((buf = strdup(conninfo)) == NULL)
	{
		strcpy(errorMessage,
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
			sprintf(errorMessage,
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
					sprintf(errorMessage,
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
			sprintf(errorMessage,
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
		if (option->environ != NULL)
		{
			if ((tmp = getenv(option->environ)) != NULL)
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
			tmp = fe_getauthname(errortmp);
			if (tmp)
				option->val = strdup(tmp);
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
char *
PQdb(PGconn *conn)
{
	if (!conn)
	{
		fprintf(stderr, "PQdb() -- pointer to PGconn is null\n");
		return (char *) NULL;
	}
	return conn->dbName;
}

char *
PQuser(PGconn *conn)
{
	if (!conn)
	{
		fprintf(stderr, "PQuser() -- pointer to PGconn is null\n");
		return (char *) NULL;
	}
	return conn->pguser;
}

char *
PQhost(PGconn *conn)
{
	if (!conn)
	{
		fprintf(stderr, "PQhost() -- pointer to PGconn is null\n");
		return (char *) NULL;
	}

	return conn->pghost;
}

char *
PQoptions(PGconn *conn)
{
	if (!conn)
	{
		fprintf(stderr, "PQoptions() -- pointer to PGconn is null\n");
		return (char *) NULL;
	}
	return conn->pgoptions;
}

char *
PQtty(PGconn *conn)
{
	if (!conn)
	{
		fprintf(stderr, "PQtty() -- pointer to PGconn is null\n");
		return (char *) NULL;
	}
	return conn->pgtty;
}

char *
PQport(PGconn *conn)
{
	if (!conn)
	{
		fprintf(stderr, "PQport() -- pointer to PGconn is null\n");
		return (char *) NULL;
	}
	return conn->pgport;
}

ConnStatusType
PQstatus(PGconn *conn)
{
	if (!conn)
	{
		fprintf(stderr, "PQstatus() -- pointer to PGconn is null\n");
		return CONNECTION_BAD;
	}
	return conn->status;
}

char *
PQerrorMessage(PGconn *conn)
{
	if (!conn)
	{
		fprintf(stderr, "PQerrorMessage() -- pointer to PGconn is null\n");
		return (char *) NULL;
	}
	return conn->errorMessage;
}

int
PQsocket(PGconn *conn)
{
	if (!conn)
	{
		fprintf(stderr, "PQsocket() -- pointer to PGconn is null\n");
		return -1;
	}
	return conn->sock;
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
