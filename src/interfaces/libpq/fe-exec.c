/*-------------------------------------------------------------------------
 *
 * fe-exec.c--
 *    functions related to sending a query down to the backend
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/interfaces/libpq/fe-exec.c,v 1.10 1996/07/31 02:20:59 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "postgres.h"
#include "libpq/pqcomm.h"
#include "libpq-fe.h"
#include <signal.h>
#include <sys/ioctl.h>
#if defined(PORTNAME_sparc_solaris) || \
    defined(PORTNAME_i386_solaris)
#include <sys/termios.h>
#endif

#ifdef TIOCGWINSZ
struct winsize screen_size;
#else
struct winsize {
  int ws_row;
  int ws_col;
} screen_size;
#endif

/* the tuples array in a PGresGroup  has to grow to accommodate the tuples */
/* returned.  Each time, we grow by this much: */
#define TUPARR_GROW_BY 100

/* keep this in same order as ExecStatusType in pgtclCmds.h */
char* pgresStatus[] = {
    "PGRES_EMPTY_QUERY",
    "PGRES_COMMAND_OK",
    "PGRES_TUPLES_OK",
    "PGRES_BAD_RESPONSE",
    "PGRES_NONFATAL_ERROR",
    "PGRES_FATAL_ERROR"
};


static PGresult* makePGresult(PGconn *conn, char *pname);
static void addTuple(PGresult *res, PGresAttValue *tup);
static PGresAttValue* getTuple(PGconn *conn, PGresult *res, int binary);
static PGresult* makeEmptyPGresult(PGconn *conn, ExecStatusType status);
static void fill(int length, int max, char filler, FILE *fp);

/*
 * PQclear -
 *    free's the memory associated with a PGresult
 *
 */
void
PQclear(PGresult* res)
{
    int i,j;

    if (!res)
	return;

    /* free all the tuples */
    for (i=0;i<res->ntups;i++) {
	for (j=0;j<res->numAttributes;j++) {
	    if (res->tuples[i][j].value)
		free(res->tuples[i][j].value);
	}
	free(res->tuples[i]);
    }
    free(res->tuples);

    /* free all the attributes */
    for (i=0;i<res->numAttributes;i++) {
	if (res->attDescs[i].name) 
	    free(res->attDescs[i].name);
    }
    free(res->attDescs);
	
    /* free the structure itself */
    free(res);
}

/*
 * PGresult -
 *   returns a newly allocated, initialized PGresult
 *
 */

static PGresult*
makeEmptyPGresult(PGconn *conn, ExecStatusType status)
{
  PGresult *result;

  result = (PGresult*)malloc(sizeof(PGresult));

  result->conn = conn;
  result->ntups = 0;
  result->numAttributes = 0;
  result->attDescs = NULL;
  result->tuples = NULL;
  result->tupArrSize = 0;
  result->resultStatus = status;
  result->cmdStatus[0] = '\0';
  result->binary = 0;
  return result;
}

/*
 * getTuple -
 *   get the next tuple from the stream
 *
 *  the CALLER is responsible from freeing the PGresAttValue returned 
 */

static PGresAttValue*
getTuple(PGconn *conn, PGresult* result, int binary)
{
  char bitmap[MAX_FIELDS]; /* the backend sends us a bitmap of  */
                           /* which attributes are null */
  int bitmap_index = 0;
  int i;
  int nbytes;              /* the number of bytes in bitmap  */
  char 	bmap;		   /*  One byte of the bitmap */
  int 	bitcnt = 0; 	   /* number of bits examined in current byte */
  int 	vlen;		   /* length of the current field value */
  FILE *Pfin = conn->Pfin;
  FILE *Pfdebug = conn->Pfdebug;

  PGresAttValue* tup;

  int nfields = result->numAttributes;

  result->binary = binary;

  tup = (PGresAttValue*) malloc(nfields * sizeof(PGresAttValue));

  nbytes = nfields / BYTELEN;
  if ( (nfields % BYTELEN) > 0)
    nbytes++;

  if (pqGetnchar(bitmap, nbytes, Pfin, Pfdebug) == 1){
      sprintf(conn->errorMessage,
	      "Error reading null-values bitmap from tuple data stream\n");
      return NULL;
    }

  bmap = bitmap[bitmap_index];
  
  for (i=0;i<nfields;i++) {
    if (!(bmap & 0200)) {
	/* if the field value is absent, make it '\0' */
	/* XXX this makes it impossible to distinguish NULL 
	   attributes from "".  Is that OK?   */
	tup[i].value = (char*)malloc(1);
	tup[i].value[0] = '\0';
	tup[i].len = 0;
    }
    else {
      /* get the value length (the first four bytes are for length) */
      pqGetInt(&vlen, VARHDRSZ, Pfin, Pfdebug);
      if (binary == 0) {
	vlen = vlen - VARHDRSZ;
	}
      if (vlen < 0)
	  vlen = 0;
      tup[i].len = vlen;
      tup[i].value = (char*) malloc(vlen + 1);
      /* read in the value; */
      if (vlen > 0)
	  pqGetnchar((char*)(tup[i].value), vlen, Pfin, Pfdebug);
      tup[i].value[vlen] = '\0';
    }
    /* get the appropriate bitmap */
    bitcnt++;
    if (bitcnt == BYTELEN) {
      bitmap_index++;
      bmap = bitmap[bitmap_index];
      bitcnt = 0;
    } else
      bmap <<= 1;
  }

  return tup;
}


/*
 * addTuple
 *    add a tuple to the PGresult structure, growing it if necessary
 *  to accommodate
 *
 */
