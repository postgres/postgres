/*-------
 * Module:			environ.c
 *
 * Description:		This module contains routines related to
 *					the environment, such as storing connection handles,
 *					and returning errors.
 *
 * Classes:			EnvironmentClass (Functions prefix: "EN_")
 *
 * API functions:	SQLAllocEnv, SQLFreeEnv, SQLError
 *
 * Comments:		See "notice.txt" for copyright and license information.
 *-------
 */

#include "environ.h"

#include "connection.h"
#include "dlg_specific.h"
#include "statement.h"
#include <stdlib.h>
#include <string.h>
#include "pgapifunc.h"

extern GLOBAL_VALUES globals;

/* The one instance of the handles */
ConnectionClass *conns[MAX_CONNECTIONS];


RETCODE		SQL_API
PGAPI_AllocEnv(HENV FAR * phenv)
{
	static char *func = "PGAPI_AllocEnv";

	mylog("**** in PGAPI_AllocEnv ** \n");

	/*
	 * Hack for systems on which none of the constructor-making techniques
	 * in psqlodbc.c work: if globals appears not to have been
	 * initialized, then cause it to be initialized.  Since this should be
	 * the first function called in this shared library, doing it here
	 * should work.
	 */
	if (globals.socket_buffersize <= 0)
		getCommonDefaults(DBMS_NAME, ODBCINST_INI, NULL);

	*phenv = (HENV) EN_Constructor();
	if (!*phenv)
	{
		*phenv = SQL_NULL_HENV;
		EN_log_error(func, "Error allocating environment", NULL);
		return SQL_ERROR;
	}

	mylog("** exit PGAPI_AllocEnv: phenv = %u **\n", *phenv);
	return SQL_SUCCESS;
}


RETCODE		SQL_API
PGAPI_FreeEnv(HENV henv)
{
	static char *func = "PGAPI_FreeEnv";
	EnvironmentClass *env = (EnvironmentClass *) henv;

	mylog("**** in PGAPI_FreeEnv: env = %u ** \n", env);

	if (env && EN_Destructor(env))
	{
		mylog("   ok\n");
		return SQL_SUCCESS;
	}

	mylog("    error\n");
	EN_log_error(func, "Error freeing environment", env);
	return SQL_ERROR;
}


