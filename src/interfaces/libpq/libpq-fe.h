/*-------------------------------------------------------------------------
 *
 * libpq-fe.h--
 *	  This file contains definitions for structures and
 *	  externs for functions used by frontend postgres applications.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: libpq-fe.h,v 1.39 1998/08/29 04:05:45 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#ifndef LIBPQ_FE_H
#define LIBPQ_FE_H

#ifdef __cplusplus
extern		"C"
{
#endif

#include <stdio.h>
/* these wouldn't need to be included if PGSockAddr weren't exported: */
#ifdef WIN32
#include <winsock.h>
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#endif
/* ----------------
 *		include stuff common to fe and be
 * ----------------
 */
#include "postgres_ext.h"
#include "lib/dllist.h"

/* Application-visible enum types */

	typedef enum
	{
		CONNECTION_OK,
		CONNECTION_BAD
	} ConnStatusType;

	typedef enum
	{
		PGRES_EMPTY_QUERY = 0,
		PGRES_COMMAND_OK,		/* a query command that doesn't return */
		/* anything was executed properly by the backend */
		PGRES_TUPLES_OK,		/* a query command that returns tuples */
		/* was executed properly by the backend, PGresult */
		/* contains the result tuples */
		PGRES_COPY_OUT,			/* Copy Out data transfer in progress */
		PGRES_COPY_IN,			/* Copy In data transfer in progress */
		PGRES_BAD_RESPONSE,		/* an unexpected response was recv'd from
								 * the backend */
		PGRES_NONFATAL_ERROR,
		PGRES_FATAL_ERROR
	} ExecStatusType;

/* string descriptions of the ExecStatusTypes */
	extern const char * const pgresStatus[];

/*
 * POSTGRES backend dependent Constants.
 */

/* ERROR_MSG_LENGTH should really be the same as ELOG_MAXLEN in utils/elog.h*/
#define ERROR_MSG_LENGTH 4096
#define CMDSTATUS_LEN 40

/* PGresult and the subsidiary types PGresAttDesc, PGresAttValue
 * represent the result of a query (or more precisely, of a single SQL
 * command --- a query string given to PQexec can contain multiple commands).
 * Note we assume that a single command can return at most one tuple group,
 * hence there is no need for multiple descriptor sets.
 */

	typedef struct pgresAttDesc
	{
		char	   *name;		/* type name */
		Oid			typid;		/* type id */
		short		typlen;		/* type size */
		int			atttypmod;	/* type-specific modifier info */
	} PGresAttDesc;

/* use char* for Attribute values,
   ASCII tuples are guaranteed to be null-terminated
   For binary tuples, the first four bytes of the value is the size,
   and the bytes afterwards are the value.	The binary value is
   not guaranteed to be null-terminated.  In fact, it can have embedded nulls
 */

#define NULL_LEN		(-1)	/* pg_result len for NULL value */

	typedef struct pgresAttValue
	{
		int			len;		/* length in bytes of the value */
		char	   *value;		/* actual value */
	} PGresAttValue;

	struct pg_conn;				/* forward reference */

	typedef struct pg_result
	{
		int			ntups;
		int			numAttributes;
		PGresAttDesc *attDescs;
		PGresAttValue **tuples; /* each PGresTuple is an array of
								 * PGresAttValue's */
		int			tupArrSize; /* size of tuples array allocated */
		ExecStatusType resultStatus;
		char		cmdStatus[CMDSTATUS_LEN];	/* cmd status from the
												 * last insert query */
		int			binary;		/* binary tuple values if binary == 1,
								 * otherwise ASCII */
		struct pg_conn *conn;	/* connection we did the query on */
	} PGresult;

/* PGnotify represents the occurrence of a NOTIFY message */
	typedef struct pgNotify
	{
		char		relname[NAMEDATALEN];		/* name of relation
												 * containing data */
		int			be_pid;		/* process id of backend */
	} PGnotify;

/* PQnoticeProcessor is a typedef for a callback function type */
	typedef void (*PQnoticeProcessor) (void * arg, const char * message);