static void 
addTuple(PGresult* res, PGresAttValue* tup)
{
  if (res->ntups == res->tupArrSize) { 
    /* grow the array */
    res->tupArrSize += TUPARR_GROW_BY;
    
    if (res->ntups == 0)
      res->tuples = (PGresAttValue**) 
	malloc(res->tupArrSize * sizeof(PGresAttValue*));
    else
    /* we can use realloc because shallow copying of the structure is okay */
      res->tuples = (PGresAttValue**) 
	realloc(res->tuples, res->tupArrSize * sizeof(PGresAttValue*));
    }

  res->tuples[res->ntups] = tup;
  res->ntups++;
}

/*
 * PGresult
 *    fill out the PGresult structure with result tuples from the backend
 *  this is called after query has been successfully run and we have
 *  a portal name
 *
 *  ASSUMPTION: we assume only *1* tuple group is returned from the backend
 *
 *  the CALLER is reponsible for free'ing the new PGresult allocated here
 *
 */

static PGresult*
makePGresult(PGconn* conn, char* pname)
{
  PGresult* result;
  int id;
  int nfields;
  int i;
  int done = 0;

  PGresAttValue* newTup;

  FILE* Pfin = conn->Pfin;
  FILE* Pfdebug = conn->Pfdebug;

  result = makeEmptyPGresult(conn, PGRES_TUPLES_OK);
  
  /* makePGresult() should only be called when the */
  /* id of the stream is 'T' to start with */

  /* the next two bytes are the number of fields  */
  if (pqGetInt(&nfields, 2, Pfin, Pfdebug) == 1) {
    sprintf(conn->errorMessage,
	    "could not get the number of fields from the 'T' message\n");
    goto makePGresult_badResponse_return;
  }
  else
    result->numAttributes = nfields;

  /* allocate space for the attribute descriptors */
  if (nfields > 0) {
    result->attDescs = (PGresAttDesc*) malloc(nfields * sizeof(PGresAttDesc));
  }

  /* get type info */
  for (i=0;i<nfields;i++) {
    char typName[MAX_MESSAGE_LEN];
    int adtid;
    int adtsize;
    
    if ( pqGets(typName, MAX_MESSAGE_LEN, Pfin, Pfdebug) ||
	pqGetInt(&adtid, 4, Pfin, Pfdebug) ||
	pqGetInt(&adtsize, 2, Pfin, Pfdebug)) {
      sprintf(conn->errorMessage,
	      "error reading type information from the 'T' message\n");
      goto makePGresult_badResponse_return;
    }
   result->attDescs[i].name = malloc(strlen(typName)+1);
   strcpy(result->attDescs[i].name,typName);
   result->attDescs[i].adtid = adtid;
   result->attDescs[i].adtsize = adtsize; /* casting from int to int2 here */
  }

  id = pqGetc(Pfin,Pfdebug);

  /* process the data stream until we're finished */
  while(!done) {
    switch (id) {
    case 'T': /* a new tuple group */
      sprintf(conn->errorMessage,
	      "makePGresult() -- is not equipped to handle multiple tuple groups.\n");
      goto makePGresult_badResponse_return;
    case 'B': /* a tuple in binary format */
    case 'D': /* a tuple in ASCII format */
      newTup = getTuple(conn, result, (id == 'B'));
      if (newTup == NULL) 
	goto makePGresult_badResponse_return;
      addTuple(result,newTup);
      break;
/*    case 'A':    
      sprintf(conn->errorMessage, "Asynchronous portals not supported");
      result->resultStatus = PGRES_NONFATAL_ERROR;
      return result;
      break;
*/
    case 'C': /* end of portal tuple stream */
      {
      char command[MAX_MESSAGE_LEN];
      pqGets(command,MAX_MESSAGE_LEN, Pfin, Pfdebug); /* read the command tag */
      done = 1;
    }
      break;
    case 'E': /* errors */
      if (pqGets(conn->errorMessage, ERROR_MSG_LENGTH, Pfin, Pfdebug) == 1) {
	sprintf(conn->errorMessage,
		"Error return detected from backend, but error message cannot be read");
      }
      result->resultStatus = PGRES_FATAL_ERROR;
      return result;
      break;
    case 'N': /* notices from the backend */
      if (pqGets(conn->errorMessage, ERROR_MSG_LENGTH, Pfin, Pfdebug) == 1) {
	sprintf(conn->errorMessage,
	"Notice return detected from backend, but error message cannot be read");
      }   else
	/* XXXX send Notices to stderr for now */
	fprintf(stderr, "%s\n", conn->errorMessage);
      break;
    default: /* uh-oh
		this should never happen but frequently does when the 
		backend dumps core */
      sprintf(conn->errorMessage,"FATAL:  unexpected results from the backend, it probably dumped core.");
      fprintf(stderr, conn->errorMessage);
      result->resultStatus = PGRES_FATAL_ERROR;
      return result;
      break;
    }
    if (!done)
      id = getc(Pfin);
  } /* while (1) */

  result->resultStatus = PGRES_TUPLES_OK;
  return result;

makePGresult_badResponse_return:
  result->resultStatus = PGRES_BAD_RESPONSE;
  return result;

}



/*
 * PQexec
 *    send a query to the backend and package up the result in a Pgresult
 *
 *  if the query failed, return NULL, conn->errorMessage is set to 
 * a relevant message
 *  if query is successful, a new PGresult is returned
 * the use is responsible for freeing that structure when done with it
 *
 */

