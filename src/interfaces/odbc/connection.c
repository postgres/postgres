/*------
 * Module:			connection.c
 *
 * Description:		This module contains routines related to
 *					connecting to and disconnecting from the Postgres DBMS.
 *
 * Classes:			ConnectionClass (Functions prefix: "CC_")
 *
 * API functions:	SQLAllocConnect, SQLConnect, SQLDisconnect, SQLFreeConnect,
 *					SQLBrowseConnect(NI)
 *
 * Comments:		See "notice.txt" for copyright and license information.
 *-------
 */
/* Multibyte support	Eiji Tokuya 2001-03-15 */

#include "connection.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#ifndef	WIN32
#include <errno.h>
#endif /* WIN32 */

#include "environ.h"
#include "socket.h"
#include "statement.h"
#include "qresult.h"
#include "lobj.h"
#include "dlg_specific.h"

#ifdef MULTIBYTE
#include "multibyte.h"
#endif

#include "pgapifunc.h"
#include "md5.h"

#define STMT_INCREMENT 16		/* how many statement holders to allocate
								 * at a time */

#define PRN_NULLCHECK

extern GLOBAL_VALUES globals;


RETCODE		SQL_API
PGAPI_AllocConnect(
				   HENV henv,
				   HDBC FAR * phdbc)
{
	EnvironmentClass *env = (EnvironmentClass *) henv;
	ConnectionClass *conn;
	static char *func = "PGAPI_AllocConnect";

	mylog("%s: entering...\n", func);

	conn = CC_Constructor();
	mylog("**** %s: henv = %u, conn = %u\n", func, henv, conn);

	if (!conn)
	{
		env->errormsg = "Couldn't allocate memory for Connection object.";
		env->errornumber = ENV_ALLOC_ERROR;
		*phdbc = SQL_NULL_HDBC;
		EN_log_error(func, "", env);
		return SQL_ERROR;
	}

	if (!EN_add_connection(env, conn))
	{
		env->errormsg = "Maximum number of connections exceeded.";
		env->errornumber = ENV_ALLOC_ERROR;
		CC_Destructor(conn);
		*phdbc = SQL_NULL_HDBC;
		EN_log_error(func, "", env);
		return SQL_ERROR;
	}

	if (phdbc)
		*phdbc = (HDBC) conn;

	return SQL_SUCCESS;
}


RETCODE		SQL_API
PGAPI_Connect(
			  HDBC hdbc,
			  UCHAR FAR * szDSN,
			  SWORD cbDSN,
			  UCHAR FAR * szUID,
			  SWORD cbUID,
			  UCHAR FAR * szAuthStr,
			  SWORD cbAuthStr)
{
	ConnectionClass *conn = (ConnectionClass *) hdbc;
	ConnInfo   *ci;
	static char *func = "PGAPI_Connect";

	mylog("%s: entering...\n", func);

	if (!conn)
	{
		CC_log_error(func, "", NULL);
		return SQL_INVALID_HANDLE;
	}

	ci = &conn->connInfo;

	make_string(szDSN, cbDSN, ci->dsn);

	/* get the values for the DSN from the registry */
	getDSNinfo(ci, CONN_OVERWRITE);
	logs_on_off(1, ci->drivers.debug, ci->drivers.commlog);
	/* initialize pg_version from connInfo.protocol    */
	CC_initialize_pg_version(conn);

	/*
	 * override values from DSN info with UID and authStr(pwd) This only
	 * occurs if the values are actually there.
	 */
	make_string(szUID, cbUID, ci->username);
	make_string(szAuthStr, cbAuthStr, ci->password);

	/* fill in any defaults */
	getDSNdefaults(ci);

	qlog("conn = %u, %s(DSN='%s', UID='%s', PWD='%s')\n", conn, func, ci->dsn, ci->username, ci->password);

	if (CC_connect(conn, AUTH_REQ_OK, NULL) <= 0)
	{
		/* Error messages are filled in */
		CC_log_error(func, "Error on CC_connect", conn);
		return SQL_ERROR;
	}

	mylog("%s: returning...\n", func);

	return SQL_SUCCESS;
}


RETCODE		SQL_API
PGAPI_BrowseConnect(
					HDBC hdbc,
					UCHAR FAR * szConnStrIn,
					SWORD cbConnStrIn,
					UCHAR FAR * szConnStrOut,
					SWORD cbConnStrOutMax,
					SWORD FAR * pcbConnStrOut)
{
	static char *func = "PGAPI_BrowseConnect";

	mylog("%s: entering...\n", func);

	return SQL_SUCCESS;
}


/* Drop any hstmts open on hdbc and disconnect from database */
RETCODE		SQL_API
PGAPI_Disconnect(
				 HDBC hdbc)
{
	ConnectionClass *conn = (ConnectionClass *) hdbc;
	static char *func = "PGAPI_Disconnect";


	mylog("%s: entering...\n", func);

	if (!conn)
	{
		CC_log_error(func, "", NULL);
		return SQL_INVALID_HANDLE;
	}

	qlog("conn=%u, %s\n", conn, func);

	if (conn->status == CONN_EXECUTING)
	{
		conn->errornumber = CONN_IN_USE;
		conn->errormsg = "A transaction is currently being executed";
		CC_log_error(func, "", conn);
		return SQL_ERROR;
	}

	logs_on_off(-1, conn->connInfo.drivers.debug, conn->connInfo.drivers.commlog);
	mylog("%s: about to CC_cleanup\n", func);

	/* Close the connection and free statements */
	CC_cleanup(conn);

	mylog("%s: done CC_cleanup\n", func);
	mylog("%s: returning...\n", func);

	return SQL_SUCCESS;
}


RETCODE		SQL_API
PGAPI_FreeConnect(
				  HDBC hdbc)
{
	ConnectionClass *conn = (ConnectionClass *) hdbc;
	static char *func = "PGAPI_FreeConnect";

	mylog("%s: entering...\n", func);
	mylog("**** in %s: hdbc=%u\n", func, hdbc);

	if (!conn)
	{
		CC_log_error(func, "", NULL);
		return SQL_INVALID_HANDLE;
	}

	/* Remove the connection from the environment */
	if (!EN_remove_connection(conn->henv, conn))
	{
		conn->errornumber = CONN_IN_USE;
		conn->errormsg = "A transaction is currently being executed";
		CC_log_error(func, "", conn);
		return SQL_ERROR;
	}

	CC_Destructor(conn);

	mylog("%s: returning...\n", func);

	return SQL_SUCCESS;
}


void
CC_conninfo_init(ConnInfo *conninfo)
{
		memset(conninfo, 0, sizeof(ConnInfo));
		conninfo->disallow_premature = -1;
		conninfo->allow_keyset = -1;
		conninfo->lf_conversion = -1;
		conninfo->true_is_minus1 = -1;
		conninfo->int8_as = -101;
		memcpy(&(conninfo->drivers), &globals, sizeof(globals));
}
/*
 *		IMPLEMENTATION CONNECTION CLASS
 */
ConnectionClass *
CC_Constructor()
{
	ConnectionClass *rv;

	rv = (ConnectionClass *) malloc(sizeof(ConnectionClass));

	if (rv != NULL)
	{
		rv->henv = NULL;		/* not yet associated with an environment */

		rv->errormsg = NULL;
		rv->errornumber = 0;
		rv->errormsg_created = FALSE;

		rv->status = CONN_NOT_CONNECTED;
		rv->transact_status = CONN_IN_AUTOCOMMIT;		/* autocommit by default */

		CC_conninfo_init(&(rv->connInfo));
		rv->sock = SOCK_Constructor(rv);
		if (!rv->sock)
			return NULL;

		rv->stmts = (StatementClass **) malloc(sizeof(StatementClass *) * STMT_INCREMENT);
		if (!rv->stmts)
			return NULL;
		memset(rv->stmts, 0, sizeof(StatementClass *) * STMT_INCREMENT);

		rv->num_stmts = STMT_INCREMENT;

		rv->lobj_type = PG_TYPE_LO;

		rv->ntables = 0;
		rv->col_info = NULL;

		rv->translation_option = 0;
		rv->translation_handle = NULL;
		rv->DataSourceToDriver = NULL;
		rv->DriverToDataSource = NULL;
		rv->driver_version = ODBCVER;
		memset(rv->pg_version, 0, sizeof(rv->pg_version));
		rv->pg_version_number = .0;
		rv->pg_version_major = 0;
		rv->pg_version_minor = 0;
		rv->ms_jet = 0;
		rv->unicode = 0;
		rv->result_uncommitted = 0;
		rv->schema_support = 0;
		rv->isolation = SQL_TXN_READ_COMMITTED;
#ifdef	MULTIBYTE
		rv->client_encoding = NULL;
		rv->server_encoding = NULL;
#endif   /* MULTIBYTE */
		rv->current_schema = NULL;


		/* Initialize statement options to defaults */
		/* Statements under this conn will inherit these options */

		InitializeStatementOptions(&rv->stmtOptions);
		InitializeARDFields(&rv->ardOptions);
		InitializeAPDFields(&rv->apdOptions);
	}
	return rv;
}