/* PGAsyncStatusType is private to libpq, really shouldn't be seen by users */
	typedef enum
	{
		PGASYNC_IDLE,			/* nothing's happening, dude */
		PGASYNC_BUSY,			/* query in progress */
		PGASYNC_READY,			/* result ready for PQgetResult */
		PGASYNC_COPY_IN,		/* Copy In data transfer in progress */
		PGASYNC_COPY_OUT		/* Copy Out data transfer in progress */
	} PGAsyncStatusType;

/* generic socket address type for PGconn connection information.
 * Really shouldn't be visible to users */
	typedef union PGSockAddr
	{
		struct sockaddr sa;
		struct sockaddr_in in;
#ifndef WIN32
		struct sockaddr_un un;
#endif
	} PGSockAddr;

/* large-object-access data ... allocated only if large-object code is used.
 * Really shouldn't be visible to users */
	typedef struct pgLobjfuncs
	{
		Oid			fn_lo_open; /* OID of backend function lo_open		*/
		Oid			fn_lo_close;/* OID of backend function lo_close		*/
		Oid			fn_lo_creat;/* OID of backend function lo_creat		*/
		Oid			fn_lo_unlink;		/* OID of backend function
										 * lo_unlink	*/
		Oid			fn_lo_lseek;/* OID of backend function lo_lseek		*/
		Oid			fn_lo_tell; /* OID of backend function lo_tell		*/
		Oid			fn_lo_read; /* OID of backend function LOread		*/
		Oid			fn_lo_write;/* OID of backend function LOwrite		*/
	} PGlobjfuncs;

/* PGconn encapsulates a connection to the backend.
 * XXX contents of this struct really shouldn't be visible to applications,
 * but we might break some existing applications if we tried to make it
 * completely opaque.
 */
	typedef struct pg_conn
	{
		/* Saved values of connection options */
		char	   *pghost;		/* the machine on which the server is
								 * running */
		char	   *pgport;		/* the server's communication port */
		char	   *pgtty;		/* tty on which the backend messages is
								 * displayed (NOT ACTUALLY USED???) */
		char	   *pgoptions;	/* options to start the backend with */
		char	   *dbName;		/* database name */
		char	   *pguser;		/* Postgres username and password, if any */
		char	   *pgpass;

		/* Optional file to write trace info to */
		FILE	   *Pfdebug;

		/* Callback procedure for notice/error message processing */
		PQnoticeProcessor	noticeHook;
		void	   *noticeArg;

		/* Status indicators */
		ConnStatusType		status;
		PGAsyncStatusType	asyncStatus;
		Dllist	   *notifyList;	/* Notify msgs not yet handed to application */

		/* Connection data */
		int			sock;		/* Unix FD for socket, -1 if not connected */
		PGSockAddr	laddr;		/* Local address */
		PGSockAddr	raddr;		/* Remote address */
		int			raddr_len;	/* Length of remote address */

		/* Miscellaneous stuff */
		int			be_pid;		/* PID of backend --- needed for cancels */
		int			be_key;		/* key of backend --- needed for cancels */
		char		salt[2];	/* password salt received from backend */
		PGlobjfuncs *lobjfuncs; /* private state for large-object access fns */

		/* Buffer for data received from backend and not yet processed */
		char		*inBuffer;	/* currently allocated buffer */
		int			inBufSize;	/* allocated size of buffer */
		int			inStart;	/* offset to first unconsumed data in buffer */
		int			inCursor;	/* next byte to tentatively consume */
		int			inEnd;		/* offset to first position after avail data */

		/* Buffer for data not yet sent to backend */
		char		*outBuffer;	/* currently allocated buffer */
		int			outBufSize;	/* allocated size of buffer */
		int			outCount;	/* number of chars waiting in buffer */

		/* Status for asynchronous result construction */
		PGresult		*result;	/* result being constructed */
		PGresAttValue	*curTuple;	/* tuple currently being read */

		/* Message space.  Placed last for code-size reasons.
		 * errorMessage is the message last returned to the application.
		 * When asyncStatus=READY, asyncErrorMessage is the pending message
		 * that will be put in errorMessage by PQgetResult. */
		char		errorMessage[ERROR_MSG_LENGTH];
		char		asyncErrorMessage[ERROR_MSG_LENGTH];
	} PGconn;

	/*
	 * We can't use the conventional "bool", because we are designed to be
	 * included in a user's program, and user may already have that type
	 * defined.  Pqbool, on the other hand, is unlikely to be used.
	 */

	typedef char pqbool;