PGresult*
PQexec(PGconn* conn, char* query)
{
  PGresult *result;
  int id, clear;
  char buffer[MAX_MESSAGE_LEN];
  char cmdStatus[MAX_MESSAGE_LEN];
  char pname[MAX_MESSAGE_LEN]; /* portal name */
  PGnotify *newNotify;
  FILE *Pfin, *Pfout, *Pfdebug;

  pname[0]='\0';

  if (!conn) return NULL;
  if (!query) {
    sprintf(conn->errorMessage, "PQexec() -- query pointer is null.");
    return NULL;
  }

  Pfin = conn->Pfin;
  Pfout = conn->Pfout;
  Pfdebug = conn->Pfdebug;

  /*clear the error string */
  conn->errorMessage[0] = '\0';

  /* check to see if the query string is too long */
  if (strlen(query) > MAX_MESSAGE_LEN) {
    sprintf(conn->errorMessage, "PQexec() -- query is too long.  Maximum length is %d\n", MAX_MESSAGE_LEN -2 );
    return NULL;
  }

  /* the frontend-backend protocol uses 'Q' to designate queries */
  sprintf(buffer,"Q%s",query);

  /* send the query to the backend; */
  if (pqPuts(buffer,Pfout, Pfdebug) == 1) {
      (void) sprintf(conn->errorMessage,
		     "PQexec() -- while sending query:  %s\n-- fprintf to Pfout failed: errno=%d\n%s\n",
		     query, errno,strerror(errno));
      return NULL;
    }

  /* loop forever because multiple messages, especially NOTICES,
     can come back from the backend
     NOTICES are output directly to stderr
   */

  while (1) {

    /* read the result id */
    id = pqGetc(Pfin,Pfdebug);
    if (id == EOF) {
      /* hmm,  no response from the backend-end, that's bad */
      (void) sprintf(conn->errorMessage,
		     "PQexec() -- No response from backend\n");
      return (PGresult*)NULL;
    }

    switch (id) {
    case 'A': 
	newNotify = (PGnotify*)malloc(sizeof(PGnotify));
	pqGetInt(&(newNotify->be_pid), 4, Pfin, Pfdebug);
	pqGets(newNotify->relname, NAMEDATALEN, Pfin, Pfdebug);
	DLAddTail(conn->notifyList, DLNewElem(newNotify));
	/* async messages are piggy'ed back on other messages,
	   so we stay in the while loop for other messages */
	break;
    case 'C': /* portal query command, no tuples returned */
      if (pqGets(cmdStatus, MAX_MESSAGE_LEN, Pfin, Pfdebug) == 1) {
	sprintf(conn->errorMessage,
		"PQexec() -- query command completed, but return message from backend cannot be read");
	return (PGresult*)NULL;
      } 
      else {
	/*
	// since backend may produce more than one result for some commands
	// need to poll until clear 
	// send an empty query down, and keep reading out of the pipe
	// until an 'I' is received.
	*/
	clear = 0;

	pqPuts("Q ",Pfout,Pfdebug); /* send an empty query */
	while (!clear)
	  {
	    if (pqGets(buffer,ERROR_MSG_LENGTH,Pfin,Pfdebug) == 1)
	      clear = 1;
	    clear = (buffer[0] == 'I');
	  }
	result = makeEmptyPGresult(conn,PGRES_COMMAND_OK);
	strncpy(result->cmdStatus,cmdStatus, CMDSTATUS_LEN-1);
	return result;
      }
      break;
    case 'E': /* error return */
      if (pqGets(conn->errorMessage, ERROR_MSG_LENGTH, Pfin, Pfdebug) == 1) {
	(void) sprintf(conn->errorMessage,
		       "PQexec() -- error return detected from backend, but error message cannot be read");
      }
      return (PGresult*)NULL;
      break;
    case 'I': /* empty query */
      /* read the throw away the closing '\0' */
      {
	int c;
	if ((c = pqGetc(Pfin,Pfdebug)) != '\0') {
	  fprintf(stderr,"error!, unexpected character %c following 'I'\n", c);
	}
	result = makeEmptyPGresult(conn, PGRES_EMPTY_QUERY);
	return result;
      }
      break;
    case 'N': /* notices from the backend */
      if (pqGets(conn->errorMessage, ERROR_MSG_LENGTH, Pfin, Pfdebug) == 1) {
	sprintf(conn->errorMessage,
		"PQexec() -- error return detected from backend, but error message cannot be read");
	return (PGresult*)NULL;
      }
      else
	fprintf(stderr,"%s", conn->errorMessage);
      break;
    case 'P': /* synchronous (normal) portal */
      pqGets(pname,MAX_MESSAGE_LEN,Pfin, Pfdebug);  /* read in the portal name*/
      break;
    case 'T': /* actual tuple results: */
      return makePGresult(conn, pname);
      break;
    case 'D': /* copy command began successfully */
      return makeEmptyPGresult(conn,PGRES_COPY_IN);
      break;
    case 'B': /* copy command began successfully */
      return makeEmptyPGresult(conn,PGRES_COPY_OUT);
      break;
    default:
      sprintf(conn->errorMessage,
	      "unknown protocol character %c read from backend\n",
	      id);
      return (PGresult*)NULL;
    } /* switch */
} /* while (1)*/

}

/*
 * PQnotifies
 *    returns a PGnotify* structure of the latest async notification
 * that has not yet been handled
 *
 * returns NULL, if there is currently 
 * no unhandled async notification from the backend
 *
 * the CALLER is responsible for FREE'ing the structure returned
 */