char
CC_Destructor(ConnectionClass *self)
{
	mylog("enter CC_Destructor, self=%u\n", self);

	if (self->status == CONN_EXECUTING)
		return 0;

	CC_cleanup(self);			/* cleanup socket and statements */

	mylog("after CC_Cleanup\n");

	/* Free up statement holders */
	if (self->stmts)
	{
		free(self->stmts);
		self->stmts = NULL;
	}
	mylog("after free statement holders\n");

	free(self);

	mylog("exit CC_Destructor\n");

	return 1;
}


/*	Return how many cursors are opened on this connection */
int
CC_cursor_count(ConnectionClass *self)
{
	StatementClass *stmt;
	int			i,
				count = 0;

	mylog("CC_cursor_count: self=%u, num_stmts=%d\n", self, self->num_stmts);

	for (i = 0; i < self->num_stmts; i++)
	{
		stmt = self->stmts[i];
		if (stmt && SC_get_Result(stmt) && SC_get_Result(stmt)->cursor)
			count++;
	}

	mylog("CC_cursor_count: returning %d\n", count);

	return count;
}


void
CC_clear_error(ConnectionClass *self)
{
	self->errornumber = 0;
	self->errormsg = NULL;
	self->errormsg_created = FALSE;
}


/*
 *	Used to begin a transaction.
 */
char
CC_begin(ConnectionClass *self)
{
	char	ret = TRUE;
	if (!CC_is_in_trans(self))
	{
		QResultClass *res = CC_send_query(self, "BEGIN", NULL, CLEAR_RESULT_ON_ABORT);
		mylog("CC_begin:  sending BEGIN!\n");

		if (res != NULL)
		{
			ret = QR_command_maybe_successful(res);
			QR_Destructor(res);
		}
		else
			return FALSE;
	}

	return ret;
}

/*
 *	Used to commit a transaction.
 *	We are almost always in the middle of a transaction.
 */
char
CC_commit(ConnectionClass *self)
{
	char	ret = FALSE;
	if (CC_is_in_trans(self))
	{
		QResultClass *res = CC_send_query(self, "COMMIT", NULL, CLEAR_RESULT_ON_ABORT);
		mylog("CC_commit:  sending COMMIT!\n");
		if (res != NULL)
		{
			ret = QR_command_maybe_successful(res);
			QR_Destructor(res);
		}
		else
			return FALSE;
	}

	return ret;
}

/*
 *	Used to cancel a transaction.
 *	We are almost always in the middle of a transaction.
 */
char
CC_abort(ConnectionClass *self)
{
	if (CC_is_in_trans(self))
	{
		QResultClass *res = CC_send_query(self, "ROLLBACK", NULL, CLEAR_RESULT_ON_ABORT);
		mylog("CC_abort:  sending ABORT!\n");
		if (res != NULL)
			QR_Destructor(res);
		else
			return FALSE;
	}

	return TRUE;
}


/* This is called by SQLDisconnect also */
char
CC_cleanup(ConnectionClass *self)
{
	int			i;
	StatementClass *stmt;

	if (self->status == CONN_EXECUTING)
		return FALSE;

	mylog("in CC_Cleanup, self=%u\n", self);

	/* Cancel an ongoing transaction */
	/* We are always in the middle of a transaction, */
	/* even if we are in auto commit. */
	if (self->sock)
		CC_abort(self);

	mylog("after CC_abort\n");

	/* This actually closes the connection to the dbase */
	if (self->sock)
	{
		SOCK_Destructor(self->sock);
		self->sock = NULL;
	}

	mylog("after SOCK destructor\n");

	/* Free all the stmts on this connection */
	for (i = 0; i < self->num_stmts; i++)
	{
		stmt = self->stmts[i];
		if (stmt)
		{
			stmt->hdbc = NULL;	/* prevent any more dbase interactions */

			SC_Destructor(stmt);

			self->stmts[i] = NULL;
		}
	}

	/* Check for translation dll */
#ifdef WIN32
	if (self->translation_handle)
	{
		FreeLibrary(self->translation_handle);
		self->translation_handle = NULL;
	}
#endif

	self->status = CONN_NOT_CONNECTED;
	self->transact_status = CONN_IN_AUTOCOMMIT;
	CC_conninfo_init(&(self->connInfo));
#ifdef	MULTIBYTE
	if (self->client_encoding)
		free(self->client_encoding);
	self->client_encoding = NULL;
	if (self->server_encoding)
		free(self->server_encoding);
	self->server_encoding = NULL;
#endif   /* MULTIBYTE */
	if (self->current_schema)
		free(self->current_schema);
	self->current_schema = NULL;
	/* Free cached table info */
	if (self->col_info)
	{
		int			i;

		for (i = 0; i < self->ntables; i++)
		{
			if (self->col_info[i]->result)	/* Free the SQLColumns result structure */
				QR_Destructor(self->col_info[i]->result);

			if (self->col_info[i]->schema)
				free(self->col_info[i]->schema);
			free(self->col_info[i]);
		}
		free(self->col_info);
		self->col_info = NULL;
	}
	self->ntables = 0;
	mylog("exit CC_Cleanup\n");
	return TRUE;
}


int
CC_set_translation(ConnectionClass *self)
{

#ifdef WIN32

	if (self->translation_handle != NULL)
	{
		FreeLibrary(self->translation_handle);
		self->translation_handle = NULL;
	}

	if (self->connInfo.translation_dll[0] == 0)
		return TRUE;

	self->translation_option = atoi(self->connInfo.translation_option);
	self->translation_handle = LoadLibrary(self->connInfo.translation_dll);

	if (self->translation_handle == NULL)
	{
		self->errornumber = CONN_UNABLE_TO_LOAD_DLL;
		self->errormsg = "Could not load the translation DLL.";
		return FALSE;
	}

	self->DataSourceToDriver
		= (DataSourceToDriverProc) GetProcAddress(self->translation_handle,
												"SQLDataSourceToDriver");

	self->DriverToDataSource
		= (DriverToDataSourceProc) GetProcAddress(self->translation_handle,
												"SQLDriverToDataSource");

	if (self->DataSourceToDriver == NULL || self->DriverToDataSource == NULL)
	{
		self->errornumber = CONN_UNABLE_TO_LOAD_DLL;
		self->errormsg = "Could not find translation DLL functions.";
		return FALSE;
	}
#endif
	return TRUE;
}

static	int
md5_auth_send(ConnectionClass *self, const char *salt)
{
	char	*pwd1 = NULL, *pwd2 = NULL;
	ConnInfo   *ci = &(self->connInfo);
	SocketClass	*sock = self->sock;

	if (!(pwd1 = malloc(MD5_PASSWD_LEN + 1)))
		return 1;
	if (!EncryptMD5(ci->password, ci->username, strlen(ci->username), pwd1))
	{
		free(pwd1);
		return 1;
	} 
	if (!(pwd2 = malloc(MD5_PASSWD_LEN + 1)))
	{
		free(pwd1);
		return 1;
	} 
	if (!EncryptMD5(pwd1 + strlen("md5"), salt, 4, pwd2))
	{
		free(pwd2);
		free(pwd1);
		return 1;
	}
	free(pwd1);
	SOCK_put_int(sock, 4 + strlen(pwd2) + 1, 4);
	SOCK_put_n_char(sock, pwd2, strlen(pwd2) + 1);
	SOCK_flush_output(sock);
	free(pwd2);
	return 0; 
}

