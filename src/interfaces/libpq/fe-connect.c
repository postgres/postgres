/*-------------------------------------------------------------------------
 *
 * fe-connect.c--
 *    functions related to setting up a connection to the backend
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/interfaces/libpq/fe-connect.c,v 1.33 1997/05/09 03:28:49 scrappy Exp $
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
#include <netinet/tcp.h>
#include <errno.h>
#include <signal.h>
#include <ctype.h>      /* for isspace() */

#include "postgres.h"
#include "libpq/pqcomm.h" /* for decls of MsgType, PacketBuf, StartupInfo */
#include "fe-auth.h"
#include "fe-connect.h"
#include "libpq-fe.h"

#ifndef HAVE_STRDUP
#include "strdup.h"
#endif


/* use a local version instead of the one found in pqpacket.c */
static ConnStatusType connectDB(PGconn *conn);

static void startup2PacketBuf(StartupInfo* s, PacketBuf* res);
static void freePGconn(PGconn *conn);
static void closePGconn(PGconn *conn);
static int conninfo_parse(const char *conninfo, char *errorMessage);
static char *conninfo_getval(char *keyword);
static void conninfo_free(void);

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
 *     ""	Normal input field
 * ----------
 */
static PQconninfoOption PQconninfoOptions[] = {
/*    ----------------------------------------------------------------- */
/*    Option-name	Environment-Var	Compiled-in	Current value	*/
/*			Label				Disp-Char	*/
/*    ----------------- --------------- --------------- --------------- */
    { "authtype",       "PGAUTHTYPE",  DefaultAuthtype, NULL,
                        "Database-Authtype",            "", 20  },

    { "user",		"PGUSER",	NULL,		NULL,
    			"Database-User",		"", 20	},

    { "password",       "PGPASSWORD",  DefaultPassword, NULL,
                        "Database-Password",            "", 20  },

    { "dbname",		"PGDATABASE",	NULL,		NULL,
    			"Database-Name",		"", 20	},

    { "host",		"PGHOST",	DefaultHost,	NULL,
    			"Database-Host",		"", 40	},

    { "port",		"PGPORT",	DEF_PGPORT,	NULL,
    			"Database-Port",		"", 6	},

    { "tty",		"PGTTY",	DefaultTty,	NULL,
    			"Backend-Debug-TTY",		"D", 40	},

    { "options",	"PGOPTIONS",	DefaultOption,	NULL,
    			"Backend-Debug-Options",	"D", 40	},
/*    ----------------- --------------- --------------- --------------- */
    { NULL,		NULL,		NULL,		NULL,
    			NULL,				NULL, 0	}
};

struct EnvironmentOptions
	{
	const char *envName, *pgName;
	} EnvironmentOptions[] =
	{
		{ "PG_DATESTYLE",		"datestyle" },
		{ NULL }
	};
	