PGnotify*
PQnotifies(PGconn *conn)
{
    Dlelem *e;

    if (!conn) return NULL;

    if (conn->status != CONNECTION_OK) 
	return NULL;
    /* RemHead returns NULL if list is empy */
    e = DLRemHead(conn->notifyList);
    if (e) 
	return (PGnotify*)DLE_VAL(e);
    else 
	return NULL;
}

/*
 * PQgetline - gets a newline-terminated string from the backend.
 * 
 * Chiefly here so that applications can use "COPY <rel> to stdout"
 * and read the output string.  Returns a null-terminated string in s.
 *
 * PQgetline reads up to maxlen-1 characters (like fgets(3)) but strips
 * the terminating \n (like gets(3)).
 *
 * RETURNS:
 *	EOF if it is detected or invalid arguments are given
 *	0 if EOL is reached (i.e., \n has been read)
 *		(this is required for backward-compatibility -- this
 *		 routine used to always return EOF or 0, assuming that
 *		 the line ended within maxlen bytes.)
 *	1 in other cases
 */
int
PQgetline(PGconn *conn, char *s, int maxlen)
{
    int c = '\0';

    if (!conn) return EOF;
    
    if (!conn->Pfin || !s || maxlen <= 1)
	return(EOF);
    
    for (; maxlen > 1 && 
	  (c = pqGetc(conn->Pfin, conn->Pfdebug)) != '\n' && 
	   c != EOF;
	 --maxlen) {
	*s++ = c;
    }
    *s = '\0';
    
    if (c == EOF) {
	return(EOF);		/* error -- reached EOF before \n */
    } else if (c == '\n') {
	return(0);		/* done with this line */
    }
    return(1);			/* returning a full buffer */
}


/*
 * PQputline -- sends a string to the backend.
 * 
 * Chiefly here so that applications can use "COPY <rel> from stdin".
 *
 */
void
PQputline(PGconn *conn, char *s)
{
    if (conn && (conn->Pfout)) {
	(void) fputs(s, conn->Pfout);
	fflush(conn->Pfout);
    }
}

/*
 * PQendcopy
 *	called while waiting for the backend to respond with success/failure
 *	to a "copy".
 *
 * RETURNS:
 *	0 on failure
 *	1 on success
 */
int
PQendcopy(PGconn *conn)
{
    char id;
    FILE *Pfin, *Pfdebug;

    if (!conn) return (int)NULL;

    Pfin = conn->Pfin;
    Pfdebug = conn->Pfdebug;

    if ( (id = pqGetc(Pfin,Pfdebug)) > 0)
	return(0);
    switch (id) {
    case 'Z': /* backend finished the copy */
	return(1);
    case 'E':
    case 'N':
	if (pqGets(conn->errorMessage, ERROR_MSG_LENGTH, Pfin, Pfdebug) == 1) {
	    sprintf(conn->errorMessage,
		    "Error return detected from backend, but error message cannot be read");
	}
	return(0);
	break;
    default:
	(void) sprintf(conn->errorMessage,
		       "FATAL: PQendcopy: protocol error: id=%x\n",
		       id);
	fputs(conn->errorMessage, stderr);
	fprintf(stderr,"resetting connection\n");
	PQreset(conn);
	return(0);
    }
}

/* simply send out max-length number of filler characters to fp */
static void
fill (int length, int max, char filler, FILE *fp)
{
  int count;
  char filltmp[2];

  filltmp[0] = filler;
  filltmp[1] = 0;
  count = max - length;
  while (count-- >= 0)
    {
      fprintf(fp, "%s", filltmp);
    }
 }


/*
 * PQdisplayTuples()
 * kept for backward compatibility
 */
void
PQdisplayTuples(PGresult *res,
		FILE *fp,      /* where to send the output */
		int fillAlign, /* pad the fields with spaces */
		char *fieldSep,  /* field separator */
		int printHeader, /* display headers? */
		int quiet
		)
{
#define DEFAULT_FIELD_SEP " "

    int i, j;
    int nFields;
    int nTuples;
    int fLength[MAX_FIELDS];

    if (fieldSep == NULL)
	fieldSep == DEFAULT_FIELD_SEP;

    /* Get some useful info about the results */
    nFields = PQnfields(res);
    nTuples = PQntuples(res);
  
    if (fp == NULL) 
	fp = stdout;

    /* Zero the initial field lengths */
    for (j=0  ; j < nFields; j++) {
      fLength[j] = strlen(PQfname(res,j));
    }
    /* Find the max length of each field in the result */
    /* will be somewhat time consuming for very large results */
    if (fillAlign) {
	for (i=0; i < nTuples; i++) {
	    for (j=0  ; j < nFields; j++) {
		if (PQgetlength(res,i,j) > fLength[j])
		    fLength[j] = PQgetlength(res,i,j);
	    }
	}
    }
    
    if (printHeader) {
	/* first, print out the attribute names */
	for (i=0; i < nFields; i++) {
	    fputs(PQfname(res,i), fp);
	    if (fillAlign)
		fill (strlen (PQfname(res,i)), fLength[i], ' ', fp);
	    fputs(fieldSep,fp);
	}
	fprintf(fp, "\n");
  
	/* Underline the attribute names */
	for (i=0; i < nFields; i++) {
	    if (fillAlign)
		fill (0, fLength[i], '-', fp);
	    fputs(fieldSep,fp);
	}
	fprintf(fp, "\n");
    }

    /* next, print out the instances */
    for (i=0; i < nTuples; i++) {
      for (j=0  ; j < nFields; j++) {
        fprintf(fp, "%s", PQgetvalue(res,i,j));
	if (fillAlign)
	    fill (strlen (PQgetvalue(res,i,j)), fLength[j], ' ', fp);
	fputs(fieldSep,fp);
      }
      fprintf(fp, "\n");
    }
  
    if (!quiet)
	fprintf (fp, "\nQuery returned %d row%s.\n",PQntuples(res),
		 (PQntuples(res) == 1) ? "" : "s");
  
    fflush(fp);
}