char
CC_connect(ConnectionClass *self, char password_req, char *salt_para)
{
	StartupPacket sp;
	StartupPacket6_2 sp62;
	QResultClass *res;
	SocketClass *sock;
	ConnInfo   *ci = &(self->connInfo);
	int			areq = -1;
	int			beresp;
	static char		msgbuffer[ERROR_MSG_LENGTH];
	char		salt[5], notice[512];
	static char *func = "CC_connect";

#ifdef	MULTIBYTE
	char	   *encoding;
#endif   /* MULTIBYTE */

	mylog("%s: entering...\n", func);

	if (password_req != AUTH_REQ_OK)

		sock = self->sock;		/* already connected, just authenticate */

	else
	{
		qlog("Global Options: Version='%s', fetch=%d, socket=%d, unknown_sizes=%d, max_varchar_size=%d, max_longvarchar_size=%d\n",
			 POSTGRESDRIVERVERSION,
			 ci->drivers.fetch_max,
			 ci->drivers.socket_buffersize,
			 ci->drivers.unknown_sizes,
			 ci->drivers.max_varchar_size,
			 ci->drivers.max_longvarchar_size);
		qlog("                disable_optimizer=%d, ksqo=%d, unique_index=%d, use_declarefetch=%d\n",
			 ci->drivers.disable_optimizer,
			 ci->drivers.ksqo,
			 ci->drivers.unique_index,
			 ci->drivers.use_declarefetch);
		qlog("                text_as_longvarchar=%d, unknowns_as_longvarchar=%d, bools_as_char=%d\n",
			 ci->drivers.text_as_longvarchar,
			 ci->drivers.unknowns_as_longvarchar,
			 ci->drivers.bools_as_char);

#ifdef MULTIBYTE
		encoding = check_client_encoding(ci->conn_settings);
		if (encoding && strcmp(encoding, "OTHER"))
			self->client_encoding = strdup(encoding);
		else
		{
			encoding = check_client_encoding(ci->drivers.conn_settings);
			if (encoding && strcmp(encoding, "OTHER"))
				self->client_encoding = strdup(encoding);
		}
		if (self->client_encoding)
			self->ccsc = pg_CS_code(self->client_encoding);
		qlog("                extra_systable_prefixes='%s', conn_settings='%s' conn_encoding='%s'\n",
			 ci->drivers.extra_systable_prefixes,
			 ci->drivers.conn_settings,
			 encoding ? encoding : "");
#else
		qlog("                extra_systable_prefixes='%s', conn_settings='%s'\n",
			 ci->drivers.extra_systable_prefixes,
			 ci->drivers.conn_settings);
#endif

		if (self->status != CONN_NOT_CONNECTED)
		{
			self->errormsg = "Already connected.";
			self->errornumber = CONN_OPENDB_ERROR;
			return 0;
		}

		if (ci->server[0] == '\0' || ci->port[0] == '\0' || ci->database[0] == '\0')
		{
			self->errornumber = CONN_INIREAD_ERROR;
			self->errormsg = "Missing server name, port, or database name in call to CC_connect.";
			return 0;
		}

		mylog("CC_connect(): DSN = '%s', server = '%s', port = '%s', database = '%s', username = '%s', password='%s'\n", ci->dsn, ci->server, ci->port, ci->database, ci->username, ci->password);

another_version_retry:

		/*
		 * If the socket was closed for some reason (like a SQLDisconnect,
		 * but no SQLFreeConnect then create a socket now.
		 */
		if (!self->sock)
		{
			self->sock = SOCK_Constructor(self);
			if (!self->sock)
			{
				self->errornumber = CONNECTION_SERVER_NOT_REACHED;
				self->errormsg = "Could not open a socket to the server";
				return 0;
			}
		}

		sock = self->sock;

		mylog("connecting to the server socket...\n");

		SOCK_connect_to(sock, (short) atoi(ci->port), ci->server);
		if (SOCK_get_errcode(sock) != 0)
		{
			mylog("connection to the server socket failed.\n");
			self->errornumber = CONNECTION_SERVER_NOT_REACHED;
			self->errormsg = "Could not connect to the server";
			return 0;
		}
		mylog("connection to the server socket succeeded.\n");

		if (PROTOCOL_62(ci))
		{
			sock->reverse = TRUE;		/* make put_int and get_int work
										 * for 6.2 */

			memset(&sp62, 0, sizeof(StartupPacket6_2));
			SOCK_put_int(sock, htonl(4 + sizeof(StartupPacket6_2)), 4);
			sp62.authtype = htonl(NO_AUTHENTICATION);
			strncpy(sp62.database, ci->database, PATH_SIZE);
			strncpy(sp62.user, ci->username, NAMEDATALEN);
			SOCK_put_n_char(sock, (char *) &sp62, sizeof(StartupPacket6_2));
			SOCK_flush_output(sock);
		}
		else
		{
			memset(&sp, 0, sizeof(StartupPacket));

			mylog("sizeof startup packet = %d\n", sizeof(StartupPacket));

			/* Send length of Authentication Block */
			SOCK_put_int(sock, 4 + sizeof(StartupPacket), 4);

			if (PROTOCOL_63(ci))
				sp.protoVersion = (ProtocolVersion) htonl(PG_PROTOCOL_63);
			else
				sp.protoVersion = (ProtocolVersion) htonl(PG_PROTOCOL_LATEST);

			strncpy(sp.database, ci->database, SM_DATABASE);
			strncpy(sp.user, ci->username, SM_USER);

			SOCK_put_n_char(sock, (char *) &sp, sizeof(StartupPacket));
			SOCK_flush_output(sock);
		}

		mylog("sent the authentication block.\n");

		if (sock->errornumber != 0)
		{
			mylog("couldn't send the authentication block properly.\n");
			self->errornumber = CONN_INVALID_AUTHENTICATION;
			self->errormsg = "Sending the authentication packet failed";
			return 0;
		}
		mylog("sent the authentication block successfully.\n");
	}


	mylog("gonna do authentication\n");


	/*
	 * Now get the authentication request from backend
	 */

	if (!PROTOCOL_62(ci))
	{
		BOOL		before_64 = PG_VERSION_LT(self, 6.4),
					ReadyForQuery = FALSE;

		do
		{
			if (password_req != AUTH_REQ_OK)
				beresp = 'R';
			else
			{
				beresp = SOCK_get_char(sock);
				mylog("auth got '%c'\n", beresp);
			}

			switch (beresp)
			{
				case 'E':

					SOCK_get_string(sock, msgbuffer, ERROR_MSG_LENGTH);
					self->errornumber = CONN_INVALID_AUTHENTICATION;
					self->errormsg = msgbuffer;
					qlog("ERROR from backend during authentication: '%s'\n", self->errormsg);
					if (strncmp(msgbuffer, "Unsupported frontend protocol", 29) == 0)
					{			/* retry older version */
						if (PROTOCOL_63(ci))
							strcpy(ci->protocol, PG62);
						else
							strcpy(ci->protocol, PG63);
						SOCK_Destructor(sock);
						self->sock = (SocketClass *) 0;
						CC_initialize_pg_version(self);
						goto another_version_retry;
					}

					return 0;
				case 'R':

					if (password_req != AUTH_REQ_OK)
					{
						mylog("in 'R' password_req=%s\n", ci->password);
						areq = password_req;
						if (salt_para)
							memcpy(salt, salt_para, sizeof(salt));
						password_req = AUTH_REQ_OK;
					}
					else
					{

						areq = SOCK_get_int(sock, 4);
						if (areq == AUTH_REQ_MD5)
							SOCK_get_n_char(sock, salt, 4);
						else if (areq == AUTH_REQ_CRYPT)
							SOCK_get_n_char(sock, salt, 2);

						mylog("areq = %d\n", areq);
					}
					switch (areq)
					{
						case AUTH_REQ_OK:
							break;

						case AUTH_REQ_KRB4:
							self->errormsg = "Kerberos 4 authentication not supported";
							self->errornumber = CONN_AUTH_TYPE_UNSUPPORTED;
							return 0;

						case AUTH_REQ_KRB5:
							self->errormsg = "Kerberos 5 authentication not supported";
							self->errornumber = CONN_AUTH_TYPE_UNSUPPORTED;
							return 0;

						case AUTH_REQ_PASSWORD:
							mylog("in AUTH_REQ_PASSWORD\n");

							if (ci->password[0] == '\0')
							{
								self->errornumber = CONNECTION_NEED_PASSWORD;
								self->errormsg = "A password is required for this connection.";
								return -areq;		/* need password */
							}

							mylog("past need password\n");

							SOCK_put_int(sock, 4 + strlen(ci->password) + 1, 4);
							SOCK_put_n_char(sock, ci->password, strlen(ci->password) + 1);
							SOCK_flush_output(sock);

							mylog("past flush\n");
							break;

						case AUTH_REQ_CRYPT:
							self->errormsg = "Password crypt authentication not supported";
							self->errornumber = CONN_AUTH_TYPE_UNSUPPORTED;
							return 0;
						case AUTH_REQ_MD5:
							mylog("in AUTH_REQ_MD5\n");
							if (ci->password[0] == '\0')
							{
								self->errornumber = CONNECTION_NEED_PASSWORD;
								self->errormsg = "A password is required for this connection.";
								if (salt_para)
									memcpy(salt_para, salt, sizeof(salt));
								return -areq; /* need password */
							}
							if (md5_auth_send(self, salt))
							{
								self->errormsg = "md5 hashing failed";
								self->errornumber = CONN_INVALID_AUTHENTICATION;
								return 0;
							}
							break;

						case AUTH_REQ_SCM_CREDS:
							self->errormsg = "Unix socket credential authentication not supported";
							self->errornumber = CONN_AUTH_TYPE_UNSUPPORTED;
							return 0;

						default:
							self->errormsg = "Unknown authentication type";
							self->errornumber = CONN_AUTH_TYPE_UNSUPPORTED;
							return 0;
					}
					break;
				case 'K':		/* Secret key (6.4 protocol) */
					self->be_pid = SOCK_get_int(sock, 4);		/* pid */
					self->be_key = SOCK_get_int(sock, 4);		/* key */

					break;
				case 'Z':		/* Backend is ready for new query (6.4) */
					ReadyForQuery = TRUE;
					break;
				case 'N':	/* Notices may come */
					while (SOCK_get_string(sock, notice, sizeof(notice) - 1)) ;
					break;
				default:
					self->errormsg = "Unexpected protocol character during authentication";
					self->errornumber = CONN_INVALID_AUTHENTICATION;
					return 0;
			}

			/*
			 * There were no ReadyForQuery responce before 6.4.
			 */
			if (before_64 && areq == AUTH_REQ_OK)
				ReadyForQuery = TRUE;
		} while (!ReadyForQuery);
	}


	CC_clear_error(self);		/* clear any password error */

	/*
	 * send an empty query in order to find out whether the specified
	 * database really exists on the server machine
	 */
	mylog("sending an empty query...\n");

	res = CC_send_query(self, " ", NULL, CLEAR_RESULT_ON_ABORT);
	if (res == NULL || QR_get_status(res) != PGRES_EMPTY_QUERY)
	{
		mylog("got no result from the empty query.  (probably database does not exist)\n");
		self->errornumber = CONNECTION_NO_SUCH_DATABASE;
		self->errormsg = "The database does not exist on the server\nor user authentication failed.";
		if (res != NULL)
			QR_Destructor(res);
		return 0;
	}
	if (res)
		QR_Destructor(res);

	mylog("empty query seems to be OK.\n");

	CC_set_translation(self);

	/*
	 * Send any initial settings
	 */

	/* 
	 * Get the version number first so we can check it before sending options
	 * that are now obsolete. DJP 21/06/2002
	 */

	CC_lookup_pg_version(self);		/* Get PostgreSQL version for
						   SQLGetInfo use */
	/*
	 * Since these functions allocate statements, and since the connection
	 * is not established yet, it would violate odbc state transition
	 * rules.  Therefore, these functions call the corresponding local
	 * function instead.
	 */
	CC_send_settings(self);
	CC_lookup_lo(self);			/* a hack to get the oid of
						   our large object oid type */

	/*
	 *	Multibyte handling is available ?
	 */
#ifdef MULTIBYTE
	if (PG_VERSION_GE(self, 6.4))
	{
		CC_lookup_characterset(self);
		if (self->errornumber != 0)
			return 0;
#ifdef UNICODE_SUPPORT
		if (self->unicode)
		{
			if (!self->client_encoding ||
			    stricmp(self->client_encoding, "UNICODE"))
			{
				QResultClass	*res;
				if (PG_VERSION_LT(self, 7.1))
				{
					self->errornumber = CONN_NOT_IMPLEMENTED_ERROR;
					self->errormsg = "UTF-8 conversion isn't implemented before 7.1";
					return 0;
				}
				if (self->client_encoding)
					free(self->client_encoding);
				self->client_encoding = NULL;
				if (res = CC_send_query(self, "set client_encoding to 'UTF8'", NULL, CLEAR_RESULT_ON_ABORT), res)
				{
					self->client_encoding = strdup("UNICODE");
					QR_Destructor(res);
					
				}
			}
		}
#else
		{
		}
#endif /* UNICODE_SUPPORT */
	}
#ifdef UNICODE_SUPPORT
	else if (self->unicode)
	{
		self->errornumber = CONN_NOT_IMPLEMENTED_ERROR;
		self->errormsg = "Unicode isn't supported before 6.4";
		return 0;
	}
#endif /* UNICODE_SUPPORT */
#endif /* MULTIBYTE */
	ci->updatable_cursors = 0;
#ifdef	DRIVER_CURSOR_IMPLEMENT
	if (!ci->drivers.use_declarefetch &&
		PG_VERSION_GE(self, 7.0)) /* Tid scan since 7.0 */
		ci->updatable_cursors = ci->allow_keyset;
#endif /* DRIVER_CURSOR_IMPLEMENT */

	CC_clear_error(self);		/* clear any initial command errors */
	self->status = CONN_CONNECTED;

	mylog("%s: returning...\n", func);

	return 1;

}


