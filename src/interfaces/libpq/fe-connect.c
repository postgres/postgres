/*-------------------------------------------------------------------------
 *
 * fe-connect.c
 *	  functions related to setting up a connection to the backend
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/interfaces/libpq/fe-connect.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <sys/stat.h>
#include <fcntl.h>
#include <ctype.h>
#include <netdb.h>
#include <time.h>
#include <unistd.h>

#include "common/base64.h"
#include "common/ip.h"
#include "common/link-canary.h"
#include "common/scram-common.h"
#include "common/string.h"
#include "fe-auth.h"
#include "libpq-fe.h"
#include "libpq-int.h"
#include "mb/pg_wchar.h"
#include "pg_config_paths.h"
#include "port/pg_bswap.h"

#ifdef WIN32
#include "win32.h"
#ifdef _WIN32_IE
#undef _WIN32_IE
#endif
#define _WIN32_IE 0x0500
#ifdef near
#undef near
#endif
#define near
#include <shlobj.h>
#include <mstcpip.h>
#else
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pwd.h>
#endif

#ifdef WIN32
#include "pthread-win32.h"
#else
#include <pthread.h>
#endif

#ifdef USE_LDAP
#ifdef WIN32
#include <winldap.h>
#else
/* OpenLDAP deprecates RFC 1823, but we want standard conformance */
#define LDAP_DEPRECATED 1
#include <ldap.h>
typedef struct timeval LDAP_TIMEVAL;
#endif
static int	ldapServiceLookup(const char *purl, PQconninfoOption *options,
							  PQExpBuffer errorMessage);
#endif

#ifndef WIN32
#define PGPASSFILE ".pgpass"
#else
#define PGPASSFILE "pgpass.conf"
#endif

/*
 * Pre-9.0 servers will return this SQLSTATE if asked to set
 * application_name in a startup packet.  We hard-wire the value rather
 * than looking into errcodes.h since it reflects historical behavior
 * rather than that of the current code.
 */
#define ERRCODE_APPNAME_UNKNOWN "42704"

/* This is part of the protocol so just define it */
#define ERRCODE_INVALID_PASSWORD "28P01"
/* This too */
#define ERRCODE_CANNOT_CONNECT_NOW "57P03"

/*
 * Cope with the various platform-specific ways to spell TCP keepalive socket
 * options.  This doesn't cover Windows, which as usual does its own thing.
 */
#if defined(TCP_KEEPIDLE)
/* TCP_KEEPIDLE is the name of this option on Linux and *BSD */
#define PG_TCP_KEEPALIVE_IDLE TCP_KEEPIDLE
#define PG_TCP_KEEPALIVE_IDLE_STR "TCP_KEEPIDLE"
#elif defined(TCP_KEEPALIVE_THRESHOLD)
/* TCP_KEEPALIVE_THRESHOLD is the name of this option on Solaris >= 11 */
#define PG_TCP_KEEPALIVE_IDLE TCP_KEEPALIVE_THRESHOLD
#define PG_TCP_KEEPALIVE_IDLE_STR "TCP_KEEPALIVE_THRESHOLD"
#elif defined(TCP_KEEPALIVE) && defined(__darwin__)
/* TCP_KEEPALIVE is the name of this option on macOS */
/* Caution: Solaris has this symbol but it means something different */
#define PG_TCP_KEEPALIVE_IDLE TCP_KEEPALIVE
#define PG_TCP_KEEPALIVE_IDLE_STR "TCP_KEEPALIVE"
#endif

/*
 * fall back options if they are not specified by arguments or defined
 * by environment variables
 */
#define DefaultHost		"localhost"
#define DefaultOption	""
#ifdef USE_SSL
#define DefaultChannelBinding	"prefer"
#else
#define DefaultChannelBinding	"disable"
#endif
#define DefaultTargetSessionAttrs	"any"
#define DefaultLoadBalanceHosts	"disable"
#ifdef USE_SSL
#define DefaultSSLMode "prefer"
#define DefaultSSLCertMode "allow"
#else
#define DefaultSSLMode	"disable"
#define DefaultSSLCertMode "disable"
#endif
#define DefaultSSLNegotiation	"postgres"
#ifdef ENABLE_GSS
#include "fe-gssapi-common.h"
#define DefaultGSSMode "prefer"
#else
#define DefaultGSSMode "disable"
#endif

/* ----------
 * Definition of the conninfo parameters and their fallback resources.
 *
 * If Environment-Var and Compiled-in are specified as NULL, no
 * fallback is available. If after all no value can be determined
 * for an option, an error is returned.
 *
 * The value for the username is treated specially in conninfo_add_defaults.
 * If the value is not obtained any other way, the username is determined
 * by pg_fe_getauthname().
 *
 * The Label and Disp-Char entries are provided for applications that
 * want to use PQconndefaults() to create a generic database connection
 * dialog. Disp-Char is defined as follows:
 *		""		Normal input field
 *		"*"		Password field - hide value
 *		"D"		Debug option - don't show by default
 *
 * PQconninfoOptions[] is a constant static array that we use to initialize
 * a dynamically allocated working copy.  All the "val" fields in
 * PQconninfoOptions[] *must* be NULL.  In a working copy, non-null "val"
 * fields point to malloc'd strings that should be freed when the working
 * array is freed (see PQconninfoFree).
 *
 * The first part of each struct is identical to the one in libpq-fe.h,
 * which is required since we memcpy() data between the two!
 * ----------
 */
typedef struct _internalPQconninfoOption
{
	char	   *keyword;		/* The keyword of the option			*/
	char	   *envvar;			/* Fallback environment variable name	*/
	char	   *compiled;		/* Fallback compiled in default value	*/
	char	   *val;			/* Option's current value, or NULL		*/
	char	   *label;			/* Label for field in connect dialog	*/
	char	   *dispchar;		/* Indicates how to display this field in a
								 * connect dialog. Values are: "" Display
								 * entered value as is "*" Password field -
								 * hide value "D"  Debug option - don't show
								 * by default */
	int			dispsize;		/* Field size in characters for dialog	*/
	/* ---
	 * Anything above this comment must be synchronized with
	 * PQconninfoOption in libpq-fe.h, since we memcpy() data
	 * between them!
	 * ---
	 */
	off_t		connofs;		/* Offset into PGconn struct, -1 if not there */
} internalPQconninfoOption;

static const internalPQconninfoOption PQconninfoOptions[] = {
	{"service", "PGSERVICE", NULL, NULL,
		"Database-Service", "", 20,
	offsetof(struct pg_conn, pgservice)},

	{"user", "PGUSER", NULL, NULL,
		"Database-User", "", 20,
	offsetof(struct pg_conn, pguser)},

	{"password", "PGPASSWORD", NULL, NULL,
		"Database-Password", "*", 20,
	offsetof(struct pg_conn, pgpass)},

	{"passfile", "PGPASSFILE", NULL, NULL,
		"Database-Password-File", "", 64,
	offsetof(struct pg_conn, pgpassfile)},

	{"channel_binding", "PGCHANNELBINDING", DefaultChannelBinding, NULL,
		"Channel-Binding", "", 8,	/* sizeof("require") == 8 */
	offsetof(struct pg_conn, channel_binding)},

	{"connect_timeout", "PGCONNECT_TIMEOUT", NULL, NULL,
		"Connect-timeout", "", 10,	/* strlen(INT32_MAX) == 10 */
	offsetof(struct pg_conn, connect_timeout)},

	{"dbname", "PGDATABASE", NULL, NULL,
		"Database-Name", "", 20,
	offsetof(struct pg_conn, dbName)},

	{"host", "PGHOST", NULL, NULL,
		"Database-Host", "", 40,
	offsetof(struct pg_conn, pghost)},

	{"hostaddr", "PGHOSTADDR", NULL, NULL,
		"Database-Host-IP-Address", "", 45,
	offsetof(struct pg_conn, pghostaddr)},

	{"port", "PGPORT", DEF_PGPORT_STR, NULL,
		"Database-Port", "", 6,
	offsetof(struct pg_conn, pgport)},

	{"client_encoding", "PGCLIENTENCODING", NULL, NULL,
		"Client-Encoding", "", 10,
	offsetof(struct pg_conn, client_encoding_initial)},

	{"options", "PGOPTIONS", DefaultOption, NULL,
		"Backend-Options", "", 40,
	offsetof(struct pg_conn, pgoptions)},

	{"application_name", "PGAPPNAME", NULL, NULL,
		"Application-Name", "", 64,
	offsetof(struct pg_conn, appname)},

	{"fallback_application_name", NULL, NULL, NULL,
		"Fallback-Application-Name", "", 64,
	offsetof(struct pg_conn, fbappname)},

	{"keepalives", NULL, NULL, NULL,
		"TCP-Keepalives", "", 1,	/* should be just '0' or '1' */
	offsetof(struct pg_conn, keepalives)},

	{"keepalives_idle", NULL, NULL, NULL,
		"TCP-Keepalives-Idle", "", 10,	/* strlen(INT32_MAX) == 10 */
	offsetof(struct pg_conn, keepalives_idle)},

	{"keepalives_interval", NULL, NULL, NULL,
		"TCP-Keepalives-Interval", "", 10,	/* strlen(INT32_MAX) == 10 */
	offsetof(struct pg_conn, keepalives_interval)},

	{"keepalives_count", NULL, NULL, NULL,
		"TCP-Keepalives-Count", "", 10, /* strlen(INT32_MAX) == 10 */
	offsetof(struct pg_conn, keepalives_count)},

	{"tcp_user_timeout", NULL, NULL, NULL,
		"TCP-User-Timeout", "", 10, /* strlen(INT32_MAX) == 10 */
	offsetof(struct pg_conn, pgtcp_user_timeout)},

	/*
	 * ssl options are allowed even without client SSL support because the
	 * client can still handle SSL modes "disable" and "allow". Other
	 * parameters have no effect on non-SSL connections, so there is no reason
	 * to exclude them since none of them are mandatory.
	 */
	{"sslmode", "PGSSLMODE", DefaultSSLMode, NULL,
		"SSL-Mode", "", 12,		/* sizeof("verify-full") == 12 */
	offsetof(struct pg_conn, sslmode)},

	{"sslnegotiation", "PGSSLNEGOTIATION", DefaultSSLNegotiation, NULL,
		"SSL-Negotiation", "", 9,	/* sizeof("postgres") == 9  */
	offsetof(struct pg_conn, sslnegotiation)},

	{"sslcompression", "PGSSLCOMPRESSION", "0", NULL,
		"SSL-Compression", "", 1,
	offsetof(struct pg_conn, sslcompression)},

	{"sslcert", "PGSSLCERT", NULL, NULL,
		"SSL-Client-Cert", "", 64,
	offsetof(struct pg_conn, sslcert)},

	{"sslkey", "PGSSLKEY", NULL, NULL,
		"SSL-Client-Key", "", 64,
	offsetof(struct pg_conn, sslkey)},

	{"sslcertmode", "PGSSLCERTMODE", NULL, NULL,
		"SSL-Client-Cert-Mode", "", 8,	/* sizeof("disable") == 8 */
	offsetof(struct pg_conn, sslcertmode)},

	{"sslpassword", NULL, NULL, NULL,
		"SSL-Client-Key-Password", "*", 20,
	offsetof(struct pg_conn, sslpassword)},

	{"sslrootcert", "PGSSLROOTCERT", NULL, NULL,
		"SSL-Root-Certificate", "", 64,
	offsetof(struct pg_conn, sslrootcert)},

	{"sslcrl", "PGSSLCRL", NULL, NULL,
		"SSL-Revocation-List", "", 64,
	offsetof(struct pg_conn, sslcrl)},

	{"sslcrldir", "PGSSLCRLDIR", NULL, NULL,
		"SSL-Revocation-List-Dir", "", 64,
	offsetof(struct pg_conn, sslcrldir)},

	{"sslsni", "PGSSLSNI", "1", NULL,
		"SSL-SNI", "", 1,
	offsetof(struct pg_conn, sslsni)},

	{"requirepeer", "PGREQUIREPEER", NULL, NULL,
		"Require-Peer", "", 10,
	offsetof(struct pg_conn, requirepeer)},

	{"require_auth", "PGREQUIREAUTH", NULL, NULL,
		"Require-Auth", "", 14, /* sizeof("scram-sha-256") == 14 */
	offsetof(struct pg_conn, require_auth)},

	{"ssl_min_protocol_version", "PGSSLMINPROTOCOLVERSION", "TLSv1.2", NULL,
		"SSL-Minimum-Protocol-Version", "", 8,	/* sizeof("TLSv1.x") == 8 */
	offsetof(struct pg_conn, ssl_min_protocol_version)},

	{"ssl_max_protocol_version", "PGSSLMAXPROTOCOLVERSION", NULL, NULL,
		"SSL-Maximum-Protocol-Version", "", 8,	/* sizeof("TLSv1.x") == 8 */
	offsetof(struct pg_conn, ssl_max_protocol_version)},

	/*
	 * As with SSL, all GSS options are exposed even in builds that don't have
	 * support.
	 */
	{"gssencmode", "PGGSSENCMODE", DefaultGSSMode, NULL,
		"GSSENC-Mode", "", 8,	/* sizeof("disable") == 8 */
	offsetof(struct pg_conn, gssencmode)},

	/* Kerberos and GSSAPI authentication support specifying the service name */
	{"krbsrvname", "PGKRBSRVNAME", PG_KRB_SRVNAM, NULL,
		"Kerberos-service-name", "", 20,
	offsetof(struct pg_conn, krbsrvname)},

	{"gsslib", "PGGSSLIB", NULL, NULL,
		"GSS-library", "", 7,	/* sizeof("gssapi") == 7 */
	offsetof(struct pg_conn, gsslib)},

	{"gssdelegation", "PGGSSDELEGATION", "0", NULL,
		"GSS-delegation", "", 1,
	offsetof(struct pg_conn, gssdelegation)},

	{"replication", NULL, NULL, NULL,
		"Replication", "D", 5,
	offsetof(struct pg_conn, replication)},

	{"target_session_attrs", "PGTARGETSESSIONATTRS",
		DefaultTargetSessionAttrs, NULL,
		"Target-Session-Attrs", "", 15, /* sizeof("prefer-standby") = 15 */
	offsetof(struct pg_conn, target_session_attrs)},

	{"load_balance_hosts", "PGLOADBALANCEHOSTS",
		DefaultLoadBalanceHosts, NULL,
		"Load-Balance-Hosts", "", 8,	/* sizeof("disable") = 8 */
	offsetof(struct pg_conn, load_balance_hosts)},

	{"scram_client_key", NULL, NULL, NULL, "SCRAM-Client-Key", "D", SCRAM_MAX_KEY_LEN * 2,
	offsetof(struct pg_conn, scram_client_key)},

	{"scram_server_key", NULL, NULL, NULL, "SCRAM-Server-Key", "D", SCRAM_MAX_KEY_LEN * 2,
	offsetof(struct pg_conn, scram_server_key)},

	/* Terminating entry --- MUST BE LAST */
	{NULL, NULL, NULL, NULL,
	NULL, NULL, 0}
};

static const PQEnvironmentOption EnvironmentOptions[] =
{
	/* common user-interface settings */
	{
		"PGDATESTYLE", "datestyle"
	},
	{
		"PGTZ", "timezone"
	},
	/* internal performance-related settings */
	{
		"PGGEQO", "geqo"
	},
	{
		NULL, NULL
	}
};

/* The connection URI must start with either of the following designators: */
static const char uri_designator[] = "postgresql://";
static const char short_uri_designator[] = "postgres://";

static bool connectOptions1(PGconn *conn, const char *conninfo);
static bool init_allowed_encryption_methods(PGconn *conn);
#if defined(USE_SSL) || defined(ENABLE_GSS)
static int	encryption_negotiation_failed(PGconn *conn);
#endif
static bool connection_failed(PGconn *conn);
static bool select_next_encryption_method(PGconn *conn, bool have_valid_connection);
static PGPing internal_ping(PGconn *conn);
static void pqFreeCommandQueue(PGcmdQueueEntry *queue);
static bool fillPGconn(PGconn *conn, PQconninfoOption *connOptions);
static void freePGconn(PGconn *conn);
static void release_conn_addrinfo(PGconn *conn);
static int	store_conn_addrinfo(PGconn *conn, struct addrinfo *addrlist);
static void sendTerminateConn(PGconn *conn);
static PQconninfoOption *conninfo_init(PQExpBuffer errorMessage);
static PQconninfoOption *parse_connection_string(const char *connstr,
												 PQExpBuffer errorMessage, bool use_defaults);
static int	uri_prefix_length(const char *connstr);
static bool recognized_connection_string(const char *connstr);
static PQconninfoOption *conninfo_parse(const char *conninfo,
										PQExpBuffer errorMessage, bool use_defaults);
static PQconninfoOption *conninfo_array_parse(const char *const *keywords,
											  const char *const *values, PQExpBuffer errorMessage,
											  bool use_defaults, int expand_dbname);
static bool conninfo_add_defaults(PQconninfoOption *options,
								  PQExpBuffer errorMessage);
static PQconninfoOption *conninfo_uri_parse(const char *uri,
											PQExpBuffer errorMessage, bool use_defaults);
static bool conninfo_uri_parse_options(PQconninfoOption *options,
									   const char *uri, PQExpBuffer errorMessage);
static bool conninfo_uri_parse_params(char *params,
									  PQconninfoOption *connOptions,
									  PQExpBuffer errorMessage);
static char *conninfo_uri_decode(const char *str, PQExpBuffer errorMessage);
static bool get_hexdigit(char digit, int *value);
static const char *conninfo_getval(PQconninfoOption *connOptions,
								   const char *keyword);
static PQconninfoOption *conninfo_storeval(PQconninfoOption *connOptions,
										   const char *keyword, const char *value,
										   PQExpBuffer errorMessage, bool ignoreMissing, bool uri_decode);
static PQconninfoOption *conninfo_find(PQconninfoOption *connOptions,
									   const char *keyword);
static void defaultNoticeReceiver(void *arg, const PGresult *res);
static void defaultNoticeProcessor(void *arg, const char *message);
static int	parseServiceInfo(PQconninfoOption *options,
							 PQExpBuffer errorMessage);
static int	parseServiceFile(const char *serviceFile,
							 const char *service,
							 PQconninfoOption *options,
							 PQExpBuffer errorMessage,
							 bool *group_found);
static char *pwdfMatchesString(char *buf, const char *token);
static char *passwordFromFile(const char *hostname, const char *port, const char *dbname,
							  const char *username, const char *pgpassfile);
static void pgpassfileWarning(PGconn *conn);
static void default_threadlock(int acquire);
static bool sslVerifyProtocolVersion(const char *version);
static bool sslVerifyProtocolRange(const char *min, const char *max);


/* global variable because fe-auth.c needs to access it */
pgthreadlock_t pg_g_threadlock = default_threadlock;


/*
 *		pqDropConnection
 *
 * Close any physical connection to the server, and reset associated
 * state inside the connection object.  We don't release state that
 * would be needed to reconnect, though, nor local state that might still
 * be useful later.
 *
 * We can always flush the output buffer, since there's no longer any hope
 * of sending that data.  However, unprocessed input data might still be
 * valuable, so the caller must tell us whether to flush that or not.
 */
void
pqDropConnection(PGconn *conn, bool flushInput)
{
	/* Drop any SSL state */
	pqsecure_close(conn);

	/* Close the socket itself */
	if (conn->sock != PGINVALID_SOCKET)
		closesocket(conn->sock);
	conn->sock = PGINVALID_SOCKET;

	/* Optionally discard any unread data */
	if (flushInput)
		conn->inStart = conn->inCursor = conn->inEnd = 0;

	/* Always discard any unsent data */
	conn->outCount = 0;

	/* Likewise, discard any pending pipelined commands */
	pqFreeCommandQueue(conn->cmd_queue_head);
	conn->cmd_queue_head = conn->cmd_queue_tail = NULL;
	pqFreeCommandQueue(conn->cmd_queue_recycle);
	conn->cmd_queue_recycle = NULL;

	/* Free authentication/encryption state */
#ifdef ENABLE_GSS
	{
		OM_uint32	min_s;

		if (conn->gcred != GSS_C_NO_CREDENTIAL)
		{
			gss_release_cred(&min_s, &conn->gcred);
			conn->gcred = GSS_C_NO_CREDENTIAL;
		}
		if (conn->gctx)
			gss_delete_sec_context(&min_s, &conn->gctx, GSS_C_NO_BUFFER);
		if (conn->gtarg_nam)
			gss_release_name(&min_s, &conn->gtarg_nam);
		if (conn->gss_SendBuffer)
		{
			free(conn->gss_SendBuffer);
			conn->gss_SendBuffer = NULL;
		}
		if (conn->gss_RecvBuffer)
		{
			free(conn->gss_RecvBuffer);
			conn->gss_RecvBuffer = NULL;
		}
		if (conn->gss_ResultBuffer)
		{
			free(conn->gss_ResultBuffer);
			conn->gss_ResultBuffer = NULL;
		}
		conn->gssenc = false;
	}
#endif
#ifdef ENABLE_SSPI
	if (conn->sspitarget)
	{
		free(conn->sspitarget);
		conn->sspitarget = NULL;
	}
	if (conn->sspicred)
	{
		FreeCredentialsHandle(conn->sspicred);
		free(conn->sspicred);
		conn->sspicred = NULL;
	}
	if (conn->sspictx)
	{
		DeleteSecurityContext(conn->sspictx);
		free(conn->sspictx);
		conn->sspictx = NULL;
	}
	conn->usesspi = 0;
#endif
	if (conn->sasl_state)
	{
		conn->sasl->free(conn->sasl_state);
		conn->sasl_state = NULL;
	}
}

/*
 * pqFreeCommandQueue
 * Free all the entries of PGcmdQueueEntry queue passed.
 */
static void
pqFreeCommandQueue(PGcmdQueueEntry *queue)
{
	while (queue != NULL)
	{
		PGcmdQueueEntry *cur = queue;

		queue = cur->next;
		free(cur->query);
		free(cur);
	}
}

/*
 *		pqDropServerData
 *
 * Clear all connection state data that was received from (or deduced about)
 * the server.  This is essential to do between connection attempts to
 * different servers, else we may incorrectly hold over some data from the
 * old server.
 *
 * It would be better to merge this into pqDropConnection, perhaps, but
 * right now we cannot because that function is called immediately on
 * detection of connection loss (cf. pqReadData, for instance).  This data
 * should be kept until we are actually starting a new connection.
 */
static void
pqDropServerData(PGconn *conn)
{
	PGnotify   *notify;
	pgParameterStatus *pstatus;

	/* Forget pending notifies */
	notify = conn->notifyHead;
	while (notify != NULL)
	{
		PGnotify   *prev = notify;

		notify = notify->next;
		free(prev);
	}
	conn->notifyHead = conn->notifyTail = NULL;

	/* Reset ParameterStatus data, as well as variables deduced from it */
	pstatus = conn->pstatus;
	while (pstatus != NULL)
	{
		pgParameterStatus *prev = pstatus;

		pstatus = pstatus->next;
		free(prev);
	}
	conn->pstatus = NULL;
	conn->client_encoding = PG_SQL_ASCII;
	conn->std_strings = false;
	conn->default_transaction_read_only = PG_BOOL_UNKNOWN;
	conn->in_hot_standby = PG_BOOL_UNKNOWN;
	conn->scram_sha_256_iterations = SCRAM_SHA_256_DEFAULT_ITERATIONS;
	conn->sversion = 0;

	/* Drop large-object lookup data */
	free(conn->lobjfuncs);
	conn->lobjfuncs = NULL;

	/* Reset assorted other per-connection state */
	conn->last_sqlstate[0] = '\0';
	conn->auth_req_received = false;
	conn->client_finished_auth = false;
	conn->password_needed = false;
	conn->gssapi_used = false;
	conn->write_failed = false;
	free(conn->write_err_msg);
	conn->write_err_msg = NULL;

	/*
	 * Cancel connections need to retain their be_pid and be_key across
	 * PQcancelReset invocations, otherwise they would not have access to the
	 * secret token of the connection they are supposed to cancel.
	 */
	if (!conn->cancelRequest)
	{
		conn->be_pid = 0;
		conn->be_key = 0;
	}
}


/*
 *		Connecting to a Database
 *
 * There are now six different ways a user of this API can connect to the
 * database.  Two are not recommended for use in new code, because of their
 * lack of extensibility with respect to the passing of options to the
 * backend.  These are PQsetdb and PQsetdbLogin (the former now being a macro
 * to the latter).
 *
 * If it is desired to connect in a synchronous (blocking) manner, use the
 * function PQconnectdb or PQconnectdbParams. The former accepts a string of
 * option = value pairs (or a URI) which must be parsed; the latter takes two
 * NULL terminated arrays instead.
 *
 * To connect in an asynchronous (non-blocking) manner, use the functions
 * PQconnectStart or PQconnectStartParams (which differ in the same way as
 * PQconnectdb and PQconnectdbParams) and PQconnectPoll.
 *
 * The non-exported functions pqConnectDBStart, pqConnectDBComplete are
 * part of the connection procedure implementation.
 */

/*
 *		PQconnectdbParams
 *
 * establishes a connection to a postgres backend through the postmaster
 * using connection information in two arrays.
 *
 * The keywords array is defined as
 *
 *	   const char *params[] = {"option1", "option2", NULL}
 *
 * The values array is defined as
 *
 *	   const char *values[] = {"value1", "value2", NULL}
 *
 * Returns a PGconn* which is needed for all subsequent libpq calls, or NULL
 * if a memory allocation failed.
 * If the status field of the connection returned is CONNECTION_BAD,
 * then some fields may be null'ed out instead of having valid values.
 *
 * You should call PQfinish (if conn is not NULL) regardless of whether this
 * call succeeded.
 */
PGconn *
PQconnectdbParams(const char *const *keywords,
				  const char *const *values,
				  int expand_dbname)
{
	PGconn	   *conn = PQconnectStartParams(keywords, values, expand_dbname);

	if (conn && conn->status != CONNECTION_BAD)
		(void) pqConnectDBComplete(conn);

	return conn;
}

/*
 *		PQpingParams
 *
 * check server status, accepting parameters identical to PQconnectdbParams
 */
PGPing
PQpingParams(const char *const *keywords,
			 const char *const *values,
			 int expand_dbname)
{
	PGconn	   *conn = PQconnectStartParams(keywords, values, expand_dbname);
	PGPing		ret;

	ret = internal_ping(conn);
	PQfinish(conn);

	return ret;
}

/*
 *		PQconnectdb
 *
 * establishes a connection to a postgres backend through the postmaster
 * using connection information in a string.
 *
 * The conninfo string is either a whitespace-separated list of
 *
 *	   option = value
 *
 * definitions or a URI (refer to the documentation for details.) Value
 * might be a single value containing no whitespaces or a single quoted
 * string. If a single quote should appear anywhere in the value, it must be
 * escaped with a backslash like \'
 *
 * Returns a PGconn* which is needed for all subsequent libpq calls, or NULL
 * if a memory allocation failed.
 * If the status field of the connection returned is CONNECTION_BAD,
 * then some fields may be null'ed out instead of having valid values.
 *
 * You should call PQfinish (if conn is not NULL) regardless of whether this
 * call succeeded.
 */
PGconn *
PQconnectdb(const char *conninfo)
{
	PGconn	   *conn = PQconnectStart(conninfo);

	if (conn && conn->status != CONNECTION_BAD)
		(void) pqConnectDBComplete(conn);

	return conn;
}

/*
 *		PQping
 *
 * check server status, accepting parameters identical to PQconnectdb
 */
PGPing
PQping(const char *conninfo)
{
	PGconn	   *conn = PQconnectStart(conninfo);
	PGPing		ret;

	ret = internal_ping(conn);
	PQfinish(conn);

	return ret;
}