/* ----------------
 *	PQconnectdb
 * 
 * establishes a connectin to a postgres backend through the postmaster
 * using connection information in a string.
 *
 * The conninfo string is a list of
 *
 *     option = value
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
PGconn*
PQconnectdb(const char *conninfo)
{
    PGconn *conn;
    PQconninfoOption *option;
    char errorMessage[ERROR_MSG_LENGTH];

    /* ----------
     * Allocate memory for the conn structure
     * ----------
     */
    conn = (PGconn*)malloc(sizeof(PGconn));
    if (conn == NULL) {
        fprintf(stderr,
            "FATAL: PQsetdb() -- unable to allocate memory for a PGconn");
        return (PGconn*)NULL;
    }
    memset((char *)conn, 0, sizeof(PGconn));

    /* ----------
     * Parse the conninfo string and get the fallback resources
     * ----------
     */
    if(conninfo_parse(conninfo, errorMessage) < 0) {
	conn->status = CONNECTION_BAD;
	strcpy(conn->errorMessage, errorMessage);
	conninfo_free();
	return conn;
    }

    /* ----------
     * Check that we have all connection parameters
     * ----------
     */
    for(option = PQconninfoOptions; option->keyword != NULL; option++) {
	if(option->val != NULL)  continue;	/* Value was in conninfo */

	/* ----------
	 * No value was found for this option. Return an error.
	 * ----------
	 */
	conn->status = CONNECTION_BAD;
	sprintf(conn->errorMessage,
	   "ERROR: PQconnectdb(): Cannot determine a value for option '%s'.\n",
	   option->keyword);
	strcat(conn->errorMessage,
	    "Option not specified in conninfo string");
	if(option->environ) {
	    strcat(conn->errorMessage,
	        ", environment variable ");
	    strcat(conn->errorMessage, option->environ);
	    strcat(conn->errorMessage, "\nnot set");
	}
	strcat(conn->errorMessage, " and no compiled in default value.\n");
	conninfo_free();
	return conn;
    }

    /* ----------
     * Setup the conn structure
     * ----------
     */
    conn->lobjfuncs = (PGlobjfuncs *) NULL;
    conn->Pfout = NULL;
    conn->Pfin = NULL;
    conn->Pfdebug = NULL;
    conn->port = NULL;
    conn->notifyList = DLNewList();

    conn->pghost    = strdup(conninfo_getval("host"));
    conn->pgport    = strdup(conninfo_getval("port"));
    conn->pgtty     = strdup(conninfo_getval("tty"));
    conn->pgoptions = strdup(conninfo_getval("options"));
    conn->pguser    = strdup(conninfo_getval("user"));
    conn->pgpass    = strdup(conninfo_getval("password"));
    conn->pgauth    = strdup(conninfo_getval("authtype"));
    conn->dbName    = strdup(conninfo_getval("dbname"));

    /* ----------
     * Free the connection info - all is in conn now
     * ----------
     */
    conninfo_free();

    /*
     * try to set the auth service if one was specified
     */
    if(conn->pgauth) {
      fe_setauthsvc(conn->pgauth, conn->errorMessage);
    }

    /* ----------
     * Connect to the database
     * ----------
     */
    conn->status = connectDB(conn);
    if (conn->status == CONNECTION_OK) {
      PGresult *res;
      /* Send a blank query to make sure everything works; in particular, that
         the database exists.
         */ 
      res = PQexec(conn," ");
      if (res == NULL || res->resultStatus != PGRES_EMPTY_QUERY) {
        /* PQexec has put error message in conn->errorMessage */
        closePGconn(conn);
      }
      PQclear(res);
    } 

    return conn;
}

/* ----------------
 *	PQconndefaults
 * 
 * Parse an empty string like PQconnectdb() would do and return the
 * address of the connection options structure. Using this function
 * an application might determine all possible options and their
 * current default values.
 * ----------------
 */
PQconninfoOption*
PQconndefaults(void)
{
    char errorMessage[ERROR_MSG_LENGTH];

    conninfo_parse("", errorMessage);
    return PQconninfoOptions;
}

/* ----------------
 *	PQsetdb
 * 
 * establishes a connection to a postgres backend through the postmaster
 * at the specified host and port.
 *
 * returns a PGconn* which is needed for all subsequent libpq calls
 * if the status field of the connection returned is CONNECTION_BAD,
 * then some fields may be null'ed out instead of having valid values 
 *
 *  Uses these environment variables:
 *
 *    PGHOST       identifies host to which to connect if <pghost> argument
 *                 is NULL or a null string.
 *
 *    PGPORT       identifies TCP port to which to connect if <pgport> argument
 *                 is NULL or a null string.
 *
 *    PGTTY        identifies tty to which to send messages if <pgtty> argument
 *                 is NULL or a null string.
 *
 *    PGOPTIONS    identifies connection options if <pgoptions> argument is
 *                 NULL or a null string.
 *
 *    PGUSER       Postgres username to associate with the connection.
 *
 *    PGPASSWORD   The user's password.
 *
 *    PGDATABASE   name of database to which to connect if <pgdatabase> 
 *                 argument is NULL or a null string
 *
 *    None of the above need be defined.  There are defaults for all of them.
 *
 * ----------------
 */