/*
 * PQprintTuples()
 *
 * kept for backward compatibility
 *
 */
void
PQprintTuples(PGresult *res,
	      FILE* fout,      /* output stream */
	      int PrintAttNames,/* print attribute names or not*/
	      int TerseOutput, /* delimiter bars or not?*/
	      int colWidth   /* width of column, if 0, use variable width */
	      )
{
    int nFields; 
    int nTups;
    int i,j;
    char formatString[80];

    char *tborder = NULL;

    nFields = PQnfields(res);
    nTups = PQntuples(res);

    if (colWidth > 0) {
      sprintf(formatString,"%%s %%-%ds",colWidth);
    } else
      sprintf(formatString,"%%s %%s");

    if ( nFields > 0 ) {  /* only print tuples with at least 1 field.  */

	if (!TerseOutput)
	{
	    int width;
	    width = nFields * 14;
	    tborder = malloc (width+1);
	    for (i = 0; i <= width; i++) 
		tborder[i] = '-';
	    tborder[i] = '\0';
	    fprintf(fout,"%s\n",tborder);
	}

	for (i=0; i < nFields; i++) {
	    if (PrintAttNames) {
		fprintf(fout,formatString,
			TerseOutput ? "" : "|",
			PQfname(res, i));
	    }
	}

	if (PrintAttNames) {
	    if (TerseOutput)
		fprintf(fout,"\n");
	    else
		fprintf(fout, "|\n%s\n",tborder);
	}
	
	for (i = 0; i < nTups; i++) {
	    for (j = 0; j < nFields; j++) {
		char *pval = PQgetvalue(res,i,j);
		fprintf(fout, formatString,
			TerseOutput ? "" : "|",
			pval ? pval : "");
	    }
	    if (TerseOutput)
		fprintf(fout,"\n");
	    else
		fprintf(fout, "|\n%s\n",tborder);
	}
    }
}

/*
 * PQprint()
 *
 * new PQprintTuples routine (proff@suburbia.net)
 * PQprintOpt is a typedef (structure) that containes
 * various flags and options. consult libpq-fe.h for
 * details
 */