/*
 *		PQconnectStartParams
 *
 * Begins the establishment of a connection to a postgres backend through the
 * postmaster using connection information in a struct.
 *
 * See comment for PQconnectdbParams for the definition of the string format.
 *
 * Returns a PGconn*.  If NULL is returned, a malloc error has occurred, and
 * you should not attempt to proceed with this connection.  If the status
 * field of the connection returned is CONNECTION_BAD, an error has
 * occurred. In this case you should call PQfinish on the result, (perhaps
 * inspecting the error message first).  Other fields of the structure may not
 * be valid if that occurs.  If the status field is not CONNECTION_BAD, then
 * this stage has succeeded - call PQconnectPoll, using select(2) to see when
 * this is necessary.
 *
 * See PQconnectPoll for more info.
 */
PGconn *
PQconnectStartParams(const char *const *keywords,
					 const char *const *values,
					 int expand_dbname)
{
	PGconn	   *conn;
	PQconninfoOption *connOptions;

	/*
	 * Allocate memory for the conn structure.  Note that we also expect this
	 * to initialize conn->errorMessage to empty.  All subsequent steps during
	 * connection initialization will only append to that buffer.
	 */
	conn = pqMakeEmptyPGconn();
	if (conn == NULL)
		return NULL;

	/*
	 * Parse the conninfo arrays
	 */
	connOptions = conninfo_array_parse(keywords, values,
									   &conn->errorMessage,
									   true, expand_dbname);
	if (connOptions == NULL)
	{
		conn->status = CONNECTION_BAD;
		/* errorMessage is already set */
		return conn;
	}

	/*
	 * Move option values into conn structure
	 */
	if (!fillPGconn(conn, connOptions))
	{
		PQconninfoFree(connOptions);
		return conn;
	}

	/*
	 * Free the option info - all is in conn now
	 */
	PQconninfoFree(connOptions);

	/*
	 * Compute derived options
	 */
	if (!pqConnectOptions2(conn))
		return conn;

	/*
	 * Connect to the database
	 */
	if (!pqConnectDBStart(conn))
	{
		/* Just in case we failed to set it in pqConnectDBStart */
		conn->status = CONNECTION_BAD;
	}

	return conn;
}

/*
 *		PQconnectStart
 *
 * Begins the establishment of a connection to a postgres backend through the
 * postmaster using connection information in a string.
 *
 * See comment for PQconnectdb for the definition of the string format.
 *
 * Returns a PGconn*.  If NULL is returned, a malloc error has occurred, and
 * you should not attempt to proceed with this connection.  If the status
 * field of the connection returned is CONNECTION_BAD, an error has
 * occurred. In this case you should call PQfinish on the result, (perhaps
 * inspecting the error message first).  Other fields of the structure may not
 * be valid if that occurs.  If the status field is not CONNECTION_BAD, then
 * this stage has succeeded - call PQconnectPoll, using select(2) to see when
 * this is necessary.
 *
 * See PQconnectPoll for more info.
 */
PGconn *
PQconnectStart(const char *conninfo)
{
	PGconn	   *conn;

	/*
	 * Allocate memory for the conn structure.  Note that we also expect this
	 * to initialize conn->errorMessage to empty.  All subsequent steps during
	 * connection initialization will only append to that buffer.
	 */
	conn = pqMakeEmptyPGconn();
	if (conn == NULL)
		return NULL;

	/*
	 * Parse the conninfo string
	 */
	if (!connectOptions1(conn, conninfo))
		return conn;

	/*
	 * Compute derived options
	 */
	if (!pqConnectOptions2(conn))
		return conn;

	/*
	 * Connect to the database
	 */
	if (!pqConnectDBStart(conn))
	{
		/* Just in case we failed to set it in pqConnectDBStart */
		conn->status = CONNECTION_BAD;
	}

	return conn;
}

/*
 * Move option values into conn structure
 *
 * Don't put anything cute here --- intelligence should be in
 * pqConnectOptions2 ...
 *
 * Returns true on success. On failure, returns false and sets error message.
 */
static bool
fillPGconn(PGconn *conn, PQconninfoOption *connOptions)
{
	const internalPQconninfoOption *option;

	for (option = PQconninfoOptions; option->keyword; option++)
	{
		if (option->connofs >= 0)
		{
			const char *tmp = conninfo_getval(connOptions, option->keyword);

			if (tmp)
			{
				char	  **connmember = (char **) ((char *) conn + option->connofs);

				free(*connmember);
				*connmember = strdup(tmp);
				if (*connmember == NULL)
				{
					libpq_append_conn_error(conn, "out of memory");
					return false;
				}
			}
		}
	}

	return true;
}

/*
 * Copy over option values from srcConn to dstConn
 *
 * Don't put anything cute here --- intelligence should be in
 * pqConnectOptions2 ...
 *
 * Returns true on success. On failure, returns false and sets error message of
 * dstConn.
 */
bool
pqCopyPGconn(PGconn *srcConn, PGconn *dstConn)
{
	const internalPQconninfoOption *option;

	/* copy over connection options */
	for (option = PQconninfoOptions; option->keyword; option++)
	{
		if (option->connofs >= 0)
		{
			const char **tmp = (const char **) ((char *) srcConn + option->connofs);

			if (*tmp)
			{
				char	  **dstConnmember = (char **) ((char *) dstConn + option->connofs);

				if (*dstConnmember)
					free(*dstConnmember);
				*dstConnmember = strdup(*tmp);
				if (*dstConnmember == NULL)
				{
					libpq_append_conn_error(dstConn, "out of memory");
					return false;
				}
			}
		}
	}
	return true;
}

/*
 *		connectOptions1
 *
 * Internal subroutine to set up connection parameters given an already-
 * created PGconn and a conninfo string.  Derived settings should be
 * processed by calling pqConnectOptions2 next.  (We split them because
 * PQsetdbLogin overrides defaults in between.)
 *
 * Returns true if OK, false if trouble (in which case errorMessage is set
 * and so is conn->status).
 */
static bool
connectOptions1(PGconn *conn, const char *conninfo)
{
	PQconninfoOption *connOptions;

	/*
	 * Parse the conninfo string
	 */
	connOptions = parse_connection_string(conninfo, &conn->errorMessage, true);
	if (connOptions == NULL)
	{
		conn->status = CONNECTION_BAD;
		/* errorMessage is already set */
		return false;
	}

	/*
	 * Move option values into conn structure
	 */
	if (!fillPGconn(conn, connOptions))
	{
		conn->status = CONNECTION_BAD;
		PQconninfoFree(connOptions);
		return false;
	}

	/*
	 * Free the option info - all is in conn now
	 */
	PQconninfoFree(connOptions);

	return true;
}

/*
 * Count the number of elements in a simple comma-separated list.
 */
static int
count_comma_separated_elems(const char *input)
{
	int			n;

	n = 1;
	for (; *input != '\0'; input++)
	{
		if (*input == ',')
			n++;
	}

	return n;
}

/*
 * Parse a simple comma-separated list.
 *
 * On each call, returns a malloc'd copy of the next element, and sets *more
 * to indicate whether there are any more elements in the list after this,
 * and updates *startptr to point to the next element, if any.
 *
 * On out of memory, returns NULL.
 */
static char *
parse_comma_separated_list(char **startptr, bool *more)
{
	char	   *p;
	char	   *s = *startptr;
	char	   *e;
	int			len;

	/*
	 * Search for the end of the current element; a comma or end-of-string
	 * acts as a terminator.
	 */
	e = s;
	while (*e != '\0' && *e != ',')
		++e;
	*more = (*e == ',');

	len = e - s;
	p = (char *) malloc(sizeof(char) * (len + 1));
	if (p)
	{
		memcpy(p, s, len);
		p[len] = '\0';
	}
	*startptr = e + 1;

	return p;
}

/*
 * Initializes the prng_state field of the connection. We want something
 * unpredictable, so if possible, use high-quality random bits for the
 * seed. Otherwise, fall back to a seed based on the connection address,
 * timestamp and PID.
 */
static void
libpq_prng_init(PGconn *conn)
{
	uint64		rseed;
	struct timeval tval = {0};

	if (pg_prng_strong_seed(&conn->prng_state))
		return;

	gettimeofday(&tval, NULL);

	rseed = ((uintptr_t) conn) ^
		((uint64) getpid()) ^
		((uint64) tval.tv_usec) ^
		((uint64) tval.tv_sec);

	pg_prng_seed(&conn->prng_state, rseed);
}

/*
 *		pqConnectOptions2
 *
 * Compute derived connection options after absorbing all user-supplied info.
 *
 * Returns true if OK, false if trouble (in which case errorMessage is set
 * and so is conn->status).
 */
bool
pqConnectOptions2(PGconn *conn)
{
	int			i;

	/*
	 * Allocate memory for details about each host to which we might possibly
	 * try to connect.  For that, count the number of elements in the hostaddr
	 * or host options.  If neither is given, assume one host.
	 */
	conn->whichhost = 0;
	if (conn->pghostaddr && conn->pghostaddr[0] != '\0')
		conn->nconnhost = count_comma_separated_elems(conn->pghostaddr);
	else if (conn->pghost && conn->pghost[0] != '\0')
		conn->nconnhost = count_comma_separated_elems(conn->pghost);
	else
		conn->nconnhost = 1;
	conn->connhost = (pg_conn_host *)
		calloc(conn->nconnhost, sizeof(pg_conn_host));
	if (conn->connhost == NULL)
		goto oom_error;

	/*
	 * We now have one pg_conn_host structure per possible host.  Fill in the
	 * host and hostaddr fields for each, by splitting the parameter strings.
	 */
	if (conn->pghostaddr != NULL && conn->pghostaddr[0] != '\0')
	{
		char	   *s = conn->pghostaddr;
		bool		more = true;

		for (i = 0; i < conn->nconnhost && more; i++)
		{
			conn->connhost[i].hostaddr = parse_comma_separated_list(&s, &more);
			if (conn->connhost[i].hostaddr == NULL)
				goto oom_error;
		}

		/*
		 * If hostaddr was given, the array was allocated according to the
		 * number of elements in the hostaddr list, so it really should be the
		 * right size.
		 */
		Assert(!more);
		Assert(i == conn->nconnhost);
	}

	if (conn->pghost != NULL && conn->pghost[0] != '\0')
	{
		char	   *s = conn->pghost;
		bool		more = true;

		for (i = 0; i < conn->nconnhost && more; i++)
		{
			conn->connhost[i].host = parse_comma_separated_list(&s, &more);
			if (conn->connhost[i].host == NULL)
				goto oom_error;
		}

		/* Check for wrong number of host items. */
		if (more || i != conn->nconnhost)
		{
			conn->status = CONNECTION_BAD;
			libpq_append_conn_error(conn, "could not match %d host names to %d hostaddr values",
									count_comma_separated_elems(conn->pghost), conn->nconnhost);
			return false;
		}
	}

	/*
	 * Now, for each host slot, identify the type of address spec, and fill in
	 * the default address if nothing was given.
	 */
	for (i = 0; i < conn->nconnhost; i++)
	{
		pg_conn_host *ch = &conn->connhost[i];

		if (ch->hostaddr != NULL && ch->hostaddr[0] != '\0')
			ch->type = CHT_HOST_ADDRESS;
		else if (ch->host != NULL && ch->host[0] != '\0')
		{
			ch->type = CHT_HOST_NAME;
			if (is_unixsock_path(ch->host))
				ch->type = CHT_UNIX_SOCKET;
		}
		else
		{
			free(ch->host);

			/*
			 * This bit selects the default host location.  If you change
			 * this, see also pg_regress.
			 */
			if (DEFAULT_PGSOCKET_DIR[0])
			{
				ch->host = strdup(DEFAULT_PGSOCKET_DIR);
				ch->type = CHT_UNIX_SOCKET;
			}
			else
			{
				ch->host = strdup(DefaultHost);
				ch->type = CHT_HOST_NAME;
			}
			if (ch->host == NULL)
				goto oom_error;
		}
	}

	/*
	 * Next, work out the port number corresponding to each host name.
	 *
	 * Note: unlike the above for host names, this could leave the port fields
	 * as null or empty strings.  We will substitute DEF_PGPORT whenever we
	 * read such a port field.
	 */
	if (conn->pgport != NULL && conn->pgport[0] != '\0')
	{
		char	   *s = conn->pgport;
		bool		more = true;

		for (i = 0; i < conn->nconnhost && more; i++)
		{
			conn->connhost[i].port = parse_comma_separated_list(&s, &more);
			if (conn->connhost[i].port == NULL)
				goto oom_error;
		}

		/*
		 * If exactly one port was given, use it for every host.  Otherwise,
		 * there must be exactly as many ports as there were hosts.
		 */
		if (i == 1 && !more)
		{
			for (i = 1; i < conn->nconnhost; i++)
			{
				conn->connhost[i].port = strdup(conn->connhost[0].port);
				if (conn->connhost[i].port == NULL)
					goto oom_error;
			}
		}
		else if (more || i != conn->nconnhost)
		{
			conn->status = CONNECTION_BAD;
			libpq_append_conn_error(conn, "could not match %d port numbers to %d hosts",
									count_comma_separated_elems(conn->pgport), conn->nconnhost);
			return false;
		}
	}

	/*
	 * If user name was not given, fetch it.  (Most likely, the fetch will
	 * fail, since the only way we get here is if pg_fe_getauthname() failed
	 * during conninfo_add_defaults().  But now we want an error message.)
	 */
	if (conn->pguser == NULL || conn->pguser[0] == '\0')
	{
		free(conn->pguser);
		conn->pguser = pg_fe_getauthname(&conn->errorMessage);
		if (!conn->pguser)
		{
			conn->status = CONNECTION_BAD;
			return false;
		}
	}

	/*
	 * If database name was not given, default it to equal user name
	 */
	if (conn->dbName == NULL || conn->dbName[0] == '\0')
	{
		free(conn->dbName);
		conn->dbName = strdup(conn->pguser);
		if (!conn->dbName)
			goto oom_error;
	}

	/*
	 * If password was not given, try to look it up in password file.  Note
	 * that the result might be different for each host/port pair.
	 */
	if (conn->pgpass == NULL || conn->pgpass[0] == '\0')
	{
		/* If password file wasn't specified, use ~/PGPASSFILE */
		if (conn->pgpassfile == NULL || conn->pgpassfile[0] == '\0')
		{
			char		homedir[MAXPGPATH];

			if (pqGetHomeDirectory(homedir, sizeof(homedir)))
			{
				free(conn->pgpassfile);
				conn->pgpassfile = malloc(MAXPGPATH);
				if (!conn->pgpassfile)
					goto oom_error;
				snprintf(conn->pgpassfile, MAXPGPATH, "%s/%s",
						 homedir, PGPASSFILE);
			}
		}

		if (conn->pgpassfile != NULL && conn->pgpassfile[0] != '\0')
		{
			for (i = 0; i < conn->nconnhost; i++)
			{
				/*
				 * Try to get a password for this host from file.  We use host
				 * for the hostname search key if given, else hostaddr (at
				 * least one of them is guaranteed nonempty by now).
				 */
				const char *pwhost = conn->connhost[i].host;

				if (pwhost == NULL || pwhost[0] == '\0')
					pwhost = conn->connhost[i].hostaddr;

				conn->connhost[i].password =
					passwordFromFile(pwhost,
									 conn->connhost[i].port,
									 conn->dbName,
									 conn->pguser,
									 conn->pgpassfile);
			}
		}
	}

	/*
	 * parse and validate require_auth option
	 */
	if (conn->require_auth && conn->require_auth[0])
	{
		char	   *s = conn->require_auth;
		bool		first,
					more;
		bool		negated = false;

		/*
		 * By default, start from an empty set of allowed options and add to
		 * it.
		 */
		conn->auth_required = true;
		conn->allowed_auth_methods = 0;

		for (first = true, more = true; more; first = false)
		{
			char	   *method,
					   *part;
			uint32		bits;

			part = parse_comma_separated_list(&s, &more);
			if (part == NULL)
				goto oom_error;

			/*
			 * Check for negation, e.g. '!password'. If one element is
			 * negated, they all have to be.
			 */
			method = part;
			if (*method == '!')
			{
				if (first)
				{
					/*
					 * Switch to a permissive set of allowed options, and
					 * subtract from it.
					 */
					conn->auth_required = false;
					conn->allowed_auth_methods = -1;
				}
				else if (!negated)
				{
					conn->status = CONNECTION_BAD;
					libpq_append_conn_error(conn, "negative require_auth method \"%s\" cannot be mixed with non-negative methods",
											method);

					free(part);
					return false;
				}

				negated = true;
				method++;
			}
			else if (negated)
			{
				conn->status = CONNECTION_BAD;
				libpq_append_conn_error(conn, "require_auth method \"%s\" cannot be mixed with negative methods",
										method);

				free(part);
				return false;
			}

			if (strcmp(method, "password") == 0)
			{
				bits = (1 << AUTH_REQ_PASSWORD);
			}
			else if (strcmp(method, "md5") == 0)
			{
				bits = (1 << AUTH_REQ_MD5);
			}
			else if (strcmp(method, "gss") == 0)
			{
				bits = (1 << AUTH_REQ_GSS);
				bits |= (1 << AUTH_REQ_GSS_CONT);
			}
			else if (strcmp(method, "sspi") == 0)
			{
				bits = (1 << AUTH_REQ_SSPI);
				bits |= (1 << AUTH_REQ_GSS_CONT);
			}
			else if (strcmp(method, "scram-sha-256") == 0)
			{
				/* This currently assumes that SCRAM is the only SASL method. */
				bits = (1 << AUTH_REQ_SASL);
				bits |= (1 << AUTH_REQ_SASL_CONT);
				bits |= (1 << AUTH_REQ_SASL_FIN);
			}
			else if (strcmp(method, "none") == 0)
			{
				/*
				 * Special case: let the user explicitly allow (or disallow)
				 * connections where the server does not send an explicit
				 * authentication challenge, such as "trust" and "cert" auth.
				 */
				if (negated)	/* "!none" */
				{
					if (conn->auth_required)
						goto duplicate;

					conn->auth_required = true;
				}
				else			/* "none" */
				{
					if (!conn->auth_required)
						goto duplicate;

					conn->auth_required = false;
				}

				free(part);
				continue;		/* avoid the bitmask manipulation below */
			}
			else
			{
				conn->status = CONNECTION_BAD;
				libpq_append_conn_error(conn, "invalid %s value: \"%s\"",
										"require_auth", method);

				free(part);
				return false;
			}

			/* Update the bitmask. */
			if (negated)
			{
				if ((conn->allowed_auth_methods & bits) == 0)
					goto duplicate;

				conn->allowed_auth_methods &= ~bits;
			}
			else
			{
				if ((conn->allowed_auth_methods & bits) == bits)
					goto duplicate;

				conn->allowed_auth_methods |= bits;
			}

			free(part);
			continue;

	duplicate:

			/*
			 * A duplicated method probably indicates a typo in a setting
			 * where typos are extremely risky.
			 */
			conn->status = CONNECTION_BAD;
			libpq_append_conn_error(conn, "require_auth method \"%s\" is specified more than once",
									part);

			free(part);
			return false;
		}
	}

	/*
	 * validate channel_binding option
	 */
	if (conn->channel_binding)
	{
		if (strcmp(conn->channel_binding, "disable") != 0
			&& strcmp(conn->channel_binding, "prefer") != 0
			&& strcmp(conn->channel_binding, "require") != 0)
		{
			conn->status = CONNECTION_BAD;
			libpq_append_conn_error(conn, "invalid %s value: \"%s\"",
									"channel_binding", conn->channel_binding);
			return false;
		}
	}
	else
	{
		conn->channel_binding = strdup(DefaultChannelBinding);
		if (!conn->channel_binding)
			goto oom_error;
	}

#ifndef USE_SSL

	/*
	 * sslrootcert=system is not supported. Since setting this changes the
	 * default sslmode, check this _before_ we validate sslmode, to avoid
	 * confusing the user with errors for an option they may not have set.
	 */
	if (conn->sslrootcert
		&& strcmp(conn->sslrootcert, "system") == 0)
	{
		conn->status = CONNECTION_BAD;
		libpq_append_conn_error(conn, "%s value \"%s\" invalid when SSL support is not compiled in",
								"sslrootcert", conn->sslrootcert);
		return false;
	}
#endif

	/*
	 * validate sslmode option
	 */
	if (conn->sslmode)
	{
		if (strcmp(conn->sslmode, "disable") != 0
			&& strcmp(conn->sslmode, "allow") != 0
			&& strcmp(conn->sslmode, "prefer") != 0
			&& strcmp(conn->sslmode, "require") != 0
			&& strcmp(conn->sslmode, "verify-ca") != 0
			&& strcmp(conn->sslmode, "verify-full") != 0)
		{
			conn->status = CONNECTION_BAD;
			libpq_append_conn_error(conn, "invalid %s value: \"%s\"",
									"sslmode", conn->sslmode);
			return false;
		}

#ifndef USE_SSL
		switch (conn->sslmode[0])
		{
			case 'a':			/* "allow" */
			case 'p':			/* "prefer" */

				/*
				 * warn user that an SSL connection will never be negotiated
				 * since SSL was not compiled in?
				 */
				break;

			case 'r':			/* "require" */
			case 'v':			/* "verify-ca" or "verify-full" */
				conn->status = CONNECTION_BAD;
				libpq_append_conn_error(conn, "%s value \"%s\" invalid when SSL support is not compiled in",
										"sslmode", conn->sslmode);
				return false;
		}
#endif
	}
	else
	{
		conn->sslmode = strdup(DefaultSSLMode);
		if (!conn->sslmode)
			goto oom_error;
	}

	/*
	 * validate sslnegotiation option, default is "postgres" for the postgres
	 * style negotiated connection with an extra round trip but more options.
	 */
	if (conn->sslnegotiation)
	{
		if (strcmp(conn->sslnegotiation, "postgres") != 0
			&& strcmp(conn->sslnegotiation, "direct") != 0)
		{
			conn->status = CONNECTION_BAD;
			libpq_append_conn_error(conn, "invalid %s value: \"%s\"",
									"sslnegotiation", conn->sslnegotiation);
			return false;
		}

#ifndef USE_SSL
		if (conn->sslnegotiation[0] != 'p')
		{
			conn->status = CONNECTION_BAD;
			libpq_append_conn_error(conn, "%s value \"%s\" invalid when SSL support is not compiled in",
									"sslnegotiation", conn->sslnegotiation);
			return false;
		}
#endif

		/*
		 * Don't allow direct SSL negotiation with sslmode='prefer', because
		 * that poses a risk of unintentional fallback to plaintext connection
		 * when connecting to a pre-v17 server that does not support direct
		 * SSL connections. To keep things simple, don't allow it with
		 * sslmode='allow' or sslmode='disable' either. If a user goes through
		 * the trouble of setting sslnegotiation='direct', they probably
		 * intend to use SSL, and sslmode=disable or allow is probably a user
		 * user mistake anyway.
		 */
		if (conn->sslnegotiation[0] == 'd' &&
			conn->sslmode[0] != 'r' && conn->sslmode[0] != 'v')
		{
			conn->status = CONNECTION_BAD;
			libpq_append_conn_error(conn, "weak sslmode \"%s\" may not be used with sslnegotiation=direct (use \"require\", \"verify-ca\", or \"verify-full\")",
									conn->sslmode);
			return false;
		}
	}
	else
	{
		conn->sslnegotiation = strdup(DefaultSSLNegotiation);
		if (!conn->sslnegotiation)
			goto oom_error;
	}

#ifdef USE_SSL

	/*
	 * If sslrootcert=system, make sure our chosen sslmode is compatible.
	 */
	if (conn->sslrootcert
		&& strcmp(conn->sslrootcert, "system") == 0
		&& strcmp(conn->sslmode, "verify-full") != 0)
	{
		conn->status = CONNECTION_BAD;
		libpq_append_conn_error(conn, "weak sslmode \"%s\" may not be used with sslrootcert=system (use \"verify-full\")",
								conn->sslmode);
		return false;
	}
#endif

	/*
	 * Validate TLS protocol versions for ssl_min_protocol_version and
	 * ssl_max_protocol_version.
	 */
	if (!sslVerifyProtocolVersion(conn->ssl_min_protocol_version))
	{
		conn->status = CONNECTION_BAD;
		libpq_append_conn_error(conn, "invalid \"%s\" value: \"%s\"",
								"ssl_min_protocol_version",
								conn->ssl_min_protocol_version);
		return false;
	}
	if (!sslVerifyProtocolVersion(conn->ssl_max_protocol_version))
	{
		conn->status = CONNECTION_BAD;
		libpq_append_conn_error(conn, "invalid \"%s\" value: \"%s\"",
								"ssl_max_protocol_version",
								conn->ssl_max_protocol_version);
		return false;
	}

	/*
	 * Check if the range of SSL protocols defined is correct.  This is done
	 * at this early step because this is independent of the SSL
	 * implementation used, and this avoids unnecessary cycles with an
	 * already-built SSL context when the connection is being established, as
	 * it would be doomed anyway.
	 */
	if (!sslVerifyProtocolRange(conn->ssl_min_protocol_version,
								conn->ssl_max_protocol_version))
	{
		conn->status = CONNECTION_BAD;
		libpq_append_conn_error(conn, "invalid SSL protocol version range");
		return false;
	}

	/*
	 * validate sslcertmode option
	 */
	if (conn->sslcertmode)
	{
		if (strcmp(conn->sslcertmode, "disable") != 0 &&
			strcmp(conn->sslcertmode, "allow") != 0 &&
			strcmp(conn->sslcertmode, "require") != 0)
		{
			conn->status = CONNECTION_BAD;
			libpq_append_conn_error(conn, "invalid %s value: \"%s\"",
									"sslcertmode", conn->sslcertmode);
			return false;
		}
#ifndef USE_SSL
		if (strcmp(conn->sslcertmode, "require") == 0)
		{
			conn->status = CONNECTION_BAD;
			libpq_append_conn_error(conn, "%s value \"%s\" invalid when SSL support is not compiled in",
									"sslcertmode", conn->sslcertmode);
			return false;
		}
#endif
#ifndef HAVE_SSL_CTX_SET_CERT_CB

		/*
		 * Without a certificate callback, the current implementation can't
		 * figure out if a certificate was actually requested, so "require" is
		 * useless.
		 */
		if (strcmp(conn->sslcertmode, "require") == 0)
		{
			conn->status = CONNECTION_BAD;
			libpq_append_conn_error(conn, "%s value \"%s\" is not supported (check OpenSSL version)",
									"sslcertmode", conn->sslcertmode);
			return false;
		}
#endif
	}
	else
	{
		conn->sslcertmode = strdup(DefaultSSLCertMode);
		if (!conn->sslcertmode)
			goto oom_error;
	}

	/*
	 * validate gssencmode option
	 */
	if (conn->gssencmode)
	{
		if (strcmp(conn->gssencmode, "disable") != 0 &&
			strcmp(conn->gssencmode, "prefer") != 0 &&
			strcmp(conn->gssencmode, "require") != 0)
		{
			conn->status = CONNECTION_BAD;
			libpq_append_conn_error(conn, "invalid %s value: \"%s\"", "gssencmode", conn->gssencmode);
			return false;
		}
#ifndef ENABLE_GSS
		if (strcmp(conn->gssencmode, "require") == 0)
		{
			conn->status = CONNECTION_BAD;
			libpq_append_conn_error(conn, "gssencmode value \"%s\" invalid when GSSAPI support is not compiled in",
									conn->gssencmode);
			return false;
		}
#endif
	}
	else
	{
		conn->gssencmode = strdup(DefaultGSSMode);
		if (!conn->gssencmode)
			goto oom_error;
	}

	/*
	 * validate target_session_attrs option, and set target_server_type
	 */
	if (conn->target_session_attrs)
	{
		if (strcmp(conn->target_session_attrs, "any") == 0)
			conn->target_server_type = SERVER_TYPE_ANY;
		else if (strcmp(conn->target_session_attrs, "read-write") == 0)
			conn->target_server_type = SERVER_TYPE_READ_WRITE;
		else if (strcmp(conn->target_session_attrs, "read-only") == 0)
			conn->target_server_type = SERVER_TYPE_READ_ONLY;
		else if (strcmp(conn->target_session_attrs, "primary") == 0)
			conn->target_server_type = SERVER_TYPE_PRIMARY;
		else if (strcmp(conn->target_session_attrs, "standby") == 0)
			conn->target_server_type = SERVER_TYPE_STANDBY;
		else if (strcmp(conn->target_session_attrs, "prefer-standby") == 0)
			conn->target_server_type = SERVER_TYPE_PREFER_STANDBY;
		else
		{
			conn->status = CONNECTION_BAD;
			libpq_append_conn_error(conn, "invalid %s value: \"%s\"",
									"target_session_attrs",
									conn->target_session_attrs);
			return false;
		}
	}
	else
		conn->target_server_type = SERVER_TYPE_ANY;

	if (conn->scram_client_key)
	{
		int			len;

		len = pg_b64_dec_len(strlen(conn->scram_client_key));
		conn->scram_client_key_binary = malloc(len);
		if (!conn->scram_client_key_binary)
			goto oom_error;
		len = pg_b64_decode(conn->scram_client_key, strlen(conn->scram_client_key),
							conn->scram_client_key_binary, len);
		if (len < 0)
		{
			libpq_append_conn_error(conn, "invalid SCRAM client key");
			free(conn->scram_client_key_binary);
			return false;
		}
		if (len != SCRAM_MAX_KEY_LEN)
		{
			libpq_append_conn_error(conn, "invalid SCRAM client key length: %d", len);
			free(conn->scram_client_key_binary);
			return false;
		}
		conn->scram_client_key_len = len;
	}

	if (conn->scram_server_key)
	{
		int			len;

		len = pg_b64_dec_len(strlen(conn->scram_server_key));
		conn->scram_server_key_binary = malloc(len);
		if (!conn->scram_server_key_binary)
			goto oom_error;
		len = pg_b64_decode(conn->scram_server_key, strlen(conn->scram_server_key),
							conn->scram_server_key_binary, len);
		if (len < 0)
		{
			libpq_append_conn_error(conn, "invalid SCRAM server key");
			free(conn->scram_server_key_binary);
			return false;
		}
		if (len != SCRAM_MAX_KEY_LEN)
		{
			libpq_append_conn_error(conn, "invalid SCRAM server key length: %d", len);
			free(conn->scram_server_key_binary);
			return false;
		}
		conn->scram_server_key_len = len;
	}

	/*
	 * validate load_balance_hosts option, and set load_balance_type
	 */
	if (conn->load_balance_hosts)
	{
		if (strcmp(conn->load_balance_hosts, "disable") == 0)
			conn->load_balance_type = LOAD_BALANCE_DISABLE;
		else if (strcmp(conn->load_balance_hosts, "random") == 0)
			conn->load_balance_type = LOAD_BALANCE_RANDOM;
		else
		{
			conn->status = CONNECTION_BAD;
			libpq_append_conn_error(conn, "invalid %s value: \"%s\"",
									"load_balance_hosts",
									conn->load_balance_hosts);
			return false;
		}
	}
	else
		conn->load_balance_type = LOAD_BALANCE_DISABLE;

	if (conn->load_balance_type == LOAD_BALANCE_RANDOM)
	{
		libpq_prng_init(conn);

		/*
		 * This is the "inside-out" variant of the Fisher-Yates shuffle
		 * algorithm. Notionally, we append each new value to the array and
		 * then swap it with a randomly-chosen array element (possibly
		 * including itself, else we fail to generate permutations with the
		 * last integer last).  The swap step can be optimized by combining it
		 * with the insertion.
		 */
		for (i = 1; i < conn->nconnhost; i++)
		{
			int			j = pg_prng_uint64_range(&conn->prng_state, 0, i);
			pg_conn_host temp = conn->connhost[j];

			conn->connhost[j] = conn->connhost[i];
			conn->connhost[i] = temp;
		}
	}

	/*
	 * Resolve special "auto" client_encoding from the locale
	 */
	if (conn->client_encoding_initial &&
		strcmp(conn->client_encoding_initial, "auto") == 0)
	{
		free(conn->client_encoding_initial);
		conn->client_encoding_initial = strdup(pg_encoding_to_char(pg_get_encoding_from_locale(NULL, true)));
		if (!conn->client_encoding_initial)
			goto oom_error;
	}

	/*
	 * Only if we get this far is it appropriate to try to connect. (We need a
	 * state flag, rather than just the boolean result of this function, in
	 * case someone tries to PQreset() the PGconn.)
	 */
	conn->options_valid = true;

	return true;

oom_error:
	conn->status = CONNECTION_BAD;
	libpq_append_conn_error(conn, "out of memory");
	return false;
}