char
CC_add_statement(ConnectionClass *self, StatementClass *stmt)
{
	int			i;

	mylog("CC_add_statement: self=%u, stmt=%u\n", self, stmt);

	for (i = 0; i < self->num_stmts; i++)
	{
		if (!self->stmts[i])
		{
			stmt->hdbc = self;
			self->stmts[i] = stmt;
			return TRUE;
		}
	}

	/* no more room -- allocate more memory */
	self->stmts = (StatementClass **) realloc(self->stmts, sizeof(StatementClass *) * (STMT_INCREMENT + self->num_stmts));
	if (!self->stmts)
		return FALSE;

	memset(&self->stmts[self->num_stmts], 0, sizeof(StatementClass *) * STMT_INCREMENT);

	stmt->hdbc = self;
	self->stmts[self->num_stmts] = stmt;

	self->num_stmts += STMT_INCREMENT;

	return TRUE;
}


char
CC_remove_statement(ConnectionClass *self, StatementClass *stmt)
{
	int			i;

	for (i = 0; i < self->num_stmts; i++)
	{
		if (self->stmts[i] == stmt && stmt->status != STMT_EXECUTING)
		{
			self->stmts[i] = NULL;
			return TRUE;
		}
	}

	return FALSE;
}


/*
 *	Create a more informative error message by concatenating the connection
 *	error message with its socket error message.
 */
char *
CC_create_errormsg(ConnectionClass *self)
{
	SocketClass *sock = self->sock;
	int			pos;
	static char msg[4096];

	mylog("enter CC_create_errormsg\n");

	msg[0] = '\0';

	if (self->errormsg)
		strcpy(msg, self->errormsg);

	mylog("msg = '%s'\n", msg);

	if (sock && sock->errormsg && sock->errormsg[0] != '\0')
	{
		pos = strlen(msg);
		sprintf(&msg[pos], ";\n%s", sock->errormsg);
	}

	mylog("exit CC_create_errormsg\n");
	return msg;
}