void
PQprint(FILE *fout,
              PGresult *res,
	      PQprintOpt *po
              )
{
    int nFields;

    nFields = PQnfields(res);

    if ( nFields > 0 ) {  /* only print tuples with at least 1 field.  */
    	int i,j;
    	int nTups;
	int *fieldMax=NULL; /* keep -Wall happy */
	unsigned char *fieldNotNum=NULL; /* keep -Wall happy */
	char **fields=NULL; /*keep -Wall happy */
	char **fieldNames;
	int fieldMaxLen=0;
	char *border=NULL;
        int numFieldName;
	int fs_len=strlen(po->fieldSep);
 	int total_line_length = 0;
 	int usePipe = 0;
 	char *pagerenv;

    	nTups = PQntuples(res);
	if (!(fieldNames=(char **)calloc(nFields, sizeof (char *))))
	{
		perror("calloc");
		exit(1);
	}
	if (!(fieldNotNum=(unsigned char *)calloc(nFields, 1)))
	{
		perror("calloc");
		exit(1);
	}
	if (!(fieldMax=(int *)calloc(nFields, sizeof(int))))
	{
		perror("calloc");
		exit(1);
	}
	for (numFieldName=0; po->fieldName && po->fieldName[numFieldName]; numFieldName++)
		;
	for (j=0; j < nFields; j++)
	{
		int len;
		char *s=(j<numFieldName && po->fieldName[j][0])? po->fieldName[j]: PQfname(res, j);
		fieldNames[j]=s;
		len=s? strlen(s): 0;
		fieldMax[j] = len;
		/*
		if (po->header && len<5)
			len=5; 
	        */
		len+=fs_len;
		if (len>fieldMaxLen)
			fieldMaxLen=len;
		total_line_length += len;
	}
 
	total_line_length += nFields * strlen(po->fieldSep) + 1;
 
	if (fout == NULL) 
		fout = stdout;
	if (po->pager && fout == stdout && isatty(fileno(stdout))) {
		/* try to pipe to the pager program if possible */
#ifdef TIOCGWINSZ
		if (ioctl(fileno(stdout),TIOCGWINSZ,&screen_size) == -1 ||
		    screen_size.ws_col == 0 ||
		    screen_size.ws_row == 0)
		{
#endif
			screen_size.ws_row = 24;
			screen_size.ws_col = 80;
#ifdef TIOCGWINSZ
		}
#endif
		pagerenv=getenv("PAGER");
		if (pagerenv != NULL &&
		    pagerenv[0] != '\0' && 
		   !po->html3 &&
		   ((po->expanded &&
			nTups * (nFields+1) >= screen_size.ws_row) ||
		    (!po->expanded &&
			nTups * (total_line_length / screen_size.ws_col + 1) *
				( 1 + (po->standard != 0)) >=
			screen_size.ws_row -
			(po->header != 0) *
				(total_line_length / screen_size.ws_col + 1) * 2
 			/*- 1 */ /* newline at end of tuple list */
			/*- (quiet == 0)*/
			)))
		{
			fout = popen(pagerenv, "w");
			if (fout) {
				usePipe = 1;
				signal(SIGPIPE, SIG_IGN);
			} else
				fout = stdout;
		}
	}
 
	if (!po->expanded && (po->align || po->html3))
	{
		if (!(fields=(char **)calloc(nFields*(nTups+1), sizeof(char *))))
		{
			perror("calloc");
			exit(1);
		}
	} else
		if (po->header && !po->html3)
        	{
			if (po->expanded)
			{
				if (po->align)
					fprintf(fout, "%-*s%s Value\n", fieldMaxLen-fs_len, "Field", po->fieldSep);
				else
					fprintf(fout, "%s%sValue\n", "Field", po->fieldSep);
			} else
			{
				int len=0;
				for (j=0; j < nFields; j++)
				{
					char *s=fieldNames[j];
					fputs(s, fout);
					len+=strlen(s)+fs_len;
					if ((j+1)<nFields)
						fputs(po->fieldSep, fout);
				}
				fputc('\n', fout);
				for (len-=fs_len; len--; fputc('-', fout));
				fputc('\n', fout);
			}
		}
	if (po->expanded && po->html3)
	{
		if (po->caption)
			fprintf(fout, "<centre><h2>%s</h2></centre>\n", po->caption);
		else
			fprintf(fout, "<centre><h2>Query retrieved %d tuples * %d fields</h2></centre>\n", nTups, nFields);
	}
        for (i = 0; i < nTups; i++) {
	    char buf[8192*2+1];
	    if (po->expanded)
	    {
	    	if (po->html3)
			fprintf(fout, "<table %s><caption align=high>%d</caption>\n", po->tableOpt? po->tableOpt: "", i);
		else
			fprintf(fout, "-- RECORD %d --\n", i);
	    }
            for (j = 0; j < nFields; j++) {
                char *pval, *p, *o;
		int plen;
		if ((plen=PQgetlength(res,i,j))<1 || !(pval=PQgetvalue(res,i,j)) || !*pval)
		{
			if (po->align || po->expanded)
				continue;
			goto efield;
		}
		for (p=pval, o=buf; *p; *(o++)=*(p++))
		{
			if ((fs_len==1 && (*p==*(po->fieldSep))) || *p=='\\')
				*(o++)='\\';
			if (po->align && !((*p >='0' && *p<='9') || *p=='.' || *p=='E' || *p=='e' || *p==' ' || *p=='-'))
				fieldNotNum[j]=1;
		}
		*o='\0';
		if (!po->expanded && (po->align || po->html3))
		{
			int n=strlen(buf);
			if (n>fieldMax[j])
				fieldMax[j]=n;
			if (!(fields[i*nFields+j]=(char *)malloc(n+1)))
			{
				perror("malloc");
				exit(1);
			}
			strcpy(fields[i*nFields+j], buf);
		} else
		{
			if (po->expanded)
			{
				if (po->html3)
					fprintf(fout, "<tr><td align=left><b>%s</b></td><td align=%s>%s</td></tr>\n",
						fieldNames[j], fieldNotNum[j]? "left": "right", buf);
				else
				{
					if (po->align)
						fprintf(fout, "%-*s%s %s\n", fieldMaxLen-fs_len, fieldNames[j], po->fieldSep, buf);
					else
						fprintf(fout, "%s%s%s\n", fieldNames[j], po->fieldSep, buf);
				}
			}
			else
			{
				if (!po->html3)
				{
					fputs(buf, fout);
efield:
					if ((j+1)<nFields)
						fputs(po->fieldSep, fout);
					else
						fputc('\n', fout);
				}
			}
		}
	    }
	    if (po->html3 && po->expanded)
	    	fputs("</table>\n", fout);
    	}
	if (!po->expanded && (po->align || po->html3))
	{
	    	if (po->html3)
		{
			if (po->header)
			{
				if (po->caption)
			                fprintf(fout, "<table %s><caption align=high>%s</caption>\n", po->tableOpt? po->tableOpt: "", po->caption);
				else
					fprintf(fout, "<table %s><caption align=high>Retrieved %d tuples * %d fields</caption>\n", po->tableOpt? po->tableOpt: "", nTups, nFields);
			} else
			        fprintf(fout, "<table %s>", po->tableOpt? po->tableOpt: "");
		}
		if (po->header)
        	{
			if (po->html3)
				fputs("<tr>", fout);
			else
			{
				int tot=0;
				int n=0;
				char *p;
				for (; n<nFields; n++)
					tot+=fieldMax[n]+fs_len+(po->standard? 2: 0);
				if (po->standard)
					tot+=fs_len*2+2;
				if (!(p=border=malloc(tot+1)))
				{
					perror("malloc");
					exit(1);
				}
				if (po->standard)
				{
					char *fs=po->fieldSep;
					while (*fs++)
						*p++='+';
				}
				for (j=0; j <nFields; j++)
				{
					int len;
					for (len=fieldMax[j] + (po->standard? 2:0) ; len--; *p++='-');
					if (po->standard || (j+1)<nFields)
					{
						char *fs=po->fieldSep;
						while (*fs++)
							*p++='+';
					} 
				}
				*p='\0';
				if (po->standard)
					fprintf(fout, "%s\n", border);
			}
			if (po->standard)
				fputs(po->fieldSep, fout);
                	for (j=0; j < nFields; j++)
			{
				char *s=PQfname(res, j);
				if (po->html3)
				{
					fprintf(fout, "<th align=%s>%s</th>", fieldNotNum[j]? "left": "right",
						fieldNames[j]);
				} else
				{
					int n=strlen(s);
					if (n>fieldMax[j])
						fieldMax[j]=n;
					if (po->standard)
						fprintf(fout, fieldNotNum[j]? " %-*s ": " %*s ", fieldMax[j], s);
					else
						fprintf(fout, fieldNotNum[j]? "%-*s": "%*s", fieldMax[j], s);
					if (po->standard || (j+1)<nFields)
						fputs(po->fieldSep, fout);
				}
			}
			if (po->html3)
				fputs("</tr>\n", fout);
			else
				fprintf(fout, "\n%s\n", border);
		}
        	for (i = 0; i < nTups; i++)
		{
			if (po->html3)
				fputs("<tr>", fout);
			else
				if (po->standard)
					fputs(po->fieldSep, fout);
			
           		for (j = 0; j < nFields; j++)
			{
				char *p=fields[i*nFields+j];
			 	if (po->html3)
			 		fprintf(fout, "<td align=%s>%s</td>", fieldNotNum[j]? "left": "right", p? p: "");

				else
				{
			 		fprintf(fout, fieldNotNum[j]? (po->standard? " %-*s ": "%-*s"): (po->standard? " %*s ": "%*s"), fieldMax[j], p? p: "");
					if (po->standard || (j+1)<nFields)
						fputs(po->fieldSep, fout);
				}
				if (p)
					free(p);
			}
			if (po->html3)
				fputs("</tr>", fout);
			else
				if (po->standard)
					fprintf(fout, "\n%s", border);
			fputc('\n', fout);
		}
		free(fields);
	}
	free(fieldMax);
	free(fieldNotNum);
	free(fieldNames);
	if (usePipe) {
		pclose(fout);
		signal(SIGPIPE, SIG_DFL);
	}
	if (border)
		free(border);
	if (po->html3 && !po->expanded)
		fputs("</table>\n", fout);
	}
}