/*
 *		PQconndefaults
 *
 * Construct a default connection options array, which identifies all the
 * available options and shows any default values that are available from the
 * environment etc.  On error (eg out of memory), NULL is returned.
 *
 * Using this function, an application may determine all possible options
 * and their current default values.
 *
 * NOTE: as of PostgreSQL 7.0, the returned array is dynamically allocated
 * and should be freed when no longer needed via PQconninfoFree().  (In prior
 * versions, the returned array was static, but that's not thread-safe.)
 * Pre-7.0 applications that use this function will see a small memory leak
 * until they are updated to call PQconninfoFree.
 */
PQconninfoOption *
PQconndefaults(void)
{
	PQExpBufferData errorBuf;
	PQconninfoOption *connOptions;

	/* We don't actually report any errors here, but callees want a buffer */
	initPQExpBuffer(&errorBuf);
	if (PQExpBufferDataBroken(errorBuf))
		return NULL;			/* out of memory already :-( */

	connOptions = conninfo_init(&errorBuf);
	if (connOptions != NULL)
	{
		/* pass NULL errorBuf to ignore errors */
		if (!conninfo_add_defaults(connOptions, NULL))
		{
			PQconninfoFree(connOptions);
			connOptions = NULL;
		}
	}

	termPQExpBuffer(&errorBuf);
	return connOptions;
}

/* ----------------
 *		PQsetdbLogin
 *
 * establishes a connection to a postgres backend through the postmaster
 * at the specified host and port.
 *
 * returns a PGconn* which is needed for all subsequent libpq calls
 *
 * if the status field of the connection returned is CONNECTION_BAD,
 * then only the errorMessage is likely to be useful.
 * ----------------
 */
PGconn *
PQsetdbLogin(const char *pghost, const char *pgport, const char *pgoptions,
			 const char *pgtty, const char *dbName, const char *login,
			 const char *pwd)
{
	PGconn	   *conn;

	/*
	 * Allocate memory for the conn structure.  Note that we also expect this
	 * to initialize conn->errorMessage to empty.  All subsequent steps during
	 * connection initialization will only append to that buffer.
	 */
	conn = pqMakeEmptyPGconn();
	if (conn == NULL)
		return NULL;

	/*
	 * If the dbName parameter contains what looks like a connection string,
	 * parse it into conn struct using connectOptions1.
	 */
	if (dbName && recognized_connection_string(dbName))
	{
		if (!connectOptions1(conn, dbName))
			return conn;
	}
	else
	{
		/*
		 * Old-style path: first, parse an empty conninfo string in order to
		 * set up the same defaults that PQconnectdb() would use.
		 */
		if (!connectOptions1(conn, ""))
			return conn;

		/* Insert dbName parameter value into struct */
		if (dbName && dbName[0] != '\0')
		{
			free(conn->dbName);
			conn->dbName = strdup(dbName);
			if (!conn->dbName)
				goto oom_error;
		}
	}

	/*
	 * Insert remaining parameters into struct, overriding defaults (as well
	 * as any conflicting data from dbName taken as a conninfo).
	 */
	if (pghost && pghost[0] != '\0')
	{
		free(conn->pghost);
		conn->pghost = strdup(pghost);
		if (!conn->pghost)
			goto oom_error;
	}

	if (pgport && pgport[0] != '\0')
	{
		free(conn->pgport);
		conn->pgport = strdup(pgport);
		if (!conn->pgport)
			goto oom_error;
	}

	if (pgoptions && pgoptions[0] != '\0')
	{
		free(conn->pgoptions);
		conn->pgoptions = strdup(pgoptions);
		if (!conn->pgoptions)
			goto oom_error;
	}

	if (login && login[0] != '\0')
	{
		free(conn->pguser);
		conn->pguser = strdup(login);
		if (!conn->pguser)
			goto oom_error;
	}

	if (pwd && pwd[0] != '\0')
	{
		free(conn->pgpass);
		conn->pgpass = strdup(pwd);
		if (!conn->pgpass)
			goto oom_error;
	}

	/*
	 * Compute derived options
	 */
	if (!pqConnectOptions2(conn))
		return conn;

	/*
	 * Connect to the database
	 */
	if (pqConnectDBStart(conn))
		(void) pqConnectDBComplete(conn);

	return conn;

oom_error:
	conn->status = CONNECTION_BAD;
	libpq_append_conn_error(conn, "out of memory");
	return conn;
}


/* ----------
 * connectNoDelay -
 * Sets the TCP_NODELAY socket option.
 * Returns 1 if successful, 0 if not.
 * ----------
 */
static int
connectNoDelay(PGconn *conn)
{
#ifdef	TCP_NODELAY
	int			on = 1;

	if (setsockopt(conn->sock, IPPROTO_TCP, TCP_NODELAY,
				   (char *) &on,
				   sizeof(on)) < 0)
	{
		char		sebuf[PG_STRERROR_R_BUFLEN];

		libpq_append_conn_error(conn, "could not set socket to TCP no delay mode: %s",
								SOCK_STRERROR(SOCK_ERRNO, sebuf, sizeof(sebuf)));
		return 0;
	}
#endif

	return 1;
}

/* ----------
 * Write currently connected IP address into host_addr (of len host_addr_len).
 * If unable to, set it to the empty string.
 * ----------
 */
static void
getHostaddr(PGconn *conn, char *host_addr, int host_addr_len)
{
	struct sockaddr_storage *addr = &conn->raddr.addr;

	if (addr->ss_family == AF_INET)
	{
		if (pg_inet_net_ntop(AF_INET,
							 &((struct sockaddr_in *) addr)->sin_addr.s_addr,
							 32,
							 host_addr, host_addr_len) == NULL)
			host_addr[0] = '\0';
	}
	else if (addr->ss_family == AF_INET6)
	{
		if (pg_inet_net_ntop(AF_INET6,
							 &((struct sockaddr_in6 *) addr)->sin6_addr.s6_addr,
							 128,
							 host_addr, host_addr_len) == NULL)
			host_addr[0] = '\0';
	}
	else
		host_addr[0] = '\0';
}

/*
 * emitHostIdentityInfo -
 * Speculatively append "connection to server so-and-so failed: " to
 * conn->errorMessage once we've identified the current connection target
 * address.  This ensures that any subsequent error message will be properly
 * attributed to the server we couldn't connect to.  conn->raddr must be
 * valid, and the result of getHostaddr() must be supplied.
 */
static void
emitHostIdentityInfo(PGconn *conn, const char *host_addr)
{
	if (conn->raddr.addr.ss_family == AF_UNIX)
	{
		char		service[NI_MAXHOST];

		pg_getnameinfo_all(&conn->raddr.addr, conn->raddr.salen,
						   NULL, 0,
						   service, sizeof(service),
						   NI_NUMERICSERV);
		appendPQExpBuffer(&conn->errorMessage,
						  libpq_gettext("connection to server on socket \"%s\" failed: "),
						  service);
	}
	else
	{
		const char *displayed_host;
		const char *displayed_port;

		/* To which host and port were we actually connecting? */
		if (conn->connhost[conn->whichhost].type == CHT_HOST_ADDRESS)
			displayed_host = conn->connhost[conn->whichhost].hostaddr;
		else
			displayed_host = conn->connhost[conn->whichhost].host;
		displayed_port = conn->connhost[conn->whichhost].port;
		if (displayed_port == NULL || displayed_port[0] == '\0')
			displayed_port = DEF_PGPORT_STR;

		/*
		 * If the user did not supply an IP address using 'hostaddr', and
		 * 'host' was missing or does not match our lookup, display the
		 * looked-up IP address.
		 */
		if (conn->connhost[conn->whichhost].type != CHT_HOST_ADDRESS &&
			host_addr[0] &&
			strcmp(displayed_host, host_addr) != 0)
			appendPQExpBuffer(&conn->errorMessage,
							  libpq_gettext("connection to server at \"%s\" (%s), port %s failed: "),
							  displayed_host, host_addr,
							  displayed_port);
		else
			appendPQExpBuffer(&conn->errorMessage,
							  libpq_gettext("connection to server at \"%s\", port %s failed: "),
							  displayed_host,
							  displayed_port);
	}
}

/* ----------
 * connectFailureMessage -
 * create a friendly error message on connection failure,
 * using the given errno value.  Use this for error cases that
 * imply that there's no server there.
 * ----------
 */
static void
connectFailureMessage(PGconn *conn, int errorno)
{
	char		sebuf[PG_STRERROR_R_BUFLEN];

	appendPQExpBuffer(&conn->errorMessage,
					  "%s\n",
					  SOCK_STRERROR(errorno, sebuf, sizeof(sebuf)));

	if (conn->raddr.addr.ss_family == AF_UNIX)
		libpq_append_conn_error(conn, "\tIs the server running locally and accepting connections on that socket?");
	else
		libpq_append_conn_error(conn, "\tIs the server running on that host and accepting TCP/IP connections?");
}

/*
 * Should we use keepalives?  Returns 1 if yes, 0 if no, and -1 if
 * conn->keepalives is set to a value which is not parseable as an
 * integer.
 */
static int
useKeepalives(PGconn *conn)
{
	int			val;

	if (conn->keepalives == NULL)
		return 1;

	if (!pqParseIntParam(conn->keepalives, &val, conn, "keepalives"))
		return -1;

	return val != 0 ? 1 : 0;
}

#ifndef WIN32
/*
 * Set the keepalive idle timer.
 */
static int
setKeepalivesIdle(PGconn *conn)
{
	int			idle;

	if (conn->keepalives_idle == NULL)
		return 1;

	if (!pqParseIntParam(conn->keepalives_idle, &idle, conn,
						 "keepalives_idle"))
		return 0;
	if (idle < 0)
		idle = 0;

#ifdef PG_TCP_KEEPALIVE_IDLE
	if (setsockopt(conn->sock, IPPROTO_TCP, PG_TCP_KEEPALIVE_IDLE,
				   (char *) &idle, sizeof(idle)) < 0)
	{
		char		sebuf[PG_STRERROR_R_BUFLEN];

		libpq_append_conn_error(conn, "%s(%s) failed: %s",
								"setsockopt",
								PG_TCP_KEEPALIVE_IDLE_STR,
								SOCK_STRERROR(SOCK_ERRNO, sebuf, sizeof(sebuf)));
		return 0;
	}
#endif

	return 1;
}

/*
 * Set the keepalive interval.
 */
static int
setKeepalivesInterval(PGconn *conn)
{
	int			interval;

	if (conn->keepalives_interval == NULL)
		return 1;

	if (!pqParseIntParam(conn->keepalives_interval, &interval, conn,
						 "keepalives_interval"))
		return 0;
	if (interval < 0)
		interval = 0;

#ifdef TCP_KEEPINTVL
	if (setsockopt(conn->sock, IPPROTO_TCP, TCP_KEEPINTVL,
				   (char *) &interval, sizeof(interval)) < 0)
	{
		char		sebuf[PG_STRERROR_R_BUFLEN];

		libpq_append_conn_error(conn, "%s(%s) failed: %s",
								"setsockopt",
								"TCP_KEEPINTVL",
								SOCK_STRERROR(SOCK_ERRNO, sebuf, sizeof(sebuf)));
		return 0;
	}
#endif

	return 1;
}

/*
 * Set the count of lost keepalive packets that will trigger a connection
 * break.
 */
static int
setKeepalivesCount(PGconn *conn)
{
	int			count;

	if (conn->keepalives_count == NULL)
		return 1;

	if (!pqParseIntParam(conn->keepalives_count, &count, conn,
						 "keepalives_count"))
		return 0;
	if (count < 0)
		count = 0;

#ifdef TCP_KEEPCNT
	if (setsockopt(conn->sock, IPPROTO_TCP, TCP_KEEPCNT,
				   (char *) &count, sizeof(count)) < 0)
	{
		char		sebuf[PG_STRERROR_R_BUFLEN];

		libpq_append_conn_error(conn, "%s(%s) failed: %s",
								"setsockopt",
								"TCP_KEEPCNT",
								SOCK_STRERROR(SOCK_ERRNO, sebuf, sizeof(sebuf)));
		return 0;
	}
#endif

	return 1;
}
#else							/* WIN32 */
#ifdef SIO_KEEPALIVE_VALS
/*
 * Enable keepalives and set the keepalive values on Win32,
 * where they are always set in one batch.
 *
 * CAUTION: This needs to be signal safe, since it's used by PQcancel.
 */
int
pqSetKeepalivesWin32(pgsocket sock, int idle, int interval)
{
	struct tcp_keepalive ka;
	DWORD		retsize;

	if (idle <= 0)
		idle = 2 * 60 * 60;		/* 2 hours = default */
	if (interval <= 0)
		interval = 1;			/* 1 second = default */

	ka.onoff = 1;
	ka.keepalivetime = idle * 1000;
	ka.keepaliveinterval = interval * 1000;

	if (WSAIoctl(sock,
				 SIO_KEEPALIVE_VALS,
				 (LPVOID) &ka,
				 sizeof(ka),
				 NULL,
				 0,
				 &retsize,
				 NULL,
				 NULL)
		!= 0)
		return 0;
	return 1;
}

static int
prepKeepalivesWin32(PGconn *conn)
{
	int			idle = -1;
	int			interval = -1;

	if (conn->keepalives_idle &&
		!pqParseIntParam(conn->keepalives_idle, &idle, conn,
						 "keepalives_idle"))
		return 0;
	if (conn->keepalives_interval &&
		!pqParseIntParam(conn->keepalives_interval, &interval, conn,
						 "keepalives_interval"))
		return 0;

	if (!pqSetKeepalivesWin32(conn->sock, idle, interval))
	{
		libpq_append_conn_error(conn, "%s(%s) failed: error code %d",
								"WSAIoctl", "SIO_KEEPALIVE_VALS",
								WSAGetLastError());
		return 0;
	}
	return 1;
}
#endif							/* SIO_KEEPALIVE_VALS */
#endif							/* WIN32 */

/*
 * Set the TCP user timeout.
 */
static int
setTCPUserTimeout(PGconn *conn)
{
	int			timeout;

	if (conn->pgtcp_user_timeout == NULL)
		return 1;

	if (!pqParseIntParam(conn->pgtcp_user_timeout, &timeout, conn,
						 "tcp_user_timeout"))
		return 0;

	if (timeout < 0)
		timeout = 0;

#ifdef TCP_USER_TIMEOUT
	if (setsockopt(conn->sock, IPPROTO_TCP, TCP_USER_TIMEOUT,
				   (char *) &timeout, sizeof(timeout)) < 0)
	{
		char		sebuf[256];

		libpq_append_conn_error(conn, "%s(%s) failed: %s",
								"setsockopt",
								"TCP_USER_TIMEOUT",
								SOCK_STRERROR(SOCK_ERRNO, sebuf, sizeof(sebuf)));
		return 0;
	}
#endif

	return 1;
}

/* ----------
 * pqConnectDBStart -
 *		Begin the process of making a connection to the backend.
 *
 * Returns 1 if successful, 0 if not.
 * ----------
 */
int
pqConnectDBStart(PGconn *conn)
{
	if (!conn)
		return 0;

	if (!conn->options_valid)
		goto connect_errReturn;

	/*
	 * Check for bad linking to backend-internal versions of src/common
	 * functions (see comments in link-canary.c for the reason we need this).
	 * Nobody but developers should see this message, so we don't bother
	 * translating it.
	 */
	if (!pg_link_canary_is_frontend())
	{
		appendPQExpBufferStr(&conn->errorMessage,
							 "libpq is incorrectly linked to backend functions\n");
		goto connect_errReturn;
	}

	/* Ensure our buffers are empty */
	conn->inStart = conn->inCursor = conn->inEnd = 0;
	conn->outCount = 0;

	/*
	 * Set up to try to connect to the first host.  (Setting whichhost = -1 is
	 * a bit of a cheat, but PQconnectPoll will advance it to 0 before
	 * anything else looks at it.)
	 *
	 * Cancel requests are special though, they should only try one host and
	 * address, and these fields have already been set up in PQcancelCreate,
	 * so leave these fields alone for cancel requests.
	 */
	if (!conn->cancelRequest)
	{
		conn->whichhost = -1;
		conn->try_next_host = true;
		conn->try_next_addr = false;
	}

	conn->status = CONNECTION_NEEDED;

	/* Also reset the target_server_type state if needed */
	if (conn->target_server_type == SERVER_TYPE_PREFER_STANDBY_PASS2)
		conn->target_server_type = SERVER_TYPE_PREFER_STANDBY;

	/*
	 * The code for processing CONNECTION_NEEDED state is in PQconnectPoll(),
	 * so that it can easily be re-executed if needed again during the
	 * asynchronous startup process.  However, we must run it once here,
	 * because callers expect a success return from this routine to mean that
	 * we are in PGRES_POLLING_WRITING connection state.
	 */
	if (PQconnectPoll(conn) == PGRES_POLLING_WRITING)
		return 1;

connect_errReturn:

	/*
	 * If we managed to open a socket, close it immediately rather than
	 * waiting till PQfinish.  (The application cannot have gotten the socket
	 * from PQsocket yet, so this doesn't risk breaking anything.)
	 */
	pqDropConnection(conn, true);
	conn->status = CONNECTION_BAD;
	return 0;
}


/*
 *		pqConnectDBComplete
 *
 * Block and complete a connection.
 *
 * Returns 1 on success, 0 on failure.
 */
int
pqConnectDBComplete(PGconn *conn)
{
	PostgresPollingStatusType flag = PGRES_POLLING_WRITING;
	pg_usec_time_t end_time = -1;
	int			timeout = 0;
	int			last_whichhost = -2;	/* certainly different from whichhost */
	int			last_whichaddr = -2;	/* certainly different from whichaddr */

	if (conn == NULL || conn->status == CONNECTION_BAD)
		return 0;

	/*
	 * Set up a time limit, if connect_timeout is greater than zero.
	 */
	if (conn->connect_timeout != NULL)
	{
		if (!pqParseIntParam(conn->connect_timeout, &timeout, conn,
							 "connect_timeout"))
		{
			/* mark the connection as bad to report the parsing failure */
			conn->status = CONNECTION_BAD;
			return 0;
		}
	}

	for (;;)
	{
		int			ret = 0;

		/*
		 * (Re)start the connect_timeout timer if it's active and we are
		 * considering a different host than we were last time through.  If
		 * we've already succeeded, though, needn't recalculate.
		 */
		if (flag != PGRES_POLLING_OK &&
			timeout > 0 &&
			(conn->whichhost != last_whichhost ||
			 conn->whichaddr != last_whichaddr))
		{
			end_time = PQgetCurrentTimeUSec() + (pg_usec_time_t) timeout * 1000000;
			last_whichhost = conn->whichhost;
			last_whichaddr = conn->whichaddr;
		}

		/*
		 * Wait, if necessary.  Note that the initial state (just after
		 * PQconnectStart) is to wait for the socket to select for writing.
		 */
		switch (flag)
		{
			case PGRES_POLLING_OK:
				return 1;		/* success! */

			case PGRES_POLLING_READING:
				ret = pqWaitTimed(1, 0, conn, end_time);
				if (ret == -1)
				{
					/* hard failure, eg select() problem, aborts everything */
					conn->status = CONNECTION_BAD;
					return 0;
				}
				break;

			case PGRES_POLLING_WRITING:
				ret = pqWaitTimed(0, 1, conn, end_time);
				if (ret == -1)
				{
					/* hard failure, eg select() problem, aborts everything */
					conn->status = CONNECTION_BAD;
					return 0;
				}
				break;

			default:
				/* Just in case we failed to set it in PQconnectPoll */
				conn->status = CONNECTION_BAD;
				return 0;
		}

		if (ret == 1)			/* connect_timeout elapsed */
		{
			/*
			 * Give up on current server/address, try the next one.
			 */
			conn->try_next_addr = true;
			conn->status = CONNECTION_NEEDED;
		}

		/*
		 * Now try to advance the state machine.
		 */
		if (conn->cancelRequest)
			flag = PQcancelPoll((PGcancelConn *) conn);
		else
			flag = PQconnectPoll(conn);
	}
}

/* ----------------
 *		PQconnectPoll
 *
 * Poll an asynchronous connection.
 *
 * Returns a PostgresPollingStatusType.
 * Before calling this function, use select(2) to determine when data
 * has arrived..
 *
 * You must call PQfinish whether or not this fails.
 *
 * This function and PQconnectStart are intended to allow connections to be
 * made without blocking the execution of your program on remote I/O. However,
 * there are a number of caveats:
 *
 *	 o	If you call PQtrace, ensure that the stream object into which you trace
 *		will not block.
 *	 o	If you do not supply an IP address for the remote host (i.e. you
 *		supply a host name instead) then PQconnectStart will block on
 *		getaddrinfo.  You will be fine if using Unix sockets (i.e. by
 *		supplying neither a host name nor a host address).
 *	 o	If your backend wants to use Kerberos authentication then you must
 *		supply both a host name and a host address, otherwise this function
 *		may block on gethostname.
 *
 * ----------------
 */
