
/* File:            psqlodbc.h
 *
 * Description:     This file contains defines and declarations that are related to
 *                  the entire driver.
 *
 * Comments:        See "notice.txt" for copyright and license information.
 *
 */

#ifndef __PSQLODBC_H__
#define __PSQLODBC_H__

#define Int4 int
#define UInt4 unsigned int
#define Int2 short
#define UInt2 unsigned short

typedef UInt4 Oid;


/* Limits */
#define MAX_MESSAGE_LEN				8192
#define MAX_CONNECT_STRING			4096
#define ERROR_MSG_LENGTH			4096
#define FETCH_MAX					100		/* default number of rows to cache for declare/fetch */
#define FETCH_INCR					1000
#define SOCK_BUFFER_SIZE			4096	/* default socket buffer size */
#define MAX_CONNECTIONS				128		/* conns per environment (arbitrary)  */
#define MAX_FIELDS					512
#define BYTELEN						8
#define VARHDRSZ					sizeof(Int4)

/*	Registry length limits */
#define LARGE_REGISTRY_LEN			4096	/* used for special cases */
#define MEDIUM_REGISTRY_LEN			256		/* normal size for user,database,etc. */
#define SMALL_REGISTRY_LEN			10		/* for 1/0 settings */


/*	These prefixes denote system tables */
#define POSTGRES_SYS_PREFIX	"pg_"
#define KEYS_TABLE			"dd_fkey"

/*	Info limits */
#define MAX_INFO_STRING		128
#define MAX_KEYPARTS		20
#define MAX_KEYLEN			512			//	max key of the form "date+outlet+invoice"
#define MAX_STATEMENT_LEN	MAX_MESSAGE_LEN

/* Driver stuff */
#define DRIVERNAME             "PostgreSQL ODBC"
#define DBMS_NAME              "PostgreSQL"
#define DBMS_VERSION           "06.30.0244 PostgreSQL 6.3"
#define POSTGRESDRIVERVERSION  "06.30.0244"
#define DRIVER_FILE_NAME		"PSQLODBC.DLL"


#define PG62	"6.2"		/* "Protocol" key setting to force Postgres 6.2 */


typedef struct ConnectionClass_ ConnectionClass;
typedef struct StatementClass_ StatementClass;
typedef struct QResultClass_ QResultClass;
typedef struct SocketClass_ SocketClass;
typedef struct BindInfoClass_ BindInfoClass;
typedef struct ParameterInfoClass_ ParameterInfoClass;
typedef struct ColumnInfoClass_ ColumnInfoClass;
typedef struct TupleListClass_ TupleListClass;
typedef struct EnvironmentClass_ EnvironmentClass;
typedef struct TupleNode_ TupleNode;
typedef struct TupleField_ TupleField;

typedef struct lo_arg LO_ARG;

typedef struct GlobalValues_
{
	int					fetch_max;
	int					socket_buffersize;
	int					unknown_sizes;
	int					max_varchar_size;
	int					max_longvarchar_size;
	char				debug;
	char				commlog;
	char				disable_optimizer;
	char				unique_index;
	char				readonly;
	char				use_declarefetch;
	char				text_as_longvarchar;
	char				unknowns_as_longvarchar;
	char				bools_as_char;
	char				extra_systable_prefixes[MEDIUM_REGISTRY_LEN];
	char				conn_settings[LARGE_REGISTRY_LEN];
} GLOBAL_VALUES;


#define PG_TYPE_LO				-999	/* hack until permanent type available */
#define PG_TYPE_LO_NAME			"lo"
#define OID_ATTNUM				-2		/* the attnum in pg_index of the oid */

/* sizes */
#define TEXT_FIELD_SIZE			4094	/* size of text fields (not including null term) */
#define NAME_FIELD_SIZE			32		/* size of name fields */
#define MAX_VARCHAR_SIZE		254		/* maximum size of a varchar (not including null term) */



#include "misc.h"

#endif