char
CC_get_error(ConnectionClass *self, int *number, char **message)
{
	int			rv;

	mylog("enter CC_get_error\n");

	/* Create a very informative errormsg if it hasn't been done yet. */
	if (!self->errormsg_created)
	{
		self->errormsg = CC_create_errormsg(self);
		self->errormsg_created = TRUE;
	}

	if (self->errornumber)
	{
		*number = self->errornumber;
		*message = self->errormsg;
	}
	rv = (self->errornumber != 0);

	self->errornumber = 0;		/* clear the error */

	mylog("exit CC_get_error\n");

	return rv;
}


void	CC_on_commit(ConnectionClass *conn)
{
	if (CC_is_in_trans(conn))
	{
#ifdef	DRIVER_CURSOR_IMPLEMENT
		if (conn->result_uncommitted)
			ProcessRollback(conn, FALSE);
#endif /* DRIVER_CURSOR_IMPLEMENT */
		CC_set_no_trans(conn);
	}
	conn->result_uncommitted = 0;
}
void	CC_on_abort(ConnectionClass *conn, UDWORD opt)
{
	if (CC_is_in_trans(conn))
	{
#ifdef	DRIVER_CURSOR_IMPLEMENT
		if (conn->result_uncommitted)
			ProcessRollback(conn, TRUE);
#endif /* DRIVER_CURSOR_IMPLEMENT */
		if (0 != (opt & NO_TRANS))
			CC_set_no_trans(conn);
	}
	if (0 != (opt & CONN_DEAD))
		conn->status = CONN_DOWN;
	conn->result_uncommitted = 0;
}

/*
 *	The "result_in" is only used by QR_next_tuple() to fetch another group of rows into
 *	the same existing QResultClass (this occurs when the tuple cache is depleted and
 *	needs to be re-filled).
 *
 *	The "cursor" is used by SQLExecute to associate a statement handle as the cursor name
 *	(i.e., C3326857) for SQL select statements.  This cursor is then used in future
 *	'declare cursor C3326857 for ...' and 'fetch 100 in C3326857' statements.
 */