PostgresPollingStatusType
PQconnectPoll(PGconn *conn)
{
	bool		reset_connection_state_machine = false;
	bool		need_new_connection = false;
	PGresult   *res;
	char		sebuf[PG_STRERROR_R_BUFLEN];
	int			optval;

	if (conn == NULL)
		return PGRES_POLLING_FAILED;

	/* Get the new data */
	switch (conn->status)
	{
			/*
			 * We really shouldn't have been polled in these two cases, but we
			 * can handle it.
			 */
		case CONNECTION_BAD:
			return PGRES_POLLING_FAILED;
		case CONNECTION_OK:
			return PGRES_POLLING_OK;

			/* These are reading states */
		case CONNECTION_AWAITING_RESPONSE:
		case CONNECTION_AUTH_OK:
		case CONNECTION_CHECK_WRITABLE:
		case CONNECTION_CONSUME:
		case CONNECTION_CHECK_STANDBY:
			{
				/* Load waiting data */
				int			n = pqReadData(conn);

				if (n < 0)
					goto error_return;
				if (n == 0)
					return PGRES_POLLING_READING;

				break;
			}

			/* These are writing states, so we just proceed. */
		case CONNECTION_STARTED:
		case CONNECTION_MADE:
			break;

			/* Special cases: proceed without waiting. */
		case CONNECTION_SSL_STARTUP:
		case CONNECTION_NEEDED:
		case CONNECTION_GSS_STARTUP:
		case CONNECTION_CHECK_TARGET:
			break;

		default:
			libpq_append_conn_error(conn, "invalid connection state, probably indicative of memory corruption");
			goto error_return;
	}


keep_going:						/* We will come back to here until there is
								 * nothing left to do. */

	/* Time to advance to next address, or next host if no more addresses? */
	if (conn->try_next_addr)
	{
		if (conn->whichaddr < conn->naddr)
		{
			conn->whichaddr++;
			reset_connection_state_machine = true;
		}
		else
			conn->try_next_host = true;
		conn->try_next_addr = false;
	}

	/* Time to advance to next connhost[] entry? */
	if (conn->try_next_host)
	{
		pg_conn_host *ch;
		struct addrinfo hint;
		struct addrinfo *addrlist;
		int			thisport;
		int			ret;
		char		portstr[MAXPGPATH];

		if (conn->whichhost + 1 < conn->nconnhost)
			conn->whichhost++;
		else
		{
			/*
			 * Oops, no more hosts.
			 *
			 * If we are trying to connect in "prefer-standby" mode, then drop
			 * the standby requirement and start over. Don't do this for
			 * cancel requests though, since we are certain the list of
			 * servers won't change as the target_server_type option is not
			 * applicable to those connections.
			 *
			 * Otherwise, an appropriate error message is already set up, so
			 * we just need to set the right status.
			 */
			if (conn->target_server_type == SERVER_TYPE_PREFER_STANDBY &&
				conn->nconnhost > 0 &&
				!conn->cancelRequest)
			{
				conn->target_server_type = SERVER_TYPE_PREFER_STANDBY_PASS2;
				conn->whichhost = 0;
			}
			else
				goto error_return;
		}

		/* Drop any address info for previous host */
		release_conn_addrinfo(conn);

		/*
		 * Look up info for the new host.  On failure, log the problem in
		 * conn->errorMessage, then loop around to try the next host.  (Note
		 * we don't clear try_next_host until we've succeeded.)
		 */
		ch = &conn->connhost[conn->whichhost];

		/* Initialize hint structure */
		MemSet(&hint, 0, sizeof(hint));
		hint.ai_socktype = SOCK_STREAM;
		hint.ai_family = AF_UNSPEC;

		/* Figure out the port number we're going to use. */
		if (ch->port == NULL || ch->port[0] == '\0')
			thisport = DEF_PGPORT;
		else
		{
			if (!pqParseIntParam(ch->port, &thisport, conn, "port"))
				goto error_return;

			if (thisport < 1 || thisport > 65535)
			{
				libpq_append_conn_error(conn, "invalid port number: \"%s\"", ch->port);
				goto keep_going;
			}
		}
		snprintf(portstr, sizeof(portstr), "%d", thisport);

		/* Use pg_getaddrinfo_all() to resolve the address */
		switch (ch->type)
		{
			case CHT_HOST_NAME:
				ret = pg_getaddrinfo_all(ch->host, portstr, &hint,
										 &addrlist);
				if (ret || !addrlist)
				{
					libpq_append_conn_error(conn, "could not translate host name \"%s\" to address: %s",
											ch->host, gai_strerror(ret));
					goto keep_going;
				}
				break;

			case CHT_HOST_ADDRESS:
				hint.ai_flags = AI_NUMERICHOST;
				ret = pg_getaddrinfo_all(ch->hostaddr, portstr, &hint,
										 &addrlist);
				if (ret || !addrlist)
				{
					libpq_append_conn_error(conn, "could not parse network address \"%s\": %s",
											ch->hostaddr, gai_strerror(ret));
					goto keep_going;
				}
				break;

			case CHT_UNIX_SOCKET:
				hint.ai_family = AF_UNIX;
				UNIXSOCK_PATH(portstr, thisport, ch->host);
				if (strlen(portstr) >= UNIXSOCK_PATH_BUFLEN)
				{
					libpq_append_conn_error(conn, "Unix-domain socket path \"%s\" is too long (maximum %d bytes)",
											portstr,
											(int) (UNIXSOCK_PATH_BUFLEN - 1));
					goto keep_going;
				}

				/*
				 * NULL hostname tells pg_getaddrinfo_all to parse the service
				 * name as a Unix-domain socket path.
				 */
				ret = pg_getaddrinfo_all(NULL, portstr, &hint,
										 &addrlist);
				if (ret || !addrlist)
				{
					libpq_append_conn_error(conn, "could not translate Unix-domain socket path \"%s\" to address: %s",
											portstr, gai_strerror(ret));
					goto keep_going;
				}
				break;
		}

		/*
		 * Store a copy of the addrlist in private memory so we can perform
		 * randomization for load balancing.
		 */
		ret = store_conn_addrinfo(conn, addrlist);
		pg_freeaddrinfo_all(hint.ai_family, addrlist);
		if (ret)
			goto error_return;	/* message already logged */

		/*
		 * If random load balancing is enabled we shuffle the addresses.
		 */
		if (conn->load_balance_type == LOAD_BALANCE_RANDOM)
		{
			/*
			 * This is the "inside-out" variant of the Fisher-Yates shuffle
			 * algorithm. Notionally, we append each new value to the array
			 * and then swap it with a randomly-chosen array element (possibly
			 * including itself, else we fail to generate permutations with
			 * the last integer last).  The swap step can be optimized by
			 * combining it with the insertion.
			 *
			 * We don't need to initialize conn->prng_state here, because that
			 * already happened in pqConnectOptions2.
			 */
			for (int i = 1; i < conn->naddr; i++)
			{
				int			j = pg_prng_uint64_range(&conn->prng_state, 0, i);
				AddrInfo	temp = conn->addr[j];

				conn->addr[j] = conn->addr[i];
				conn->addr[i] = temp;
			}
		}

		reset_connection_state_machine = true;
		conn->try_next_host = false;
	}

	/* Reset connection state machine? */
	if (reset_connection_state_machine)
	{
		/*
		 * (Re) initialize our connection control variables for a set of
		 * connection attempts to a single server address.  These variables
		 * must persist across individual connection attempts, but we must
		 * reset them when we start to consider a new server.
		 */
		conn->pversion = PG_PROTOCOL(3, 0);
		conn->send_appname = true;
		conn->failed_enc_methods = 0;
		conn->current_enc_method = 0;
		conn->allowed_enc_methods = 0;
		reset_connection_state_machine = false;
		need_new_connection = true;
	}

	/* Force a new connection (perhaps to the same server as before)? */
	if (need_new_connection)
	{
		/* Drop any existing connection */
		pqDropConnection(conn, true);

		/* Reset all state obtained from old server */
		pqDropServerData(conn);

		/* Drop any PGresult we might have, too */
		conn->asyncStatus = PGASYNC_IDLE;
		conn->xactStatus = PQTRANS_IDLE;
		conn->pipelineStatus = PQ_PIPELINE_OFF;
		pqClearAsyncResult(conn);

		/* Reset conn->status to put the state machine in the right state */
		conn->status = CONNECTION_NEEDED;

		need_new_connection = false;
	}

	/*
	 * Decide what to do next, if server rejects SSL or GSS negotiation, but
	 * the connection is still valid.  If there are no options left, error out
	 * with 'msg'.
	 */
#define ENCRYPTION_NEGOTIATION_FAILED(msg) \
	do { \
		switch (encryption_negotiation_failed(conn)) \
		{ \
			case 0: \
				libpq_append_conn_error(conn, (msg)); \
				goto error_return; \
			case 1: \
				conn->status = CONNECTION_MADE; \
				return PGRES_POLLING_WRITING; \
			case 2: \
				need_new_connection = true; \
				goto keep_going; \
		} \
	} while(0);

	/*
	 * Decide what to do next, if connection fails.  If there are no options
	 * left, return with an error.  The error message has already been written
	 * to the connection's error buffer.
	 */
#define CONNECTION_FAILED() \
	do { \
		if (connection_failed(conn)) \
		{ \
			need_new_connection = true; \
			goto keep_going; \
		} \
		else \
			goto error_return; \
	} while(0);

	/* Now try to advance the state machine for this connection */
	switch (conn->status)
	{
		case CONNECTION_NEEDED:
			{
				/*
				 * Try to initiate a connection to one of the addresses
				 * returned by pg_getaddrinfo_all().  conn->whichaddr is the
				 * next one to try.
				 *
				 * The extra level of braces here is historical.  It's not
				 * worth reindenting this whole switch case to remove 'em.
				 */
				{
					char		host_addr[NI_MAXHOST];
					int			sock_type;
					AddrInfo   *addr_cur;

					/*
					 * Advance to next possible host, if we've tried all of
					 * the addresses for the current host.
					 */
					if (conn->whichaddr == conn->naddr)
					{
						conn->try_next_host = true;
						goto keep_going;
					}
					addr_cur = &conn->addr[conn->whichaddr];

					/* Remember current address for possible use later */
					memcpy(&conn->raddr, &addr_cur->addr, sizeof(SockAddr));

#ifdef ENABLE_GSS

					/*
					 * Before establishing the connection, check if it's
					 * doomed to fail because gssencmode='require' but GSSAPI
					 * is not available.
					 */
					if (conn->gssencmode[0] == 'r')
					{
						if (conn->raddr.addr.ss_family == AF_UNIX)
						{
							libpq_append_conn_error(conn,
													"GSSAPI encryption required but it is not supported over a local socket");
							goto error_return;
						}
						if (conn->gcred == GSS_C_NO_CREDENTIAL)
						{
							if (!pg_GSS_have_cred_cache(&conn->gcred))
							{
								libpq_append_conn_error(conn,
														"GSSAPI encryption required but no credential cache");
								goto error_return;
							}
						}
					}
#endif

					/*
					 * Choose the encryption method to try first.  Do this
					 * before establishing the connection, so that if none of
					 * the modes allowed by the connections options are
					 * available, we can error out before establishing the
					 * connection.
					 */
					if (!init_allowed_encryption_methods(conn))
						goto error_return;

					/*
					 * Set connip, too.  Note we purposely ignore strdup
					 * failure; not a big problem if it fails.
					 */
					if (conn->connip != NULL)
					{
						free(conn->connip);
						conn->connip = NULL;
					}
					getHostaddr(conn, host_addr, NI_MAXHOST);
					if (host_addr[0])
						conn->connip = strdup(host_addr);

					/* Try to create the socket */
					sock_type = SOCK_STREAM;
#ifdef SOCK_CLOEXEC

					/*
					 * Atomically mark close-on-exec, if possible on this
					 * platform, so that there isn't a window where a
					 * subprogram executed by another thread inherits the
					 * socket.  See fallback code below.
					 */
					sock_type |= SOCK_CLOEXEC;
#endif
#ifdef SOCK_NONBLOCK

					/*
					 * We might as well skip a system call for nonblocking
					 * mode too, if we can.
					 */
					sock_type |= SOCK_NONBLOCK;
#endif
					conn->sock = socket(addr_cur->family, sock_type, 0);
					if (conn->sock == PGINVALID_SOCKET)
					{
						int			errorno = SOCK_ERRNO;

						/*
						 * Silently ignore socket() failure if we have more
						 * addresses to try; this reduces useless chatter in
						 * cases where the address list includes both IPv4 and
						 * IPv6 but kernel only accepts one family.
						 */
						if (conn->whichaddr < conn->naddr ||
							conn->whichhost + 1 < conn->nconnhost)
						{
							conn->try_next_addr = true;
							goto keep_going;
						}
						emitHostIdentityInfo(conn, host_addr);
						libpq_append_conn_error(conn, "could not create socket: %s",
												SOCK_STRERROR(errorno, sebuf, sizeof(sebuf)));
						goto error_return;
					}

					/*
					 * Once we've identified a target address, all errors
					 * except the preceding socket()-failure case should be
					 * prefixed with host-identity information.  (If the
					 * connection succeeds, the contents of conn->errorMessage
					 * won't matter, so this is harmless.)
					 */
					emitHostIdentityInfo(conn, host_addr);

					/*
					 * Select socket options: no delay of outgoing data for
					 * TCP sockets, nonblock mode, close-on-exec.  Try the
					 * next address if any of this fails.
					 */
					if (addr_cur->family != AF_UNIX)
					{
						if (!connectNoDelay(conn))
						{
							/* error message already created */
							conn->try_next_addr = true;
							goto keep_going;
						}
					}
#ifndef SOCK_NONBLOCK
					if (!pg_set_noblock(conn->sock))
					{
						libpq_append_conn_error(conn, "could not set socket to nonblocking mode: %s",
												SOCK_STRERROR(SOCK_ERRNO, sebuf, sizeof(sebuf)));
						conn->try_next_addr = true;
						goto keep_going;
					}
#endif

#ifndef SOCK_CLOEXEC
#ifdef F_SETFD
					if (fcntl(conn->sock, F_SETFD, FD_CLOEXEC) == -1)
					{
						libpq_append_conn_error(conn, "could not set socket to close-on-exec mode: %s",
												SOCK_STRERROR(SOCK_ERRNO, sebuf, sizeof(sebuf)));
						conn->try_next_addr = true;
						goto keep_going;
					}
#endif							/* F_SETFD */
#endif

					if (addr_cur->family != AF_UNIX)
					{
#ifndef WIN32
						int			on = 1;
#endif
						int			usekeepalives = useKeepalives(conn);
						int			err = 0;

						if (usekeepalives < 0)
						{
							/* error is already reported */
							err = 1;
						}
						else if (usekeepalives == 0)
						{
							/* Do nothing */
						}
#ifndef WIN32
						else if (setsockopt(conn->sock,
											SOL_SOCKET, SO_KEEPALIVE,
											(char *) &on, sizeof(on)) < 0)
						{
							libpq_append_conn_error(conn, "%s(%s) failed: %s",
													"setsockopt",
													"SO_KEEPALIVE",
													SOCK_STRERROR(SOCK_ERRNO, sebuf, sizeof(sebuf)));
							err = 1;
						}
						else if (!setKeepalivesIdle(conn)
								 || !setKeepalivesInterval(conn)
								 || !setKeepalivesCount(conn))
							err = 1;
#else							/* WIN32 */
#ifdef SIO_KEEPALIVE_VALS
						else if (!prepKeepalivesWin32(conn))
							err = 1;
#endif							/* SIO_KEEPALIVE_VALS */
#endif							/* WIN32 */
						else if (!setTCPUserTimeout(conn))
							err = 1;

						if (err)
						{
							conn->try_next_addr = true;
							goto keep_going;
						}
					}

					/*----------
					 * We have three methods of blocking SIGPIPE during
					 * send() calls to this socket:
					 *
					 *	- setsockopt(sock, SO_NOSIGPIPE)
					 *	- send(sock, ..., MSG_NOSIGNAL)
					 *	- setting the signal mask to SIG_IGN during send()
					 *
					 * The third method requires three syscalls per send,
					 * so we prefer either of the first two, but they are
					 * less portable.  The state is tracked in the following
					 * members of PGconn:
					 *
					 * conn->sigpipe_so		- we have set up SO_NOSIGPIPE
					 * conn->sigpipe_flag	- we're specifying MSG_NOSIGNAL
					 *
					 * If we can use SO_NOSIGPIPE, then set sigpipe_so here
					 * and we're done.  Otherwise, set sigpipe_flag so that
					 * we will try MSG_NOSIGNAL on sends.  If we get an error
					 * with MSG_NOSIGNAL, we'll clear that flag and revert to
					 * signal masking.
					 *----------
					 */
					conn->sigpipe_so = false;
#ifdef MSG_NOSIGNAL
					conn->sigpipe_flag = true;
#else
					conn->sigpipe_flag = false;
#endif							/* MSG_NOSIGNAL */

#ifdef SO_NOSIGPIPE
					optval = 1;
					if (setsockopt(conn->sock, SOL_SOCKET, SO_NOSIGPIPE,
								   (char *) &optval, sizeof(optval)) == 0)
					{
						conn->sigpipe_so = true;
						conn->sigpipe_flag = false;
					}
#endif							/* SO_NOSIGPIPE */

					/*
					 * Start/make connection.  This should not block, since we
					 * are in nonblock mode.  If it does, well, too bad.
					 */
					if (connect(conn->sock, (struct sockaddr *) &addr_cur->addr.addr,
								addr_cur->addr.salen) < 0)
					{
						if (SOCK_ERRNO == EINPROGRESS ||
#ifdef WIN32
							SOCK_ERRNO == EWOULDBLOCK ||
#endif
							SOCK_ERRNO == EINTR)
						{
							/*
							 * This is fine - we're in non-blocking mode, and
							 * the connection is in progress.  Tell caller to
							 * wait for write-ready on socket.
							 */
							conn->status = CONNECTION_STARTED;
							return PGRES_POLLING_WRITING;
						}
						/* otherwise, trouble */
					}
					else
					{
						/*
						 * Hm, we're connected already --- seems the "nonblock
						 * connection" wasn't.  Advance the state machine and
						 * go do the next stuff.
						 */
						conn->status = CONNECTION_STARTED;
						goto keep_going;
					}

					/*
					 * This connection failed.  Add the error report to
					 * conn->errorMessage, then try the next address if any.
					 */
					connectFailureMessage(conn, SOCK_ERRNO);
					conn->try_next_addr = true;
					goto keep_going;
				}
			}

		case CONNECTION_STARTED:
			{
				socklen_t	optlen = sizeof(optval);

				/*
				 * Write ready, since we've made it here, so the connection
				 * has been made ... or has failed.
				 */

				/*
				 * Now check (using getsockopt) that there is not an error
				 * state waiting for us on the socket.
				 */

				if (getsockopt(conn->sock, SOL_SOCKET, SO_ERROR,
							   (char *) &optval, &optlen) == -1)
				{
					libpq_append_conn_error(conn, "could not get socket error status: %s",
											SOCK_STRERROR(SOCK_ERRNO, sebuf, sizeof(sebuf)));
					goto error_return;
				}
				else if (optval != 0)
				{
					/*
					 * When using a nonblocking connect, we will typically see
					 * connect failures at this point, so provide a friendly
					 * error message.
					 */
					connectFailureMessage(conn, optval);

					/*
					 * Try the next address if any, just as in the case where
					 * connect() returned failure immediately.
					 */
					conn->try_next_addr = true;
					goto keep_going;
				}

				/* Fill in the client address */
				conn->laddr.salen = sizeof(conn->laddr.addr);
				if (getsockname(conn->sock,
								(struct sockaddr *) &conn->laddr.addr,
								&conn->laddr.salen) < 0)
				{
					libpq_append_conn_error(conn, "could not get client address from socket: %s",
											SOCK_STRERROR(SOCK_ERRNO, sebuf, sizeof(sebuf)));
					goto error_return;
				}

				/*
				 * Implement requirepeer check, if requested and it's a
				 * Unix-domain socket.
				 */
				if (conn->requirepeer && conn->requirepeer[0] &&
					conn->raddr.addr.ss_family == AF_UNIX)
				{
#ifndef WIN32
					char	   *remote_username;
#endif
					uid_t		uid;
					gid_t		gid;

					errno = 0;
					if (getpeereid(conn->sock, &uid, &gid) != 0)
					{
						/*
						 * Provide special error message if getpeereid is a
						 * stub
						 */
						if (errno == ENOSYS)
							libpq_append_conn_error(conn, "requirepeer parameter is not supported on this platform");
						else
							libpq_append_conn_error(conn, "could not get peer credentials: %s",
													strerror_r(errno, sebuf, sizeof(sebuf)));
						goto error_return;
					}

#ifndef WIN32
					remote_username = pg_fe_getusername(uid,
														&conn->errorMessage);
					if (remote_username == NULL)
						goto error_return;	/* message already logged */

					if (strcmp(remote_username, conn->requirepeer) != 0)
					{
						libpq_append_conn_error(conn, "requirepeer specifies \"%s\", but actual peer user name is \"%s\"",
												conn->requirepeer, remote_username);
						free(remote_username);
						goto error_return;
					}
					free(remote_username);
#else							/* WIN32 */
					/* should have failed with ENOSYS above */
					Assert(false);
#endif							/* WIN32 */
				}

				/*
				 * Make sure we can write before advancing to next step.
				 */
				conn->status = CONNECTION_MADE;
				return PGRES_POLLING_WRITING;
			}

		case CONNECTION_MADE:
			{
				char	   *startpacket;
				int			packetlen;

#ifdef ENABLE_GSS

				/*
				 * If GSSAPI encryption is enabled, send a packet to the
				 * server asking for GSSAPI Encryption and proceed with GSSAPI
				 * handshake.  We will come back here after GSSAPI encryption
				 * has been established, with conn->gctx set.
				 */
				if (conn->current_enc_method == ENC_GSSAPI && !conn->gctx)
				{
					ProtocolVersion pv = pg_hton32(NEGOTIATE_GSS_CODE);

					if (pqPacketSend(conn, 0, &pv, sizeof(pv)) != STATUS_OK)
					{
						libpq_append_conn_error(conn, "could not send GSSAPI negotiation packet: %s",
												SOCK_STRERROR(SOCK_ERRNO, sebuf, sizeof(sebuf)));
						goto error_return;
					}

					/* Ok, wait for response */
					conn->status = CONNECTION_GSS_STARTUP;
					return PGRES_POLLING_READING;
				}
#endif

#ifdef USE_SSL

				/*
				 * If SSL is enabled, start the SSL negotiation. We will come
				 * back here after SSL encryption has been established, with
				 * ssl_in_use set.
				 */
				if (conn->current_enc_method == ENC_SSL && !conn->ssl_in_use)
				{
					/*
					 * If traditional postgres SSL negotiation is used, send
					 * the SSL request.  In direct negotiation, jump straight
					 * into the SSL handshake.
					 */
					if (conn->sslnegotiation[0] == 'p')
					{
						ProtocolVersion pv;

						/*
						 * Send the SSL request packet.
						 *
						 * Theoretically, this could block, but it really
						 * shouldn't since we only got here if the socket is
						 * write-ready.
						 */
						pv = pg_hton32(NEGOTIATE_SSL_CODE);
						if (pqPacketSend(conn, 0, &pv, sizeof(pv)) != STATUS_OK)
						{
							libpq_append_conn_error(conn, "could not send SSL negotiation packet: %s",
													SOCK_STRERROR(SOCK_ERRNO, sebuf, sizeof(sebuf)));
							goto error_return;
						}
						/* Ok, wait for response */
						conn->status = CONNECTION_SSL_STARTUP;
						return PGRES_POLLING_READING;
					}
					else
					{
						Assert(conn->sslnegotiation[0] == 'd');
						conn->status = CONNECTION_SSL_STARTUP;
						return PGRES_POLLING_WRITING;
					}
				}
#endif							/* USE_SSL */

				/*
				 * For cancel requests this is as far as we need to go in the
				 * connection establishment. Now we can actually send our
				 * cancellation request.
				 */
				if (conn->cancelRequest)
				{
					CancelRequestPacket cancelpacket;

					packetlen = sizeof(cancelpacket);
					cancelpacket.cancelRequestCode = (MsgType) pg_hton32(CANCEL_REQUEST_CODE);
					cancelpacket.backendPID = pg_hton32(conn->be_pid);
					cancelpacket.cancelAuthCode = pg_hton32(conn->be_key);
					if (pqPacketSend(conn, 0, &cancelpacket, packetlen) != STATUS_OK)
					{
						libpq_append_conn_error(conn, "could not send cancel packet: %s",
												SOCK_STRERROR(SOCK_ERRNO, sebuf, sizeof(sebuf)));
						goto error_return;
					}
					conn->status = CONNECTION_AWAITING_RESPONSE;
					return PGRES_POLLING_READING;
				}

				/*
				 * We have now established encryption, or we are happy to
				 * proceed without.
				 */

				/* Build the startup packet. */
				startpacket = pqBuildStartupPacket3(conn, &packetlen,
													EnvironmentOptions);
				if (!startpacket)
				{
					libpq_append_conn_error(conn, "out of memory");
					goto error_return;
				}

				/*
				 * Send the startup packet.
				 *
				 * Theoretically, this could block, but it really shouldn't
				 * since we only got here if the socket is write-ready.
				 */
				if (pqPacketSend(conn, 0, startpacket, packetlen) != STATUS_OK)
				{
					libpq_append_conn_error(conn, "could not send startup packet: %s",
											SOCK_STRERROR(SOCK_ERRNO, sebuf, sizeof(sebuf)));
					free(startpacket);
					goto error_return;
				}

				free(startpacket);

				conn->status = CONNECTION_AWAITING_RESPONSE;
				return PGRES_POLLING_READING;
			}

			/*
			 * Handle SSL negotiation: wait for postmaster messages and
			 * respond as necessary.
			 */
		case CONNECTION_SSL_STARTUP:
			{
#ifdef USE_SSL
				PostgresPollingStatusType pollres;

				/*
				 * On first time through with traditional SSL negotiation, get
				 * the postmaster's response to our SSLRequest packet. With
				 * sslnegotiation='direct', go straight to initiating SSL.
				 */
				if (!conn->ssl_in_use && conn->sslnegotiation[0] == 'p')
				{
					/*
					 * We use pqReadData here since it has the logic to
					 * distinguish no-data-yet from connection closure. Since
					 * conn->ssl isn't set, a plain recv() will occur.
					 */
					char		SSLok;
					int			rdresult;

					rdresult = pqReadData(conn);
					if (rdresult < 0)
					{
						/* errorMessage is already filled in */
						goto error_return;
					}
					if (rdresult == 0)
					{
						/* caller failed to wait for data */
						return PGRES_POLLING_READING;
					}
					if (pqGetc(&SSLok, conn) < 0)
					{
						/* should not happen really */
						return PGRES_POLLING_READING;
					}
					if (SSLok == 'S')
					{
						if (conn->Pfdebug)
							pqTraceOutputCharResponse(conn, "SSLResponse",
													  SSLok);
						/* mark byte consumed */
						conn->inStart = conn->inCursor;
					}
					else if (SSLok == 'N')
					{
						if (conn->Pfdebug)
							pqTraceOutputCharResponse(conn, "SSLResponse",
													  SSLok);
						/* mark byte consumed */
						conn->inStart = conn->inCursor;

						/*
						 * The connection is still valid, so if it's OK to
						 * continue without SSL, we can proceed using this
						 * connection.  Otherwise return with an error.
						 */
						ENCRYPTION_NEGOTIATION_FAILED(libpq_gettext("server does not support SSL, but SSL was required"));
					}
					else if (SSLok == 'E')
					{
						/*
						 * Server failure of some sort, such as failure to
						 * fork a backend process.  Don't bother retrieving
						 * the error message; we should not trust it as the
						 * server has not been authenticated yet.
						 */
						libpq_append_conn_error(conn, "server sent an error response during SSL exchange");
						goto error_return;
					}
					else
					{
						libpq_append_conn_error(conn, "received invalid response to SSL negotiation: %c",
												SSLok);
						goto error_return;
					}
				}

				/*
				 * Begin or continue the SSL negotiation process.
				 */
				pollres = pqsecure_open_client(conn);
				if (pollres == PGRES_POLLING_OK)
				{
					/*
					 * At this point we should have no data already buffered.
					 * If we do, it was received before we performed the SSL
					 * handshake, so it wasn't encrypted and indeed may have
					 * been injected by a man-in-the-middle.
					 */
					if (conn->inCursor != conn->inEnd)
					{
						libpq_append_conn_error(conn, "received unencrypted data after SSL response");
						goto error_return;
					}

					/* SSL handshake done, ready to send startup packet */
					conn->status = CONNECTION_MADE;
					return PGRES_POLLING_WRITING;
				}
				if (pollres == PGRES_POLLING_FAILED)
				{
					/*
					 * SSL handshake failed.  We will retry with a plaintext
					 * connection, if permitted by sslmode.
					 */
					CONNECTION_FAILED();
				}
				/* Else, return POLLING_READING or POLLING_WRITING status */
				return pollres;
#else							/* !USE_SSL */
				/* can't get here */
				goto error_return;
#endif							/* USE_SSL */
			}

		case CONNECTION_GSS_STARTUP:
			{
#ifdef ENABLE_GSS
				PostgresPollingStatusType pollres;

				/*
				 * If we haven't yet, get the postmaster's response to our
				 * negotiation packet
				 */
				if (!conn->gctx)
				{
					char		gss_ok;
					int			rdresult = pqReadData(conn);

					if (rdresult < 0)
						/* pqReadData fills in error message */
						goto error_return;
					else if (rdresult == 0)
						/* caller failed to wait for data */
						return PGRES_POLLING_READING;
					if (pqGetc(&gss_ok, conn) < 0)
						/* shouldn't happen... */
						return PGRES_POLLING_READING;

					if (gss_ok == 'E')
					{
						/*
						 * Server failure of some sort, possibly protocol
						 * version support failure.  Don't bother retrieving
						 * the error message; we should not trust it anyway as
						 * the server has not authenticated yet.
						 *
						 * Note that unlike on an error response to
						 * SSLRequest, we allow falling back to SSL or
						 * plaintext connection here.  GSS support was
						 * introduced in PostgreSQL version 12, so an error
						 * response might mean that we are connecting to a
						 * pre-v12 server.
						 */
						libpq_append_conn_error(conn, "server sent an error response during GSS encryption exchange");
						CONNECTION_FAILED();
					}

					/* mark byte consumed */
					conn->inStart = conn->inCursor;

					if (gss_ok == 'N')
					{
						if (conn->Pfdebug)
							pqTraceOutputCharResponse(conn, "GSSENCResponse",
													  gss_ok);

						/*
						 * The connection is still valid, so if it's OK to
						 * continue without GSS, we can proceed using this
						 * connection.  Otherwise return with an error.
						 */
						ENCRYPTION_NEGOTIATION_FAILED(libpq_gettext("server doesn't support GSSAPI encryption, but it was required"));
					}
					else if (gss_ok != 'G')
					{
						libpq_append_conn_error(conn, "received invalid response to GSSAPI negotiation: %c",
												gss_ok);
						goto error_return;
					}

					if (conn->Pfdebug)
						pqTraceOutputCharResponse(conn, "GSSENCResponse",
												  gss_ok);
				}

				/* Begin or continue GSSAPI negotiation */
				pollres = pqsecure_open_gss(conn);
				if (pollres == PGRES_POLLING_OK)
				{
					/*
					 * At this point we should have no data already buffered.
					 * If we do, it was received before we performed the GSS
					 * handshake, so it wasn't encrypted and indeed may have
					 * been injected by a man-in-the-middle.
					 */
					if (conn->inCursor != conn->inEnd)
					{
						libpq_append_conn_error(conn, "received unencrypted data after GSSAPI encryption response");
						goto error_return;
					}

					/* All set for startup packet */
					conn->status = CONNECTION_MADE;
					return PGRES_POLLING_WRITING;
				}
				else if (pollres == PGRES_POLLING_FAILED)
				{
					/*
					 * GSS handshake failed.  We will retry with an SSL or
					 * plaintext connection, if permitted by the options.
					 */
					CONNECTION_FAILED();
				}
				/* Else, return POLLING_READING or POLLING_WRITING status */
				return pollres;
#else							/* !ENABLE_GSS */
				/* unreachable */
				goto error_return;
#endif							/* ENABLE_GSS */
			}

			/*
			 * Handle authentication exchange: wait for postmaster messages
			 * and respond as necessary.
			 */
		case CONNECTION_AWAITING_RESPONSE:
			{
				char		beresp;
				int			msgLength;
				int			avail;
				AuthRequest areq;
				int			res;

				/*
				 * Scan the message from current point (note that if we find
				 * the message is incomplete, we will return without advancing
				 * inStart, and resume here next time).
				 */
				conn->inCursor = conn->inStart;

				/* Read type byte */
				if (pqGetc(&beresp, conn))
				{
					/* We'll come back when there is more data */
					return PGRES_POLLING_READING;
				}

				/*
				 * Validate message type: we expect only an authentication
				 * request, NegotiateProtocolVersion, or an error here.
				 * Anything else probably means it's not Postgres on the other
				 * end at all.
				 */
				if (beresp != PqMsg_AuthenticationRequest &&
					beresp != PqMsg_ErrorResponse &&
					beresp != PqMsg_NegotiateProtocolVersion)
				{
					libpq_append_conn_error(conn, "expected authentication request from server, but received %c",
											beresp);
					goto error_return;
				}

				/* Read message length word */
				if (pqGetInt(&msgLength, 4, conn))
				{
					/* We'll come back when there is more data */
					return PGRES_POLLING_READING;
				}

				/*
				 * Try to validate message length before using it.
				 *
				 * Authentication requests can't be very large, although GSS
				 * auth requests may not be that small.  Same for
				 * NegotiateProtocolVersion.
				 *
				 * Errors can be a little larger, but not huge.  If we see a
				 * large apparent length in an error, it means we're really
				 * talking to a pre-3.0-protocol server; cope.  (Before
				 * version 14, the server also used the old protocol for
				 * errors that happened before processing the startup packet.)
				 */
				if (beresp == PqMsg_AuthenticationRequest &&
					(msgLength < 8 || msgLength > 2000))
				{
					libpq_append_conn_error(conn, "received invalid authentication request");
					goto error_return;
				}
				if (beresp == PqMsg_NegotiateProtocolVersion &&
					(msgLength < 8 || msgLength > 2000))
				{
					libpq_append_conn_error(conn, "received invalid protocol negotiation message");
					goto error_return;
				}

#define MAX_ERRLEN 30000
				if (beresp == PqMsg_ErrorResponse &&
					(msgLength < 8 || msgLength > MAX_ERRLEN))
				{
					/* Handle error from a pre-3.0 server */
					conn->inCursor = conn->inStart + 1; /* reread data */
					if (pqGets_append(&conn->errorMessage, conn))
					{
						/*
						 * We may not have authenticated the server yet, so
						 * don't let the buffer grow forever.
						 */
						avail = conn->inEnd - conn->inCursor;
						if (avail > MAX_ERRLEN)
						{
							libpq_append_conn_error(conn, "received invalid error message");
							goto error_return;
						}

						/* We'll come back when there is more data */
						return PGRES_POLLING_READING;
					}
					/* OK, we read the message; mark data consumed */
					pqParseDone(conn, conn->inCursor);

					/*
					 * Before 7.2, the postmaster didn't always end its
					 * messages with a newline, so add one if needed to
					 * conform to libpq conventions.
					 */
					if (conn->errorMessage.len == 0 ||
						conn->errorMessage.data[conn->errorMessage.len - 1] != '\n')
					{
						appendPQExpBufferChar(&conn->errorMessage, '\n');
					}

					goto error_return;
				}
#undef MAX_ERRLEN

				/*
				 * Can't process if message body isn't all here yet.
				 *
				 * After this check passes, any further EOF during parsing
				 * implies that the server sent a bad/truncated message.
				 * Reading more bytes won't help in that case, so don't return
				 * PGRES_POLLING_READING after this point.
				 */
				msgLength -= 4;
				avail = conn->inEnd - conn->inCursor;
				if (avail < msgLength)
				{
					/*
					 * Before returning, try to enlarge the input buffer if
					 * needed to hold the whole message; see notes in
					 * pqParseInput3.
					 */
					if (pqCheckInBufferSpace(conn->inCursor + (size_t) msgLength,
											 conn))
						goto error_return;
					/* We'll come back when there is more data */
					return PGRES_POLLING_READING;
				}

				/* Handle errors. */
				if (beresp == PqMsg_ErrorResponse)
				{
					if (pqGetErrorNotice3(conn, true))
					{
						libpq_append_conn_error(conn, "received invalid error message");
						goto error_return;
					}
					/* OK, we read the message; mark data consumed */
					pqParseDone(conn, conn->inCursor);

					/*
					 * If error is "cannot connect now", try the next host if
					 * any (but we don't want to consider additional addresses
					 * for this host, nor is there much point in changing SSL
					 * or GSS mode).  This is helpful when dealing with
					 * standby servers that might not be in hot-standby state.
					 */
					if (strcmp(conn->last_sqlstate,
							   ERRCODE_CANNOT_CONNECT_NOW) == 0)
					{
						conn->try_next_host = true;
						goto keep_going;
					}

					/* Check to see if we should mention pgpassfile */
					pgpassfileWarning(conn);

					CONNECTION_FAILED();
				}
				else if (beresp == PqMsg_NegotiateProtocolVersion)
				{
					if (pqGetNegotiateProtocolVersion3(conn))
					{
						libpq_append_conn_error(conn, "received invalid protocol negotiation message");
						goto error_return;
					}
					/* OK, we read the message; mark data consumed */
					pqParseDone(conn, conn->inCursor);
					goto error_return;
				}

				/* It is an authentication request. */
				conn->auth_req_received = true;

				/* Get the type of request. */
				if (pqGetInt((int *) &areq, 4, conn))
				{
					/* can't happen because we checked the length already */
					libpq_append_conn_error(conn, "received invalid authentication request");
					goto error_return;
				}
				msgLength -= 4;

				/*
				 * Process the rest of the authentication request message, and
				 * respond to it if necessary.
				 *
				 * Note that conn->pghost must be non-NULL if we are going to
				 * avoid the Kerberos code doing a hostname look-up.
				 */
				res = pg_fe_sendauth(areq, msgLength, conn);

				/*
				 * OK, we have processed the message; mark data consumed.  We
				 * don't call pqParseDone here because we already traced this
				 * message inside pg_fe_sendauth.
				 */
				conn->inStart = conn->inCursor;

				if (res != STATUS_OK)
					goto error_return;

				/*
				 * Just make sure that any data sent by pg_fe_sendauth is
				 * flushed out.  Although this theoretically could block, it
				 * really shouldn't since we don't send large auth responses.
				 */
				if (pqFlush(conn))
					goto error_return;

				if (areq == AUTH_REQ_OK)
				{
					/* We are done with authentication exchange */
					conn->status = CONNECTION_AUTH_OK;

					/*
					 * Set asyncStatus so that PQgetResult will think that
					 * what comes back next is the result of a query.  See
					 * below.
					 */
					conn->asyncStatus = PGASYNC_BUSY;
				}

				/* Look to see if we have more data yet. */
				goto keep_going;
			}

		case CONNECTION_AUTH_OK:
			{
				/*
				 * Now we expect to hear from the backend. A ReadyForQuery
				 * message indicates that startup is successful, but we might
				 * also get an Error message indicating failure. (Notice
				 * messages indicating nonfatal warnings are also allowed by
				 * the protocol, as are ParameterStatus and BackendKeyData
				 * messages.) Easiest way to handle this is to let
				 * PQgetResult() read the messages. We just have to fake it
				 * out about the state of the connection, by setting
				 * asyncStatus = PGASYNC_BUSY (done above).
				 */

				if (PQisBusy(conn))
					return PGRES_POLLING_READING;

				res = PQgetResult(conn);

				/*
				 * NULL return indicating we have gone to IDLE state is
				 * expected
				 */
				if (res)
				{
					if (res->resultStatus != PGRES_FATAL_ERROR)
						libpq_append_conn_error(conn, "unexpected message from server during startup");
					else if (conn->send_appname &&
							 (conn->appname || conn->fbappname))
					{
						/*
						 * If we tried to send application_name, check to see
						 * if the error is about that --- pre-9.0 servers will
						 * reject it at this stage of the process.  If so,
						 * close the connection and retry without sending
						 * application_name.  We could possibly get a false
						 * SQLSTATE match here and retry uselessly, but there
						 * seems no great harm in that; we'll just get the
						 * same error again if it's unrelated.
						 */
						const char *sqlstate;

						sqlstate = PQresultErrorField(res, PG_DIAG_SQLSTATE);
						if (sqlstate &&
							strcmp(sqlstate, ERRCODE_APPNAME_UNKNOWN) == 0)
						{
							PQclear(res);
							conn->send_appname = false;
							need_new_connection = true;
							goto keep_going;
						}
					}

					/*
					 * if the resultStatus is FATAL, then conn->errorMessage
					 * already has a copy of the error; needn't copy it back.
					 * But add a newline if it's not there already, since
					 * postmaster error messages may not have one.
					 */
					if (conn->errorMessage.len <= 0 ||
						conn->errorMessage.data[conn->errorMessage.len - 1] != '\n')
						appendPQExpBufferChar(&conn->errorMessage, '\n');
					PQclear(res);
					goto error_return;
				}

				/* Almost there now ... */
				conn->status = CONNECTION_CHECK_TARGET;
				goto keep_going;
			}

		case CONNECTION_CHECK_TARGET:
			{
				/*
				 * If a read-write, read-only, primary, or standby connection
				 * is required, see if we have one.
				 */
				if (conn->target_server_type == SERVER_TYPE_READ_WRITE ||
					conn->target_server_type == SERVER_TYPE_READ_ONLY)
				{
					bool		read_only_server;

					/*
					 * If the server didn't report
					 * "default_transaction_read_only" or "in_hot_standby" at
					 * startup, we must determine its state by sending the
					 * query "SHOW transaction_read_only".  This GUC exists in
					 * all server versions that support 3.0 protocol.
					 */
					if (conn->default_transaction_read_only == PG_BOOL_UNKNOWN ||
						conn->in_hot_standby == PG_BOOL_UNKNOWN)
					{
						/*
						 * We use PQsendQueryContinue so that
						 * conn->errorMessage does not get cleared.  We need
						 * to preserve any error messages related to previous
						 * hosts we have tried and failed to connect to.
						 */
						conn->status = CONNECTION_OK;
						if (!PQsendQueryContinue(conn,
												 "SHOW transaction_read_only"))
							goto error_return;
						/* We'll return to this state when we have the answer */
						conn->status = CONNECTION_CHECK_WRITABLE;
						return PGRES_POLLING_READING;
					}

					/* OK, we can make the test */
					read_only_server =
						(conn->default_transaction_read_only == PG_BOOL_YES ||
						 conn->in_hot_standby == PG_BOOL_YES);

					if ((conn->target_server_type == SERVER_TYPE_READ_WRITE) ?
						read_only_server : !read_only_server)
					{
						/* Wrong server state, reject and try the next host */
						if (conn->target_server_type == SERVER_TYPE_READ_WRITE)
							libpq_append_conn_error(conn, "session is read-only");
						else
							libpq_append_conn_error(conn, "session is not read-only");

						/* Close connection politely. */
						conn->status = CONNECTION_OK;
						sendTerminateConn(conn);

						/*
						 * Try next host if any, but we don't want to consider
						 * additional addresses for this host.
						 */
						conn->try_next_host = true;
						goto keep_going;
					}
				}
				else if (conn->target_server_type == SERVER_TYPE_PRIMARY ||
						 conn->target_server_type == SERVER_TYPE_STANDBY ||
						 conn->target_server_type == SERVER_TYPE_PREFER_STANDBY)
				{
					/*
					 * If the server didn't report "in_hot_standby" at
					 * startup, we must determine its state by sending the
					 * query "SELECT pg_catalog.pg_is_in_recovery()".  Servers
					 * before 9.0 don't have that function, but by the same
					 * token they don't have any standby mode, so we may just
					 * assume the result.
					 */
					if (conn->sversion < 90000)
						conn->in_hot_standby = PG_BOOL_NO;

					if (conn->in_hot_standby == PG_BOOL_UNKNOWN)
					{
						/*
						 * We use PQsendQueryContinue so that
						 * conn->errorMessage does not get cleared.  We need
						 * to preserve any error messages related to previous
						 * hosts we have tried and failed to connect to.
						 */
						conn->status = CONNECTION_OK;
						if (!PQsendQueryContinue(conn,
												 "SELECT pg_catalog.pg_is_in_recovery()"))
							goto error_return;
						/* We'll return to this state when we have the answer */
						conn->status = CONNECTION_CHECK_STANDBY;
						return PGRES_POLLING_READING;
					}

					/* OK, we can make the test */
					if ((conn->target_server_type == SERVER_TYPE_PRIMARY) ?
						(conn->in_hot_standby == PG_BOOL_YES) :
						(conn->in_hot_standby == PG_BOOL_NO))
					{
						/* Wrong server state, reject and try the next host */
						if (conn->target_server_type == SERVER_TYPE_PRIMARY)
							libpq_append_conn_error(conn, "server is in hot standby mode");
						else
							libpq_append_conn_error(conn, "server is not in hot standby mode");

						/* Close connection politely. */
						conn->status = CONNECTION_OK;
						sendTerminateConn(conn);

						/*
						 * Try next host if any, but we don't want to consider
						 * additional addresses for this host.
						 */
						conn->try_next_host = true;
						goto keep_going;
					}
				}

				/*
				 * For non cancel requests we can release the address list
				 * now. For cancel requests we never actually resolve
				 * addresses and instead the addrinfo exists for the lifetime
				 * of the connection.
				 */
				if (!conn->cancelRequest)
					release_conn_addrinfo(conn);

				/*
				 * Contents of conn->errorMessage are no longer interesting
				 * (and it seems some clients expect it to be empty after a
				 * successful connection).
				 */
				pqClearConnErrorState(conn);

				/* We are open for business! */
				conn->status = CONNECTION_OK;
				return PGRES_POLLING_OK;
			}

		case CONNECTION_CONSUME:
			{
				/*
				 * This state just makes sure the connection is idle after
				 * we've obtained the result of a SHOW or SELECT query.  Once
				 * we're clear, return to CONNECTION_CHECK_TARGET state to
				 * decide what to do next.  We must transiently set status =
				 * CONNECTION_OK in order to use the result-consuming
				 * subroutines.
				 */
				conn->status = CONNECTION_OK;
				if (!PQconsumeInput(conn))
					goto error_return;

				if (PQisBusy(conn))
				{
					conn->status = CONNECTION_CONSUME;
					return PGRES_POLLING_READING;
				}

				/* Call PQgetResult() again until we get a NULL result */
				res = PQgetResult(conn);
				if (res != NULL)
				{
					PQclear(res);
					conn->status = CONNECTION_CONSUME;
					return PGRES_POLLING_READING;
				}

				conn->status = CONNECTION_CHECK_TARGET;
				goto keep_going;
			}

		case CONNECTION_CHECK_WRITABLE:
			{
				/*
				 * Waiting for result of "SHOW transaction_read_only".  We
				 * must transiently set status = CONNECTION_OK in order to use
				 * the result-consuming subroutines.
				 */
				conn->status = CONNECTION_OK;
				if (!PQconsumeInput(conn))
					goto error_return;

				if (PQisBusy(conn))
				{
					conn->status = CONNECTION_CHECK_WRITABLE;
					return PGRES_POLLING_READING;
				}

				res = PQgetResult(conn);
				if (res && PQresultStatus(res) == PGRES_TUPLES_OK &&
					PQntuples(res) == 1)
				{
					char	   *val = PQgetvalue(res, 0, 0);

					/*
					 * "transaction_read_only = on" proves that at least one
					 * of default_transaction_read_only and in_hot_standby is
					 * on, but we don't actually know which.  We don't care
					 * though for the purpose of identifying a read-only
					 * session, so satisfy the CONNECTION_CHECK_TARGET code by
					 * claiming they are both on.  On the other hand, if it's
					 * a read-write session, they are certainly both off.
					 */
					if (strncmp(val, "on", 2) == 0)
					{
						conn->default_transaction_read_only = PG_BOOL_YES;
						conn->in_hot_standby = PG_BOOL_YES;
					}
					else
					{
						conn->default_transaction_read_only = PG_BOOL_NO;
						conn->in_hot_standby = PG_BOOL_NO;
					}
					PQclear(res);

					/* Finish reading messages before continuing */
					conn->status = CONNECTION_CONSUME;
					goto keep_going;
				}

				/* Something went wrong with "SHOW transaction_read_only". */
				PQclear(res);

				/* Append error report to conn->errorMessage. */
				libpq_append_conn_error(conn, "\"%s\" failed",
										"SHOW transaction_read_only");

				/* Close connection politely. */
				conn->status = CONNECTION_OK;
				sendTerminateConn(conn);

				/* Try next host. */
				conn->try_next_host = true;
				goto keep_going;
			}

		case CONNECTION_CHECK_STANDBY:
			{
				/*
				 * Waiting for result of "SELECT pg_is_in_recovery()".  We
				 * must transiently set status = CONNECTION_OK in order to use
				 * the result-consuming subroutines.
				 */
				conn->status = CONNECTION_OK;
				if (!PQconsumeInput(conn))
					goto error_return;

				if (PQisBusy(conn))
				{
					conn->status = CONNECTION_CHECK_STANDBY;
					return PGRES_POLLING_READING;
				}

				res = PQgetResult(conn);
				if (res && PQresultStatus(res) == PGRES_TUPLES_OK &&
					PQntuples(res) == 1)
				{
					char	   *val = PQgetvalue(res, 0, 0);

					if (strncmp(val, "t", 1) == 0)
						conn->in_hot_standby = PG_BOOL_YES;
					else
						conn->in_hot_standby = PG_BOOL_NO;
					PQclear(res);

					/* Finish reading messages before continuing */
					conn->status = CONNECTION_CONSUME;
					goto keep_going;
				}

				/* Something went wrong with "SELECT pg_is_in_recovery()". */
				PQclear(res);

				/* Append error report to conn->errorMessage. */
				libpq_append_conn_error(conn, "\"%s\" failed",
										"SELECT pg_is_in_recovery()");

				/* Close connection politely. */
				conn->status = CONNECTION_OK;
				sendTerminateConn(conn);

				/* Try next host. */
				conn->try_next_host = true;
				goto keep_going;
			}

		default:
			libpq_append_conn_error(conn,
									"invalid connection state %d, probably indicative of memory corruption",
									conn->status);
			goto error_return;
	}

	/* Unreachable */

error_return:

	/*
	 * We used to close the socket at this point, but that makes it awkward
	 * for those above us if they wish to remove this socket from their own
	 * records (an fd_set for example).  We'll just have this socket closed
	 * when PQfinish is called (which is compulsory even after an error, since
	 * the connection structure must be freed).
	 */
	conn->status = CONNECTION_BAD;
	return PGRES_POLLING_FAILED;
}

