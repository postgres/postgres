/*-------------------------------------------------------------------------
 *
 * libpq-fe.h
 *	  This file contains definitions for structures and
 *	  externs for functions used by frontend postgres applications.
 *
 * Portions Copyright (c) 1996-2011, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/interfaces/libpq/libpq-fe.h
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

/*
 * postgres_ext.h defines the backend's externally visible types,
 * such as Oid.
 */
#include "postgres_ext.h"

/*
 * Option flags for PQcopyResult
 */
#define PG_COPYRES_ATTRS		  0x01
#define PG_COPYRES_TUPLES		  0x02	/* Implies PG_COPYRES_ATTRS */
#define PG_COPYRES_EVENTS		  0x04
#define PG_COPYRES_NOTICEHOOKS	  0x08

/* Application-visible enum types */

typedef enum
{
	/*
	 * Although it is okay to add to this list, values which become unused
	 * should never be removed, nor should constants be redefined - that would
	 * break compatibility with existing code.
	 */
	CONNECTION_OK,
	CONNECTION_BAD,
	/* Non-blocking mode only below here */

	/*
	 * The existence of these should never be relied upon - they should only
	 * be used for user feedback or similar purposes.
	 */
	CONNECTION_STARTED,			/* Waiting for connection to be made.  */
	CONNECTION_MADE,			/* Connection OK; waiting to send.	   */
	CONNECTION_AWAITING_RESPONSE,		/* Waiting for a response from the
										 * postmaster.		  */
	CONNECTION_AUTH_OK,			/* Received authentication; waiting for
								 * backend startup. */
	CONNECTION_SETENV,			/* Negotiating environment. */
	CONNECTION_SSL_STARTUP,		/* Negotiating SSL. */
	CONNECTION_NEEDED			/* Internal state: connect() needed */
} ConnStatusType;

typedef enum
{
	PGRES_POLLING_FAILED = 0,
	PGRES_POLLING_READING,		/* These two indicate that one may	  */
	PGRES_POLLING_WRITING,		/* use select before polling again.   */
	PGRES_POLLING_OK,
	PGRES_POLLING_ACTIVE		/* unused; keep for awhile for backwards
								 * compatibility */
} PostgresPollingStatusType;

typedef enum
{
	PGRES_EMPTY_QUERY = 0,		/* empty query string was executed */
	PGRES_COMMAND_OK,			/* a query command that doesn't return
								 * anything was executed properly by the
								 * backend */
	PGRES_TUPLES_OK,			/* a query command that returns tuples was
								 * executed properly by the backend, PGresult
								 * contains the result tuples */
	PGRES_COPY_OUT,				/* Copy Out data transfer in progress */
	PGRES_COPY_IN,				/* Copy In data transfer in progress */
	PGRES_BAD_RESPONSE,			/* an unexpected response was recv'd from the
								 * backend */
	PGRES_NONFATAL_ERROR,		/* notice or warning message */
	PGRES_FATAL_ERROR,			/* query failed */
	PGRES_COPY_BOTH				/* Copy In/Out data transfer in progress */
} ExecStatusType;

typedef enum
{
	PQTRANS_IDLE,				/* connection idle */
	PQTRANS_ACTIVE,				/* command in progress */
	PQTRANS_INTRANS,			/* idle, within transaction block */
	PQTRANS_INERROR,			/* idle, within failed transaction */
	PQTRANS_UNKNOWN				/* cannot determine status */
} PGTransactionStatusType;

typedef enum
{
	PQERRORS_TERSE,				/* single-line error messages */
	PQERRORS_DEFAULT,			/* recommended style */
	PQERRORS_VERBOSE			/* all the facts, ma'am */
} PGVerbosity;

typedef enum
{
	PQPING_OK,					/* server is accepting connections */
	PQPING_REJECT,				/* server is alive but rejecting connections */
	PQPING_NO_RESPONSE,			/* could not establish connection */
	PQPING_NO_ATTEMPT			/* connection not attempted (bad params) */
} PGPing;

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

/* PGcancel encapsulates the information needed to cancel a running
 * query on an existing connection.
 * The contents of this struct are not supposed to be known to applications.
 */
typedef struct pg_cancel PGcancel;