QResultClass *
CC_send_query(ConnectionClass *self, char *query, QueryInfo *qi, UDWORD flag)
{
	QResultClass *cmdres = NULL,
			   *retres = NULL,
			   *res = NULL;
	BOOL	clear_result_on_abort = ((flag & CLEAR_RESULT_ON_ABORT) != 0),
		create_keyset = ((flag & CREATE_KEYSET) != 0),
		issue_begin = ((flag & GO_INTO_TRANSACTION) != 0 && !CC_is_in_trans(self));
	char		swallow, *wq, *ptr;
	int			id;
	SocketClass *sock = self->sock;
	int			maxlen,
				empty_reqs;
	BOOL		msg_truncated,
				ReadyToReturn,
				query_completed = FALSE,
				before_64 = PG_VERSION_LT(self, 6.4),
				aborted = FALSE,
				used_passed_result_object = FALSE;
	UDWORD		abort_opt;

	/* ERROR_MSG_LENGTH is suffcient */
	static char msgbuffer[ERROR_MSG_LENGTH + 1];

	/* QR_set_command() dups this string so doesn't need static */
	char		cmdbuffer[ERROR_MSG_LENGTH + 1];

	mylog("send_query(): conn=%u, query='%s'\n", self, query);
	qlog("conn=%u, query='%s'\n", self, query);

	/* Indicate that we are sending a query to the backend */
	maxlen = CC_get_max_query_len(self);
	if (maxlen > 0 && maxlen < (int) strlen(query) + 1)
	{
		self->errornumber = CONNECTION_MSG_TOO_LONG;
		self->errormsg = "Query string is too long";
		return NULL;
	}

	if ((NULL == query) || (query[0] == '\0'))
		return NULL;

	if (SOCK_get_errcode(sock) != 0)
	{
		self->errornumber = CONNECTION_COULD_NOT_SEND;
		self->errormsg = "Could not send Query to backend";
		CC_on_abort(self, NO_TRANS | CONN_DEAD);
		return NULL;
	}

	SOCK_put_char(sock, 'Q');
	if (SOCK_get_errcode(sock) != 0)
	{
		self->errornumber = CONNECTION_COULD_NOT_SEND;
		self->errormsg = "Could not send Query to backend";
		CC_on_abort(self, NO_TRANS | CONN_DEAD);
		return NULL;
	}

	if (issue_begin)
		SOCK_put_n_char(sock, "begin;", 6);
	SOCK_put_string(sock, query);
	SOCK_flush_output(sock);

	if (SOCK_get_errcode(sock) != 0)
	{
		self->errornumber = CONNECTION_COULD_NOT_SEND;
		self->errormsg = "Could not send Query to backend";
		CC_on_abort(self, NO_TRANS | CONN_DEAD);
		return NULL;
	}

	mylog("send_query: done sending query\n");

	ReadyToReturn = FALSE;
	empty_reqs = 0;
	for (wq = query; isspace((unsigned char) *wq); wq++)
		;
	if (*wq == '\0')
		empty_reqs = 1;
	cmdres = qi ? qi->result_in : NULL;
	if (cmdres)
		used_passed_result_object = TRUE;
	else
	{
		cmdres = QR_Constructor();
		if (!cmdres)
		{
			self->errornumber = CONNECTION_COULD_NOT_RECEIVE;
			self->errormsg = "Could not create result info in send_query.";
			return NULL;
		}
	}
	res = cmdres;
	while (!ReadyToReturn)
	{
		/* what type of message is coming now ? */
		id = SOCK_get_char(sock);

		if ((SOCK_get_errcode(sock) != 0) || (id == EOF))
		{
			self->errornumber = CONNECTION_NO_RESPONSE;
			self->errormsg = "No response from the backend";

			mylog("send_query: 'id' - %s\n", self->errormsg);
			CC_on_abort(self, NO_TRANS | CONN_DEAD);
			ReadyToReturn = TRUE;
			retres = NULL;
			break;
		}

		mylog("send_query: got id = '%c'\n", id);

		switch (id)
		{
			case 'A':			/* Asynchronous Messages are ignored */
				(void) SOCK_get_int(sock, 4);	/* id of notification */
				SOCK_get_string(sock, msgbuffer, ERROR_MSG_LENGTH);
				/* name of the relation the message comes from */
				break;
			case 'C':			/* portal query command, no tuples
								 * returned */
				/* read in the return message from the backend */
				SOCK_get_string(sock, cmdbuffer, ERROR_MSG_LENGTH);
				if (SOCK_get_errcode(sock) != 0)
				{
					self->errornumber = CONNECTION_NO_RESPONSE;
					self->errormsg = "No response from backend while receiving a portal query command";
					mylog("send_query: 'C' - %s\n", self->errormsg);
					CC_on_abort(self, NO_TRANS | CONN_DEAD);
					ReadyToReturn = TRUE;
					retres = NULL;
				}
				else
				{
					mylog("send_query: ok - 'C' - %s\n", cmdbuffer);

					if (query_completed)	/* allow for "show" style notices */
					{
						res->next = QR_Constructor();
						res = res->next;
					} 

					mylog("send_query: setting cmdbuffer = '%s'\n", cmdbuffer);

					if (strnicmp(cmdbuffer, "BEGIN", 5) == 0)
					{
						CC_set_in_trans(self);
						if (issue_begin)
						{
							issue_begin = FALSE;
							continue;
						}
					}
					else if (strnicmp(cmdbuffer, "COMMIT", 6) == 0)
						CC_on_commit(self);
					else if (strnicmp(cmdbuffer, "ROLLBACK", 8) == 0)
						CC_on_abort(self, NO_TRANS);
					else if (strnicmp(cmdbuffer, "END", 3) == 0)
						CC_on_commit(self);
					else if (strnicmp(cmdbuffer, "ABORT", 5) == 0)
						CC_on_abort(self, NO_TRANS);
					else
					{
						trim(cmdbuffer); /* get rid of trailing space */ 
						ptr = strrchr(cmdbuffer, ' ');
						if (ptr)
							res->recent_processed_row_count = atoi(ptr + 1);
						else
							res->recent_processed_row_count = -1;
					}

					if (QR_command_successful(res))
						QR_set_status(res, PGRES_COMMAND_OK);
					QR_set_command(res, cmdbuffer);
					query_completed = TRUE;
					mylog("send_query: returning res = %u\n", res);
					if (!before_64)
						break;

					/*
					 * (Quotation from the original comments) since
					 * backend may produce more than one result for some
					 * commands we need to poll until clear so we send an
					 * empty query, and keep reading out of the pipe until
					 * an 'I' is received
					 */

					if (empty_reqs == 0)
					{
						SOCK_put_string(sock, "Q ");
						SOCK_flush_output(sock);
						empty_reqs++;
					}
				}
				break;
			case 'Z':			/* Backend is ready for new query (6.4) */
				if (empty_reqs == 0)
				{
					ReadyToReturn = TRUE;
					if (aborted || query_completed)
						retres = cmdres;
					else
						ReadyToReturn = FALSE;
				}
				break;
			case 'N':			/* NOTICE: */
				msg_truncated = SOCK_get_string(sock, cmdbuffer, ERROR_MSG_LENGTH);
				if (QR_command_successful(res))
					QR_set_status(res, PGRES_NONFATAL_ERROR);
				QR_set_notice(res, cmdbuffer);	/* will dup this string */
				mylog("~~~ NOTICE: '%s'\n", cmdbuffer);
				qlog("NOTICE from backend during send_query: '%s'\n", cmdbuffer);
				while (msg_truncated)
					msg_truncated = SOCK_get_string(sock, cmdbuffer, ERROR_MSG_LENGTH);

				continue;		/* dont return a result -- continue
								 * reading */

			case 'I':			/* The server sends an empty query */
				/* There is a closing '\0' following the 'I', so we eat it */
				swallow = SOCK_get_char(sock);
				if ((swallow != '\0') || SOCK_get_errcode(sock) != 0)
				{
					self->errornumber = CONNECTION_BACKEND_CRAZY;
					QR_set_message(res, "Unexpected protocol character from backend (send_query - I)");
					QR_set_status(res, PGRES_FATAL_ERROR);
					ReadyToReturn = TRUE;
					retres = cmdres;
					break;
				}
				else
				{
					/* We return the empty query */
					QR_set_status(res, PGRES_EMPTY_QUERY);
				}
				if (empty_reqs > 0)
				{
					if (--empty_reqs == 0)
						query_completed = TRUE;
				}
				break;
			case 'E':
				msg_truncated = SOCK_get_string(sock, msgbuffer, ERROR_MSG_LENGTH);

				/* Remove a newline */
				if (msgbuffer[0] != '\0' && msgbuffer[strlen(msgbuffer) - 1] == '\n')
					msgbuffer[strlen(msgbuffer) - 1] = '\0';


				mylog("send_query: 'E' - %s\n", msgbuffer);
				qlog("ERROR from backend during send_query: '%s'\n", msgbuffer);

				/* We should report that an error occured. Zoltan */
				abort_opt = 0;
				if (!strncmp(msgbuffer, "FATAL", 5))
				{
					self->errornumber = CONNECTION_SERVER_REPORTED_ERROR;
					abort_opt = NO_TRANS | CONN_DEAD;
				}
				else
					self->errornumber = CONNECTION_SERVER_REPORTED_WARNING;
				CC_on_abort(self, abort_opt);
				QR_set_status(res, PGRES_FATAL_ERROR);
				QR_set_message(res, msgbuffer);
				QR_set_aborted(res, TRUE);
				aborted = TRUE;
				while (msg_truncated)
					msg_truncated = SOCK_get_string(sock, cmdbuffer, ERROR_MSG_LENGTH);

				query_completed = TRUE;
				break;

			case 'P':			/* get the Portal name */
				SOCK_get_string(sock, msgbuffer, ERROR_MSG_LENGTH);
				break;
			case 'T':			/* Tuple results start here */
				if (query_completed)
				{
					res->next = QR_Constructor();
					if (!res->next)
					{
						self->errornumber = CONNECTION_COULD_NOT_RECEIVE;
						self->errormsg = "Could not create result info in send_query.";
						ReadyToReturn = TRUE;
						retres = NULL;
						break;
					}
					if (create_keyset)
						QR_set_haskeyset(res->next);
					mylog("send_query: 'T' no result_in: res = %u\n", res->next);
					res = res->next;

					if (qi)
						QR_set_cache_size(res, qi->row_size);
				}
				if (!used_passed_result_object)
				{
					if (create_keyset)
						QR_set_haskeyset(res);
					if (!QR_fetch_tuples(res, self, qi ? qi->cursor : NULL))
					{
						self->errornumber = CONNECTION_COULD_NOT_RECEIVE;
						self->errormsg = QR_get_message(res);
						ReadyToReturn = TRUE;
						if (PGRES_FATAL_ERROR == QR_get_status(res))
							retres = cmdres;
						else
							retres = NULL;
						break;
					}
					query_completed = TRUE;
				}
				else
				{				/* next fetch, so reuse an existing result */

					/*
					 * called from QR_next_tuple and must return
					 * immediately.
					 */
					ReadyToReturn = TRUE;
					if (!QR_fetch_tuples(res, NULL, NULL))
					{
						self->errornumber = CONNECTION_COULD_NOT_RECEIVE;
						self->errormsg = QR_get_message(res);
						retres = NULL;
						break;
					}
					retres = cmdres;
				}
				break;
			case 'D':			/* Copy in command began successfully */
				if (query_completed)
				{
					res->next = QR_Constructor();
					res = res->next;
				}
				QR_set_status(res, PGRES_COPY_IN);
				ReadyToReturn = TRUE;
				retres = cmdres;
				break;
			case 'B':			/* Copy out command began successfully */
				if (query_completed)
				{
					res->next = QR_Constructor();
					res = res->next;
				}
				QR_set_status(res, PGRES_COPY_OUT);
				ReadyToReturn = TRUE;
				retres = cmdres;
				break;
			default:
				self->errornumber = CONNECTION_BACKEND_CRAZY;
				self->errormsg = "Unexpected protocol character from backend (send_query)";
				CC_on_abort(self, NO_TRANS | CONN_DEAD);

				mylog("send_query: error - %s\n", self->errormsg);
				ReadyToReturn = TRUE;
				retres = NULL;
				break;
		}

		/*
		 * There were no ReadyForQuery response before 6.4.
		 */
		if (before_64)
		{
			if (empty_reqs == 0 && query_completed)
				break;
		}
	}

	/*
	 * Break before being ready to return.
	 */
	if (!ReadyToReturn)
		retres = cmdres;

	/*
	 * Cleanup garbage results before returning.
	 */
	if (cmdres && retres != cmdres && !used_passed_result_object)
		QR_Destructor(cmdres);
	/*
	 * Cleanup the aborted result if specified
	 */
	if (retres)
	{
		if (aborted)
		{
			if (clear_result_on_abort)
			{
	   			if (!used_passed_result_object)
				{
					QR_Destructor(retres);
					retres = NULL;
				}
			}
			if (retres)
			{
				/*
				 *	discard results other than errors.
				 */
				QResultClass	*qres;
				for (qres = retres; qres->next; qres = retres)
				{
					if (QR_get_aborted(qres))
						break;
					retres = qres->next;
					qres->next = NULL;
					QR_Destructor(qres);
				}
				/*
				 *	If error message isn't set
				 */
				if (retres && (!self->errormsg || !self->errormsg[0]))
					self->errormsg = QR_get_message(retres);
			}
		}
	}
	return retres;
}


