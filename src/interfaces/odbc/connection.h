
/* File:            connection.h
 *
 * Description:     See "connection.c"
 *
 * Comments:        See "notice.txt" for copyright and license information.
 *
 */

#ifndef __CONNECTION_H__
#define __CONNECTION_H__

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "psqlodbc.h"

#ifndef WIN32
#include "iodbc.h"
#include "isql.h"
#include "isqlext.h"
#else
#include <windows.h>
#include <sql.h>
#include <sqlext.h>
#endif


typedef enum {
    CONN_NOT_CONNECTED,      /* Connection has not been established */
    CONN_CONNECTED,      /* Connection is up and has been established */
    CONN_DOWN,            /* Connection is broken */
    CONN_EXECUTING     /* the connection is currently executing a statement */
} CONN_Status;

/*	These errors have general sql error state */
#define CONNECTION_SERVER_NOT_REACHED 101
#define CONNECTION_MSG_TOO_LONG 103
#define CONNECTION_COULD_NOT_SEND 104
#define CONNECTION_NO_SUCH_DATABASE 105
#define CONNECTION_BACKEND_CRAZY 106
#define CONNECTION_NO_RESPONSE 107
#define CONNECTION_SERVER_REPORTED_ERROR 108
#define CONNECTION_COULD_NOT_RECEIVE 109
#define CONNECTION_SERVER_REPORTED_WARNING 110
#define CONNECTION_NEED_PASSWORD 112

/*	These errors correspond to specific SQL states */
#define CONN_INIREAD_ERROR 201
#define CONN_OPENDB_ERROR 202
#define CONN_STMT_ALLOC_ERROR 203
#define CONN_IN_USE 204 
#define CONN_UNSUPPORTED_OPTION 205
/* Used by SetConnectoption to indicate unsupported options */
#define CONN_INVALID_ARGUMENT_NO 206
/* SetConnectOption: corresponds to ODBC--"S1009" */
#define CONN_TRANSACT_IN_PROGRES 207
#define CONN_NO_MEMORY_ERROR 208
#define CONN_NOT_IMPLEMENTED_ERROR 209
#define CONN_INVALID_AUTHENTICATION 210
#define CONN_AUTH_TYPE_UNSUPPORTED 211
#define CONN_UNABLE_TO_LOAD_DLL 212

#define CONN_OPTION_VALUE_CHANGED 213
#define CONN_VALUE_OUT_OF_RANGE 214

#define CONN_TRUNCATED 215

/* Conn_status defines */
#define CONN_IN_AUTOCOMMIT 0x01
#define CONN_IN_TRANSACTION 0x02

/* AutoCommit functions */
#define CC_set_autocommit_off(x)	(x->transact_status &= ~CONN_IN_AUTOCOMMIT)
#define CC_set_autocommit_on(x)		(x->transact_status |= CONN_IN_AUTOCOMMIT)
#define CC_is_in_autocommit(x)		(x->transact_status & CONN_IN_AUTOCOMMIT)

/* Transaction in/not functions */
#define CC_set_in_trans(x)	(x->transact_status |= CONN_IN_TRANSACTION)
#define CC_set_no_trans(x)	(x->transact_status &= ~CONN_IN_TRANSACTION)
#define CC_is_in_trans(x)	(x->transact_status & CONN_IN_TRANSACTION)


/* Authentication types */
#define AUTH_REQ_OK			0
#define AUTH_REQ_KRB4		1
#define AUTH_REQ_KRB5		2
#define AUTH_REQ_PASSWORD	3
#define AUTH_REQ_CRYPT		4

/*	Startup Packet sizes */
#define SM_DATABASE		64
#define SM_USER			32
#define SM_OPTIONS		64
#define SM_UNUSED		64
#define SM_TTY			64

/*	Old 6.2 protocol defines */
#define NO_AUTHENTICATION	7
#define PATH_SIZE			64
#define ARGV_SIZE			64
#define NAMEDATALEN			16

typedef unsigned int ProtocolVersion;

#define PG_PROTOCOL(major, minor)	(((major) << 16) | (minor))
#define PG_PROTOCOL_LATEST		PG_PROTOCOL(2, 0)
#define PG_PROTOCOL_63			PG_PROTOCOL(1, 0)
#define PG_PROTOCOL_62			PG_PROTOCOL(0, 0)

/*	This startup packet is to support latest Postgres protocol (6.4, 6.3) */
typedef struct _StartupPacket
{
	ProtocolVersion	protoVersion;
	char			database[SM_DATABASE];
	char			user[SM_USER];
	char			options[SM_OPTIONS];
	char			unused[SM_UNUSED];
	char			tty[SM_TTY];
} StartupPacket;


/*	This startup packet is to support pre-Postgres 6.3 protocol */
typedef struct _StartupPacket6_2
{
	unsigned int	authtype;
	char			database[PATH_SIZE];
	char			user[NAMEDATALEN];
	char			options[ARGV_SIZE];
	char			execfile[ARGV_SIZE];
	char			tty[PATH_SIZE];
} StartupPacket6_2;