/* PGnotify represents the occurrence of a NOTIFY message.
 * Ideally this would be an opaque typedef, but it's so simple that it's
 * unlikely to change.
 * NOTE: in Postgres 6.4 and later, the be_pid is the notifying backend's,
 * whereas in earlier versions it was always your own backend's PID.
 */
typedef struct pgNotify
{
	char	   *relname;		/* notification condition name */
	int			be_pid;			/* process ID of notifying server process */
	char	   *extra;			/* notification parameter */
	/* Fields below here are private to libpq; apps should not use 'em */
	struct pgNotify *next;		/* list link */
} PGnotify;

/* Function types for notice-handling callbacks */
typedef void (*PQnoticeReceiver) (void *arg, const PGresult *res);
typedef void (*PQnoticeProcessor) (void *arg, const char *message);

/* Print options for PQprint() */
typedef char pqbool;

typedef struct _PQprintOpt
{
	pqbool		header;			/* print output field headings and row count */
	pqbool		align;			/* fill align the fields */
	pqbool		standard;		/* old brain dead format */
	pqbool		html3;			/* output html tables */
	pqbool		expanded;		/* expand tables */
	pqbool		pager;			/* use pager for output if needed */
	char	   *fieldSep;		/* field separator */
	char	   *tableOpt;		/* insert to HTML <table ...> */
	char	   *caption;		/* HTML <caption> */
	char	  **fieldName;		/* null terminated array of replacement field
								 * names */
} PQprintOpt;

/* ----------------
 * Structure for the conninfo parameter definitions returned by PQconndefaults
 * or PQconninfoParse.
 *
 * All fields except "val" point at static strings which must not be altered.
 * "val" is either NULL or a malloc'd current-value string.  PQconninfoFree()
 * will release both the val strings and the PQconninfoOption array itself.
 * ----------------
 */
typedef struct _PQconninfoOption
{
	char	   *keyword;		/* The keyword of the option			*/
	char	   *envvar;			/* Fallback environment variable name	*/
	char	   *compiled;		/* Fallback compiled in default value	*/
	char	   *val;			/* Option's current value, or NULL		 */
	char	   *label;			/* Label for field in connect dialog	*/
	char	   *dispchar;		/* Indicates how to display this field in a
								 * connect dialog. Values are: "" Display
								 * entered value as is "*" Password field -
								 * hide value "D"  Debug option - don't show
								 * by default */
	int			dispsize;		/* Field size in characters for dialog	*/
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
		int		   *ptr;		/* can't use void (dec compiler barfs)	 */
		int			integer;
	}			u;
} PQArgBlock;

/* ----------------
 * PGresAttDesc -- Data about a single attribute (column) of a query result
 * ----------------
 */
typedef struct pgresAttDesc
{
	char	   *name;			/* column name */
	Oid			tableid;		/* source table, if known */
	int			columnid;		/* source column, if known */
	int			format;			/* format code for value (text/binary) */
	Oid			typid;			/* type id */
	int			typlen;			/* type size */
	int			atttypmod;		/* type-specific modifier info */
} PGresAttDesc;

/* ----------------
 * Exported functions of libpq
 * ----------------
 */

/* ===	in fe-connect.c === */

/* make a new client connection to the backend */
/* Asynchronous (non-blocking) */
extern PGconn *PQconnectStart(const char *conninfo);
extern PGconn *PQconnectStartParams(const char **keywords,
					 const char **values, int expand_dbname);
extern PostgresPollingStatusType PQconnectPoll(PGconn *conn);

/* Synchronous (blocking) */
extern PGconn *PQconnectdb(const char *conninfo);
extern PGconn *PQconnectdbParams(const char **keywords,
				  const char **values, int expand_dbname);
extern PGconn *PQsetdbLogin(const char *pghost, const char *pgport,
			 const char *pgoptions, const char *pgtty,
			 const char *dbName,
			 const char *login, const char *pwd);

#define PQsetdb(M_PGHOST,M_PGPORT,M_PGOPT,M_PGTTY,M_DBNAME)  \
	PQsetdbLogin(M_PGHOST, M_PGPORT, M_PGOPT, M_PGTTY, M_DBNAME, NULL, NULL)

/* close the current connection and free the PGconn data structure */
extern void PQfinish(PGconn *conn);

/* get info about connection options known to PQconnectdb */
extern PQconninfoOption *PQconndefaults(void);

/* parse connection options in same way as PQconnectdb */
extern PQconninfoOption *PQconninfoParse(const char *conninfo, char **errmsg);