int
CC_send_function(ConnectionClass *self, int fnid, void *result_buf, int *actual_result_len, int result_is_int, LO_ARG *args, int nargs)
{
	char		id,
				c,
				done;
	SocketClass *sock = self->sock;

	/* ERROR_MSG_LENGTH is sufficient */
	static char msgbuffer[ERROR_MSG_LENGTH + 1];
	int			i;

	mylog("send_function(): conn=%u, fnid=%d, result_is_int=%d, nargs=%d\n", self, fnid, result_is_int, nargs);

	if (SOCK_get_errcode(sock) != 0)
	{
		self->errornumber = CONNECTION_COULD_NOT_SEND;
		self->errormsg = "Could not send function to backend";
		CC_on_abort(self, NO_TRANS | CONN_DEAD);
		return FALSE;
	}

	SOCK_put_string(sock, "F ");
	if (SOCK_get_errcode(sock) != 0)
	{
		self->errornumber = CONNECTION_COULD_NOT_SEND;
		self->errormsg = "Could not send function to backend";
		CC_on_abort(self, NO_TRANS | CONN_DEAD);
		return FALSE;
	}

	SOCK_put_int(sock, fnid, 4);
	SOCK_put_int(sock, nargs, 4);


	mylog("send_function: done sending function\n");

	for (i = 0; i < nargs; ++i)
	{
		mylog("  arg[%d]: len = %d, isint = %d, integer = %d, ptr = %u\n", i, args[i].len, args[i].isint, args[i].u.integer, args[i].u.ptr);

		SOCK_put_int(sock, args[i].len, 4);
		if (args[i].isint)
			SOCK_put_int(sock, args[i].u.integer, 4);
		else
			SOCK_put_n_char(sock, (char *) args[i].u.ptr, args[i].len);


	}

	mylog("    done sending args\n");

	SOCK_flush_output(sock);
	mylog("  after flush output\n");

	done = FALSE;
	while (!done)
	{
		id = SOCK_get_char(sock);
		mylog("   got id = %c\n", id);

		switch (id)
		{
			case 'V':
				done = TRUE;
				break;			/* ok */

			case 'N':
				SOCK_get_string(sock, msgbuffer, ERROR_MSG_LENGTH);
				mylog("send_function(V): 'N' - %s\n", msgbuffer);
				/* continue reading */
				break;

			case 'E':
				SOCK_get_string(sock, msgbuffer, ERROR_MSG_LENGTH);
				self->errormsg = msgbuffer;
				CC_on_abort(self, 0);

				mylog("send_function(V): 'E' - %s\n", self->errormsg);
				qlog("ERROR from backend during send_function: '%s'\n", self->errormsg);

				return FALSE;

			case 'Z':
				break;

			default:
				self->errornumber = CONNECTION_BACKEND_CRAZY;
				self->errormsg = "Unexpected protocol character from backend (send_function, args)";
				CC_on_abort(self, NO_TRANS | CONN_DEAD);

				mylog("send_function: error - %s\n", self->errormsg);
				return FALSE;
		}
	}

	id = SOCK_get_char(sock);
	for (;;)
	{
		switch (id)
		{
			case 'G':			/* function returned properly */
				mylog("  got G!\n");

				*actual_result_len = SOCK_get_int(sock, 4);
				mylog("  actual_result_len = %d\n", *actual_result_len);

				if (result_is_int)
					*((int *) result_buf) = SOCK_get_int(sock, 4);
				else
					SOCK_get_n_char(sock, (char *) result_buf, *actual_result_len);

				mylog("  after get result\n");

				c = SOCK_get_char(sock);		/* get the last '0' */

				mylog("   after get 0\n");

				return TRUE;

			case 'E':
				SOCK_get_string(sock, msgbuffer, ERROR_MSG_LENGTH);
				self->errormsg = msgbuffer;
				CC_on_abort(self, 0);
				mylog("send_function(G): 'E' - %s\n", self->errormsg);
				qlog("ERROR from backend during send_function: '%s'\n", self->errormsg);

				return FALSE;

			case 'N':
				SOCK_get_string(sock, msgbuffer, ERROR_MSG_LENGTH);

				mylog("send_function(G): 'N' - %s\n", msgbuffer);
				qlog("NOTICE from backend during send_function: '%s'\n", msgbuffer);

				continue;		/* dont return a result -- continue
								 * reading */

			case '0':			/* empty result */
				return TRUE;

			default:
				self->errornumber = CONNECTION_BACKEND_CRAZY;
				self->errormsg = "Unexpected protocol character from backend (send_function, result)";
				CC_on_abort(self, NO_TRANS | CONN_DEAD);

				mylog("send_function: error - %s\n", self->errormsg);
				return FALSE;
		}
	}
}


char
CC_send_settings(ConnectionClass *self)
{
	/* char ini_query[MAX_MESSAGE_LEN]; */
	ConnInfo   *ci = &(self->connInfo);

/* QResultClass *res; */
	HSTMT		hstmt;
	StatementClass *stmt;
	RETCODE		result;
	char		status = TRUE;
	char	   *cs,
			   *ptr;
	static char *func = "CC_send_settings";


	mylog("%s: entering...\n", func);

/*
 *	This function must use the local odbc API functions since the odbc state
 *	has not transitioned to "connected" yet.
 */

	result = PGAPI_AllocStmt(self, &hstmt);
	if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
		return FALSE;
	stmt = (StatementClass *) hstmt;

	stmt->internal = TRUE;		/* ensure no BEGIN/COMMIT/ABORT stuff */

	/* Set the Datestyle to the format the driver expects it to be in */
	result = PGAPI_ExecDirect(hstmt, "set DateStyle to 'ISO'", SQL_NTS);
	if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
		status = FALSE;

	mylog("%s: result %d, status %d from set DateStyle\n", func, result, status);

	/* Disable genetic optimizer based on global flag */
	if (ci->drivers.disable_optimizer)
	{
		result = PGAPI_ExecDirect(hstmt, "set geqo to 'OFF'", SQL_NTS);
		if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
			status = FALSE;

		mylog("%s: result %d, status %d from set geqo\n", func, result, status);

	}

	/* KSQO (not applicable to 7.1+ - DJP 21/06/2002) */
	if (ci->drivers.ksqo && PG_VERSION_LT(self, 7.1))
	{
		result = PGAPI_ExecDirect(hstmt, "set ksqo to 'ON'", SQL_NTS);
		if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
			status = FALSE;

		mylog("%s: result %d, status %d from set ksqo\n", func, result, status);

	}

	/* Global settings */
	if (ci->drivers.conn_settings[0] != '\0')
	{
		cs = strdup(ci->drivers.conn_settings);
		ptr = strtok(cs, ";");
		while (ptr)
		{
			result = PGAPI_ExecDirect(hstmt, ptr, SQL_NTS);
			if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
				status = FALSE;

			mylog("%s: result %d, status %d from '%s'\n", func, result, status, ptr);

			ptr = strtok(NULL, ";");
		}

		free(cs);
	}

	/* Per Datasource settings */
	if (ci->conn_settings[0] != '\0')
	{
		cs = strdup(ci->conn_settings);
		ptr = strtok(cs, ";");
		while (ptr)
		{
			result = PGAPI_ExecDirect(hstmt, ptr, SQL_NTS);
			if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
				status = FALSE;

			mylog("%s: result %d, status %d from '%s'\n", func, result, status, ptr);

			ptr = strtok(NULL, ";");
		}

		free(cs);
	}


	PGAPI_FreeStmt(hstmt, SQL_DROP);

	return status;
}


/*
 *	This function is just a hack to get the oid of our Large Object oid type.
 *	If a real Large Object oid type is made part of Postgres, this function
 *	will go away and the define 'PG_TYPE_LO' will be updated.
 */
void
CC_lookup_lo(ConnectionClass *self)
{
	HSTMT		hstmt;
	StatementClass *stmt;
	RETCODE		result;
	static char *func = "CC_lookup_lo";

	mylog("%s: entering...\n", func);

/*
 *	This function must use the local odbc API functions since the odbc state
 *	has not transitioned to "connected" yet.
 */
	result = PGAPI_AllocStmt(self, &hstmt);
	if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
		return;
	stmt = (StatementClass *) hstmt;

	result = PGAPI_ExecDirect(hstmt, "select oid from pg_type where typname='" PG_TYPE_LO_NAME "'", SQL_NTS);
	if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
	{
		PGAPI_FreeStmt(hstmt, SQL_DROP);
		return;
	}

	result = PGAPI_Fetch(hstmt);
	if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
	{
		PGAPI_FreeStmt(hstmt, SQL_DROP);
		return;
	}

	result = PGAPI_GetData(hstmt, 1, SQL_C_SLONG, &self->lobj_type, sizeof(self->lobj_type), NULL);
	if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
	{
		PGAPI_FreeStmt(hstmt, SQL_DROP);
		return;
	}

	mylog("Got the large object oid: %d\n", self->lobj_type);
	qlog("    [ Large Object oid = %d ]\n", self->lobj_type);

	result = PGAPI_FreeStmt(hstmt, SQL_DROP);
}


