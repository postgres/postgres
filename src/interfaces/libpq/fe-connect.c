/*-------------------------------------------------------------------------
 *
 * fe-connect.c--
 *    functions related to setting up a connection to the backend
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/interfaces/libpq/fe-connect.c,v 1.4.2.2 1996/08/19 13:40:26 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */

#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <netdb.h>
#include <errno.h>
#include "libpq/pqcomm.h" /* for decls of MsgType, PacketBuf, StartupInfo */
#include "fe-auth.h"
#include "libpq-fe.h"

#if defined(PORTNAME_ultrix4) || defined(PORTNAME_next)
  /* ultrix is lame and doesn't have strdup in libc for some reason */
 /* [TRH] So doesn't NEXTSTEP.  But whaddaya expect for a non-ANSI  
standard function? (My, my. Touchy today, are we?) */
static
char *
strdup(char *string)
{
    char *nstr;

    nstr = strcpy((char *)malloc(strlen(string)+1), string);
    return nstr;
}
#endif

/* use a local version instead of the one found in pqpacket.c */
static ConnStatusType connectDB(PGconn *conn);

static int packetSend(Port *port, PacketBuf *buf, PacketLen len,
		      bool nonBlocking);
static void startup2PacketBuf(StartupInfo* s, PacketBuf* res);
static void freePGconn(PGconn *conn);
static void closePGconn(PGconn *conn);

#define NOTIFYLIST_INITIAL_SIZE 10
#define NOTIFYLIST_GROWBY 10

/* ----------------
 *	PQsetdb
 * 
 * establishes a connectin to a postgres backend through the postmaster
 * at the specified host and port.
 *
 * returns a PGconn* which is needed for all subsequent libpq calls
 * if the status field of the connection returned is CONNECTION_BAD,
 * then some fields may be null'ed out instead of having valid values 
 * ----------------
 */
PGconn* 
PQsetdb(char *pghost, char* pgport, char* pgoptions, char* pgtty, char* dbName)
{
  PGconn *conn;
  char *tmp;

  conn = (PGconn*)malloc(sizeof(PGconn));

  if (conn == NULL) {
    fprintf(stderr,
            "FATAL: PQsetdb() -- unable to allocate memory for a PGconn");
    return (PGconn*)NULL;
  }
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
	    tmp = POSTPORT;
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
    if (((tmp = dbName) && (dbName[0] != '\0')) ||
        ((tmp = getenv("PGDATABASE")))) {
      conn->dbName = strdup(tmp);
    } else {
	char errorMessage[ERROR_MSG_LENGTH];
	if ((tmp = fe_getauthname(errorMessage)) != 0) {
          conn->dbName = strdup(tmp);
          free((char *)tmp);
        } else {
	    sprintf(conn->errorMessage,
		    "FATAL: PQsetdb: Unable to determine a database name!\n");
	    conn->dbName = NULL;
	    return conn;
	}
    }
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
  }     
  return conn;
}

/*
 * connectDB -
 * make a connection to the database,  returns 1 if successful or 0 if not
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

    char        *user;
    /*
    //
    // Initialize the startup packet. 
    //
    // This data structure is used for the seq-packet protocol.  It
    // describes the frontend-backend connection.
    //
    //
    */
    user = fe_getauthname(conn->errorMessage);
    if (!user)
	goto connect_errReturn;
    strncpy(startup.user,user,sizeof(startup.user));
    free(user);
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
		    conn->errorMessage) != STATUS_OK) {
      (void) sprintf(conn->errorMessage,
	     "connectDB() --  authentication failed with %s\n",
	       conn->pghost);
      goto connect_errReturn;	
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
    fputs("X\0", conn->Pfout);
    fflush(conn->Pfout);
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
static int
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