/*		Returns the next SQL error information. */
RETCODE		SQL_API
PGAPI_Error(
			HENV henv,
			HDBC hdbc,
			HSTMT hstmt,
			UCHAR FAR * szSqlState,
			SDWORD FAR * pfNativeError,
			UCHAR FAR * szErrorMsg,
			SWORD cbErrorMsgMax,
			SWORD FAR * pcbErrorMsg)
{
	char	   *msg;
	int			status;
	BOOL		once_again = FALSE;
	SWORD		msglen;

	mylog("**** PGAPI_Error: henv=%u, hdbc=%u, hstmt=%u <%d>\n", henv, hdbc, hstmt, cbErrorMsgMax);

	if (cbErrorMsgMax < 0)
		return SQL_ERROR;
	if (SQL_NULL_HSTMT != hstmt)
	{
		/* CC: return an error of a hstmt  */
		StatementClass *stmt = (StatementClass *) hstmt;

		if (SC_get_error(stmt, &status, &msg))
		{
			mylog("SC_get_error: status = %d, msg = #%s#\n", status, msg);
			if (NULL == msg)
			{
				if (NULL != szSqlState)
					strcpy(szSqlState, "00000");
				if (NULL != pcbErrorMsg)
					*pcbErrorMsg = 0;
				if ((NULL != szErrorMsg) && (cbErrorMsgMax > 0))
					szErrorMsg[0] = '\0';

				return SQL_NO_DATA_FOUND;
			}
			msglen = (SWORD) strlen(msg);
			if (NULL != pcbErrorMsg)
			{
				*pcbErrorMsg = msglen;
				if (cbErrorMsgMax == 0)
					once_again = TRUE;
				else if (msglen >= cbErrorMsgMax)
				{
					once_again = TRUE;
					*pcbErrorMsg = cbErrorMsgMax - 1;
				}
			}

			if ((NULL != szErrorMsg) && (cbErrorMsgMax > 0))
				strncpy_null(szErrorMsg, msg, cbErrorMsgMax);

			if (NULL != pfNativeError)
				*pfNativeError = status;

			if (NULL != szSqlState)

				switch (status)
				{
						/* now determine the SQLSTATE to be returned */
					case STMT_ROW_VERSION_CHANGED:
						strcpy(szSqlState, "01001");
						/* data truncated */
						break;
					case STMT_TRUNCATED:
						strcpy(szSqlState, "01004");
						/* data truncated */
						break;
					case STMT_INFO_ONLY:
						strcpy(szSqlState, "00000");
						/* just information that is returned, no error */
						break;
					case STMT_BAD_ERROR:
						strcpy(szSqlState, "08S01");
						/* communication link failure */
						break;
					case STMT_CREATE_TABLE_ERROR:
						strcpy(szSqlState, "S0001");
						/* table already exists */
						break;
					case STMT_STATUS_ERROR:
					case STMT_SEQUENCE_ERROR:
						strcpy(szSqlState, "S1010");
						/* Function sequence error */
						break;
					case STMT_NO_MEMORY_ERROR:
						strcpy(szSqlState, "S1001");
						/* memory allocation failure */
						break;
					case STMT_COLNUM_ERROR:
						strcpy(szSqlState, "S1002");
						/* invalid column number */
						break;
					case STMT_NO_STMTSTRING:
						strcpy(szSqlState, "S1001");
						/* having no stmtstring is also a malloc problem */
						break;
					case STMT_ERROR_TAKEN_FROM_BACKEND:
						strcpy(szSqlState, "S1000");
						/* general error */
						break;
					case STMT_INTERNAL_ERROR:
						strcpy(szSqlState, "S1000");
						/* general error */
						break;
					case STMT_ROW_OUT_OF_RANGE:
						strcpy(szSqlState, "S1107");
						break;

					case STMT_OPERATION_CANCELLED:
						strcpy(szSqlState, "S1008");
						break;

					case STMT_NOT_IMPLEMENTED_ERROR:
						strcpy(szSqlState, "S1C00");	/* == 'driver not
														 * capable' */
						break;
					case STMT_OPTION_OUT_OF_RANGE_ERROR:
						strcpy(szSqlState, "S1092");
						break;
					case STMT_BAD_PARAMETER_NUMBER_ERROR:
						strcpy(szSqlState, "S1093");
						break;
					case STMT_INVALID_COLUMN_NUMBER_ERROR:
						strcpy(szSqlState, "S1002");
						break;
					case STMT_RESTRICTED_DATA_TYPE_ERROR:
						strcpy(szSqlState, "07006");
						break;
					case STMT_INVALID_CURSOR_STATE_ERROR:
						strcpy(szSqlState, "24000");
						break;
					case STMT_OPTION_VALUE_CHANGED:
						strcpy(szSqlState, "01S02");
						break;
					case STMT_POS_BEFORE_RECORDSET:
						strcpy(szSqlState, "01S06");
						break;
					case STMT_INVALID_CURSOR_NAME:
						strcpy(szSqlState, "34000");
						break;
					case STMT_NO_CURSOR_NAME:
						strcpy(szSqlState, "S1015");
						break;
					case STMT_INVALID_ARGUMENT_NO:
						strcpy(szSqlState, "S1009");
						/* invalid argument value */
						break;
					case STMT_INVALID_CURSOR_POSITION:
						strcpy(szSqlState, "S1109");
						break;
					case STMT_VALUE_OUT_OF_RANGE:
						strcpy(szSqlState, "22003");
						break;
					case STMT_OPERATION_INVALID:
						strcpy(szSqlState, "S1011");
						break;
					case STMT_INVALID_OPTION_IDENTIFIER:
						strcpy(szSqlState, "HY092");
						break;
					case STMT_EXEC_ERROR:
					default:
						strcpy(szSqlState, "S1000");
						/* also a general error */
						break;
				}
			mylog("       szSqlState = '%s', szError='%s'\n", szSqlState, szErrorMsg);
		}
		else
		{
			if (NULL != szSqlState)
				strcpy(szSqlState, "00000");
			if (NULL != pcbErrorMsg)
				*pcbErrorMsg = 0;
			if ((NULL != szErrorMsg) && (cbErrorMsgMax > 0))
				szErrorMsg[0] = '\0';

			mylog("       returning NO_DATA_FOUND\n");

			return SQL_NO_DATA_FOUND;
		}

		if (once_again)
		{
			int			outlen;

			stmt->errornumber = status;
			if (cbErrorMsgMax > 0)
				outlen = *pcbErrorMsg;
			else
				outlen = 0;
			if (!stmt->errormsg_malloced || !stmt->errormsg)
			{
				stmt->errormsg = malloc(msglen - outlen + 1);
				stmt->errormsg_malloced = TRUE;
			}
			memmove(stmt->errormsg, msg + outlen, msglen - outlen + 1);
		}
		else if (stmt->errormsg_malloced)
			SC_clear_error(stmt);
		if (cbErrorMsgMax == 0)
			return SQL_SUCCESS_WITH_INFO;
		else
			return SQL_SUCCESS;
	}
	else if (SQL_NULL_HDBC != hdbc)
	{
		ConnectionClass *conn = (ConnectionClass *) hdbc;

		mylog("calling CC_get_error\n");
		if (CC_get_error(conn, &status, &msg))
		{
			mylog("CC_get_error: status = %d, msg = #%s#\n", status, msg);
			if (NULL == msg)
			{
				if (NULL != szSqlState)
					strcpy(szSqlState, "00000");
				if (NULL != pcbErrorMsg)
					*pcbErrorMsg = 0;
				if ((NULL != szErrorMsg) && (cbErrorMsgMax > 0))
					szErrorMsg[0] = '\0';

				return SQL_NO_DATA_FOUND;
			}

			msglen = strlen(msg);
			if (NULL != pcbErrorMsg)
			{
				*pcbErrorMsg = msglen;
				if (cbErrorMsgMax == 0)
					once_again = TRUE;
				else if (msglen >= cbErrorMsgMax)
					*pcbErrorMsg = cbErrorMsgMax - 1;
			}
			if ((NULL != szErrorMsg) && (cbErrorMsgMax > 0))
				strncpy_null(szErrorMsg, msg, cbErrorMsgMax);
			if (NULL != pfNativeError)
				*pfNativeError = status;

			if (NULL != szSqlState)
				switch (status)
				{
					case STMT_OPTION_VALUE_CHANGED:
					case CONN_OPTION_VALUE_CHANGED:
						strcpy(szSqlState, "01S02");
						break;
					case STMT_TRUNCATED:
					case CONN_TRUNCATED:
						strcpy(szSqlState, "01004");
						/* data truncated */
						break;
					case CONN_INIREAD_ERROR:
						strcpy(szSqlState, "IM002");
						/* data source not found */
						break;
					case CONN_OPENDB_ERROR:
						strcpy(szSqlState, "08001");
						/* unable to connect to data source */
						break;
					case CONN_INVALID_AUTHENTICATION:
					case CONN_AUTH_TYPE_UNSUPPORTED:
						strcpy(szSqlState, "28000");
						break;
					case CONN_STMT_ALLOC_ERROR:
						strcpy(szSqlState, "S1001");
						/* memory allocation failure */
						break;
					case CONN_IN_USE:
						strcpy(szSqlState, "S1000");
						/* general error */
						break;
					case CONN_UNSUPPORTED_OPTION:
						strcpy(szSqlState, "IM001");
						/* driver does not support this function */
					case CONN_INVALID_ARGUMENT_NO:
						strcpy(szSqlState, "S1009");
						/* invalid argument value */
						break;
					case CONN_TRANSACT_IN_PROGRES:
						strcpy(szSqlState, "S1010");

						/*
						 * when the user tries to switch commit mode in a
						 * transaction
						 */
						/* -> function sequence error */
						break;
					case CONN_NO_MEMORY_ERROR:
						strcpy(szSqlState, "S1001");
						break;
					case CONN_NOT_IMPLEMENTED_ERROR:
					case STMT_NOT_IMPLEMENTED_ERROR:
						strcpy(szSqlState, "S1C00");
						break;
					case CONN_VALUE_OUT_OF_RANGE:
					case STMT_VALUE_OUT_OF_RANGE:
						strcpy(szSqlState, "22003");
						break;
					default:
						strcpy(szSqlState, "S1000");
						/* general error */
						break;
				}
		}
		else
		{
			mylog("CC_Get_error returned nothing.\n");
			if (NULL != szSqlState)
				strcpy(szSqlState, "00000");
			if (NULL != pcbErrorMsg)
				*pcbErrorMsg = 0;
			if ((NULL != szErrorMsg) && (cbErrorMsgMax > 0))
				szErrorMsg[0] = '\0';

			return SQL_NO_DATA_FOUND;
		}

		if (once_again)
		{
			conn->errornumber = status;
			return SQL_SUCCESS_WITH_INFO;
		}
		else
			return SQL_SUCCESS;
	}
	else if (SQL_NULL_HENV != henv)
	{
		EnvironmentClass *env = (EnvironmentClass *) henv;

		if (EN_get_error(env, &status, &msg))
		{
			mylog("EN_get_error: status = %d, msg = #%s#\n", status, msg);
			if (NULL == msg)
			{
				if (NULL != szSqlState)
					strcpy(szSqlState, "00000");
				if (NULL != pcbErrorMsg)
					*pcbErrorMsg = 0;
				if ((NULL != szErrorMsg) && (cbErrorMsgMax > 0))
					szErrorMsg[0] = '\0';

				return SQL_NO_DATA_FOUND;
			}

			if (NULL != pcbErrorMsg)
				*pcbErrorMsg = (SWORD) strlen(msg);
			if ((NULL != szErrorMsg) && (cbErrorMsgMax > 0))
				strncpy_null(szErrorMsg, msg, cbErrorMsgMax);
			if (NULL != pfNativeError)
				*pfNativeError = status;

			if (szSqlState)
			{
				switch (status)
				{
					case ENV_ALLOC_ERROR:
						/* memory allocation failure */
						strcpy(szSqlState, "S1001");
						break;
					default:
						strcpy(szSqlState, "S1000");
						/* general error */
						break;
				}
			}
		}
		else
		{
			if (NULL != szSqlState)
				strcpy(szSqlState, "00000");
			if (NULL != pcbErrorMsg)
				*pcbErrorMsg = 0;
			if ((NULL != szErrorMsg) && (cbErrorMsgMax > 0))
				szErrorMsg[0] = '\0';

			return SQL_NO_DATA_FOUND;
		}

		return SQL_SUCCESS;
	}

	if (NULL != szSqlState)
		strcpy(szSqlState, "00000");
	if (NULL != pcbErrorMsg)
		*pcbErrorMsg = 0;
	if ((NULL != szErrorMsg) && (cbErrorMsgMax > 0))
		szErrorMsg[0] = '\0';

	return SQL_NO_DATA_FOUND;
}