/*	Structure to hold all the connection attributes for a specific
	connection (used for both registry and file, DSN and DRIVER)
*/
typedef struct {
	char	dsn[MEDIUM_REGISTRY_LEN];
	char	desc[MEDIUM_REGISTRY_LEN];
	char	driver[MEDIUM_REGISTRY_LEN];
	char	server[MEDIUM_REGISTRY_LEN];
	char	database[MEDIUM_REGISTRY_LEN];
	char	username[MEDIUM_REGISTRY_LEN];
	char	password[MEDIUM_REGISTRY_LEN];
	char	conn_settings[LARGE_REGISTRY_LEN];
	char	protocol[SMALL_REGISTRY_LEN];
	char	port[SMALL_REGISTRY_LEN];
	char	onlyread[SMALL_REGISTRY_LEN];	
	char	fake_oid_index[SMALL_REGISTRY_LEN];
	char	show_oid_column[SMALL_REGISTRY_LEN];
	char	row_versioning[SMALL_REGISTRY_LEN];
	char	show_system_tables[SMALL_REGISTRY_LEN];
	char    translation_dll[MEDIUM_REGISTRY_LEN];
	char    translation_option[SMALL_REGISTRY_LEN];
	char	focus_password;
} ConnInfo;

/*	Macro to determine is the connection using 6.2 protocol? */
#define PROTOCOL_62(conninfo_)		(strncmp((conninfo_)->protocol, PG62, strlen(PG62)) == 0)

/*	Macro to determine is the connection using 6.3 protocol? */
#define PROTOCOL_63(conninfo_)		(strncmp((conninfo_)->protocol, PG63, strlen(PG63)) == 0)


/*	This is used to store cached table information in the connection */
struct col_info {
	QResultClass	*result;
	char			name[MAX_TABLE_LEN+1];
};

 /* Translation DLL entry points */
#ifdef WIN32
#define DLLHANDLE HINSTANCE
#else
#define WINAPI CALLBACK
#define DLLHANDLE void *
#define HINSTANCE void *
#endif

typedef BOOL (FAR WINAPI *DataSourceToDriverProc) (UDWORD,
					SWORD,
					PTR,
					SDWORD,
					PTR,
					SDWORD,
					SDWORD FAR *,
					UCHAR FAR *,
					SWORD,
					SWORD FAR *);

typedef BOOL (FAR WINAPI *DriverToDataSourceProc) (UDWORD,
					SWORD,
					PTR,
					SDWORD,
					PTR,
					SDWORD,
					SDWORD FAR *,
					UCHAR FAR *,
					SWORD,
					SWORD FAR *);

/*******	The Connection handle	************/
struct ConnectionClass_ {
	HENV			henv;					/* environment this connection was created on */
	StatementOptions stmtOptions;
	char			*errormsg;
	int				errornumber;
	CONN_Status		status;
	ConnInfo		connInfo;
	StatementClass	**stmts;
	int				num_stmts;
	SocketClass		*sock;
	int				lobj_type;
	int				ntables;
	COL_INFO		**col_info;
	long            translation_option;
	HINSTANCE       translation_handle;
	DataSourceToDriverProc  DataSourceToDriver;
	DriverToDataSourceProc  DriverToDataSource;
	char			transact_status;		/* Is a transaction is currently in progress */
	char			errormsg_created;		/* has an informative error msg been created?  */
	char			pg_version[MAX_INFO_STRING];	/* Version of PostgreSQL we're connected to - DJP 25-1-2001 */
	float			pg_version_number;
};


/* Accessor functions */
#define CC_get_socket(x)	(x->sock)
#define CC_get_database(x)	(x->connInfo.database)
#define CC_get_server(x)	(x->connInfo.server)
#define CC_get_DSN(x)		(x->connInfo.dsn)
#define CC_get_username(x)	(x->connInfo.username)
#define CC_is_onlyread(x)	(x->connInfo.onlyread[0] == '1')


/*  for CC_DSN_info */
#define CONN_DONT_OVERWRITE		0
#define CONN_OVERWRITE			1 


/*	prototypes */
ConnectionClass *CC_Constructor(void);
char CC_Destructor(ConnectionClass *self);
int CC_cursor_count(ConnectionClass *self);
char CC_cleanup(ConnectionClass *self);
char CC_abort(ConnectionClass *self);
int CC_set_translation (ConnectionClass *self);
char CC_connect(ConnectionClass *self, char do_password);
char CC_add_statement(ConnectionClass *self, StatementClass *stmt);
char CC_remove_statement(ConnectionClass *self, StatementClass *stmt);
char CC_get_error(ConnectionClass *self, int *number, char **message);
QResultClass *CC_send_query(ConnectionClass *self, char *query, QueryInfo *qi);
void CC_clear_error(ConnectionClass *self);
char *CC_create_errormsg(ConnectionClass *self);
int CC_send_function(ConnectionClass *conn, int fnid, void *result_buf, int *actual_result_len, int result_is_int, LO_ARG *argv, int nargs);
char CC_send_settings(ConnectionClass *self);
void CC_lookup_lo(ConnectionClass *conn);
void CC_lookup_pg_version(ConnectionClass *conn);
void CC_log_error(char *func, char *desc, ConnectionClass *self);


#endif