/*
 * Initialize the state machine for negotiating encryption
 */
static bool
init_allowed_encryption_methods(PGconn *conn)
{
	if (conn->raddr.addr.ss_family == AF_UNIX)
	{
		/* Don't request SSL or GSSAPI over Unix sockets */
		conn->allowed_enc_methods &= ~(ENC_SSL | ENC_GSSAPI);

		/*
		 * XXX: we probably should not do this. sslmode=require works
		 * differently
		 */
		if (conn->gssencmode[0] == 'r')
		{
			libpq_append_conn_error(conn,
									"GSSAPI encryption required but it is not supported over a local socket");
			conn->allowed_enc_methods = 0;
			conn->current_enc_method = ENC_ERROR;
			return false;
		}

		conn->allowed_enc_methods = ENC_PLAINTEXT;
		conn->current_enc_method = ENC_PLAINTEXT;
		return true;
	}

	/* initialize based on sslmode and gssencmode */
	conn->allowed_enc_methods = 0;

#ifdef USE_SSL
	/* sslmode anything but 'disable', and GSSAPI not required */
	if (conn->sslmode[0] != 'd' && conn->gssencmode[0] != 'r')
	{
		conn->allowed_enc_methods |= ENC_SSL;
	}
#endif

#ifdef ENABLE_GSS
	if (conn->gssencmode[0] != 'd')
		conn->allowed_enc_methods |= ENC_GSSAPI;
#endif

	if ((conn->sslmode[0] == 'd' || conn->sslmode[0] == 'p' || conn->sslmode[0] == 'a') &&
		(conn->gssencmode[0] == 'd' || conn->gssencmode[0] == 'p'))
	{
		conn->allowed_enc_methods |= ENC_PLAINTEXT;
	}

	return select_next_encryption_method(conn, false);
}

/*
 * Out-of-line portion of the ENCRYPTION_NEGOTIATION_FAILED() macro in the
 * PQconnectPoll state machine.
 *
 * Return value:
 *  0: connection failed and we are out of encryption methods to try. return an error
 *  1: Retry with next connection method. The TCP connection is still valid and in
 *     known state, so we can proceed with the negotiating next method without
 *     reconnecting.
 *  2: Disconnect, and retry with next connection method.
 *
 * conn->current_enc_method is updated to the next method to try.
 */
#if defined(USE_SSL) || defined(ENABLE_GSS)
static int
encryption_negotiation_failed(PGconn *conn)
{
	Assert((conn->failed_enc_methods & conn->current_enc_method) == 0);
	conn->failed_enc_methods |= conn->current_enc_method;

	if (select_next_encryption_method(conn, true))
	{
		/* An existing connection cannot be reused for direct SSL */
		if (conn->current_enc_method == ENC_SSL && conn->sslnegotiation[0] == 'd')
			return 2;
		else
			return 1;
	}
	else
		return 0;
}
#endif

/*
 * Out-of-line portion of the CONNECTION_FAILED() macro
 *
 * Returns true, if we should reconnect and retry with a different encryption
 * method.  conn->current_enc_method is updated to the next method to try.
 */
static bool
connection_failed(PGconn *conn)
{
	Assert((conn->failed_enc_methods & conn->current_enc_method) == 0);
	conn->failed_enc_methods |= conn->current_enc_method;

	return select_next_encryption_method(conn, false);
}

/*
 * Choose the next encryption method to try. If this is a retry,
 * conn->failed_enc_methods has already been updated. The function sets
 * conn->current_enc_method to the next method to try. Returns false if no
 * encryption methods remain.
 */
static bool
select_next_encryption_method(PGconn *conn, bool have_valid_connection)
{
	int			remaining_methods;

#define SELECT_NEXT_METHOD(method) \
	do { \
		if ((remaining_methods & method) != 0) \
		{ \
			conn->current_enc_method = method; \
			return true; \
		} \
	} while (false)

	remaining_methods = conn->allowed_enc_methods & ~conn->failed_enc_methods;

	/*
	 * Try GSSAPI before SSL
	 */
#ifdef ENABLE_GSS
	if ((remaining_methods & ENC_GSSAPI) != 0)
	{
		/*
		 * If GSSAPI encryption is enabled, then call pg_GSS_have_cred_cache()
		 * which will return true if we can acquire credentials (and give us a
		 * handle to use in conn->gcred), and then send a packet to the server
		 * asking for GSSAPI Encryption (and skip past SSL negotiation and
		 * regular startup below).
		 */
		if (!conn->gctx)
		{
			if (!pg_GSS_have_cred_cache(&conn->gcred))
			{
				conn->allowed_enc_methods &= ~ENC_GSSAPI;
				remaining_methods &= ~ENC_GSSAPI;

				if (conn->gssencmode[0] == 'r')
				{
					libpq_append_conn_error(conn,
											"GSSAPI encryption required but no credential cache");
				}
			}
		}
	}

	SELECT_NEXT_METHOD(ENC_GSSAPI);
#endif

	/*
	 * The order between SSL encryption and plaintext depends on sslmode. With
	 * sslmode=allow, try plaintext connection before SSL. With
	 * sslmode=prefer, it's the other way round. With other modes, we only try
	 * plaintext or SSL connections so the order they're listed here doesn't
	 * matter.
	 */
	if (conn->sslmode[0] == 'a')
		SELECT_NEXT_METHOD(ENC_PLAINTEXT);

	SELECT_NEXT_METHOD(ENC_SSL);

	if (conn->sslmode[0] != 'a')
		SELECT_NEXT_METHOD(ENC_PLAINTEXT);

	/* No more options */
	conn->current_enc_method = ENC_ERROR;
	return false;
#undef SELECT_NEXT_METHOD
}

/*
 * internal_ping
 *		Determine if a server is running and if we can connect to it.
 *
 * The argument is a connection that's been started, but not completed.
 */
static PGPing
internal_ping(PGconn *conn)
{
	/* Say "no attempt" if we never got to PQconnectPoll */
	if (!conn || !conn->options_valid)
		return PQPING_NO_ATTEMPT;

	/* Attempt to complete the connection */
	if (conn->status != CONNECTION_BAD)
		(void) pqConnectDBComplete(conn);

	/* Definitely OK if we succeeded */
	if (conn->status != CONNECTION_BAD)
		return PQPING_OK;

	/*
	 * Here begins the interesting part of "ping": determine the cause of the
	 * failure in sufficient detail to decide what to return.  We do not want
	 * to report that the server is not up just because we didn't have a valid
	 * password, for example.  In fact, any sort of authentication request
	 * implies the server is up.  (We need this check since the libpq side of
	 * things might have pulled the plug on the connection before getting an
	 * error as such from the postmaster.)
	 */
	if (conn->auth_req_received)
		return PQPING_OK;

	/*
	 * If we failed to get any ERROR response from the postmaster, report
	 * PQPING_NO_RESPONSE.  This result could be somewhat misleading for a
	 * pre-7.4 server, since it won't send back a SQLSTATE, but those are long
	 * out of support.  Another corner case where the server could return a
	 * failure without a SQLSTATE is fork failure, but PQPING_NO_RESPONSE
	 * isn't totally unreasonable for that anyway.  We expect that every other
	 * failure case in a modern server will produce a report with a SQLSTATE.
	 *
	 * NOTE: whenever we get around to making libpq generate SQLSTATEs for
	 * client-side errors, we should either not store those into
	 * last_sqlstate, or add an extra flag so we can tell client-side errors
	 * apart from server-side ones.
	 */
	if (strlen(conn->last_sqlstate) != 5)
		return PQPING_NO_RESPONSE;

	/*
	 * Report PQPING_REJECT if server says it's not accepting connections.
	 */
	if (strcmp(conn->last_sqlstate, ERRCODE_CANNOT_CONNECT_NOW) == 0)
		return PQPING_REJECT;

	/*
	 * Any other SQLSTATE can be taken to indicate that the server is up.
	 * Presumably it didn't like our username, password, or database name; or
	 * perhaps it had some transient failure, but that should not be taken as
	 * meaning "it's down".
	 */
	return PQPING_OK;
}