/* Print options for PQprint() */

	typedef struct _PQprintOpt
	{
		pqbool		header;		/* print output field headings and row
								 * count */
		pqbool		align;		/* fill align the fields */
		pqbool		standard;	/* old brain dead format */
		pqbool		html3;		/* output html tables */
		pqbool		expanded;	/* expand tables */
		pqbool		pager;		/* use pager for output if needed */
		char	   *fieldSep;	/* field separator */
		char	   *tableOpt;	/* insert to HTML <table ...> */
		char	   *caption;	/* HTML <caption> */
		char	  **fieldName;	/* null terminated array of repalcement
								 * field names */
	} PQprintOpt;

/* ----------------
 * PQArgBlock -- structure for PQfn() arguments
 * ----------------
 */
	typedef struct
	{
		int			len;
		int			isint;
		union
		{
			int		   *ptr;	/* can't use void (dec compiler barfs)	 */
			int			integer;
		}			u;
	} PQArgBlock;

/* ----------------
 * Structure for the conninfo parameter definitions returned by PQconndefaults
 * ----------------
 */
	typedef struct _PQconninfoOption
	{
		char	   *keyword;	/* The keyword of the option			*/
		char	   *environ;	/* Fallback environment variable name	*/
		char	   *compiled;	/* Fallback compiled in default value	*/
		char	   *val;		/* Options value						*/
		char	   *label;		/* Label for field in connect dialog	*/
		char	   *dispchar;	/* Character to display for this field	*/
		/* in a connect dialog. Values are:		*/
		/* ""	Display entered value as is  */
		/* "*"	Password field - hide value  */
		/* "D"	Debug options - don't 	 */
		/* create a field by default	*/
		int			dispsize;	/* Field size in characters for dialog	*/
	} PQconninfoOption;

/* ----------------
 * Exported functions of libpq
 * ----------------
 */

/* ===	in fe-connect.c === */

	/* make a new client connection to the backend */
	extern PGconn *PQconnectdb(const char *conninfo);
	extern PGconn *PQsetdbLogin(const char *pghost, const char *pgport,
								const char *pgoptions, const char *pgtty,
								const char *dbName,
								const char *login, const char *pwd);
#define PQsetdb(M_PGHOST,M_PGPORT,M_PGOPT,M_PGTTY,M_DBNAME)  \
	PQsetdbLogin(M_PGHOST, M_PGPORT, M_PGOPT, M_PGTTY, M_DBNAME, NULL, NULL)

	/* get info about connection options known to PQconnectdb */
	extern PQconninfoOption *PQconndefaults(void);

	/* close the current connection and free the PGconn data structure */
	extern void PQfinish(PGconn *conn);
	/*
	 * close the current connection and restablish a new one with the same
	 * parameters
	 */
	extern void PQreset(PGconn *conn);

	/* issue a cancel request */
	extern int	PQrequestCancel(PGconn *conn);

	/* Accessor functions for PGconn objects */
	extern char *PQdb(PGconn *conn);
	extern char *PQuser(PGconn *conn);
	extern char *PQhost(PGconn *conn);
	extern char *PQoptions(PGconn *conn);
	extern char *PQport(PGconn *conn);
	extern char *PQtty(PGconn *conn);
	extern ConnStatusType PQstatus(PGconn *conn);
	extern char *PQerrorMessage(PGconn *conn);
	extern int PQsocket(PGconn *conn);

	/* Enable/disable tracing */
	extern void PQtrace(PGconn *conn, FILE *debug_port);
	extern void PQuntrace(PGconn *conn);

	/* Override default notice processor */
	extern void PQsetNoticeProcessor (PGconn *conn,
									  PQnoticeProcessor proc,
									  void *arg);

