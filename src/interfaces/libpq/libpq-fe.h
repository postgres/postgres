/*-------------------------------------------------------------------------
 *
 * libpq-fe.h--
 *	  This file contains definitions for structures and
 *	  externs for functions used by frontend postgres applications.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: libpq-fe.h,v 1.47 1999/02/05 04:25:55 momjian Exp $
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
/* postgres_ext.h defines the backend's externally visible types,
 * such as Oid.
 */
#include "postgres_ext.h"

/* Application-visible enum types */

	typedef enum
	{
		CONNECTION_OK,
		CONNECTION_BAD
	} ConnStatusType;

	typedef enum
	{
		PGRES_EMPTY_QUERY = 0,
		PGRES_COMMAND_OK,		/* a query command that doesn't return anything
								 * was executed properly by the backend */
		PGRES_TUPLES_OK,		/* a query command that returns tuples
								 * was executed properly by the backend,
								 * PGresult contains the result tuples */
		PGRES_COPY_OUT,			/* Copy Out data transfer in progress */
		PGRES_COPY_IN,			/* Copy In data transfer in progress */
		PGRES_BAD_RESPONSE,		/* an unexpected response was recv'd from
								 * the backend */
		PGRES_NONFATAL_ERROR,
		PGRES_FATAL_ERROR
	} ExecStatusType;

/* String descriptions of the ExecStatusTypes */
	extern const char * const pgresStatus[];

/* PGconn encapsulates a connection to the backend.
 * The contents of this struct are not supposed to be known to applications.
 */
	typedef struct pg_conn PGconn;

/* PGresult encapsulates the result of a query (or more precisely, of a single
 * SQL command --- a query string given to PQsendQuery can contain multiple
 * commands and thus return multiple PGresult objects).
 * The contents of this struct are not supposed to be known to applications.
 */
	typedef struct pg_result PGresult;

/* PGnotify represents the occurrence of a NOTIFY message.
 * Ideally this would be an opaque typedef, but it's so simple that it's
 * unlikely to change.
 * NOTE: in Postgres 6.4 and later, the be_pid is the notifying backend's,
 * whereas in earlier versions it was always your own backend's PID.
 */
	typedef struct pgNotify
	{
		char		relname[NAMEDATALEN];		/* name of relation
												 * containing data */
		int			be_pid;						/* process id of backend */
	} PGnotify;

/* PQnoticeProcessor is the function type for the notice-message callback.
 */

	typedef void (*PQnoticeProcessor) (void * arg, const char * message);

/* Print options for PQprint() */
  
  	/*
  	 * We can't use the conventional "bool", because we are designed to be
  	 * included in a user's program, and user may already have that type
  	 * defined.  Pqbool, on the other hand, is unlikely to be used.
  	 */
  	typedef char pqbool;

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
 * Structure for the conninfo parameter definitions returned by PQconndefaults
 * ----------------
 */
	typedef struct _PQconninfoOption
	{
		char	   *keyword;	/* The keyword of the option			*/
		char	   *envvar;	/* Fallback environment variable name	*/
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
	extern char *PQpass(PGconn *conn);
	extern char *PQhost(PGconn *conn);
	extern char *PQport(PGconn *conn);
	extern char *PQtty(PGconn *conn);
	extern char *PQoptions(PGconn *conn);
	extern ConnStatusType PQstatus(PGconn *conn);
	extern char *PQerrorMessage(PGconn *conn);
	extern int	PQsocket(PGconn *conn);
	extern int	PQbackendPID(PGconn *conn);

	/* Enable/disable tracing */
	extern void PQtrace(PGconn *conn, FILE *debug_port);
	extern void PQuntrace(PGconn *conn);

	/* Override default notice processor */
	extern void PQsetNoticeProcessor(PGconn *conn,
												 PQnoticeProcessor proc,
												 void *arg);

/* === in fe-exec.c === */

	/* Simple synchronous query */
	extern PGresult *PQexec(PGconn *conn, const char *query);
	extern PGnotify *PQnotifies(PGconn *conn);

	/* Interface for multiple-result or asynchronous queries */
	extern int	PQsendQuery(PGconn *conn, const char *query);
	extern PGresult *PQgetResult(PGconn *conn);

	/* Routines for managing an asychronous query */
	extern int	PQisBusy(PGconn *conn);
	extern int	PQconsumeInput(PGconn *conn);

	/* Routines for copy in/out */
	extern int	PQgetline(PGconn *conn, char *string, int length);
	extern int	PQputline(PGconn *conn, const char *string);
	extern int	PQgetlineAsync(PGconn *conn, char *buffer, int bufsize);
	extern int	PQputnbytes(PGconn *conn, const char *buffer, int nbytes);
	extern int	PQendcopy(PGconn *conn);

	/*
	 * "Fast path" interface --- not really recommended for application
	 * use
	 */
	extern PGresult *PQfn(PGconn *conn,
						  int fnid,
						  int *result_buf,
						  int *result_len,
						  int result_is_int,
						  PQArgBlock *args,
						  int nargs);

	/* Accessor functions for PGresult objects */
	extern ExecStatusType PQresultStatus(PGresult *res);
	extern const char *PQresultErrorMessage(PGresult *res);
	extern int	PQntuples(PGresult *res);
	extern int	PQnfields(PGresult *res);
	extern int	PQbinaryTuples(PGresult *res);
	extern char *PQfname(PGresult *res, int field_num);
	extern int	PQfnumber(PGresult *res, const char *field_name);
	extern Oid	PQftype(PGresult *res, int field_num);
	extern int	PQfsize(PGresult *res, int field_num);
	extern int	PQfmod(PGresult *res, int field_num);
	extern char *PQcmdStatus(PGresult *res);
	extern const char *PQoidStatus(PGresult *res);
	extern const char *PQcmdTuples(PGresult *res);
	extern char *PQgetvalue(PGresult *res, int tup_num, int field_num);
	extern int	PQgetlength(PGresult *res, int tup_num, int field_num);
	extern int	PQgetisnull(PGresult *res, int tup_num, int field_num);

	/* Delete a PGresult */
	extern void PQclear(PGresult *res);

	/* Make an empty PGresult with given status (some apps find this useful).
	 * If conn is not NULL and status indicates an error, the conn's
	 * errorMessage is copied.
	 */
	extern PGresult * PQmakeEmptyPGresult(PGconn *conn, ExecStatusType status);

/* === in fe-print.c === */

	extern void PQprint(FILE *fout,			/* output stream */
						PGresult *res,
						PQprintOpt *ps);	/* option structure */

	/*
	 * PQdisplayTuples() is a better version of PQprintTuples(), but both
	 * are obsoleted by PQprint().
	 */
	extern void PQdisplayTuples(PGresult *res,
								FILE *fp,			/* where to send the
													 * output */
								int fillAlign,		/* pad the fields with
													 * spaces */
								const char *fieldSep,	/* field separator */
								int printHeader,	/* display headers? */
								int quiet);

	extern void PQprintTuples(PGresult *res,
							  FILE *fout,			/* output stream */
							  int printAttName,		/* print attribute names
													 * or not */
							  int terseOutput,		/* delimiter bars or
													 * not? */
							  int width);			/* width of column, if
													 * 0, use variable width */

	/* Determine length of multibyte encoded char at *s */
	extern int	PQmblen(unsigned char *s);

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

#endif	 /* LIBPQ_FE_H */