/* ----------------
 *	PQfn -  Send a function call to the POSTGRES backend.
 *
 *      conn            : backend connection
 *	fnid		: function id
 * 	result_buf      : pointer to result buffer (&int if integer)
 * 	result_len	: length of return value.
 *      actual_result_len: actual length returned. (differs from result_len
 *			  for varlena structures.)
 *      result_type     : If the result is an integer, this must be 1,
 *                        otherwise this should be 0
 * 	args		: pointer to a NULL terminated arg array.
 *			  (length, if integer, and result-pointer)
 * 	nargs		: # of arguments in args array.
 *
 * RETURNS
 *	NULL on failure.  PQerrormsg will be set.
 *	"G" if there is a return value.
 *	"V" if there is no return value.
 * ----------------
 */

PGresult*
PQfn(PGconn *conn,
     int fnid,
     int *result_buf,
     int *actual_result_len,
     int result_is_int,
     PQArgBlock *args,
     int nargs)
{
    FILE *Pfin, *Pfout, *Pfdebug;
    int id;
    int i;

    if (!conn) return NULL;

    Pfin = conn->Pfin;
    Pfout = conn->Pfout;
    Pfdebug = conn->Pfdebug;

    /* clear the error string */
    conn->errorMessage[0] = '\0';

    pqPuts("F ",Pfout,Pfdebug);           /* function */
    pqPutInt(fnid, 4, Pfout, Pfdebug);    /* function id */
    pqPutInt(nargs, 4, Pfout, Pfdebug);	     /*	# of args */

    for (i = 0; i < nargs; ++i) { /*	len.int4 + contents	*/
	pqPutInt(args[i].len, 4, Pfout, Pfdebug);
	if (args[i].isint) {
	    pqPutInt(args[i].u.integer, 4, Pfout, Pfdebug);
	} else {
	    pqPutnchar((char *)args[i].u.ptr, args[i].len, Pfout, Pfdebug);
	}
    }
    pqFlush(Pfout, Pfdebug);

    id = pqGetc(Pfin, Pfdebug);
    if (id != 'V') {
	if (id == 'E') {
	    pqGets(conn->errorMessage,ERROR_MSG_LENGTH,Pfin,Pfdebug);
	} else
	    sprintf(conn->errorMessage,
		    "PQfn: expected a 'V' from the backend. Got '%c' instead",
		    id);
	return makeEmptyPGresult(conn,PGRES_FATAL_ERROR);
    }

    id = pqGetc(Pfin, Pfdebug);
    for (;;) {
	int c;
	switch (id) {
	case 'G':		/* function returned properly */
	    pqGetInt(actual_result_len,4,Pfin,Pfdebug);
	    if (result_is_int) {
		pqGetInt(result_buf,4,Pfin,Pfdebug);
	    } else {
		pqGetnchar((char *) result_buf, *actual_result_len,
			   Pfin, Pfdebug);
	    }
	    c = pqGetc(Pfin, Pfdebug); /* get the last '0'*/
	    return makeEmptyPGresult(conn,PGRES_COMMAND_OK);
	case 'E':
	    sprintf(conn->errorMessage,
		    "PQfn: returned an error");
	    return makeEmptyPGresult(conn,PGRES_FATAL_ERROR);
	case 'N':
	    /* print notice and go back to processing return values */
	    if (pqGets(conn->errorMessage, ERROR_MSG_LENGTH, Pfin, Pfdebug) == 1) {
		sprintf(conn->errorMessage,
			"Notice return detected from backend, but error message cannot be read");
	    }   else
		fprintf(stderr, "%s\n", conn->errorMessage);
	    /* keep iterating */
	    break;
	case '0':		/* no return value */
	    return makeEmptyPGresult(conn,PGRES_COMMAND_OK);
	default:
	    /* The backend violates the protocol. */
	    sprintf(conn->errorMessage,
		    "FATAL: PQfn: protocol error: id=%x\n", id);
	    return makeEmptyPGresult(conn,PGRES_FATAL_ERROR);
	}
    }
}

