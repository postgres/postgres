
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
#define SOCK_BUFFER_SIZE			4096	/* default socket buffer size */
#define MAX_CONNECTIONS				128		/* conns per environment (arbitrary)  */
#define MAX_FIELDS					512
#define BYTELEN						8
#define VARHDRSZ					sizeof(Int4)

/*	Registry length limits */
#define LARGE_REGISTRY_LEN			4096	/* used for special cases */
#define MEDIUM_REGISTRY_LEN			128		/* normal size for user,database,etc. */
#define SMALL_REGISTRY_LEN			10		/* for 1/0 settings */


/*	Connection Defaults */
#define DEFAULT_PORT				"5432"
#define DEFAULT_READONLY			"1"

/*	These prefixes denote system tables */
#define INSIGHT_SYS_PREFIX	"dd_"
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
#define DBMS_VERSION           "06.30.0000 PostgreSQL 6.3"
#define POSTGRESDRIVERVERSION  "06.30.0000"
#define DRIVER_FILE_NAME		"PSQLODBC.DLL"


#define PG62	"6.2"		/* "Protocol" key setting to force Postgres 6.2 */

/* INI File Stuff */
#define ODBC_INI     "ODBC.INI"         /* ODBC initialization file */
#define ODBCINST_INI "ODBCINST.INI"		/* ODBC Installation file */

#define INI_DSN           DBMS_NAME         /* Name of default Datasource in ini file (not used?) */
#define INI_KDESC         "Description"     /* Data source description */
#define INI_SERVER        "Servername"      /* Name of Server running the Postgres service */
#define INI_PORT          "Port"            /* Port on which the Postmaster is listening */ 
#define INI_DATABASE      "Database"        /* Database Name */
#define INI_USER          "Username"        /* Default User Name */
#define INI_PASSWORD      "Password"		/* Default Password */
#define INI_DEBUG         "Debug"			/* Debug flag */
#define INI_FETCH         "Fetch"			/* Fetch Max Count */
#define INI_SOCKET        "Socket"			/* Socket buffer size */
#define INI_READONLY      "ReadOnly"		/* Database is read only */
#define INI_COMMLOG       "CommLog"			/* Communication to backend logging */
#define INI_PROTOCOL      "Protocol"		/* What protocol (6.2) */
#define INI_OPTIMIZER     "Optimizer"		/* Use backend genetic optimizer */
#define INI_CONNSETTINGS  "ConnSettings"	/* Anything to send to backend on successful connection */


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


typedef struct GlobalValues_
{
	int					fetch_max;
	int					socket_buffersize;
	int					debug;
	int					commlog;
	char				optimizer[MEDIUM_REGISTRY_LEN];
	char				conn_settings[LARGE_REGISTRY_LEN];
} GLOBAL_VALUES;


/* sizes */
#define TEXT_FIELD_SIZE			4094	/* size of text fields (not including null term) */
#define MAX_VARCHAR_SIZE		254		/* maximum size of a varchar (not including null term) */


/* global prototypes */
void updateGlobals(void);


#include "misc.h"

#endif