/*
 * pqMakeEmptyPGconn
 *	 - create a PGconn data structure with (as yet) no interesting data
 */
PGconn *
pqMakeEmptyPGconn(void)
{
	PGconn	   *conn;

#ifdef WIN32

	/*
	 * Make sure socket support is up and running in this process.
	 *
	 * Note: the Windows documentation says that we should eventually do a
	 * matching WSACleanup() call, but experience suggests that that is at
	 * least as likely to cause problems as fix them.  So we don't.
	 */
	static bool wsastartup_done = false;

	if (!wsastartup_done)
	{
		WSADATA		wsaData;

		if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
			return NULL;
		wsastartup_done = true;
	}

	/* Forget any earlier error */
	WSASetLastError(0);
#endif							/* WIN32 */

	conn = (PGconn *) malloc(sizeof(PGconn));
	if (conn == NULL)
		return conn;

	/* Zero all pointers and booleans */
	MemSet(conn, 0, sizeof(PGconn));

	/* install default notice hooks */
	conn->noticeHooks.noticeRec = defaultNoticeReceiver;
	conn->noticeHooks.noticeProc = defaultNoticeProcessor;

	conn->status = CONNECTION_BAD;
	conn->asyncStatus = PGASYNC_IDLE;
	conn->pipelineStatus = PQ_PIPELINE_OFF;
	conn->xactStatus = PQTRANS_IDLE;
	conn->options_valid = false;
	conn->nonblocking = false;
	conn->client_encoding = PG_SQL_ASCII;
	conn->std_strings = false;	/* unless server says differently */
	conn->default_transaction_read_only = PG_BOOL_UNKNOWN;
	conn->in_hot_standby = PG_BOOL_UNKNOWN;
	conn->scram_sha_256_iterations = SCRAM_SHA_256_DEFAULT_ITERATIONS;
	conn->verbosity = PQERRORS_DEFAULT;
	conn->show_context = PQSHOW_CONTEXT_ERRORS;
	conn->sock = PGINVALID_SOCKET;
	conn->Pfdebug = NULL;

	/*
	 * We try to send at least 8K at a time, which is the usual size of pipe
	 * buffers on Unix systems.  That way, when we are sending a large amount
	 * of data, we avoid incurring extra kernel context swaps for partial
	 * bufferloads.  The output buffer is initially made 16K in size, and we
	 * try to dump it after accumulating 8K.
	 *
	 * With the same goal of minimizing context swaps, the input buffer will
	 * be enlarged anytime it has less than 8K free, so we initially allocate
	 * twice that.
	 */
	conn->inBufSize = 16 * 1024;
	conn->inBuffer = (char *) malloc(conn->inBufSize);
	conn->outBufSize = 16 * 1024;
	conn->outBuffer = (char *) malloc(conn->outBufSize);
	conn->rowBufLen = 32;
	conn->rowBuf = (PGdataValue *) malloc(conn->rowBufLen * sizeof(PGdataValue));
	initPQExpBuffer(&conn->errorMessage);
	initPQExpBuffer(&conn->workBuffer);

	if (conn->inBuffer == NULL ||
		conn->outBuffer == NULL ||
		conn->rowBuf == NULL ||
		PQExpBufferBroken(&conn->errorMessage) ||
		PQExpBufferBroken(&conn->workBuffer))
	{
		/* out of memory already :-( */
		freePGconn(conn);
		conn = NULL;
	}

	return conn;
}

/*
 * freePGconn
 *	 - free an idle (closed) PGconn data structure
 *
 * NOTE: this should not overlap any functionality with pqClosePGconn().
 * Clearing/resetting of transient state belongs there; what we do here is
 * release data that is to be held for the life of the PGconn structure.
 * If a value ought to be cleared/freed during PQreset(), do it there not here.
 */
static void
freePGconn(PGconn *conn)
{
	/* let any event procs clean up their state data */
	for (int i = 0; i < conn->nEvents; i++)
	{
		PGEventConnDestroy evt;

		evt.conn = conn;
		(void) conn->events[i].proc(PGEVT_CONNDESTROY, &evt,
									conn->events[i].passThrough);
		free(conn->events[i].name);
	}

	release_conn_addrinfo(conn);
	pqReleaseConnHosts(conn);

	free(conn->client_encoding_initial);
	free(conn->events);
	free(conn->pghost);
	free(conn->pghostaddr);
	free(conn->pgport);
	free(conn->connect_timeout);
	free(conn->pgtcp_user_timeout);
	free(conn->pgoptions);
	free(conn->appname);
	free(conn->fbappname);
	free(conn->dbName);
	free(conn->replication);
	free(conn->pguser);
	if (conn->pgpass)
	{
		explicit_bzero(conn->pgpass, strlen(conn->pgpass));
		free(conn->pgpass);
	}
	free(conn->pgpassfile);
	free(conn->channel_binding);
	free(conn->keepalives);
	free(conn->keepalives_idle);
	free(conn->keepalives_interval);
	free(conn->keepalives_count);
	free(conn->sslmode);
	free(conn->sslnegotiation);
	free(conn->sslcert);
	free(conn->sslkey);
	if (conn->sslpassword)
	{
		explicit_bzero(conn->sslpassword, strlen(conn->sslpassword));
		free(conn->sslpassword);
	}
	free(conn->sslcertmode);
	free(conn->sslrootcert);
	free(conn->sslcrl);
	free(conn->sslcrldir);
	free(conn->sslcompression);
	free(conn->sslsni);
	free(conn->requirepeer);
	free(conn->require_auth);
	free(conn->ssl_min_protocol_version);
	free(conn->ssl_max_protocol_version);
	free(conn->gssencmode);
	free(conn->krbsrvname);
	free(conn->gsslib);
	free(conn->gssdelegation);
	free(conn->connip);
	/* Note that conn->Pfdebug is not ours to close or free */
	free(conn->write_err_msg);
	free(conn->inBuffer);
	free(conn->outBuffer);
	free(conn->rowBuf);
	free(conn->target_session_attrs);
	free(conn->load_balance_hosts);
	free(conn->scram_client_key);
	free(conn->scram_server_key);
	termPQExpBuffer(&conn->errorMessage);
	termPQExpBuffer(&conn->workBuffer);

	free(conn);
}

/*
 * pqReleaseConnHosts
 *	 - Free the host list in the PGconn.
 */
void
pqReleaseConnHosts(PGconn *conn)
{
	if (conn->connhost)
	{
		for (int i = 0; i < conn->nconnhost; ++i)
		{
			free(conn->connhost[i].host);
			free(conn->connhost[i].hostaddr);
			free(conn->connhost[i].port);
			if (conn->connhost[i].password != NULL)
			{
				explicit_bzero(conn->connhost[i].password,
							   strlen(conn->connhost[i].password));
				free(conn->connhost[i].password);
			}
		}
		free(conn->connhost);
	}
}

/*
 * store_conn_addrinfo
 *	 - copy addrinfo to PGconn object
 *
 * Copies the addrinfos from addrlist to the PGconn object such that the
 * addrinfos can be manipulated by libpq. Returns a positive integer on
 * failure, otherwise zero.
 */
static int
store_conn_addrinfo(PGconn *conn, struct addrinfo *addrlist)
{
	struct addrinfo *ai = addrlist;

	conn->whichaddr = 0;

	conn->naddr = 0;
	while (ai)
	{
		ai = ai->ai_next;
		conn->naddr++;
	}

	conn->addr = calloc(conn->naddr, sizeof(AddrInfo));
	if (conn->addr == NULL)
	{
		libpq_append_conn_error(conn, "out of memory");
		return 1;
	}

	ai = addrlist;
	for (int i = 0; i < conn->naddr; i++)
	{
		conn->addr[i].family = ai->ai_family;

		memcpy(&conn->addr[i].addr.addr, ai->ai_addr,
			   ai->ai_addrlen);
		conn->addr[i].addr.salen = ai->ai_addrlen;
		ai = ai->ai_next;
	}

	return 0;
}

/*
 * release_conn_addrinfo
 *	 - Free any addrinfo list in the PGconn.
 */
static void
release_conn_addrinfo(PGconn *conn)
{
	if (conn->addr)
	{
		free(conn->addr);
		conn->addr = NULL;
	}
}

/*
 * sendTerminateConn
 *	 - Send a terminate message to backend.
 */
static void
sendTerminateConn(PGconn *conn)
{
	/*
	 * The Postgres cancellation protocol does not have a notion of a
	 * Terminate message, so don't send one.
	 */
	if (conn->cancelRequest)
		return;

	/*
	 * Note that the protocol doesn't allow us to send Terminate messages
	 * during the startup phase.
	 */
	if (conn->sock != PGINVALID_SOCKET && conn->status == CONNECTION_OK)
	{
		/*
		 * Try to send "close connection" message to backend. Ignore any
		 * error.
		 */
		pqPutMsgStart(PqMsg_Terminate, conn);
		pqPutMsgEnd(conn);
		(void) pqFlush(conn);
	}
}

/*
 * pqClosePGconn
 *	 - properly close a connection to the backend
 *
 * This should reset or release all transient state, but NOT the connection
 * parameters.  On exit, the PGconn should be in condition to start a fresh
 * connection with the same parameters (see PQreset()).
 */
void
pqClosePGconn(PGconn *conn)
{
	/*
	 * If possible, send Terminate message to close the connection politely.
	 */
	sendTerminateConn(conn);

	/*
	 * Must reset the blocking status so a possible reconnect will work.
	 *
	 * Don't call PQsetnonblocking() because it will fail if it's unable to
	 * flush the connection.
	 */
	conn->nonblocking = false;

	/*
	 * Close the connection, reset all transient state, flush I/O buffers.
	 * Note that this includes clearing conn's error state; we're no longer
	 * interested in any failures associated with the old connection, and we
	 * want a clean slate for any new connection attempt.
	 */
	pqDropConnection(conn, true);
	conn->status = CONNECTION_BAD;	/* Well, not really _bad_ - just absent */
	conn->asyncStatus = PGASYNC_IDLE;
	conn->xactStatus = PQTRANS_IDLE;
	conn->pipelineStatus = PQ_PIPELINE_OFF;
	pqClearAsyncResult(conn);	/* deallocate result */
	pqClearConnErrorState(conn);

	/*
	 * Release addrinfo, but since cancel requests never change their addrinfo
	 * we don't do that. Otherwise we would have to rebuild it during a
	 * PQcancelReset.
	 */
	if (!conn->cancelRequest)
		release_conn_addrinfo(conn);

	/* Reset all state obtained from server, too */
	pqDropServerData(conn);
}

/*
 * PQfinish: properly close a connection to the backend. Also frees
 * the PGconn data structure so it shouldn't be re-used after this.
 */
void
PQfinish(PGconn *conn)
{
	if (conn)
	{
		pqClosePGconn(conn);
		freePGconn(conn);
	}
}

/*
 * PQreset: resets the connection to the backend by closing the
 * existing connection and creating a new one.
 */
void
PQreset(PGconn *conn)
{
	if (conn)
	{
		pqClosePGconn(conn);

		if (pqConnectDBStart(conn) && pqConnectDBComplete(conn))
		{
			/*
			 * Notify event procs of successful reset.
			 */
			int			i;

			for (i = 0; i < conn->nEvents; i++)
			{
				PGEventConnReset evt;

				evt.conn = conn;
				(void) conn->events[i].proc(PGEVT_CONNRESET, &evt,
											conn->events[i].passThrough);
			}
		}
	}
}


/*
 * PQresetStart:
 * resets the connection to the backend
 * closes the existing connection and makes a new one
 * Returns 1 on success, 0 on failure.
 */
int
PQresetStart(PGconn *conn)
{
	if (conn)
	{
		pqClosePGconn(conn);

		return pqConnectDBStart(conn);
	}

	return 0;
}


/*
 * PQresetPoll:
 * resets the connection to the backend
 * closes the existing connection and makes a new one
 */
PostgresPollingStatusType
PQresetPoll(PGconn *conn)
{
	if (conn)
	{
		PostgresPollingStatusType status = PQconnectPoll(conn);

		if (status == PGRES_POLLING_OK)
		{
			/*
			 * Notify event procs of successful reset.
			 */
			int			i;

			for (i = 0; i < conn->nEvents; i++)
			{
				PGEventConnReset evt;

				evt.conn = conn;
				(void) conn->events[i].proc(PGEVT_CONNRESET, &evt,
											conn->events[i].passThrough);
			}
		}

		return status;
	}

	return PGRES_POLLING_FAILED;
}

/*
 * pqPacketSend() -- convenience routine to send a message to server.
 *
 * pack_type: the single-byte message type code.  (Pass zero for startup
 * packets, which have no message type code.)
 *
 * buf, buf_len: contents of message.  The given length includes only what
 * is in buf; the message type and message length fields are added here.
 *
 * RETURNS: STATUS_ERROR if the write fails, STATUS_OK otherwise.
 * SIDE_EFFECTS: may block.
 */
int
pqPacketSend(PGconn *conn, char pack_type,
			 const void *buf, size_t buf_len)
{
	/* Start the message. */
	if (pqPutMsgStart(pack_type, conn))
		return STATUS_ERROR;

	/* Send the message body. */
	if (pqPutnchar(buf, buf_len, conn))
		return STATUS_ERROR;

	/* Finish the message. */
	if (pqPutMsgEnd(conn))
		return STATUS_ERROR;

	/* Flush to ensure backend gets it. */
	if (pqFlush(conn))
		return STATUS_ERROR;

	return STATUS_OK;
}

#ifdef USE_LDAP

#define LDAP_URL	"ldap://"
#define LDAP_DEF_PORT	389
#define PGLDAP_TIMEOUT 2

#define ld_is_sp_tab(x) ((x) == ' ' || (x) == '\t')
#define ld_is_nl_cr(x) ((x) == '\r' || (x) == '\n')


/*
 *		ldapServiceLookup
 *
 * Search the LDAP URL passed as first argument, treat the result as a
 * string of connection options that are parsed and added to the array of
 * options passed as second argument.
 *
 * LDAP URLs must conform to RFC 1959 without escape sequences.
 *	ldap://host:port/dn?attributes?scope?filter?extensions
 *
 * Returns
 *	0 if the lookup was successful,
 *	1 if the connection to the LDAP server could be established but
 *	  the search was unsuccessful,
 *	2 if a connection could not be established, and
 *	3 if a fatal error occurred.
 *
 * An error message is appended to *errorMessage for return codes 1 and 3.
 */
static int
ldapServiceLookup(const char *purl, PQconninfoOption *options,
				  PQExpBuffer errorMessage)
{
	int			port = LDAP_DEF_PORT,
				scope,
				rc,
				size,
				state,
				oldstate,
				i;
#ifndef WIN32
	int			msgid;
#endif
	bool		found_keyword;
	char	   *url,
			   *hostname,
			   *portstr,
			   *endptr,
			   *dn,
			   *scopestr,
			   *filter,
			   *result,
			   *p,
			   *p1 = NULL,
			   *optname = NULL,
			   *optval = NULL;
	char	   *attrs[2] = {NULL, NULL};
	LDAP	   *ld = NULL;
	LDAPMessage *res,
			   *entry;
	struct berval **values;
	LDAP_TIMEVAL time = {PGLDAP_TIMEOUT, 0};

	if ((url = strdup(purl)) == NULL)
	{
		libpq_append_error(errorMessage, "out of memory");
		return 3;
	}

	/*
	 * Parse URL components, check for correctness.  Basically, url has '\0'
	 * placed at component boundaries and variables are pointed at each
	 * component.
	 */

	if (pg_strncasecmp(url, LDAP_URL, strlen(LDAP_URL)) != 0)
	{
		libpq_append_error(errorMessage,
						   "invalid LDAP URL \"%s\": scheme must be ldap://", purl);
		free(url);
		return 3;
	}

	/* hostname */
	hostname = url + strlen(LDAP_URL);
	if (*hostname == '/')		/* no hostname? */
		hostname = DefaultHost; /* the default */

	/* dn, "distinguished name" */
	p = strchr(url + strlen(LDAP_URL), '/');
	if (p == NULL || *(p + 1) == '\0' || *(p + 1) == '?')
	{
		libpq_append_error(errorMessage,
						   "invalid LDAP URL \"%s\": missing distinguished name",
						   purl);
		free(url);
		return 3;
	}
	*p = '\0';					/* terminate hostname */
	dn = p + 1;

	/* attribute */
	if ((p = strchr(dn, '?')) == NULL || *(p + 1) == '\0' || *(p + 1) == '?')
	{
		libpq_append_error(errorMessage,
						   "invalid LDAP URL \"%s\": must have exactly one attribute",
						   purl);
		free(url);
		return 3;
	}
	*p = '\0';
	attrs[0] = p + 1;

	/* scope */
	if ((p = strchr(attrs[0], '?')) == NULL || *(p + 1) == '\0' || *(p + 1) == '?')
	{
		libpq_append_error(errorMessage,
						   "invalid LDAP URL \"%s\": must have search scope (base/one/sub)",
						   purl);
		free(url);
		return 3;
	}
	*p = '\0';
	scopestr = p + 1;

	/* filter */
	if ((p = strchr(scopestr, '?')) == NULL || *(p + 1) == '\0' || *(p + 1) == '?')
	{
		libpq_append_error(errorMessage,
						   "invalid LDAP URL \"%s\": no filter",
						   purl);
		free(url);
		return 3;
	}
	*p = '\0';
	filter = p + 1;
	if ((p = strchr(filter, '?')) != NULL)
		*p = '\0';

	/* port number? */
	if ((p1 = strchr(hostname, ':')) != NULL)
	{
		long		lport;

		*p1 = '\0';
		portstr = p1 + 1;
		errno = 0;
		lport = strtol(portstr, &endptr, 10);
		if (*portstr == '\0' || *endptr != '\0' || errno || lport < 0 || lport > 65535)
		{
			libpq_append_error(errorMessage,
							   "invalid LDAP URL \"%s\": invalid port number",
							   purl);
			free(url);
			return 3;
		}
		port = (int) lport;
	}

	/* Allow only one attribute */
	if (strchr(attrs[0], ',') != NULL)
	{
		libpq_append_error(errorMessage,
						   "invalid LDAP URL \"%s\": must have exactly one attribute",
						   purl);
		free(url);
		return 3;
	}

	/* set scope */
	if (pg_strcasecmp(scopestr, "base") == 0)
		scope = LDAP_SCOPE_BASE;
	else if (pg_strcasecmp(scopestr, "one") == 0)
		scope = LDAP_SCOPE_ONELEVEL;
	else if (pg_strcasecmp(scopestr, "sub") == 0)
		scope = LDAP_SCOPE_SUBTREE;
	else
	{
		libpq_append_error(errorMessage,
						   "invalid LDAP URL \"%s\": must have search scope (base/one/sub)",
						   purl);
		free(url);
		return 3;
	}

	/* initialize LDAP structure */
	if ((ld = ldap_init(hostname, port)) == NULL)
	{
		libpq_append_error(errorMessage, "could not create LDAP structure");
		free(url);
		return 3;
	}

	/*
	 * Perform an explicit anonymous bind.
	 *
	 * LDAP does not require that an anonymous bind is performed explicitly,
	 * but we want to distinguish between the case where LDAP bind does not
	 * succeed within PGLDAP_TIMEOUT seconds (return 2 to continue parsing the
	 * service control file) and the case where querying the LDAP server fails
	 * (return 1 to end parsing).
	 *
	 * Unfortunately there is no way of setting a timeout that works for both
	 * Windows and OpenLDAP.
	 */
#ifdef WIN32
	/* the nonstandard ldap_connect function performs an anonymous bind */
	if (ldap_connect(ld, &time) != LDAP_SUCCESS)
	{
		/* error or timeout in ldap_connect */
		free(url);
		ldap_unbind(ld);
		return 2;
	}
#else							/* !WIN32 */
	/* in OpenLDAP, use the LDAP_OPT_NETWORK_TIMEOUT option */
	if (ldap_set_option(ld, LDAP_OPT_NETWORK_TIMEOUT, &time) != LDAP_SUCCESS)
	{
		free(url);
		ldap_unbind(ld);
		return 3;
	}

	/* anonymous bind */
	if ((msgid = ldap_simple_bind(ld, NULL, NULL)) == -1)
	{
		/* error or network timeout */
		free(url);
		ldap_unbind(ld);
		return 2;
	}

	/* wait some time for the connection to succeed */
	res = NULL;
	if ((rc = ldap_result(ld, msgid, LDAP_MSG_ALL, &time, &res)) == -1 ||
		res == NULL)
	{
		/* error or timeout */
		if (res != NULL)
			ldap_msgfree(res);
		free(url);
		ldap_unbind(ld);
		return 2;
	}
	ldap_msgfree(res);

	/* reset timeout */
	time.tv_sec = -1;
	if (ldap_set_option(ld, LDAP_OPT_NETWORK_TIMEOUT, &time) != LDAP_SUCCESS)
	{
		free(url);
		ldap_unbind(ld);
		return 3;
	}
#endif							/* WIN32 */

	/* search */
	res = NULL;
	if ((rc = ldap_search_st(ld, dn, scope, filter, attrs, 0, &time, &res))
		!= LDAP_SUCCESS)
	{
		if (res != NULL)
			ldap_msgfree(res);
		libpq_append_error(errorMessage, "lookup on LDAP server failed: %s", ldap_err2string(rc));
		ldap_unbind(ld);
		free(url);
		return 1;
	}

	/* complain if there was not exactly one result */
	if ((rc = ldap_count_entries(ld, res)) != 1)
	{
		if (rc > 1)
			libpq_append_error(errorMessage, "more than one entry found on LDAP lookup");
		else
			libpq_append_error(errorMessage, "no entry found on LDAP lookup");
		ldap_msgfree(res);
		ldap_unbind(ld);
		free(url);
		return 1;
	}

	/* get entry */
	if ((entry = ldap_first_entry(ld, res)) == NULL)
	{
		/* should never happen */
		libpq_append_error(errorMessage, "no entry found on LDAP lookup");
		ldap_msgfree(res);
		ldap_unbind(ld);
		free(url);
		return 1;
	}

	/* get values */
	if ((values = ldap_get_values_len(ld, entry, attrs[0])) == NULL)
	{
		libpq_append_error(errorMessage, "attribute has no values on LDAP lookup");
		ldap_msgfree(res);
		ldap_unbind(ld);
		free(url);
		return 1;
	}

	ldap_msgfree(res);
	free(url);

	if (values[0] == NULL)
	{
		libpq_append_error(errorMessage, "attribute has no values on LDAP lookup");
		ldap_value_free_len(values);
		ldap_unbind(ld);
		return 1;
	}

	/* concatenate values into a single string with newline terminators */
	size = 1;					/* for the trailing null */
	for (i = 0; values[i] != NULL; i++)
		size += values[i]->bv_len + 1;
	if ((result = malloc(size)) == NULL)
	{
		libpq_append_error(errorMessage, "out of memory");
		ldap_value_free_len(values);
		ldap_unbind(ld);
		return 3;
	}
	p = result;
	for (i = 0; values[i] != NULL; i++)
	{
		memcpy(p, values[i]->bv_val, values[i]->bv_len);
		p += values[i]->bv_len;
		*(p++) = '\n';
	}
	*p = '\0';

	ldap_value_free_len(values);
	ldap_unbind(ld);

	/* parse result string */
	oldstate = state = 0;
	for (p = result; *p != '\0'; ++p)
	{
		switch (state)
		{
			case 0:				/* between entries */
				if (!ld_is_sp_tab(*p) && !ld_is_nl_cr(*p))
				{
					optname = p;
					state = 1;
				}
				break;
			case 1:				/* in option name */
				if (ld_is_sp_tab(*p))
				{
					*p = '\0';
					state = 2;
				}
				else if (ld_is_nl_cr(*p))
				{
					libpq_append_error(errorMessage,
									   "missing \"=\" after \"%s\" in connection info string",
									   optname);
					free(result);
					return 3;
				}
				else if (*p == '=')
				{
					*p = '\0';
					state = 3;
				}
				break;
			case 2:				/* after option name */
				if (*p == '=')
				{
					state = 3;
				}
				else if (!ld_is_sp_tab(*p))
				{
					libpq_append_error(errorMessage,
									   "missing \"=\" after \"%s\" in connection info string",
									   optname);
					free(result);
					return 3;
				}
				break;
			case 3:				/* before option value */
				if (*p == '\'')
				{
					optval = p + 1;
					p1 = p + 1;
					state = 5;
				}
				else if (ld_is_nl_cr(*p))
				{
					optval = optname + strlen(optname); /* empty */
					state = 0;
				}
				else if (!ld_is_sp_tab(*p))
				{
					optval = p;
					state = 4;
				}
				break;
			case 4:				/* in unquoted option value */
				if (ld_is_sp_tab(*p) || ld_is_nl_cr(*p))
				{
					*p = '\0';
					state = 0;
				}
				break;
			case 5:				/* in quoted option value */
				if (*p == '\'')
				{
					*p1 = '\0';
					state = 0;
				}
				else if (*p == '\\')
					state = 6;
				else
					*(p1++) = *p;
				break;
			case 6:				/* in quoted option value after escape */
				*(p1++) = *p;
				state = 5;
				break;
		}

		if (state == 0 && oldstate != 0)
		{
			found_keyword = false;
			for (i = 0; options[i].keyword; i++)
			{
				if (strcmp(options[i].keyword, optname) == 0)
				{
					if (options[i].val == NULL)
					{
						options[i].val = strdup(optval);
						if (!options[i].val)
						{
							libpq_append_error(errorMessage, "out of memory");
							free(result);
							return 3;
						}
					}
					found_keyword = true;
					break;
				}
			}
			if (!found_keyword)
			{
				libpq_append_error(errorMessage, "invalid connection option \"%s\"", optname);
				free(result);
				return 1;
			}
			optname = NULL;
			optval = NULL;
		}
		oldstate = state;
	}

	free(result);

	if (state == 5 || state == 6)
	{
		libpq_append_error(errorMessage,
						   "unterminated quoted string in connection info string");
		return 3;
	}

	return 0;
}

#endif							/* USE_LDAP */

/*
 * parseServiceInfo: if a service name has been given, look it up and absorb
 * connection options from it into *options.
 *
 * Returns 0 on success, nonzero on failure.  On failure, if errorMessage
 * isn't null, also store an error message there.  (Note: the only reason
 * this function and related ones don't dump core on errorMessage == NULL
 * is the undocumented fact that appendPQExpBuffer does nothing when passed
 * a null PQExpBuffer pointer.)
 */
static int
parseServiceInfo(PQconninfoOption *options, PQExpBuffer errorMessage)
{
	const char *service = conninfo_getval(options, "service");
	char		serviceFile[MAXPGPATH];
	char	   *env;
	bool		group_found = false;
	int			status;
	struct stat stat_buf;

	/*
	 * We have to special-case the environment variable PGSERVICE here, since
	 * this is and should be called before inserting environment defaults for
	 * other connection options.
	 */
	if (service == NULL)
		service = getenv("PGSERVICE");

	/* If no service name given, nothing to do */
	if (service == NULL)
		return 0;

	/*
	 * Try PGSERVICEFILE if specified, else try ~/.pg_service.conf (if that
	 * exists).
	 */
	if ((env = getenv("PGSERVICEFILE")) != NULL)
		strlcpy(serviceFile, env, sizeof(serviceFile));
	else
	{
		char		homedir[MAXPGPATH];

		if (!pqGetHomeDirectory(homedir, sizeof(homedir)))
			goto next_file;
		snprintf(serviceFile, MAXPGPATH, "%s/%s", homedir, ".pg_service.conf");
		if (stat(serviceFile, &stat_buf) != 0)
			goto next_file;
	}

	status = parseServiceFile(serviceFile, service, options, errorMessage, &group_found);
	if (group_found || status != 0)
		return status;

next_file:

	/*
	 * This could be used by any application so we can't use the binary
	 * location to find our config files.
	 */
	snprintf(serviceFile, MAXPGPATH, "%s/pg_service.conf",
			 getenv("PGSYSCONFDIR") ? getenv("PGSYSCONFDIR") : SYSCONFDIR);
	if (stat(serviceFile, &stat_buf) != 0)
		goto last_file;

	status = parseServiceFile(serviceFile, service, options, errorMessage, &group_found);
	if (status != 0)
		return status;

last_file:
	if (!group_found)
	{
		libpq_append_error(errorMessage, "definition of service \"%s\" not found", service);
		return 3;
	}

	return 0;
}