/* free the data structure returned by PQconndefaults() or PQconninfoParse() */
extern void PQconninfoFree(PQconninfoOption *connOptions);

/*
 * close the current connection and restablish a new one with the same
 * parameters
 */
/* Asynchronous (non-blocking) */
extern int	PQresetStart(PGconn *conn);
extern PostgresPollingStatusType PQresetPoll(PGconn *conn);

/* Synchronous (blocking) */
extern void PQreset(PGconn *conn);

/* request a cancel structure */
extern PGcancel *PQgetCancel(PGconn *conn);

/* free a cancel structure */
extern void PQfreeCancel(PGcancel *cancel);

/* issue a cancel request */
extern int	PQcancel(PGcancel *cancel, char *errbuf, int errbufsize);

/* backwards compatible version of PQcancel; not thread-safe */
extern int	PQrequestCancel(PGconn *conn);

/* Accessor functions for PGconn objects */
extern char *PQdb(const PGconn *conn);
extern char *PQuser(const PGconn *conn);
extern char *PQpass(const PGconn *conn);
extern char *PQhost(const PGconn *conn);
extern char *PQport(const PGconn *conn);
extern char *PQtty(const PGconn *conn);
extern char *PQoptions(const PGconn *conn);
extern ConnStatusType PQstatus(const PGconn *conn);
extern PGTransactionStatusType PQtransactionStatus(const PGconn *conn);
extern const char *PQparameterStatus(const PGconn *conn,
				  const char *paramName);
extern int	PQprotocolVersion(const PGconn *conn);
extern int	PQserverVersion(const PGconn *conn);
extern char *PQerrorMessage(const PGconn *conn);
extern int	PQsocket(const PGconn *conn);
extern int	PQbackendPID(const PGconn *conn);
extern int	PQconnectionNeedsPassword(const PGconn *conn);
extern int	PQconnectionUsedPassword(const PGconn *conn);
extern int	PQclientEncoding(const PGconn *conn);
extern int	PQsetClientEncoding(PGconn *conn, const char *encoding);

/* Get the OpenSSL structure associated with a connection. Returns NULL for
 * unencrypted connections or if any other TLS library is in use. */
extern void *PQgetssl(PGconn *conn);

/* Tell libpq whether it needs to initialize OpenSSL */
extern void PQinitSSL(int do_init);

/* More detailed way to tell libpq whether it needs to initialize OpenSSL */
extern void PQinitOpenSSL(int do_ssl, int do_crypto);

/* Set verbosity for PQerrorMessage and PQresultErrorMessage */
extern PGVerbosity PQsetErrorVerbosity(PGconn *conn, PGVerbosity verbosity);

/* Enable/disable tracing */
extern void PQtrace(PGconn *conn, FILE *debug_port);
extern void PQuntrace(PGconn *conn);

/* Override default notice handling routines */
extern PQnoticeReceiver PQsetNoticeReceiver(PGconn *conn,
					PQnoticeReceiver proc,
					void *arg);
extern PQnoticeProcessor PQsetNoticeProcessor(PGconn *conn,
					 PQnoticeProcessor proc,
					 void *arg);

/*
 *	   Used to set callback that prevents concurrent access to
 *	   non-thread safe functions that libpq needs.
 *	   The default implementation uses a libpq internal mutex.
 *	   Only required for multithreaded apps that use kerberos
 *	   both within their app and for postgresql connections.
 */
typedef void (*pgthreadlock_t) (int acquire);

extern pgthreadlock_t PQregisterThreadLock(pgthreadlock_t newhandler);

/* === in fe-exec.c === */

/* Simple synchronous query */
extern PGresult *PQexec(PGconn *conn, const char *query);
extern PGresult *PQexecParams(PGconn *conn,
			 const char *command,
			 int nParams,
			 const Oid *paramTypes,
			 const char *const * paramValues,
			 const int *paramLengths,
			 const int *paramFormats,
			 int resultFormat);
extern PGresult *PQprepare(PGconn *conn, const char *stmtName,
		  const char *query, int nParams,
		  const Oid *paramTypes);
extern PGresult *PQexecPrepared(PGconn *conn,
			   const char *stmtName,
			   int nParams,
			   const char *const * paramValues,
			   const int *paramLengths,
			   const int *paramFormats,
			   int resultFormat);