/*
 * EnvironmentClass implementation
 */
EnvironmentClass *
EN_Constructor(void)
{
	EnvironmentClass *rv;

	rv = (EnvironmentClass *) malloc(sizeof(EnvironmentClass));
	if (rv)
	{
		rv->errormsg = 0;
		rv->errornumber = 0;
	}

	return rv;
}


char
EN_Destructor(EnvironmentClass *self)
{
	int			lf;
	char		rv = 1;

	mylog("in EN_Destructor, self=%u\n", self);

	/*
	 * the error messages are static strings distributed throughout the
	 * source--they should not be freed
	 */

	/* Free any connections belonging to this environment */
	for (lf = 0; lf < MAX_CONNECTIONS; lf++)
	{
		if (conns[lf] && conns[lf]->henv == self)
			rv = rv && CC_Destructor(conns[lf]);
	}
	free(self);

	mylog("exit EN_Destructor: rv = %d\n", rv);
#ifdef	_MEMORY_DEBUG_
	debug_memory_inouecheck();
#endif   /* _MEMORY_DEBUG_ */
	return rv;
}


char
EN_get_error(EnvironmentClass *self, int *number, char **message)
{
	if (self && self->errormsg && self->errornumber)
	{
		*message = self->errormsg;
		*number = self->errornumber;
		self->errormsg = 0;
		self->errornumber = 0;
		return 1;
	}
	else
		return 0;
}


char
EN_add_connection(EnvironmentClass *self, ConnectionClass *conn)
{
	int			i;

	mylog("EN_add_connection: self = %u, conn = %u\n", self, conn);

	for (i = 0; i < MAX_CONNECTIONS; i++)
	{
		if (!conns[i])
		{
			conn->henv = self;
			conns[i] = conn;

			mylog("       added at i =%d, conn->henv = %u, conns[i]->henv = %u\n", i, conn->henv, conns[i]->henv);

			return TRUE;
		}
	}

	return FALSE;
}


char
EN_remove_connection(EnvironmentClass *self, ConnectionClass *conn)
{
	int			i;

	for (i = 0; i < MAX_CONNECTIONS; i++)
		if (conns[i] == conn && conns[i]->status != CONN_EXECUTING)
		{
			conns[i] = NULL;
			return TRUE;
		}

	return FALSE;
}


void
EN_log_error(char *func, char *desc, EnvironmentClass *self)
{
	if (self)
		qlog("ENVIRON ERROR: func=%s, desc='%s', errnum=%d, errmsg='%s'\n", func, desc, self->errornumber, self->errormsg);
	else
		qlog("INVALID ENVIRON HANDLE ERROR: func=%s, desc='%s'\n", func, desc);
}