static int
parseServiceFile(const char *serviceFile,
				 const char *service,
				 PQconninfoOption *options,
				 PQExpBuffer errorMessage,
				 bool *group_found)
{
	int			result = 0,
				linenr = 0,
				i;
	FILE	   *f;
	char	   *line;
	char		buf[1024];

	*group_found = false;

	f = fopen(serviceFile, "r");
	if (f == NULL)
	{
		libpq_append_error(errorMessage, "service file \"%s\" not found", serviceFile);
		return 1;
	}

	while ((line = fgets(buf, sizeof(buf), f)) != NULL)
	{
		int			len;

		linenr++;

		if (strlen(line) >= sizeof(buf) - 1)
		{
			libpq_append_error(errorMessage,
							   "line %d too long in service file \"%s\"",
							   linenr,
							   serviceFile);
			result = 2;
			goto exit;
		}

		/* ignore whitespace at end of line, especially the newline */
		len = strlen(line);
		while (len > 0 && isspace((unsigned char) line[len - 1]))
			line[--len] = '\0';

		/* ignore leading whitespace too */
		while (*line && isspace((unsigned char) line[0]))
			line++;

		/* ignore comments and empty lines */
		if (line[0] == '\0' || line[0] == '#')
			continue;

		/* Check for right groupname */
		if (line[0] == '[')
		{
			if (*group_found)
			{
				/* end of desired group reached; return success */
				goto exit;
			}

			if (strncmp(line + 1, service, strlen(service)) == 0 &&
				line[strlen(service) + 1] == ']')
				*group_found = true;
			else
				*group_found = false;
		}
		else
		{
			if (*group_found)
			{
				/*
				 * Finally, we are in the right group and can parse the line
				 */
				char	   *key,
						   *val;
				bool		found_keyword;

#ifdef USE_LDAP
				if (strncmp(line, "ldap", 4) == 0)
				{
					int			rc = ldapServiceLookup(line, options, errorMessage);

					/* if rc = 2, go on reading for fallback */
					switch (rc)
					{
						case 0:
							goto exit;
						case 1:
						case 3:
							result = 3;
							goto exit;
						case 2:
							continue;
					}
				}
#endif

				key = line;
				val = strchr(line, '=');
				if (val == NULL)
				{
					libpq_append_error(errorMessage,
									   "syntax error in service file \"%s\", line %d",
									   serviceFile,
									   linenr);
					result = 3;
					goto exit;
				}
				*val++ = '\0';

				if (strcmp(key, "service") == 0)
				{
					libpq_append_error(errorMessage,
									   "nested service specifications not supported in service file \"%s\", line %d",
									   serviceFile,
									   linenr);
					result = 3;
					goto exit;
				}

				/*
				 * Set the parameter --- but don't override any previous
				 * explicit setting.
				 */
				found_keyword = false;
				for (i = 0; options[i].keyword; i++)
				{
					if (strcmp(options[i].keyword, key) == 0)
					{
						if (options[i].val == NULL)
							options[i].val = strdup(val);
						if (!options[i].val)
						{
							libpq_append_error(errorMessage, "out of memory");
							result = 3;
							goto exit;
						}
						found_keyword = true;
						break;
					}
				}

				if (!found_keyword)
				{
					libpq_append_error(errorMessage,
									   "syntax error in service file \"%s\", line %d",
									   serviceFile,
									   linenr);
					result = 3;
					goto exit;
				}
			}
		}
	}

exit:
	fclose(f);

	return result;
}


/*
 *		PQconninfoParse
 *
 * Parse a string like PQconnectdb() would do and return the
 * resulting connection options array.  NULL is returned on failure.
 * The result contains only options specified directly in the string,
 * not any possible default values.
 *
 * If errmsg isn't NULL, *errmsg is set to NULL on success, or a malloc'd
 * string on failure (use PQfreemem to free it).  In out-of-memory conditions
 * both *errmsg and the result could be NULL.
 *
 * NOTE: the returned array is dynamically allocated and should
 * be freed when no longer needed via PQconninfoFree().
 */
PQconninfoOption *
PQconninfoParse(const char *conninfo, char **errmsg)
{
	PQExpBufferData errorBuf;
	PQconninfoOption *connOptions;

	if (errmsg)
		*errmsg = NULL;			/* default */
	initPQExpBuffer(&errorBuf);
	if (PQExpBufferDataBroken(errorBuf))
		return NULL;			/* out of memory already :-( */
	connOptions = parse_connection_string(conninfo, &errorBuf, false);
	if (connOptions == NULL && errmsg)
		*errmsg = errorBuf.data;
	else
		termPQExpBuffer(&errorBuf);
	return connOptions;
}

/*
 * Build a working copy of the constant PQconninfoOptions array.
 */
static PQconninfoOption *
conninfo_init(PQExpBuffer errorMessage)
{
	PQconninfoOption *options;
	PQconninfoOption *opt_dest;
	const internalPQconninfoOption *cur_opt;

	/*
	 * Get enough memory for all options in PQconninfoOptions, even if some
	 * end up being filtered out.
	 */
	options = (PQconninfoOption *) malloc(sizeof(PQconninfoOption) * sizeof(PQconninfoOptions) / sizeof(PQconninfoOptions[0]));
	if (options == NULL)
	{
		libpq_append_error(errorMessage, "out of memory");
		return NULL;
	}
	opt_dest = options;

	for (cur_opt = PQconninfoOptions; cur_opt->keyword; cur_opt++)
	{
		/* Only copy the public part of the struct, not the full internal */
		memcpy(opt_dest, cur_opt, sizeof(PQconninfoOption));
		opt_dest++;
	}
	MemSet(opt_dest, 0, sizeof(PQconninfoOption));

	return options;
}

/*
 * Connection string parser
 *
 * Returns a malloc'd PQconninfoOption array, if parsing is successful.
 * Otherwise, NULL is returned and an error message is added to errorMessage.
 *
 * If use_defaults is true, default values are filled in (from a service file,
 * environment variables, etc).
 */
static PQconninfoOption *
parse_connection_string(const char *connstr, PQExpBuffer errorMessage,
						bool use_defaults)
{
	/* Parse as URI if connection string matches URI prefix */
	if (uri_prefix_length(connstr) != 0)
		return conninfo_uri_parse(connstr, errorMessage, use_defaults);

	/* Parse as default otherwise */
	return conninfo_parse(connstr, errorMessage, use_defaults);
}

/*
 * Checks if connection string starts with either of the valid URI prefix
 * designators.
 *
 * Returns the URI prefix length, 0 if the string doesn't contain a URI prefix.
 *
 * XXX this is duplicated in psql/common.c.
 */
static int
uri_prefix_length(const char *connstr)
{
	if (strncmp(connstr, uri_designator,
				sizeof(uri_designator) - 1) == 0)
		return sizeof(uri_designator) - 1;

	if (strncmp(connstr, short_uri_designator,
				sizeof(short_uri_designator) - 1) == 0)
		return sizeof(short_uri_designator) - 1;

	return 0;
}

/*
 * Recognized connection string either starts with a valid URI prefix or
 * contains a "=" in it.
 *
 * Must be consistent with parse_connection_string: anything for which this
 * returns true should at least look like it's parseable by that routine.
 *
 * XXX this is duplicated in psql/common.c
 */
static bool
recognized_connection_string(const char *connstr)
{
	return uri_prefix_length(connstr) != 0 || strchr(connstr, '=') != NULL;
}

/*
 * Subroutine for parse_connection_string
 *
 * Deal with a string containing key=value pairs.
 */