/* Interface for multiple-result or asynchronous queries */
extern int	PQsendQuery(PGconn *conn, const char *query);
extern int PQsendQueryParams(PGconn *conn,
				  const char *command,
				  int nParams,
				  const Oid *paramTypes,
				  const char *const * paramValues,
				  const int *paramLengths,
				  const int *paramFormats,
				  int resultFormat);
extern int PQsendPrepare(PGconn *conn, const char *stmtName,
			  const char *query, int nParams,
			  const Oid *paramTypes);
extern int PQsendQueryPrepared(PGconn *conn,
					const char *stmtName,
					int nParams,
					const char *const * paramValues,
					const int *paramLengths,
					const int *paramFormats,
					int resultFormat);
extern PGresult *PQgetResult(PGconn *conn);

/* Routines for managing an asynchronous query */
extern int	PQisBusy(PGconn *conn);
extern int	PQconsumeInput(PGconn *conn);

/* LISTEN/NOTIFY support */
extern PGnotify *PQnotifies(PGconn *conn);

/* Routines for copy in/out */
extern int	PQputCopyData(PGconn *conn, const char *buffer, int nbytes);
extern int	PQputCopyEnd(PGconn *conn, const char *errormsg);
extern int	PQgetCopyData(PGconn *conn, char **buffer, int async);

/* Deprecated routines for copy in/out */
extern int	PQgetline(PGconn *conn, char *string, int length);
extern int	PQputline(PGconn *conn, const char *string);
extern int	PQgetlineAsync(PGconn *conn, char *buffer, int bufsize);
extern int	PQputnbytes(PGconn *conn, const char *buffer, int nbytes);
extern int	PQendcopy(PGconn *conn);

/* Set blocking/nonblocking connection to the backend */
extern int	PQsetnonblocking(PGconn *conn, int arg);
extern int	PQisnonblocking(const PGconn *conn);
extern int	PQisthreadsafe(void);
extern PGPing PQping(const char *conninfo);
extern PGPing PQpingParams(const char **keywords,
			 const char **values, int expand_dbname);

/* Force the write buffer to be written (or at least try) */
extern int	PQflush(PGconn *conn);

/*
 * "Fast path" interface --- not really recommended for application
 * use
 */
extern PGresult *PQfn(PGconn *conn,
	 int fnid,
	 int *result_buf,
	 int *result_len,
	 int result_is_int,
	 const PQArgBlock *args,
	 int nargs);

/* Accessor functions for PGresult objects */
extern ExecStatusType PQresultStatus(const PGresult *res);
extern char *PQresStatus(ExecStatusType status);
extern char *PQresultErrorMessage(const PGresult *res);
extern char *PQresultErrorField(const PGresult *res, int fieldcode);
extern int	PQntuples(const PGresult *res);
extern int	PQnfields(const PGresult *res);
extern int	PQbinaryTuples(const PGresult *res);
extern char *PQfname(const PGresult *res, int field_num);
extern int	PQfnumber(const PGresult *res, const char *field_name);
extern Oid	PQftable(const PGresult *res, int field_num);
extern int	PQftablecol(const PGresult *res, int field_num);
extern int	PQfformat(const PGresult *res, int field_num);
extern Oid	PQftype(const PGresult *res, int field_num);
extern int	PQfsize(const PGresult *res, int field_num);
extern int	PQfmod(const PGresult *res, int field_num);
extern char *PQcmdStatus(PGresult *res);
extern char *PQoidStatus(const PGresult *res);	/* old and ugly */
extern Oid	PQoidValue(const PGresult *res);	/* new and improved */
extern char *PQcmdTuples(PGresult *res);
extern char *PQgetvalue(const PGresult *res, int tup_num, int field_num);
extern int	PQgetlength(const PGresult *res, int tup_num, int field_num);
extern int	PQgetisnull(const PGresult *res, int tup_num, int field_num);
extern int	PQnparams(const PGresult *res);
extern Oid	PQparamtype(const PGresult *res, int param_num);

/* Describe prepared statements and portals */
extern PGresult *PQdescribePrepared(PGconn *conn, const char *stmt);
extern PGresult *PQdescribePortal(PGconn *conn, const char *portal);
extern int	PQsendDescribePrepared(PGconn *conn, const char *stmt);
extern int	PQsendDescribePortal(PGconn *conn, const char *portal);