PGconn* 
PQsetdb(const char *pghost, const char* pgport, const char* pgoptions, const char* pgtty, const char* dbName)
{
  PGconn *conn;
  char *tmp;
  char errorMessage[ERROR_MSG_LENGTH];
    /* An error message from some service we call. */
  bool error;   
    /* We encountered an error that prevents successful completion */

  conn = (PGconn*)malloc(sizeof(PGconn));

  if (conn == NULL) 
    fprintf(stderr,
            "FATAL: PQsetdb() -- unable to allocate memory for a PGconn");
  else {
    conn->lobjfuncs = (PGlobjfuncs *) NULL;
    conn->Pfout = NULL;
    conn->Pfin = NULL;
    conn->Pfdebug = NULL;
    conn->port = NULL;
    conn->notifyList = DLNewList();
    
    if (!pghost || pghost[0] == '\0') {
      if (!(tmp = getenv("PGHOST"))) {
        tmp = DefaultHost;
      }
      conn->pghost = strdup(tmp);
    } else
      conn->pghost = strdup(pghost);
    
    if (!pgport || pgport[0] == '\0') {
      if (!(tmp = getenv("PGPORT"))) {
        tmp = DEF_PGPORT;
      }
      conn->pgport = strdup(tmp);
    } else
      conn->pgport = strdup(pgport);
    
    if (!pgtty || pgtty[0] == '\0') {
      if (!(tmp = getenv("PGTTY"))) {
        tmp = DefaultTty;
      }
      conn->pgtty = strdup(tmp);
    } else
      conn->pgtty = strdup(pgtty);
    
    if (!pgoptions || pgoptions[0] == '\0') {
      if (!(tmp = getenv("PGOPTIONS"))) {
        tmp = DefaultOption;
      }
      conn->pgoptions = strdup(tmp);
    } else
      conn->pgoptions = strdup(pgoptions);

    if ((tmp = getenv("PGUSER"))) {
      error = FALSE;
      conn->pguser = strdup(tmp);
    } else {
      tmp = fe_getauthname(errorMessage);
      if (tmp == 0) {
        error = TRUE;
        sprintf(conn->errorMessage,
                "FATAL: PQsetdb: Unable to determine a Postgres username!\n");
      } else {
        error = FALSE;
        conn->pguser = tmp;
      }
    }

    if((tmp = getenv("PGPASSWORD"))) {
      conn->pgpass = strdup(tmp);
    } else {
      conn->pgpass = 0;
    }

    if (!error) {
      if (((tmp = (char *)dbName) && (dbName[0] != '\0')) ||
          ((tmp = getenv("PGDATABASE")))) {
        conn->dbName = strdup(tmp);
      } else conn->dbName = strdup(conn->pguser);
    } else conn->dbName = NULL;

    if (error) conn->status = CONNECTION_BAD;
    else {
      conn->status = connectDB(conn);  
        /* Puts message in conn->errorMessage */
      if (conn->status == CONNECTION_OK) {
        PGresult *res;
        /* Send a blank query to make sure everything works; 
           in particular, that the database exists.
           */
        res = PQexec(conn," ");
        if (res == NULL || res->resultStatus != PGRES_EMPTY_QUERY) {
          /* PQexec has put error message in conn->errorMessage */
          closePGconn(conn);
        }
        PQclear(res);
      }
    }
  }        
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
    struct hostent *hp;

    StartupInfo startup;
    PacketBuf   pacBuf;
    int		status;
    MsgType	msgtype;
    int         laddrlen = sizeof(struct sockaddr);
    Port        *port = conn->port;
    int         portno;

    /*
    //
    // Initialize the startup packet. 
    //
    // This data structure is used for the seq-packet protocol.  It
    // describes the frontend-backend connection.
    //
    //
    */
    strncpy(startup.user,conn->pguser,sizeof(startup.user));
    strncpy(startup.database,conn->dbName,sizeof(startup.database));
    strncpy(startup.tty,conn->pgtty,sizeof(startup.tty));
    if (conn->pgoptions) {
	strncpy(startup.options,conn->pgoptions, sizeof(startup.options));
    }
    else
	startup.options[0]='\0'; 
    startup.execFile[0]='\0';  /* not used */

    /*
    //
    // Open a connection to postmaster/backend.
    //
    */
    port = (Port *) malloc(sizeof(Port));
    memset((char *) port, 0, sizeof(Port));

    if (!(hp = gethostbyname(conn->pghost)) || hp->h_addrtype != AF_INET) {
	(void) sprintf(conn->errorMessage,
		       "connectDB() --  unknown hostname: %s\n",
		       conn->pghost);
	goto connect_errReturn;
    }
    memset((char *) &port->raddr, 0, sizeof(port->raddr));
    memmove((char *) &(port->raddr.sin_addr),
	    (char *) hp->h_addr, 
	    hp->h_length);
    port->raddr.sin_family = AF_INET;
    portno = atoi(conn->pgport);
    port->raddr.sin_port = htons((unsigned short)(portno));
    
    /* connect to the server  */
    if ((port->sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
	(void) sprintf(conn->errorMessage,
	       "connectDB() -- socket() failed: errno=%d\n%s\n",
	       errno, strerror(errno));
	goto connect_errReturn;	
    }
    if (connect(port->sock, (struct sockaddr *)&port->raddr,
		sizeof(port->raddr)) < 0) {
	(void) sprintf(conn->errorMessage,
		       "connectDB() failed: Is the postmaster running at '%s' on port '%s'?\n",
		       conn->pghost,conn->pgport);
	goto connect_errReturn;	
    }
    {
    	struct protoent *pe;
    	int on=1;
    	
    	pe = getprotobyname ("TCP");
    	if ( pe == NULL )
    	{
	    (void) sprintf(conn->errorMessage,
	    		"connectDB(): getprotobyname failed\n");
	    goto connect_errReturn;
	}
    	if ( setsockopt (port->sock, pe->p_proto, TCP_NODELAY, 
    						&on, sizeof (on)) < 0 )
    	{
	    (void) sprintf(conn->errorMessage,
	    		"connectDB(): setsockopt failed\n");
	    goto connect_errReturn;
	}
    }

    /* fill in the client address */
    if (getsockname(port->sock, (struct sockaddr *) &port->laddr,
		    &laddrlen) < 0) {
	(void) sprintf(conn->errorMessage,
	       "connectDB() -- getsockname() failed: errno=%d\n%s\n",
	       errno, strerror(errno));
	goto connect_errReturn;	
    }
    
    /* by this point, connection has been opened */
    msgtype = fe_getauthsvc(conn->errorMessage);

/*    pacBuf = startup2PacketBuf(&startup);*/
    startup2PacketBuf(&startup, &pacBuf);
    pacBuf.msgtype = (MsgType) htonl(msgtype);
    status = packetSend(port, &pacBuf, sizeof(PacketBuf), BLOCKING);
    
    if (status == STATUS_ERROR)
	{
	sprintf(conn->errorMessage,
	       "connectDB() --  couldn't send complete packet: errno=%d\n%s\n", errno,strerror(errno));
	goto connect_errReturn;
	}

    /* authenticate as required*/
    if (fe_sendauth(msgtype, port, conn->pghost, 
		    conn->pguser, conn->pgpass,
		    conn->errorMessage) != STATUS_OK) {
      (void) sprintf(conn->errorMessage,
	     "connectDB() --  authentication failed with %s\n",
	       conn->pghost);
      goto connect_errReturn;	
    }
    
    /* free the password so it's not hanging out in memory forever */
    if(conn->pgpass) {
      free(conn->pgpass);
    }
    
    /* set up the socket file descriptors */
    conn->Pfout = fdopen(port->sock, "w");
    conn->Pfin = fdopen(dup(port->sock), "r");
    if (!conn->Pfout || !conn->Pfin) {
	(void) sprintf(conn->errorMessage,
	       "connectDB() -- fdopen() failed: errno=%d\n%s\n",
	       errno, strerror(errno));
      goto connect_errReturn;	
    }
    
    conn->port = port;

		{	
		struct EnvironmentOptions *eo;
		char setQuery[80]; /* mjl: size okay? XXX */
		
		for(eo = EnvironmentOptions; eo->envName; eo++)
			{
			const char *val;
			
			if((val = getenv(eo->envName)))
				{
				PGresult *res;
				
				sprintf(setQuery, "SET %s TO '%.60s'", eo->pgName, val);
				res = PQexec(conn, setQuery);
				PQclear(res);	/* Don't care? */
				}
			}
		}
    return CONNECTION_OK;

connect_errReturn:
    return CONNECTION_BAD;

}

/*
 * freePGconn
 *   - free the PGconn data structure 
 *
 */
static void 
freePGconn(PGconn *conn)
{
  if (conn->pghost) free(conn->pghost);
  if (conn->pgtty) free(conn->pgtty);
  if (conn->pgoptions) free(conn->pgoptions);
  if (conn->pgport) free(conn->pgport);
  if (conn->dbName) free(conn->dbName);
  if (conn->pguser) free(conn->pguser);
  if (conn->notifyList) DLFreeList(conn->notifyList);
  free(conn);
}

/*
   closePGconn
     - properly close a connection to the backend
*/
static void
closePGconn(PGconn *conn)
{
/* GH: What to do for !USE_POSIX_SIGNALS ? */
#if defined(USE_POSIX_SIGNALS)
    struct sigaction ignore_action;
      /* This is used as a constant, but not declared as such because the
         sigaction structure is defined differently on different systems */
    struct sigaction oldaction;

    /* If connection is already gone, that's cool.  No reason for kernel
       to kill us when we try to write to it.  So ignore SIGPIPE signals.
       */
    ignore_action.sa_handler = SIG_IGN;
    sigemptyset(&ignore_action.sa_mask);
    ignore_action.sa_flags = 0;
    sigaction(SIGPIPE, (struct sigaction *) &ignore_action, &oldaction);

    fputs("X\0", conn->Pfout);
    fflush(conn->Pfout);
    sigaction(SIGPIPE, &oldaction, NULL);
#else
    signal(SIGPIPE, SIG_IGN);
    fputs("X\0", conn->Pfout);
    fflush(conn->Pfout);
    signal(SIGPIPE, SIG_DFL);
#endif
    if (conn->Pfout) fclose(conn->Pfout);
    if (conn->Pfin)  fclose(conn->Pfin);
    if (conn->Pfdebug) fclose(conn->Pfdebug);
    conn->status = CONNECTION_BAD;  /* Well, not really _bad_ - just absent */
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
  if (!conn) {
    fprintf(stderr,"PQfinish() -- pointer to PGconn is null");
  } else {
    if (conn->status == CONNECTION_OK)
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
  if (!conn) {
    fprintf(stderr,"PQreset() -- pointer to PGconn is null");
  } else {
    closePGconn(conn);
    conn->status = connectDB(conn);
  }
}

/*
 * PacketSend()
 *
 this is just like PacketSend(), defined in backend/libpq/pqpacket.c
 but we define it here to avoid linking in all of libpq.a

 * packetSend -- send a single-packet message.
 *
 * RETURNS: STATUS_ERROR if the write fails, STATUS_OK otherwise.
 * SIDE_EFFECTS: may block.
 * NOTES: Non-blocking writes would significantly complicate 
 *	buffer management.  For now, we're not going to do it.
 *
*/
int
packetSend(Port *port,
	   PacketBuf *buf,
	   PacketLen len,
	   bool nonBlocking)
{
    PacketLen	totalLen;
    int		addrLen = sizeof(struct sockaddr_in);
    
    totalLen = len;
    
    len = sendto(port->sock, (Addr) buf, totalLen, /* flags */ 0,
		 (struct sockaddr *)&(port->raddr), addrLen);
    
    if (len < totalLen) {
	return(STATUS_ERROR);
    }
    
    return(STATUS_OK);
}

/*
 * startup2PacketBuf()
 *
 * this is just like StartupInfo2Packet(), defined in backend/libpq/pqpacket.c
 * but we repeat it here so we don't have to link in libpq.a
 * 
 * converts a StartupInfo structure to a PacketBuf
 */
static void
startup2PacketBuf(StartupInfo* s, PacketBuf* res)
{
  char* tmp;

/*  res = (PacketBuf*)malloc(sizeof(PacketBuf)); */
  res->len = htonl(sizeof(PacketBuf));
  /* use \n to delimit the strings */
  res->data[0] = '\0';

  tmp= res->data;

  strncpy(tmp, s->database, sizeof(s->database));
  tmp += sizeof(s->database);
  strncpy(tmp, s->user, sizeof(s->user));
  tmp += sizeof(s->user);
  strncpy(tmp, s->options, sizeof(s->options));
  tmp += sizeof(s->options);
  strncpy(tmp, s->execFile, sizeof(s->execFile));
  tmp += sizeof(s->execFile);
  strncpy(tmp, s->tty, sizeof(s->execFile));
}

/* ----------------
 * Conninfo parser routine
 * ----------------
 */
static int conninfo_parse(const char *conninfo, char *errorMessage)
{
    char *pname;
    char *pval;
    char *buf;
    char *tmp;
    char *cp;
    char *cp2;
    PQconninfoOption *option;
    char errortmp[ERROR_MSG_LENGTH];

    conninfo_free();

    if((buf = strdup(conninfo)) == NULL) {
        strcpy(errorMessage, 
		"FATAL: cannot allocate memory for copy of conninfo string\n");
        return -1;
    }
    cp = buf;

    while(*cp) {
	/* Skip blanks before the parameter name */
        if(isspace(*cp)) {
	    cp++;
	    continue;
	}

	/* Get the parameter name */
	pname = cp;
	while(*cp) {
	    if(*cp == '=') {
	        break;
	    }
	    if(isspace(*cp)) {
	        *cp++ = '\0';
		while(*cp) {
		    if(!isspace(*cp)) {
		        break;
		    }
		    cp++;
		}
		break;
	    }
	    cp++;
	}

	/* Check that there is a following '=' */
	if(*cp != '=') {
	    sprintf(errorMessage,
	        "ERROR: PQconnectdb() - Missing '=' after '%s' in conninfo\n",
		pname);
	    free(buf);
	    return -1;
	}
	*cp++ = '\0';

	/* Skip blanks after the '=' */
	while(*cp) {
	    if(!isspace(*cp)) {
	        break;
	    }
	    cp++;
	}

	pval = cp;

	if(*cp != '\'') {
	    cp2 = pval;
	    while(*cp) {
	        if(isspace(*cp)) {
		    *cp++ = '\0';
		    break;
		}
		if(*cp == '\\') {
		    cp++;
		    if(*cp != '\0') {
		        *cp2++ = *cp++;
		    }
		} else {
		    *cp2++ = *cp++;
		}
	    }
	    *cp2  = '\0';
	} else {
	    cp2 = pval;
	    cp++;
	    for(;;) {
	        if(*cp == '\0') {
		    sprintf(errorMessage,
		      "ERROR: PQconnectdb() - unterminated quoted string in conninfo\n");
		    free(buf);
		    return -1;
		}
		if(*cp == '\\') {
		    cp++;
		    if(*cp != '\0') {
		        *cp2++ = *cp++;
		    }
		    continue;
		}
		if(*cp == '\'') {
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
        for(option = PQconninfoOptions; option->keyword != NULL; option++) {
	    if(!strcmp(option->keyword, pname)) {
	        break;
	    }
	}
	if(option->keyword == NULL) {
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
    for(option = PQconninfoOptions; option->keyword != NULL; option++) {
	if(option->val != NULL)  continue;	/* Value was in conninfo */

	/* ----------
	 * Try to get the environment variable fallback
	 * ----------
	 */
	if(option->environ != NULL) {
	    if((tmp = getenv(option->environ)) != NULL) {
	        option->val = strdup(tmp);
		continue;
	    }
	}

	/* ----------
	 * No environment variable specified or this one isn't set -
	 * try compiled in
	 * ----------
	 */
	if(option->compiled != NULL) {
	    option->val = strdup(option->compiled);
	    continue;
	}

	/* ----------
	 * Special handling for user
	 * ----------
	 */
	if(!strcmp(option->keyword, "user")) {
	    tmp = fe_getauthname(errortmp);
	    if (tmp) {
	        option->val = strdup(tmp);
	    }
	}

	/* ----------
	 * Special handling for dbname
	 * ----------
	 */
	if(!strcmp(option->keyword, "dbname")) {
	    tmp = conninfo_getval("user");
	    if (tmp) {
	        option->val = strdup(tmp);
	    }
	}
    }

    return 0;
}


static char*
conninfo_getval(char *keyword)
{
    PQconninfoOption *option;

    for(option = PQconninfoOptions; option->keyword != NULL; option++) {
        if (!strcmp(option->keyword, keyword)) {
	    return option->val;
	}
    }

    return NULL;
}


static void
conninfo_free()
{
    PQconninfoOption *option;

    for(option = PQconninfoOptions; option->keyword != NULL; option++) {
        if(option->val != NULL) {
	    free(option->val);
	    option->val = NULL;
	}
    }
}

/* =========== accessor functions for PGconn ========= */
char* 
PQdb(PGconn* conn)
{
  if (!conn) {
    fprintf(stderr,"PQdb() -- pointer to PGconn is null");
    return (char *)NULL;
  }
  return conn->dbName;
}

char* 
PQuser(PGconn* conn)
{
  if (!conn) {
    fprintf(stderr,"PQuser() -- pointer to PGconn is null");
    return (char *)NULL;
  }
  return conn->pguser;
}

char* 
PQhost(PGconn* conn)
{
  if (!conn) {
    fprintf(stderr,"PQhost() -- pointer to PGconn is null");
    return (char *)NULL;
  }

  return conn->pghost;
}

char* 
PQoptions(PGconn* conn)
{
  if (!conn) {
    fprintf(stderr,"PQoptions() -- pointer to PGconn is null");
    return (char *)NULL;
  }
  return conn->pgoptions;
}

char* 
PQtty(PGconn* conn)
{
  if (!conn) {
    fprintf(stderr,"PQtty() -- pointer to PGconn is null");
    return (char *)NULL;
  }
  return conn->pgtty;
}

char*
PQport(PGconn* conn)
{
  if (!conn) {
    fprintf(stderr,"PQport() -- pointer to PGconn is null");
    return (char *)NULL;
  }
  return conn->pgport;
}

ConnStatusType
PQstatus(PGconn* conn)
{
  if (!conn) {
    fprintf(stderr,"PQstatus() -- pointer to PGconn is null");
    return CONNECTION_BAD;
  }
  return conn->status;
}

char* 
PQerrorMessage(PGconn* conn)
{
  if (!conn) {
    fprintf(stderr,"PQerrorMessage() -- pointer to PGconn is null");
    return (char *)NULL;
  }
  return conn->errorMessage;
}

void
PQtrace(PGconn *conn, FILE* debug_port)
{
  if (conn == NULL ||
      conn->status == CONNECTION_BAD) {
    return;
  }
  PQuntrace(conn);
  conn->Pfdebug = debug_port;
}

void 
PQuntrace(PGconn *conn)
{
  if (conn == NULL ||
      conn->status == CONNECTION_BAD) {
    return;
  }
  if (conn->Pfdebug) {
    fflush(conn->Pfdebug);
    fclose(conn->Pfdebug);
    conn->Pfdebug = NULL;
  }
}