/*
 *	This function initializes the version of PostgreSQL from
 *	connInfo.protocol that we're connected to.
 *	h-inoue 01-2-2001
 */
void
CC_initialize_pg_version(ConnectionClass *self)
{
	strcpy(self->pg_version, self->connInfo.protocol);
	if (PROTOCOL_62(&self->connInfo))
	{
		self->pg_version_number = (float) 6.2;
		self->pg_version_major = 6;
		self->pg_version_minor = 2;
	}
	else if (PROTOCOL_63(&self->connInfo))
	{
		self->pg_version_number = (float) 6.3;
		self->pg_version_major = 6;
		self->pg_version_minor = 3;
	}
	else
	{
		self->pg_version_number = (float) 6.4;
		self->pg_version_major = 6;
		self->pg_version_minor = 4;
	}
}


/*
 *	This function gets the version of PostgreSQL that we're connected to.
 *	This is used to return the correct info in SQLGetInfo
 *	DJP - 25-1-2001
 */
void
CC_lookup_pg_version(ConnectionClass *self)
{
	HSTMT		hstmt;
	StatementClass *stmt;
	RETCODE		result;
	char		szVersion[32];
	int			major,
				minor;
	static char *func = "CC_lookup_pg_version";

	mylog("%s: entering...\n", func);

/*
 *	This function must use the local odbc API functions since the odbc state
 *	has not transitioned to "connected" yet.
 */
	result = PGAPI_AllocStmt(self, &hstmt);
	if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
		return;
	stmt = (StatementClass *) hstmt;

	/* get the server's version if possible	 */
	result = PGAPI_ExecDirect(hstmt, "select version()", SQL_NTS);
	if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
	{
		PGAPI_FreeStmt(hstmt, SQL_DROP);
		return;
	}

	result = PGAPI_Fetch(hstmt);
	if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
	{
		PGAPI_FreeStmt(hstmt, SQL_DROP);
		return;
	}

	result = PGAPI_GetData(hstmt, 1, SQL_C_CHAR, self->pg_version, MAX_INFO_STRING, NULL);
	if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
	{
		PGAPI_FreeStmt(hstmt, SQL_DROP);
		return;
	}

	/*
	 * Extract the Major and Minor numbers from the string. This assumes
	 * the string starts 'Postgresql X.X'
	 */
	strcpy(szVersion, "0.0");
	if (sscanf(self->pg_version, "%*s %d.%d", &major, &minor) >= 2)
	{
		sprintf(szVersion, "%d.%d", major, minor);
		self->pg_version_major = major;
		self->pg_version_minor = minor;
	}
	self->pg_version_number = (float) atof(szVersion);
	if (PG_VERSION_GE(self, 7.3))
		self->schema_support = 1;

	mylog("Got the PostgreSQL version string: '%s'\n", self->pg_version);
	mylog("Extracted PostgreSQL version number: '%1.1f'\n", self->pg_version_number);
	qlog("    [ PostgreSQL version string = '%s' ]\n", self->pg_version);
	qlog("    [ PostgreSQL version number = '%1.1f' ]\n", self->pg_version_number);

	result = PGAPI_FreeStmt(hstmt, SQL_DROP);
}


void
CC_log_error(const char *func, const char *desc, const ConnectionClass *self)
{
#ifdef PRN_NULLCHECK
#define nullcheck(a) (a ? a : "(NULL)")
#endif

	if (self)
	{
		qlog("CONN ERROR: func=%s, desc='%s', errnum=%d, errmsg='%s'\n", func, desc, self->errornumber, nullcheck(self->errormsg));
		mylog("CONN ERROR: func=%s, desc='%s', errnum=%d, errmsg='%s'\n", func, desc, self->errornumber, nullcheck(self->errormsg));
		qlog("            ------------------------------------------------------------\n");
		qlog("            henv=%u, conn=%u, status=%u, num_stmts=%d\n", self->henv, self, self->status, self->num_stmts);
		qlog("            sock=%u, stmts=%u, lobj_type=%d\n", self->sock, self->stmts, self->lobj_type);

		qlog("            ---------------- Socket Info -------------------------------\n");
		if (self->sock)
		{
			SocketClass *sock = self->sock;

			qlog("            socket=%d, reverse=%d, errornumber=%d, errormsg='%s'\n", sock->socket, sock->reverse, sock->errornumber, nullcheck(sock->errormsg));
			qlog("            buffer_in=%u, buffer_out=%u\n", sock->buffer_in, sock->buffer_out);
			qlog("            buffer_filled_in=%d, buffer_filled_out=%d, buffer_read_in=%d\n", sock->buffer_filled_in, sock->buffer_filled_out, sock->buffer_read_in);
		}
	}
	else
{
		qlog("INVALID CONNECTION HANDLE ERROR: func=%s, desc='%s'\n", func, desc);
		mylog("INVALID CONNECTION HANDLE ERROR: func=%s, desc='%s'\n", func, desc);
}
#undef PRN_NULLCHECK
}

int
CC_get_max_query_len(const ConnectionClass *conn)
{
	int			value;

	/* Long Queries in 7.0+ */
	if (PG_VERSION_GE(conn, 7.0))
		value = 0 /* MAX_STATEMENT_LEN */ ;
	/* Prior to 7.0 we used 2*BLCKSZ */
	else if (PG_VERSION_GE(conn, 6.5))
		value = (2 * BLCKSZ);
	else
		/* Prior to 6.5 we used BLCKSZ */
		value = BLCKSZ;
	return value;
}

/*
 *	This deosn't really return the CURRENT SCHEMA
 *	but there's no alternative.
 */
const char *
CC_get_current_schema(ConnectionClass *conn)
{
	if (!conn->current_schema && conn->schema_support)
	{
		QResultClass	*res;

		if (res = CC_send_query(conn, "select current_schema()", NULL, CLEAR_RESULT_ON_ABORT), res)
		{
			if (QR_get_num_total_tuples(res) == 1)
				conn->current_schema = strdup(QR_get_value_backend_row(res, 0, 0));
			QR_Destructor(res);
		}
	}
	return (const char *) conn->current_schema;
}

int
CC_send_cancel_request(const ConnectionClass *conn)
{
#ifdef WIN32
	int			save_errno = (WSAGetLastError());
#else
	int			save_errno = errno;
#endif
	int			tmpsock = -1;
	struct
	{
		uint32		packetlen;
		CancelRequestPacket cp;
	}			crp;

	/* Check we have an open connection */
	if (!conn)
		return FALSE;

	if (conn->sock == NULL )
	{
		return FALSE;
	}

	/*
	 * We need to open a temporary connection to the postmaster. Use the
	 * information saved by connectDB to do this with only kernel calls.
	*/
	if ((tmpsock = socket(AF_INET, SOCK_STREAM, 0)) < 0)
	{
		return FALSE;
	}
	if (connect(tmpsock, (struct sockaddr *)&(conn->sock->sadr),
				sizeof(conn->sock->sadr)) < 0)
	{
		return FALSE;
	}

	/*
	 * We needn't set nonblocking I/O or NODELAY options here.
	 */
	crp.packetlen = htonl((uint32) sizeof(crp));
	crp.cp.cancelRequestCode = (MsgType) htonl(CANCEL_REQUEST_CODE);
	crp.cp.backendPID = htonl(conn->be_pid);
	crp.cp.cancelAuthCode = htonl(conn->be_key);

	if (send(tmpsock, (char *) &crp, sizeof(crp), 0) != (int) sizeof(crp))
	{
		return FALSE;
	}

	/* Sent it, done */
	closesocket(tmpsock);
#ifdef WIN32
	WSASetLastError(save_errno);
#else
	errno = save_errno;
#endif
	return TRUE;
}