/* Delete a PGresult */
extern void PQclear(PGresult *res);

/* For freeing other alloc'd results, such as PGnotify structs */
extern void PQfreemem(void *ptr);

/* Exists for backward compatibility.  bjm 2003-03-24 */
#define PQfreeNotify(ptr) PQfreemem(ptr)

/* Error when no password was given. */
/* Note: depending on this is deprecated; use PQconnectionNeedsPassword(). */
#define PQnoPasswordSupplied	"fe_sendauth: no password supplied\n"

/* Create and manipulate PGresults */
extern PGresult *PQmakeEmptyPGresult(PGconn *conn, ExecStatusType status);
extern PGresult *PQcopyResult(const PGresult *src, int flags);
extern int	PQsetResultAttrs(PGresult *res, int numAttributes, PGresAttDesc *attDescs);
extern void *PQresultAlloc(PGresult *res, size_t nBytes);
extern int	PQsetvalue(PGresult *res, int tup_num, int field_num, char *value, int len);

/* Quoting strings before inclusion in queries. */
extern size_t PQescapeStringConn(PGconn *conn,
				   char *to, const char *from, size_t length,
				   int *error);
extern char *PQescapeLiteral(PGconn *conn, const char *str, size_t len);
extern char *PQescapeIdentifier(PGconn *conn, const char *str, size_t len);
extern unsigned char *PQescapeByteaConn(PGconn *conn,
				  const unsigned char *from, size_t from_length,
				  size_t *to_length);
extern unsigned char *PQunescapeBytea(const unsigned char *strtext,
				size_t *retbuflen);

/* These forms are deprecated! */
extern size_t PQescapeString(char *to, const char *from, size_t length);
extern unsigned char *PQescapeBytea(const unsigned char *from, size_t from_length,
			  size_t *to_length);



/* === in fe-print.c === */

extern void
PQprint(FILE *fout,				/* output stream */
		const PGresult *res,
		const PQprintOpt *ps);	/* option structure */

/*
 * really old printing routines
 */
extern void
PQdisplayTuples(const PGresult *res,
				FILE *fp,		/* where to send the output */
				int fillAlign,	/* pad the fields with spaces */
				const char *fieldSep,	/* field separator */
				int printHeader,	/* display headers? */
				int quiet);

extern void
PQprintTuples(const PGresult *res,
			  FILE *fout,		/* output stream */
			  int printAttName, /* print attribute names */
			  int terseOutput,	/* delimiter bars */
			  int width);		/* width of column, if 0, use variable width */


/* === in fe-lobj.c === */

/* Large-object access routines */
extern int	lo_open(PGconn *conn, Oid lobjId, int mode);
extern int	lo_close(PGconn *conn, int fd);
extern int	lo_read(PGconn *conn, int fd, char *buf, size_t len);
extern int	lo_write(PGconn *conn, int fd, const char *buf, size_t len);
extern int	lo_lseek(PGconn *conn, int fd, int offset, int whence);
extern Oid	lo_creat(PGconn *conn, int mode);
extern Oid	lo_create(PGconn *conn, Oid lobjId);
extern int	lo_tell(PGconn *conn, int fd);
extern int	lo_truncate(PGconn *conn, int fd, size_t len);
extern int	lo_unlink(PGconn *conn, Oid lobjId);
extern Oid	lo_import(PGconn *conn, const char *filename);
extern Oid	lo_import_with_oid(PGconn *conn, const char *filename, Oid lobjId);
extern int	lo_export(PGconn *conn, Oid lobjId, const char *filename);

/* === in fe-misc.c === */

/* Get the version of the libpq library in use */
extern int	PQlibVersion(void);

/* Determine length of multibyte encoded char at *s */
extern int	PQmblen(const char *s, int encoding);

/* Determine display length of multibyte encoded char at *s */
extern int	PQdsplen(const char *s, int encoding);

/* Get encoding id from environment variable PGCLIENTENCODING */
extern int	PQenv2encoding(void);

/* === in fe-auth.c === */

extern char *PQencryptPassword(const char *passwd, const char *user);

/* === in encnames.c === */

extern int	pg_char_to_encoding(const char *name);
extern const char *pg_encoding_to_char(int encoding);
extern int	pg_valid_server_encoding_id(int encoding);

#ifdef __cplusplus
}
#endif

#endif   /* LIBPQ_FE_H */