/* === in fe-exec.c === */

	/* Simple synchronous query */
	extern PGresult *PQexec(PGconn *conn, const char *query);
	extern PGnotify *PQnotifies(PGconn *conn);

	/* Interface for multiple-result or asynchronous queries */
	extern int  PQsendQuery(PGconn *conn, const char *query);
	extern PGresult *PQgetResult(PGconn *conn);

	/* Routines for managing an asychronous query */
	extern int	PQisBusy(PGconn *conn);
	extern void PQconsumeInput(PGconn *conn);

	/* Routines for copy in/out */
	extern int	PQgetline(PGconn *conn, char *string, int length);
	extern void PQputline(PGconn *conn, const char *string);
	extern void PQputnbytes(PGconn *conn, const char *buffer, int nbytes);
	extern int	PQendcopy(PGconn *conn);

	/* "Fast path" interface --- not really recommended for application use */
	extern PGresult *PQfn(PGconn *conn,
						  int fnid,
						  int *result_buf,
						  int *result_len,
						  int result_is_int,
						  PQArgBlock *args,
						  int nargs);

	/* Accessor functions for PGresult objects */
	extern ExecStatusType PQresultStatus(PGresult *res);
	extern int	PQntuples(PGresult *res);
	extern int	PQnfields(PGresult *res);
	extern char *PQfname(PGresult *res, int field_num);
	extern int	PQfnumber(PGresult *res, const char *field_name);
	extern Oid	PQftype(PGresult *res, int field_num);
	extern short PQfsize(PGresult *res, int field_num);
	extern int	PQfmod(PGresult *res, int field_num);
	extern char *PQcmdStatus(PGresult *res);
	extern const char *PQoidStatus(PGresult *res);
	extern const char *PQcmdTuples(PGresult *res);
	extern char *PQgetvalue(PGresult *res, int tup_num, int field_num);
	extern int	PQgetlength(PGresult *res, int tup_num, int field_num);
	extern int	PQgetisnull(PGresult *res, int tup_num, int field_num);

	/* Delete a PGresult */
	extern void PQclear(PGresult *res);

/* === in fe-print.c === */

	extern void PQprint(FILE *fout,		/* output stream */
						PGresult *res,
						PQprintOpt *ps); /* option structure */

	/* PQdisplayTuples() is a better version of PQprintTuples(),
	 * but both are obsoleted by PQprint().
	 */
	extern void PQdisplayTuples(PGresult *res,
								FILE *fp,	/* where to send the
											 * output */
								int fillAlign, /* pad the fields with
												* spaces */
								const char *fieldSep, /* field separator */
								int printHeader,	/* display headers? */
								int quiet);
	extern void PQprintTuples(PGresult *res,
							  FILE *fout,	/* output stream */
							  int printAttName,		/* print attribute names
													 * or not */
							  int terseOutput,		/* delimiter bars or
													 * not? */
							  int width);	/* width of column, if
											 * 0, use variable width */

#ifdef MULTIBYTE
	extern int PQmblen(unsigned char *s);
#endif

/* === in fe-lobj.c === */

	/* Large-object access routines */
	extern int	lo_open(PGconn *conn, Oid lobjId, int mode);
	extern int	lo_close(PGconn *conn, int fd);
	extern int	lo_read(PGconn *conn, int fd, char *buf, int len);
	extern int	lo_write(PGconn *conn, int fd, char *buf, int len);
	extern int	lo_lseek(PGconn *conn, int fd, int offset, int whence);
	extern Oid	lo_creat(PGconn *conn, int mode);
	extern int	lo_tell(PGconn *conn, int fd);
	extern int	lo_unlink(PGconn *conn, Oid lobjId);
	extern Oid	lo_import(PGconn *conn, char *filename);
	extern int	lo_export(PGconn *conn, Oid lobjId, char *filename);

#ifdef __cplusplus
};
#endif

#endif							/* LIBPQ_FE_H */