static PQconninfoOption *
conninfo_parse(const char *conninfo, PQExpBuffer errorMessage,
			   bool use_defaults)
{
	char	   *pname;
	char	   *pval;
	char	   *buf;
	char	   *cp;
	char	   *cp2;
	PQconninfoOption *options;

	/* Make a working copy of PQconninfoOptions */
	options = conninfo_init(errorMessage);
	if (options == NULL)
		return NULL;

	/* Need a modifiable copy of the input string */
	if ((buf = strdup(conninfo)) == NULL)
	{
		libpq_append_error(errorMessage, "out of memory");
		PQconninfoFree(options);
		return NULL;
	}
	cp = buf;

	while (*cp)
	{
		/* Skip blanks before the parameter name */
		if (isspace((unsigned char) *cp))
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
			if (isspace((unsigned char) *cp))
			{
				*cp++ = '\0';
				while (*cp)
				{
					if (!isspace((unsigned char) *cp))
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
			libpq_append_error(errorMessage,
							   "missing \"=\" after \"%s\" in connection info string",
							   pname);
			PQconninfoFree(options);
			free(buf);
			return NULL;
		}
		*cp++ = '\0';

		/* Skip blanks after the '=' */
		while (*cp)
		{
			if (!isspace((unsigned char) *cp))
				break;
			cp++;
		}

		/* Get the parameter value */
		pval = cp;

		if (*cp != '\'')
		{
			cp2 = pval;
			while (*cp)
			{
				if (isspace((unsigned char) *cp))
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
					libpq_append_error(errorMessage, "unterminated quoted string in connection info string");
					PQconninfoFree(options);
					free(buf);
					return NULL;
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

		/*
		 * Now that we have the name and the value, store the record.
		 */
		if (!conninfo_storeval(options, pname, pval, errorMessage, false, false))
		{
			PQconninfoFree(options);
			free(buf);
			return NULL;
		}
	}

	/* Done with the modifiable input string */
	free(buf);

	/*
	 * Add in defaults if the caller wants that.
	 */
	if (use_defaults)
	{
		if (!conninfo_add_defaults(options, errorMessage))
		{
			PQconninfoFree(options);
			return NULL;
		}
	}

	return options;
}

/*
 * Conninfo array parser routine
 *
 * If successful, a malloc'd PQconninfoOption array is returned.
 * If not successful, NULL is returned and an error message is
 * appended to errorMessage.
 * Defaults are supplied (from a service file, environment variables, etc)
 * for unspecified options, but only if use_defaults is true.
 *
 * If expand_dbname is non-zero, and the value passed for the first occurrence
 * of "dbname" keyword is a connection string (as indicated by
 * recognized_connection_string) then parse and process it, overriding any
 * previously processed conflicting keywords. Subsequent keywords will take
 * precedence, however. In-tree programs generally specify expand_dbname=true,
 * so command-line arguments naming a database can use a connection string.
 * Some code acquires arbitrary database names from known-literal sources like
 * PQdb(), PQconninfoParse() and pg_database.datname.  When connecting to such
 * a database, in-tree code first wraps the name in a connection string.
 */
static PQconninfoOption *
conninfo_array_parse(const char *const *keywords, const char *const *values,
					 PQExpBuffer errorMessage, bool use_defaults,
					 int expand_dbname)
{
	PQconninfoOption *options;
	PQconninfoOption *dbname_options = NULL;
	PQconninfoOption *option;
	int			i = 0;

	/*
	 * If expand_dbname is non-zero, check keyword "dbname" to see if val is
	 * actually a recognized connection string.
	 */
	while (expand_dbname && keywords[i])
	{
		const char *pname = keywords[i];
		const char *pvalue = values[i];

		/* first find "dbname" if any */
		if (strcmp(pname, "dbname") == 0 && pvalue)
		{
			/*
			 * If value is a connection string, parse it, but do not use
			 * defaults here -- those get picked up later. We only want to
			 * override for those parameters actually passed.
			 */
			if (recognized_connection_string(pvalue))
			{
				dbname_options = parse_connection_string(pvalue, errorMessage, false);
				if (dbname_options == NULL)
					return NULL;
			}
			break;
		}
		++i;
	}

	/* Make a working copy of PQconninfoOptions */
	options = conninfo_init(errorMessage);
	if (options == NULL)
	{
		PQconninfoFree(dbname_options);
		return NULL;
	}

	/* Parse the keywords/values arrays */
	i = 0;
	while (keywords[i])
	{
		const char *pname = keywords[i];
		const char *pvalue = values[i];

		if (pvalue != NULL && pvalue[0] != '\0')
		{
			/* Search for the param record */
			for (option = options; option->keyword != NULL; option++)
			{
				if (strcmp(option->keyword, pname) == 0)
					break;
			}

			/* Check for invalid connection option */
			if (option->keyword == NULL)
			{
				libpq_append_error(errorMessage, "invalid connection option \"%s\"", pname);
				PQconninfoFree(options);
				PQconninfoFree(dbname_options);
				return NULL;
			}

			/*
			 * If we are on the first dbname parameter, and we have a parsed
			 * connection string, copy those parameters across, overriding any
			 * existing previous settings.
			 */
			if (strcmp(pname, "dbname") == 0 && dbname_options)
			{
				PQconninfoOption *str_option;

				for (str_option = dbname_options; str_option->keyword != NULL; str_option++)
				{
					if (str_option->val != NULL)
					{
						int			k;

						for (k = 0; options[k].keyword; k++)
						{
							if (strcmp(options[k].keyword, str_option->keyword) == 0)
							{
								free(options[k].val);
								options[k].val = strdup(str_option->val);
								if (!options[k].val)
								{
									libpq_append_error(errorMessage, "out of memory");
									PQconninfoFree(options);
									PQconninfoFree(dbname_options);
									return NULL;
								}
								break;
							}
						}
					}
				}

				/*
				 * Forget the parsed connection string, so that any subsequent
				 * dbname parameters will not be expanded.
				 */
				PQconninfoFree(dbname_options);
				dbname_options = NULL;
			}
			else
			{
				/*
				 * Store the value, overriding previous settings
				 */
				free(option->val);
				option->val = strdup(pvalue);
				if (!option->val)
				{
					libpq_append_error(errorMessage, "out of memory");
					PQconninfoFree(options);
					PQconninfoFree(dbname_options);
					return NULL;
				}
			}
		}
		++i;
	}
	PQconninfoFree(dbname_options);

	/*
	 * Add in defaults if the caller wants that.
	 */
	if (use_defaults)
	{
		if (!conninfo_add_defaults(options, errorMessage))
		{
			PQconninfoFree(options);
			return NULL;
		}
	}

	return options;
}

/*
 * Add the default values for any unspecified options to the connection
 * options array.
 *
 * Defaults are obtained from a service file, environment variables, etc.
 *
 * Returns true if successful, otherwise false; errorMessage, if supplied,
 * is filled in upon failure.  Note that failure to locate a default value
 * is not an error condition here --- we just leave the option's value as
 * NULL.
 */
static bool
conninfo_add_defaults(PQconninfoOption *options, PQExpBuffer errorMessage)
{
	PQconninfoOption *option;
	PQconninfoOption *sslmode_default = NULL,
			   *sslrootcert = NULL;
	char	   *tmp;

	/*
	 * If there's a service spec, use it to obtain any not-explicitly-given
	 * parameters.  Ignore error if no error message buffer is passed because
	 * there is no way to pass back the failure message.
	 */
	if (parseServiceInfo(options, errorMessage) != 0 && errorMessage)
		return false;

	/*
	 * Get the fallback resources for parameters not specified in the conninfo
	 * string nor the service.
	 */
	for (option = options; option->keyword != NULL; option++)
	{
		if (strcmp(option->keyword, "sslrootcert") == 0)
			sslrootcert = option;	/* save for later */

		if (option->val != NULL)
			continue;			/* Value was in conninfo or service */

		/*
		 * Try to get the environment variable fallback
		 */
		if (option->envvar != NULL)
		{
			if ((tmp = getenv(option->envvar)) != NULL)
			{
				option->val = strdup(tmp);
				if (!option->val)
				{
					if (errorMessage)
						libpq_append_error(errorMessage, "out of memory");
					return false;
				}
				continue;
			}
		}

		/*
		 * Interpret the deprecated PGREQUIRESSL environment variable.  Per
		 * tradition, translate values starting with "1" to sslmode=require,
		 * and ignore other values.  Given both PGREQUIRESSL=1 and PGSSLMODE,
		 * PGSSLMODE takes precedence; the opposite was true before v9.3.
		 */
		if (strcmp(option->keyword, "sslmode") == 0)
		{
			const char *requiresslenv = getenv("PGREQUIRESSL");

			if (requiresslenv != NULL && requiresslenv[0] == '1')
			{
				option->val = strdup("require");
				if (!option->val)
				{
					if (errorMessage)
						libpq_append_error(errorMessage, "out of memory");
					return false;
				}
				continue;
			}

			/*
			 * sslmode is not specified. Let it be filled in with the compiled
			 * default for now, but if sslrootcert=system, we'll override the
			 * default later before returning.
			 */
			sslmode_default = option;
		}

		/*
		 * No environment variable specified or the variable isn't set - try
		 * compiled-in default
		 */
		if (option->compiled != NULL)
		{
			option->val = strdup(option->compiled);
			if (!option->val)
			{
				if (errorMessage)
					libpq_append_error(errorMessage, "out of memory");
				return false;
			}
			continue;
		}

		/*
		 * Special handling for "user" option.  Note that if pg_fe_getauthname
		 * fails, we just leave the value as NULL; there's no need for this to
		 * be an error condition if the caller provides a user name.  The only
		 * reason we do this now at all is so that callers of PQconndefaults
		 * will see a correct default (barring error, of course).
		 */
		if (strcmp(option->keyword, "user") == 0)
		{
			option->val = pg_fe_getauthname(NULL);
			continue;
		}
	}

	/*
	 * Special handling for sslrootcert=system with no sslmode explicitly
	 * defined. In this case we want to strengthen the default sslmode to
	 * verify-full.
	 */
	if (sslmode_default && sslrootcert)
	{
		if (sslrootcert->val && strcmp(sslrootcert->val, "system") == 0)
		{
			free(sslmode_default->val);

			sslmode_default->val = strdup("verify-full");
			if (!sslmode_default->val)
			{
				if (errorMessage)
					libpq_append_error(errorMessage, "out of memory");
				return false;
			}
		}
	}

	return true;
}

/*
 * Subroutine for parse_connection_string
 *
 * Deal with a URI connection string.
 */
static PQconninfoOption *
conninfo_uri_parse(const char *uri, PQExpBuffer errorMessage,
				   bool use_defaults)
{
	PQconninfoOption *options;

	/* Make a working copy of PQconninfoOptions */
	options = conninfo_init(errorMessage);
	if (options == NULL)
		return NULL;

	if (!conninfo_uri_parse_options(options, uri, errorMessage))
	{
		PQconninfoFree(options);
		return NULL;
	}

	/*
	 * Add in defaults if the caller wants that.
	 */
	if (use_defaults)
	{
		if (!conninfo_add_defaults(options, errorMessage))
		{
			PQconninfoFree(options);
			return NULL;
		}
	}

	return options;
}

/*
 * conninfo_uri_parse_options
 *		Actual URI parser.
 *
 * If successful, returns true while the options array is filled with parsed
 * options from the URI.
 * If not successful, returns false and fills errorMessage accordingly.
 *
 * Parses the connection URI string in 'uri' according to the URI syntax (RFC
 * 3986):
 *
 * postgresql://[user[:password]@][netloc][:port][/dbname][?param1=value1&...]
 *
 * where "netloc" is a hostname, an IPv4 address, or an IPv6 address surrounded
 * by literal square brackets.  As an extension, we also allow multiple
 * netloc[:port] specifications, separated by commas:
 *
 * postgresql://[user[:password]@][netloc][:port][,...][/dbname][?param1=value1&...]
 *
 * Any of the URI parts might use percent-encoding (%xy).
 */
static bool
conninfo_uri_parse_options(PQconninfoOption *options, const char *uri,
						   PQExpBuffer errorMessage)
{
	int			prefix_len;
	char	   *p;
	char	   *buf = NULL;
	char	   *start;
	char		prevchar = '\0';
	char	   *user = NULL;
	char	   *host = NULL;
	bool		retval = false;
	PQExpBufferData hostbuf;
	PQExpBufferData portbuf;

	initPQExpBuffer(&hostbuf);
	initPQExpBuffer(&portbuf);
	if (PQExpBufferDataBroken(hostbuf) || PQExpBufferDataBroken(portbuf))
	{
		libpq_append_error(errorMessage, "out of memory");
		goto cleanup;
	}

	/* need a modifiable copy of the input URI */
	buf = strdup(uri);
	if (buf == NULL)
	{
		libpq_append_error(errorMessage, "out of memory");
		goto cleanup;
	}
	start = buf;

	/* Skip the URI prefix */
	prefix_len = uri_prefix_length(uri);
	if (prefix_len == 0)
	{
		/* Should never happen */
		libpq_append_error(errorMessage,
						   "invalid URI propagated to internal parser routine: \"%s\"",
						   uri);
		goto cleanup;
	}
	start += prefix_len;
	p = start;

	/* Look ahead for possible user credentials designator */
	while (*p && *p != '@' && *p != '/')
		++p;
	if (*p == '@')
	{
		/*
		 * Found username/password designator, so URI should be of the form
		 * "scheme://user[:password]@[netloc]".
		 */
		user = start;

		p = user;
		while (*p != ':' && *p != '@')
			++p;

		/* Save last char and cut off at end of user name */
		prevchar = *p;
		*p = '\0';

		if (*user &&
			!conninfo_storeval(options, "user", user,
							   errorMessage, false, true))
			goto cleanup;

		if (prevchar == ':')
		{
			const char *password = p + 1;

			while (*p != '@')
				++p;
			*p = '\0';

			if (*password &&
				!conninfo_storeval(options, "password", password,
								   errorMessage, false, true))
				goto cleanup;
		}

		/* Advance past end of parsed user name or password token */
		++p;
	}
	else
	{
		/*
		 * No username/password designator found.  Reset to start of URI.
		 */
		p = start;
	}

	/*
	 * There may be multiple netloc[:port] pairs, each separated from the next
	 * by a comma.  When we initially enter this loop, "p" has been
	 * incremented past optional URI credential information at this point and
	 * now points at the "netloc" part of the URI.  On subsequent loop
	 * iterations, "p" has been incremented past the comma separator and now
	 * points at the start of the next "netloc".
	 */
	for (;;)
	{
		/*
		 * Look for IPv6 address.
		 */
		if (*p == '[')
		{
			host = ++p;
			while (*p && *p != ']')
				++p;
			if (!*p)
			{
				libpq_append_error(errorMessage,
								   "end of string reached when looking for matching \"]\" in IPv6 host address in URI: \"%s\"",
								   uri);
				goto cleanup;
			}
			if (p == host)
			{
				libpq_append_error(errorMessage,
								   "IPv6 host address may not be empty in URI: \"%s\"",
								   uri);
				goto cleanup;
			}

			/* Cut off the bracket and advance */
			*(p++) = '\0';

			/*
			 * The address may be followed by a port specifier or a slash or a
			 * query or a separator comma.
			 */
			if (*p && *p != ':' && *p != '/' && *p != '?' && *p != ',')
			{
				libpq_append_error(errorMessage,
								   "unexpected character \"%c\" at position %d in URI (expected \":\" or \"/\"): \"%s\"",
								   *p, (int) (p - buf + 1), uri);
				goto cleanup;
			}
		}
		else
		{
			/* not an IPv6 address: DNS-named or IPv4 netloc */
			host = p;

			/*
			 * Look for port specifier (colon) or end of host specifier
			 * (slash) or query (question mark) or host separator (comma).
			 */
			while (*p && *p != ':' && *p != '/' && *p != '?' && *p != ',')
				++p;
		}

		/* Save the hostname terminator before we null it */
		prevchar = *p;
		*p = '\0';

		appendPQExpBufferStr(&hostbuf, host);

		if (prevchar == ':')
		{
			const char *port = ++p; /* advance past host terminator */

			while (*p && *p != '/' && *p != '?' && *p != ',')
				++p;

			prevchar = *p;
			*p = '\0';

			appendPQExpBufferStr(&portbuf, port);
		}

		if (prevchar != ',')
			break;
		++p;					/* advance past comma separator */
		appendPQExpBufferChar(&hostbuf, ',');
		appendPQExpBufferChar(&portbuf, ',');
	}

	/* Save final values for host and port. */
	if (PQExpBufferDataBroken(hostbuf) || PQExpBufferDataBroken(portbuf))
		goto cleanup;
	if (hostbuf.data[0] &&
		!conninfo_storeval(options, "host", hostbuf.data,
						   errorMessage, false, true))
		goto cleanup;
	if (portbuf.data[0] &&
		!conninfo_storeval(options, "port", portbuf.data,
						   errorMessage, false, true))
		goto cleanup;

	if (prevchar && prevchar != '?')
	{
		const char *dbname = ++p;	/* advance past host terminator */

		/* Look for query parameters */
		while (*p && *p != '?')
			++p;

		prevchar = *p;
		*p = '\0';

		/*
		 * Avoid setting dbname to an empty string, as it forces the default
		 * value (username) and ignores $PGDATABASE, as opposed to not setting
		 * it at all.
		 */
		if (*dbname &&
			!conninfo_storeval(options, "dbname", dbname,
							   errorMessage, false, true))
			goto cleanup;
	}

	if (prevchar)
	{
		++p;					/* advance past terminator */

		if (!conninfo_uri_parse_params(p, options, errorMessage))
			goto cleanup;
	}

	/* everything parsed okay */
	retval = true;

cleanup:
	termPQExpBuffer(&hostbuf);
	termPQExpBuffer(&portbuf);
	free(buf);
	return retval;
}

/*
 * Connection URI parameters parser routine
 *
 * If successful, returns true while connOptions is filled with parsed
 * parameters.  Otherwise, returns false and fills errorMessage appropriately.
 *
 * Destructively modifies 'params' buffer.
 */
static bool
conninfo_uri_parse_params(char *params,
						  PQconninfoOption *connOptions,
						  PQExpBuffer errorMessage)
{
	while (*params)
	{
		char	   *keyword = params;
		char	   *value = NULL;
		char	   *p = params;
		bool		malloced = false;
		int			oldmsglen;

		/*
		 * Scan the params string for '=' and '&', marking the end of keyword
		 * and value respectively.
		 */
		for (;;)
		{
			if (*p == '=')
			{
				/* Was there '=' already? */
				if (value != NULL)
				{
					libpq_append_error(errorMessage,
									   "extra key/value separator \"=\" in URI query parameter: \"%s\"",
									   keyword);
					return false;
				}
				/* Cut off keyword, advance to value */
				*p++ = '\0';
				value = p;
			}
			else if (*p == '&' || *p == '\0')
			{
				/*
				 * If not at the end, cut off value and advance; leave p
				 * pointing to start of the next parameter, if any.
				 */
				if (*p != '\0')
					*p++ = '\0';
				/* Was there '=' at all? */
				if (value == NULL)
				{
					libpq_append_error(errorMessage,
									   "missing key/value separator \"=\" in URI query parameter: \"%s\"",
									   keyword);
					return false;
				}
				/* Got keyword and value, go process them. */
				break;
			}
			else
				++p;			/* Advance over all other bytes. */
		}

		keyword = conninfo_uri_decode(keyword, errorMessage);
		if (keyword == NULL)
		{
			/* conninfo_uri_decode already set an error message */
			return false;
		}
		value = conninfo_uri_decode(value, errorMessage);
		if (value == NULL)
		{
			/* conninfo_uri_decode already set an error message */
			free(keyword);
			return false;
		}
		malloced = true;

		/*
		 * Special keyword handling for improved JDBC compatibility.
		 */
		if (strcmp(keyword, "ssl") == 0 &&
			strcmp(value, "true") == 0)
		{
			free(keyword);
			free(value);
			malloced = false;

			keyword = "sslmode";
			value = "require";
		}

		/*
		 * Store the value if the corresponding option exists; ignore
		 * otherwise.  At this point both keyword and value are not
		 * URI-encoded.
		 */
		oldmsglen = errorMessage->len;
		if (!conninfo_storeval(connOptions, keyword, value,
							   errorMessage, true, false))
		{
			/* Insert generic message if conninfo_storeval didn't give one. */
			if (errorMessage->len == oldmsglen)
				libpq_append_error(errorMessage,
								   "invalid URI query parameter: \"%s\"",
								   keyword);
			/* And fail. */
			if (malloced)
			{
				free(keyword);
				free(value);
			}
			return false;
		}

		if (malloced)
		{
			free(keyword);
			free(value);
		}

		/* Proceed to next key=value pair, if any */
		params = p;
	}

	return true;
}

/*
 * Connection URI decoder routine
 *
 * If successful, returns the malloc'd decoded string.
 * If not successful, returns NULL and fills errorMessage accordingly.
 *
 * The string is decoded by replacing any percent-encoded tokens with
 * corresponding characters, while preserving any non-encoded characters.  A
 * percent-encoded token is a character triplet: a percent sign, followed by a
 * pair of hexadecimal digits (0-9A-F), where lower- and upper-case letters are
 * treated identically.
 */
static char *
conninfo_uri_decode(const char *str, PQExpBuffer errorMessage)
{
	char	   *buf;			/* result */
	char	   *p;				/* output location */
	const char *q = str;		/* input location */

	buf = malloc(strlen(str) + 1);
	if (buf == NULL)
	{
		libpq_append_error(errorMessage, "out of memory");
		return NULL;
	}
	p = buf;

	/* skip leading whitespaces */
	for (const char *s = q; *s == ' '; s++)
	{
		q++;
		continue;
	}

	for (;;)
	{
		if (*q != '%')
		{
			/* if found a whitespace or NUL, the string ends */
			if (*q == ' ' || *q == '\0')
				goto end;

			/* copy character */
			*(p++) = *(q++);
		}
		else
		{
			int			hi;
			int			lo;
			int			c;

			++q;				/* skip the percent sign itself */

			/*
			 * Possible EOL will be caught by the first call to
			 * get_hexdigit(), so we never dereference an invalid q pointer.
			 */
			if (!(get_hexdigit(*q++, &hi) && get_hexdigit(*q++, &lo)))
			{
				libpq_append_error(errorMessage,
								   "invalid percent-encoded token: \"%s\"",
								   str);
				free(buf);
				return NULL;
			}

			c = (hi << 4) | lo;
			if (c == 0)
			{
				libpq_append_error(errorMessage,
								   "forbidden value %%00 in percent-encoded value: \"%s\"",
								   str);
				free(buf);
				return NULL;
			}
			*(p++) = c;
		}
	}

end:

	/* skip trailing whitespaces */
	for (const char *s = q; *s == ' '; s++)
	{
		q++;
		continue;
	}

	/* Not at the end of the string yet?  Fail. */
	if (*q != '\0')
	{
		libpq_append_error(errorMessage,
						   "unexpected spaces found in \"%s\", use percent-encoded spaces (%%20) instead",
						   str);
		free(buf);
		return NULL;
	}

	/* Copy NUL terminator */
	*p = '\0';

	return buf;
}

/*
 * Convert hexadecimal digit character to its integer value.
 *
 * If successful, returns true and value is filled with digit's base 16 value.
 * If not successful, returns false.
 *
 * Lower- and upper-case letters in the range A-F are treated identically.
 */
static bool
get_hexdigit(char digit, int *value)
{
	if ('0' <= digit && digit <= '9')
		*value = digit - '0';
	else if ('A' <= digit && digit <= 'F')
		*value = digit - 'A' + 10;
	else if ('a' <= digit && digit <= 'f')
		*value = digit - 'a' + 10;
	else
		return false;

	return true;
}

/*
 * Find an option value corresponding to the keyword in the connOptions array.
 *
 * If successful, returns a pointer to the corresponding option's value.
 * If not successful, returns NULL.
 */
static const char *
conninfo_getval(PQconninfoOption *connOptions,
				const char *keyword)
{
	PQconninfoOption *option;

	option = conninfo_find(connOptions, keyword);

	return option ? option->val : NULL;
}

/*
 * Store a (new) value for an option corresponding to the keyword in
 * connOptions array.
 *
 * If uri_decode is true, the value is URI-decoded.  The keyword is always
 * assumed to be non URI-encoded.
 *
 * If successful, returns a pointer to the corresponding PQconninfoOption,
 * which value is replaced with a strdup'd copy of the passed value string.
 * The existing value for the option is free'd before replacing, if any.
 *
 * If not successful, returns NULL and fills errorMessage accordingly.
 * However, if the reason of failure is an invalid keyword being passed and
 * ignoreMissing is true, errorMessage will be left untouched.
 */
static PQconninfoOption *
conninfo_storeval(PQconninfoOption *connOptions,
				  const char *keyword, const char *value,
				  PQExpBuffer errorMessage, bool ignoreMissing,
				  bool uri_decode)
{
	PQconninfoOption *option;
	char	   *value_copy;

	/*
	 * For backwards compatibility, requiressl=1 gets translated to
	 * sslmode=require, and requiressl=0 gets translated to sslmode=prefer
	 * (which is the default for sslmode).
	 */
	if (strcmp(keyword, "requiressl") == 0)
	{
		keyword = "sslmode";
		if (value[0] == '1')
			value = "require";
		else
			value = "prefer";
	}

	option = conninfo_find(connOptions, keyword);
	if (option == NULL)
	{
		if (!ignoreMissing)
			libpq_append_error(errorMessage,
							   "invalid connection option \"%s\"",
							   keyword);
		return NULL;
	}

	if (uri_decode)
	{
		value_copy = conninfo_uri_decode(value, errorMessage);
		if (value_copy == NULL)
			/* conninfo_uri_decode already set an error message */
			return NULL;
	}
	else
	{
		value_copy = strdup(value);
		if (value_copy == NULL)
		{
			libpq_append_error(errorMessage, "out of memory");
			return NULL;
		}
	}

	free(option->val);
	option->val = value_copy;

	return option;
}

/*
 * Find a PQconninfoOption option corresponding to the keyword in the
 * connOptions array.
 *
 * If successful, returns a pointer to the corresponding PQconninfoOption
 * structure.
 * If not successful, returns NULL.
 */
static PQconninfoOption *
conninfo_find(PQconninfoOption *connOptions, const char *keyword)
{
	PQconninfoOption *option;

	for (option = connOptions; option->keyword != NULL; option++)
	{
		if (strcmp(option->keyword, keyword) == 0)
			return option;
	}

	return NULL;
}


/*
 * Return the connection options used for the connection
 */
PQconninfoOption *
PQconninfo(PGconn *conn)
{
	PQExpBufferData errorBuf;
	PQconninfoOption *connOptions;

	if (conn == NULL)
		return NULL;

	/*
	 * We don't actually report any errors here, but callees want a buffer,
	 * and we prefer not to trash the conn's errorMessage.
	 */
	initPQExpBuffer(&errorBuf);
	if (PQExpBufferDataBroken(errorBuf))
		return NULL;			/* out of memory already :-( */

	connOptions = conninfo_init(&errorBuf);

	if (connOptions != NULL)
	{
		const internalPQconninfoOption *option;

		for (option = PQconninfoOptions; option->keyword; option++)
		{
			char	  **connmember;

			if (option->connofs < 0)
				continue;

			connmember = (char **) ((char *) conn + option->connofs);

			if (*connmember)
				conninfo_storeval(connOptions, option->keyword, *connmember,
								  &errorBuf, true, false);
		}
	}

	termPQExpBuffer(&errorBuf);

	return connOptions;
}


void
PQconninfoFree(PQconninfoOption *connOptions)
{
	if (connOptions == NULL)
		return;

	for (PQconninfoOption *option = connOptions; option->keyword != NULL; option++)
		free(option->val);
	free(connOptions);
}


/* =========== accessor functions for PGconn ========= */
char *
PQdb(const PGconn *conn)
{
	if (!conn)
		return NULL;
	return conn->dbName;
}

char *
PQservice(const PGconn *conn)
{
	if (!conn)
		return NULL;
	return conn->pgservice;
}

char *
PQuser(const PGconn *conn)
{
	if (!conn)
		return NULL;
	return conn->pguser;
}

char *
PQpass(const PGconn *conn)
{
	char	   *password = NULL;

	if (!conn)
		return NULL;
	if (conn->connhost != NULL)
		password = conn->connhost[conn->whichhost].password;
	if (password == NULL)
		password = conn->pgpass;
	/* Historically we've returned "" not NULL for no password specified */
	if (password == NULL)
		password = "";
	return password;
}

char *
PQhost(const PGconn *conn)
{
	if (!conn)
		return NULL;

	if (conn->connhost != NULL)
	{
		/*
		 * Return the verbatim host value provided by user, or hostaddr in its
		 * lack.
		 */
		if (conn->connhost[conn->whichhost].host != NULL &&
			conn->connhost[conn->whichhost].host[0] != '\0')
			return conn->connhost[conn->whichhost].host;
		else if (conn->connhost[conn->whichhost].hostaddr != NULL &&
				 conn->connhost[conn->whichhost].hostaddr[0] != '\0')
			return conn->connhost[conn->whichhost].hostaddr;
	}

	return "";
}

char *
PQhostaddr(const PGconn *conn)
{
	if (!conn)
		return NULL;

	/* Return the parsed IP address */
	if (conn->connhost != NULL && conn->connip != NULL)
		return conn->connip;

	return "";
}

char *
PQport(const PGconn *conn)
{
	if (!conn)
		return NULL;

	if (conn->connhost != NULL)
		return conn->connhost[conn->whichhost].port;

	return "";
}

/*
 * No longer does anything, but the function remains for API backwards
 * compatibility.
 */
char *
PQtty(const PGconn *conn)
{
	if (!conn)
		return NULL;
	return "";
}

char *
PQoptions(const PGconn *conn)
{
	if (!conn)
		return NULL;
	return conn->pgoptions;
}

ConnStatusType
PQstatus(const PGconn *conn)
{
	if (!conn)
		return CONNECTION_BAD;
	return conn->status;
}

PGTransactionStatusType
PQtransactionStatus(const PGconn *conn)
{
	if (!conn || conn->status != CONNECTION_OK)
		return PQTRANS_UNKNOWN;
	if (conn->asyncStatus != PGASYNC_IDLE)
		return PQTRANS_ACTIVE;
	return conn->xactStatus;
}

const char *
PQparameterStatus(const PGconn *conn, const char *paramName)
{
	const pgParameterStatus *pstatus;

	if (!conn || !paramName)
		return NULL;
	for (pstatus = conn->pstatus; pstatus != NULL; pstatus = pstatus->next)
	{
		if (strcmp(pstatus->name, paramName) == 0)
			return pstatus->value;
	}
	return NULL;
}

int
PQprotocolVersion(const PGconn *conn)
{
	if (!conn)
		return 0;
	if (conn->status == CONNECTION_BAD)
		return 0;
	return PG_PROTOCOL_MAJOR(conn->pversion);
}

int
PQfullProtocolVersion(const PGconn *conn)
{
	if (!conn)
		return 0;
	if (conn->status == CONNECTION_BAD)
		return 0;
	return PG_PROTOCOL_FULL(conn->pversion);
}

int
PQserverVersion(const PGconn *conn)
{
	if (!conn)
		return 0;
	if (conn->status == CONNECTION_BAD)
		return 0;
	return conn->sversion;
}

char *
PQerrorMessage(const PGconn *conn)
{
	if (!conn)
		return libpq_gettext("connection pointer is NULL\n");

	/*
	 * The errorMessage buffer might be marked "broken" due to having
	 * previously failed to allocate enough memory for the message.  In that
	 * case, tell the application we ran out of memory.
	 */
	if (PQExpBufferBroken(&conn->errorMessage))
		return libpq_gettext("out of memory\n");

	return conn->errorMessage.data;
}

/*
 * In Windows, socket values are unsigned, and an invalid socket value
 * (INVALID_SOCKET) is ~0, which equals -1 in comparisons (with no compiler
 * warning). Ideally we would return an unsigned value for PQsocket() on
 * Windows, but that would cause the function's return value to differ from
 * Unix, so we just return -1 for invalid sockets.
 * http://msdn.microsoft.com/en-us/library/windows/desktop/cc507522%28v=vs.85%29.aspx
 * http://stackoverflow.com/questions/10817252/why-is-invalid-socket-defined-as-0-in-winsock2-h-c
 */
int
PQsocket(const PGconn *conn)
{
	if (!conn)
		return -1;
	return (conn->sock != PGINVALID_SOCKET) ? conn->sock : -1;
}

int
PQbackendPID(const PGconn *conn)
{
	if (!conn || conn->status != CONNECTION_OK)
		return 0;
	return conn->be_pid;
}

PGpipelineStatus
PQpipelineStatus(const PGconn *conn)
{
	if (!conn)
		return PQ_PIPELINE_OFF;

	return conn->pipelineStatus;
}

int
PQconnectionNeedsPassword(const PGconn *conn)
{
	char	   *password;

	if (!conn)
		return false;
	password = PQpass(conn);
	if (conn->password_needed &&
		(password == NULL || password[0] == '\0'))
		return true;
	else
		return false;
}

int
PQconnectionUsedPassword(const PGconn *conn)
{
	if (!conn)
		return false;
	if (conn->password_needed)
		return true;
	else
		return false;
}

int
PQconnectionUsedGSSAPI(const PGconn *conn)
{
	if (!conn)
		return false;
	if (conn->gssapi_used)
		return true;
	else
		return false;
}

int
PQclientEncoding(const PGconn *conn)
{
	if (!conn || conn->status != CONNECTION_OK)
		return -1;
	return conn->client_encoding;
}

int
PQsetClientEncoding(PGconn *conn, const char *encoding)
{
	char		qbuf[128];
	static const char query[] = "set client_encoding to '%s'";
	PGresult   *res;
	int			status;

	if (!conn || conn->status != CONNECTION_OK)
		return -1;

	if (!encoding)
		return -1;

	/* Resolve special "auto" value from the locale */
	if (strcmp(encoding, "auto") == 0)
		encoding = pg_encoding_to_char(pg_get_encoding_from_locale(NULL, true));

	/* check query buffer overflow */
	if (sizeof(qbuf) < (sizeof(query) + strlen(encoding)))
		return -1;

	/* ok, now send a query */
	sprintf(qbuf, query, encoding);
	res = PQexec(conn, qbuf);

	if (res == NULL)
		return -1;
	if (res->resultStatus != PGRES_COMMAND_OK)
		status = -1;
	else
	{
		/*
		 * We rely on the backend to report the parameter value, and we'll
		 * change state at that time.
		 */
		status = 0;				/* everything is ok */
	}
	PQclear(res);
	return status;
}

PGVerbosity
PQsetErrorVerbosity(PGconn *conn, PGVerbosity verbosity)
{
	PGVerbosity old;

	if (!conn)
		return PQERRORS_DEFAULT;
	old = conn->verbosity;
	conn->verbosity = verbosity;
	return old;
}

PGContextVisibility
PQsetErrorContextVisibility(PGconn *conn, PGContextVisibility show_context)
{
	PGContextVisibility old;

	if (!conn)
		return PQSHOW_CONTEXT_ERRORS;
	old = conn->show_context;
	conn->show_context = show_context;
	return old;
}

PQnoticeReceiver
PQsetNoticeReceiver(PGconn *conn, PQnoticeReceiver proc, void *arg)
{
	PQnoticeReceiver old;

	if (conn == NULL)
		return NULL;

	old = conn->noticeHooks.noticeRec;
	if (proc)
	{
		conn->noticeHooks.noticeRec = proc;
		conn->noticeHooks.noticeRecArg = arg;
	}
	return old;
}

PQnoticeProcessor
PQsetNoticeProcessor(PGconn *conn, PQnoticeProcessor proc, void *arg)
{
	PQnoticeProcessor old;

	if (conn == NULL)
		return NULL;

	old = conn->noticeHooks.noticeProc;
	if (proc)
	{
		conn->noticeHooks.noticeProc = proc;
		conn->noticeHooks.noticeProcArg = arg;
	}
	return old;
}

/*
 * The default notice message receiver just gets the standard notice text
 * and sends it to the notice processor.  This two-level setup exists
 * mostly for backwards compatibility; perhaps we should deprecate use of
 * PQsetNoticeProcessor?
 */
static void
defaultNoticeReceiver(void *arg, const PGresult *res)
{
	(void) arg;					/* not used */
	if (res->noticeHooks.noticeProc != NULL)
		res->noticeHooks.noticeProc(res->noticeHooks.noticeProcArg,
									PQresultErrorMessage(res));
}

/*
 * The default notice message processor just prints the
 * message on stderr.  Applications can override this if they
 * want the messages to go elsewhere (a window, for example).
 * Note that simply discarding notices is probably a bad idea.
 */
static void
defaultNoticeProcessor(void *arg, const char *message)
{
	(void) arg;					/* not used */
	/* Note: we expect the supplied string to end with a newline already. */
	fprintf(stderr, "%s", message);
}

/*
 * returns a pointer to the next token or NULL if the current
 * token doesn't match
 */
static char *
pwdfMatchesString(char *buf, const char *token)
{
	char	   *tbuf;
	const char *ttok;
	bool		bslash = false;

	if (buf == NULL || token == NULL)
		return NULL;
	tbuf = buf;
	ttok = token;
	if (tbuf[0] == '*' && tbuf[1] == ':')
		return tbuf + 2;
	while (*tbuf != 0)
	{
		if (*tbuf == '\\' && !bslash)
		{
			tbuf++;
			bslash = true;
		}
		if (*tbuf == ':' && *ttok == 0 && !bslash)
			return tbuf + 1;
		bslash = false;
		if (*ttok == 0)
			return NULL;
		if (*tbuf == *ttok)
		{
			tbuf++;
			ttok++;
		}
		else
			return NULL;
	}
	return NULL;
}

/* Get a password from the password file. Return value is malloc'd. */
static char *
passwordFromFile(const char *hostname, const char *port, const char *dbname,
				 const char *username, const char *pgpassfile)
{
	FILE	   *fp;
#ifndef WIN32
	struct stat stat_buf;
#endif
	PQExpBufferData buf;

	if (dbname == NULL || dbname[0] == '\0')
		return NULL;

	if (username == NULL || username[0] == '\0')
		return NULL;

	/* 'localhost' matches pghost of '' or the default socket directory */
	if (hostname == NULL || hostname[0] == '\0')
		hostname = DefaultHost;
	else if (is_unixsock_path(hostname))

		/*
		 * We should probably use canonicalize_path(), but then we have to
		 * bring path.c into libpq, and it doesn't seem worth it.
		 */
		if (strcmp(hostname, DEFAULT_PGSOCKET_DIR) == 0)
			hostname = DefaultHost;

	if (port == NULL || port[0] == '\0')
		port = DEF_PGPORT_STR;

	/* If password file cannot be opened, ignore it. */
	fp = fopen(pgpassfile, "r");
	if (fp == NULL)
		return NULL;

#ifndef WIN32
	if (fstat(fileno(fp), &stat_buf) != 0)
	{
		fclose(fp);
		return NULL;
	}

	if (!S_ISREG(stat_buf.st_mode))
	{
		fprintf(stderr,
				libpq_gettext("WARNING: password file \"%s\" is not a plain file\n"),
				pgpassfile);
		fclose(fp);
		return NULL;
	}

	/* If password file is insecure, alert the user and ignore it. */
	if (stat_buf.st_mode & (S_IRWXG | S_IRWXO))
	{
		fprintf(stderr,
				libpq_gettext("WARNING: password file \"%s\" has group or world access; permissions should be u=rw (0600) or less\n"),
				pgpassfile);
		fclose(fp);
		return NULL;
	}
#else

	/*
	 * On Win32, the directory is protected, so we don't have to check the
	 * file.
	 */
#endif

	/* Use an expansible buffer to accommodate any reasonable line length */
	initPQExpBuffer(&buf);

	while (!feof(fp) && !ferror(fp))
	{
		/* Make sure there's a reasonable amount of room in the buffer */
		if (!enlargePQExpBuffer(&buf, 128))
			break;

		/* Read some data, appending it to what we already have */
		if (fgets(buf.data + buf.len, buf.maxlen - buf.len, fp) == NULL)
			break;
		buf.len += strlen(buf.data + buf.len);

		/* If we don't yet have a whole line, loop around to read more */
		if (!(buf.len > 0 && buf.data[buf.len - 1] == '\n') && !feof(fp))
			continue;

		/* ignore comments */
		if (buf.data[0] != '#')
		{
			char	   *t = buf.data;
			int			len;

			/* strip trailing newline and carriage return */
			len = pg_strip_crlf(t);

			if (len > 0 &&
				(t = pwdfMatchesString(t, hostname)) != NULL &&
				(t = pwdfMatchesString(t, port)) != NULL &&
				(t = pwdfMatchesString(t, dbname)) != NULL &&
				(t = pwdfMatchesString(t, username)) != NULL)
			{
				/* Found a match. */
				char	   *ret,
						   *p1,
						   *p2;

				ret = strdup(t);

				fclose(fp);
				explicit_bzero(buf.data, buf.maxlen);
				termPQExpBuffer(&buf);

				if (!ret)
				{
					/* Out of memory. XXX: an error message would be nice. */
					return NULL;
				}

				/* De-escape password. */
				for (p1 = p2 = ret; *p1 != ':' && *p1 != '\0'; ++p1, ++p2)
				{
					if (*p1 == '\\' && p1[1] != '\0')
						++p1;
					*p2 = *p1;
				}
				*p2 = '\0';

				return ret;
			}
		}

		/* No match, reset buffer to prepare for next line. */
		buf.len = 0;
	}

	fclose(fp);
	explicit_bzero(buf.data, buf.maxlen);
	termPQExpBuffer(&buf);
	return NULL;
}


/*
 *	If the connection failed due to bad password, we should mention
 *	if we got the password from the pgpassfile.
 */
static void
pgpassfileWarning(PGconn *conn)
{
	/* If it was 'invalid authorization', add pgpassfile mention */
	/* only works with >= 9.0 servers */
	if (conn->password_needed &&
		conn->connhost[conn->whichhost].password != NULL &&
		conn->result)
	{
		const char *sqlstate = PQresultErrorField(conn->result,
												  PG_DIAG_SQLSTATE);

		if (sqlstate && strcmp(sqlstate, ERRCODE_INVALID_PASSWORD) == 0)
			libpq_append_conn_error(conn, "password retrieved from file \"%s\"",
									conn->pgpassfile);
	}
}

/*
 * Check if the SSL protocol value given in input is valid or not.
 * This is used as a sanity check routine for the connection parameters
 * ssl_min_protocol_version and ssl_max_protocol_version.
 */
static bool
sslVerifyProtocolVersion(const char *version)
{
	/*
	 * An empty string and a NULL value are considered valid as it is
	 * equivalent to ignoring the parameter.
	 */
	if (!version || strlen(version) == 0)
		return true;

	if (pg_strcasecmp(version, "TLSv1") == 0 ||
		pg_strcasecmp(version, "TLSv1.1") == 0 ||
		pg_strcasecmp(version, "TLSv1.2") == 0 ||
		pg_strcasecmp(version, "TLSv1.3") == 0)
		return true;

	/* anything else is wrong */
	return false;
}


/*
 * Ensure that the SSL protocol range given in input is correct.  The check
 * is performed on the input string to keep it TLS backend agnostic.  Input
 * to this function is expected verified with sslVerifyProtocolVersion().
 */
static bool
sslVerifyProtocolRange(const char *min, const char *max)
{
	Assert(sslVerifyProtocolVersion(min) &&
		   sslVerifyProtocolVersion(max));

	/* If at least one of the bounds is not set, the range is valid */
	if (min == NULL || max == NULL || strlen(min) == 0 || strlen(max) == 0)
		return true;

	/*
	 * If the minimum version is the lowest one we accept, then all options
	 * for the maximum are valid.
	 */
	if (pg_strcasecmp(min, "TLSv1") == 0)
		return true;

	/*
	 * The minimum bound is valid, and cannot be TLSv1, so using TLSv1 for the
	 * maximum is incorrect.
	 */
	if (pg_strcasecmp(max, "TLSv1") == 0)
		return false;

	/*
	 * At this point we know that we have a mix of TLSv1.1 through 1.3
	 * versions.
	 */
	if (pg_strcasecmp(min, max) > 0)
		return false;

	return true;
}


/*
 * Obtain user's home directory, return in given buffer
 *
 * On Unix, this actually returns the user's home directory.  On Windows
 * it returns the PostgreSQL-specific application data folder.
 *
 * This is essentially the same as get_home_path(), but we don't use that
 * because we don't want to pull path.c into libpq (it pollutes application
 * namespace).
 *
 * Returns true on success, false on failure to obtain the directory name.
 *
 * CAUTION: although in most situations failure is unexpected, there are users
 * who like to run applications in a home-directory-less environment.  On
 * failure, you almost certainly DO NOT want to report an error.  Just act as
 * though whatever file you were hoping to find in the home directory isn't
 * there (which it isn't).
 */
bool
pqGetHomeDirectory(char *buf, int bufsize)
{
#ifndef WIN32
	const char *home;

	home = getenv("HOME");
	if (home && home[0])
	{
		strlcpy(buf, home, bufsize);
		return true;
	}
	else
	{
		struct passwd pwbuf;
		struct passwd *pw;
		char		tmpbuf[1024];
		int			rc;

		rc = getpwuid_r(geteuid(), &pwbuf, tmpbuf, sizeof tmpbuf, &pw);
		if (rc != 0 || !pw)
			return false;
		strlcpy(buf, pw->pw_dir, bufsize);
		return true;
	}
#else
	char		tmppath[MAX_PATH];

	ZeroMemory(tmppath, sizeof(tmppath));
	if (SHGetFolderPath(NULL, CSIDL_APPDATA, NULL, 0, tmppath) != S_OK)
		return false;
	snprintf(buf, bufsize, "%s/postgresql", tmppath);
	return true;
#endif
}

/*
 * Parse and try to interpret "value" as an integer value, and if successful,
 * store it in *result, complaining if there is any trailing garbage or an
 * overflow.  This allows any number of leading and trailing whitespaces.
 */
bool
pqParseIntParam(const char *value, int *result, PGconn *conn,
				const char *context)
{
	char	   *end;
	long		numval;

	Assert(value != NULL);

	*result = 0;

	/* strtol(3) skips leading whitespaces */
	errno = 0;
	numval = strtol(value, &end, 10);

	/*
	 * If no progress was done during the parsing or an error happened, fail.
	 * This tests properly for overflows of the result.
	 */
	if (value == end || errno != 0 || numval != (int) numval)
		goto error;

	/*
	 * Skip any trailing whitespace; if anything but whitespace remains before
	 * the terminating character, fail
	 */
	while (*end != '\0' && isspace((unsigned char) *end))
		end++;

	if (*end != '\0')
		goto error;

	*result = numval;
	return true;

error:
	libpq_append_conn_error(conn, "invalid integer value \"%s\" for connection option \"%s\"",
							value, context);
	return false;
}

/*
 * To keep the API consistent, the locking stubs are always provided, even
 * if they are not required.
 *
 * Since we neglected to provide any error-return convention in the
 * pgthreadlock_t API, we can't do much except Assert upon failure of any
 * mutex primitive.  Fortunately, such failures appear to be nonexistent in
 * the field.
 */

static void
default_threadlock(int acquire)
{
	static pthread_mutex_t singlethread_lock = PTHREAD_MUTEX_INITIALIZER;

	if (acquire)
	{
		if (pthread_mutex_lock(&singlethread_lock))
			Assert(false);
	}
	else
	{
		if (pthread_mutex_unlock(&singlethread_lock))
			Assert(false);
	}
}

pgthreadlock_t
PQregisterThreadLock(pgthreadlock_t newhandler)
{
	pgthreadlock_t prev = pg_g_threadlock;

	if (newhandler)
		pg_g_threadlock = newhandler;
	else
		pg_g_threadlock = default_threadlock;

	return prev;
}