/* ====== accessor funcs for PGresult ======== */

ExecStatusType 
PQresultStatus(PGresult* res)
{ 
    if (!res) {
      fprintf(stderr, "PQresultStatus() -- pointer to PQresult is null");
      return PGRES_NONFATAL_ERROR;
    }

    return res->resultStatus; 
}

int 
PQntuples(PGresult *res) 
{
    if (!res) {
      fprintf(stderr, "PQntuples() -- pointer to PQresult is null");
      return (int)NULL;
    }
    return res->ntups;
}

int
PQnfields(PGresult *res) 
{
    if (!res) {
      fprintf(stderr, "PQnfields() -- pointer to PQresult is null");
      return (int)NULL;
    }
    return res->numAttributes;
}

/*
   returns NULL if the field_num is invalid
*/
char* 
PQfname(PGresult *res, int field_num) 
{
    if (!res) {
      fprintf(stderr, "PQfname() -- pointer to PQresult is null");
      return NULL;
    }

    if (field_num > (res->numAttributes - 1))  {
	fprintf(stderr,
		"PQfname: ERROR! name of field %d(of %d) is not available", 
		field_num, res->numAttributes -1);
	return NULL;
    }
    if (res->attDescs) {
	return res->attDescs[field_num].name;
    } else
	return NULL;
}

/*
   returns -1 on a bad field name
*/
int
PQfnumber(PGresult *res, char* field_name) 
{
  int i;

  if (!res) {
    fprintf(stderr, "PQfnumber() -- pointer to PQresult is null");
    return -1;
  }

  if (field_name == NULL ||
      field_name[0] == '\0' ||
      res->attDescs == NULL)
    return  -1;

  for (i=0;i<res->numAttributes;i++) {
    if ( strcmp(field_name, res->attDescs[i].name) == 0 )
      return i;
  }
  return -1;

}

Oid
PQftype(PGresult *res, int field_num) 
{
    if (!res) {
      fprintf(stderr, "PQftype() -- pointer to PQresult is null");
      return InvalidOid;
    }

    if (field_num > (res->numAttributes - 1))  {
	fprintf(stderr,
		"PQftype: ERROR! type of field %d(of %d) is not available", 
		field_num, res->numAttributes -1);
    }
    if (res->attDescs) {
	return res->attDescs[field_num].adtid;
    } else
	return InvalidOid;
}

int2
PQfsize(PGresult *res, int field_num) 
{
    if (!res) {
      fprintf(stderr, "PQfsize() -- pointer to PQresult is null");
      return (int2)NULL;
    }

    if (field_num > (res->numAttributes - 1))  {
	fprintf(stderr,
		"PQfsize: ERROR! size of field %d(of %d) is not available", 
		field_num, res->numAttributes -1);
    }
    if (res->attDescs) {
	return res->attDescs[field_num].adtsize;
    } else
	return 0;
}

char* PQcmdStatus(PGresult *res) {
  if (!res) {
    fprintf(stderr, "PQcmdStatus() -- pointer to PQresult is null");
    return NULL;
  }
  return res->cmdStatus;
}

/*
   PQoidStatus -
    if the last command was an INSERT, return the oid string 
    if not, return ""
*/
char* PQoidStatus(PGresult *res) {
  if (!res) {
    fprintf(stderr, "PQoidStatus() -- pointer to PQresult is null");
    return NULL;
  }

  if (!res->cmdStatus)
    return "";

  if (strncmp(res->cmdStatus, "INSERT",6) == 0) {
    return res->cmdStatus+7;
  } else
    return "";
}

/*
   PQgetvalue:
    return the attribute value of field 'field_num' of
    tuple 'tup_num'

    If res is binary, then the value returned is NOT a null-terminated 
    ASCII string, but the binary representation in the server's native
    format.

    if res is not binary, a null-terminated ASCII string is returned.
*/
char* 
PQgetvalue(PGresult *res, int tup_num, int field_num)
{
    if (!res) {
      fprintf(stderr, "PQgetvalue() -- pointer to PQresult is null");
      return NULL;
    }

    if (tup_num > (res->ntups - 1) ||
	field_num > (res->numAttributes - 1))  {
	fprintf(stderr,
		"PQgetvalue: ERROR! field %d(of %d) of tuple %d(of %d) is not available", 
		field_num, res->numAttributes - 1, tup_num, res->ntups);
    }
    
    return res->tuples[tup_num][field_num].value;
}

/* PQgetlength:
     returns the length of a field value in bytes.  If res is binary,
     i.e. a result of a binary portal, then the length returned does
     NOT include the size field of the varlena.
*/
int
PQgetlength(PGresult *res, int tup_num, int field_num)
{
    if (!res) {
      fprintf(stderr, "PQgetlength() -- pointer to PQresult is null");
        return (int)NULL;
    }

    if (tup_num > (res->ntups - 1 )||
	field_num > (res->numAttributes - 1))  {
	fprintf(stderr,
		"PQgetlength: ERROR! field %d(of %d) of tuple %d(of %d) is not available", 
		field_num, res->numAttributes - 1, tup_num, res->ntups);
    }
	
    return res->tuples[tup_num][field_num].len;
  }
