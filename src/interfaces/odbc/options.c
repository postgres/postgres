
/* Module:			options.c
 *
 * Description:		This module contains routines for getting/setting
 *					connection and statement options.
 *
 * Classes:			n/a
 *
 * API functions:	SQLSetConnectOption, SQLSetStmtOption, SQLGetConnectOption,
 *					SQLGetStmtOption
 *
 * Comments:		See "notice.txt" for copyright and license information.
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "psqlodbc.h"
#include <string.h>

#ifndef WIN32
#include "iodbc.h"
#include "isql.h"
#include "isqlext.h"
#else
#include <windows.h>
#include <sql.h>
#include <sqlext.h>
#endif

#include "environ.h"
#include "connection.h"
#include "statement.h"
#include "qresult.h"


extern GLOBAL_VALUES globals;

RETCODE set_statement_option(ConnectionClass *conn,
					 StatementClass *stmt,
					 UWORD fOption,
					 UDWORD vParam);



RETCODE
set_statement_option(ConnectionClass *conn,
					 StatementClass *stmt,
					 UWORD fOption,
					 UDWORD vParam)
{
	static char *func = "set_statement_option";
	char		changed = FALSE;


	switch (fOption)
	{
		case SQL_ASYNC_ENABLE:	/* ignored */
			break;

		case SQL_BIND_TYPE:
			/* now support multi-column and multi-row binding */
			if (conn)
				conn->stmtOptions.bind_size = vParam;
			if (stmt)
				stmt->options.bind_size = vParam;
			break;

		case SQL_CONCURRENCY:

			/*
			 * positioned update isn't supported so cursor concurrency is
			 * read-only
			 */

			if (conn)
				conn->stmtOptions.scroll_concurrency = vParam;
			if (stmt)
				stmt->options.scroll_concurrency = vParam;
			break;

			/*
			 * if (globals.lie) { if (conn)
			 * conn->stmtOptions.scroll_concurrency = vParam; if (stmt)
			 * stmt->options.scroll_concurrency = vParam; } else {
			 *
			 * if (conn) conn->stmtOptions.scroll_concurrency =
			 * SQL_CONCUR_READ_ONLY; if (stmt)
			 * stmt->options.scroll_concurrency = SQL_CONCUR_READ_ONLY;
			 *
			 * if (vParam != SQL_CONCUR_READ_ONLY) changed = TRUE; } break;
			 */

		case SQL_CURSOR_TYPE:

			/*
			 * if declare/fetch, then type can only be forward. otherwise,
			 * it can only be forward or static.
			 */
			mylog("SetStmtOption(): SQL_CURSOR_TYPE = %d\n", vParam);

			if (globals.lie)
			{

				if (conn)
					conn->stmtOptions.cursor_type = vParam;
				if (stmt)
					stmt->options.cursor_type = vParam;

			}
			else
			{
				if (globals.use_declarefetch)
				{

					if (conn)
						conn->stmtOptions.cursor_type = SQL_CURSOR_FORWARD_ONLY;
					if (stmt)
						stmt->options.cursor_type = SQL_CURSOR_FORWARD_ONLY;

					if (vParam != SQL_CURSOR_FORWARD_ONLY)
						changed = TRUE;
				}
				else
				{
					if (vParam == SQL_CURSOR_FORWARD_ONLY || vParam == SQL_CURSOR_STATIC)
					{

						if (conn)
							conn->stmtOptions.cursor_type = vParam;		/* valid type */
						if (stmt)
							stmt->options.cursor_type = vParam; /* valid type */
					}
					else
					{

						if (conn)
							conn->stmtOptions.cursor_type = SQL_CURSOR_STATIC;
						if (stmt)
							stmt->options.cursor_type = SQL_CURSOR_STATIC;

						changed = TRUE;
					}
				}
			}
			break;

		case SQL_KEYSET_SIZE:	/* ignored, but saved and returned	*/
			mylog("SetStmtOption(): SQL_KEYSET_SIZE, vParam = %d\n", vParam);

			if (conn)
				conn->stmtOptions.keyset_size = vParam;
			if (stmt)
				stmt->options.keyset_size = vParam;

			break;

			/*
			 * if (globals.lie) stmt->keyset_size = vParam; else {
			 * stmt->errornumber = STMT_NOT_IMPLEMENTED_ERROR;
			 * stmt->errormsg = "Driver does not support keyset size
			 * option"; SC_log_error(func, "", stmt); return SQL_ERROR; }
			 */

		case SQL_MAX_LENGTH:	/* ignored, but saved */
			mylog("SetStmtOption(): SQL_MAX_LENGTH, vParam = %d\n", vParam);
			if (conn)
				conn->stmtOptions.maxLength = vParam;
			if (stmt)
				stmt->options.maxLength = vParam;
			break;

		case SQL_MAX_ROWS:		/* ignored, but saved */
			mylog("SetStmtOption(): SQL_MAX_ROWS, vParam = %d\n", vParam);
			if (conn)
				conn->stmtOptions.maxRows = vParam;
			if (stmt)
				stmt->options.maxRows = vParam;
			break;

		case SQL_NOSCAN:		/* ignored */
			mylog("SetStmtOption: SQL_NOSCAN, vParam = %d\n", vParam);
			break;

		case SQL_QUERY_TIMEOUT:/* ignored */
			mylog("SetStmtOption: SQL_QUERY_TIMEOUT, vParam = %d\n", vParam);
			/* "0" returned in SQLGetStmtOption */
			break;

		case SQL_RETRIEVE_DATA:/* ignored, but saved */
			mylog("SetStmtOption(): SQL_RETRIEVE_DATA, vParam = %d\n", vParam);
			if (conn)
				conn->stmtOptions.retrieve_data = vParam;
			if (stmt)
				stmt->options.retrieve_data = vParam;
			break;

		case SQL_ROWSET_SIZE:
			mylog("SetStmtOption(): SQL_ROWSET_SIZE, vParam = %d\n", vParam);


			/*
			 * Save old rowset size for SQLExtendedFetch purposes If the
			 * rowset_size is being changed since the last call to fetch
			 * rows.
			 */

			if (stmt && stmt->save_rowset_size <= 0 && stmt->last_fetch_count > 0)
				stmt->save_rowset_size = stmt->options.rowset_size;

			if (vParam < 1)
			{
				vParam = 1;
				changed = TRUE;
			}

			if (conn)
				conn->stmtOptions.rowset_size = vParam;
			if (stmt)
				stmt->options.rowset_size = vParam;

			break;

		case SQL_SIMULATE_CURSOR:		/* NOT SUPPORTED */
			if (stmt)
			{
				stmt->errornumber = STMT_NOT_IMPLEMENTED_ERROR;
				stmt->errormsg = "Simulated positioned update/delete not supported.  Use the cursor library.";
				SC_log_error(func, "", stmt);
			}
			if (conn)
			{
				conn->errornumber = STMT_NOT_IMPLEMENTED_ERROR;
				conn->errormsg = "Simulated positioned update/delete not supported.  Use the cursor library.";
				CC_log_error(func, "", conn);
			}
			return SQL_ERROR;

		case SQL_USE_BOOKMARKS:

			if (stmt)
				stmt->options.use_bookmarks = vParam;
			if (conn)
				conn->stmtOptions.use_bookmarks = vParam;
			break;

		default:
			{
				char		option[64];

				if (stmt)
				{
					stmt->errornumber = STMT_NOT_IMPLEMENTED_ERROR;
					stmt->errormsg = "Unknown statement option (Set)";
					sprintf(option, "fOption=%d, vParam=%ld", fOption, vParam);
					SC_log_error(func, option, stmt);
				}
				if (conn)
				{
					conn->errornumber = STMT_NOT_IMPLEMENTED_ERROR;
					conn->errormsg = "Unknown statement option (Set)";
					sprintf(option, "fOption=%d, vParam=%ld", fOption, vParam);
					CC_log_error(func, option, conn);
				}

				return SQL_ERROR;
			}
	}

	if (changed)
	{
		if (stmt)
		{
			stmt->errormsg = "Requested value changed.";
			stmt->errornumber = STMT_OPTION_VALUE_CHANGED;
		}
		if (conn)
		{
			conn->errormsg = "Requested value changed.";
			conn->errornumber = STMT_OPTION_VALUE_CHANGED;
		}
		return SQL_SUCCESS_WITH_INFO;
	}
	else
		return SQL_SUCCESS;
}



/* Implements only SQL_AUTOCOMMIT */
RETCODE SQL_API
SQLSetConnectOption(
					HDBC hdbc,
					UWORD fOption,
					UDWORD vParam)
{
	static char *func = "SQLSetConnectOption";
	ConnectionClass *conn = (ConnectionClass *) hdbc;
	char		changed = FALSE;
	RETCODE		retval;
	int			i;

	mylog("%s: entering...\n", func);

	if (!conn)
	{
		CC_log_error(func, "", NULL);
		return SQL_INVALID_HANDLE;
	}


	switch (fOption)
	{

			/*
			 * Statement Options (apply to all stmts on the connection and
			 * become defaults for new stmts)
			 */
		case SQL_ASYNC_ENABLE:
		case SQL_BIND_TYPE:
		case SQL_CONCURRENCY:
		case SQL_CURSOR_TYPE:
		case SQL_KEYSET_SIZE:
		case SQL_MAX_LENGTH:
		case SQL_MAX_ROWS:
		case SQL_NOSCAN:
		case SQL_QUERY_TIMEOUT:
		case SQL_RETRIEVE_DATA:
		case SQL_ROWSET_SIZE:
		case SQL_SIMULATE_CURSOR:
		case SQL_USE_BOOKMARKS:

			/* Affect all current Statements */
			for (i = 0; i < conn->num_stmts; i++)
			{
				if (conn->stmts[i])
					set_statement_option(NULL, conn->stmts[i], fOption, vParam);
			}

			/*
			 * Become the default for all future statements on this
			 * connection
			 */
			retval = set_statement_option(conn, NULL, fOption, vParam);

			if (retval == SQL_SUCCESS_WITH_INFO)
				changed = TRUE;
			else if (retval == SQL_ERROR)
				return SQL_ERROR;

			break;

			/**********************************/
			/*****	Connection Options	*******/
			/**********************************/

		case SQL_ACCESS_MODE:	/* ignored */
			break;

		case SQL_AUTOCOMMIT:

			if (CC_is_in_trans(conn))
			{
				conn->errormsg = "Cannot switch commit mode while a transaction is in progress";
				conn->errornumber = CONN_TRANSACT_IN_PROGRES;
				CC_log_error(func, "", conn);
				return SQL_ERROR;
			}

			mylog("SQLSetConnectOption: AUTOCOMMIT: transact_status=%d, vparam=%d\n", conn->transact_status, vParam);

			switch (vParam)
			{
				case SQL_AUTOCOMMIT_OFF:
					CC_set_autocommit_off(conn);
					break;

				case SQL_AUTOCOMMIT_ON:
					CC_set_autocommit_on(conn);
					break;

				default:
					conn->errormsg = "Illegal parameter value for SQL_AUTOCOMMIT";
					conn->errornumber = CONN_INVALID_ARGUMENT_NO;
					CC_log_error(func, "", conn);
					return SQL_ERROR;
			}

			break;

		case SQL_CURRENT_QUALIFIER:		/* ignored */
			break;

		case SQL_LOGIN_TIMEOUT:/* ignored */
			break;

		case SQL_PACKET_SIZE:	/* ignored */
			break;

		case SQL_QUIET_MODE:	/* ignored */
			break;

		case SQL_TXN_ISOLATION:/* ignored */
			break;

			/* These options should be handled by driver manager */
		case SQL_ODBC_CURSORS:
		case SQL_OPT_TRACE:
		case SQL_OPT_TRACEFILE:
		case SQL_TRANSLATE_DLL:
		case SQL_TRANSLATE_OPTION:
			CC_log_error(func, "This connect option (Set) is only used by the Driver Manager", conn);
			break;

		default:
			{
				char		option[64];

				conn->errormsg = "Unknown connect option (Set)";
				conn->errornumber = CONN_UNSUPPORTED_OPTION;
				sprintf(option, "fOption=%d, vParam=%ld", fOption, vParam);
				CC_log_error(func, option, conn);
				return SQL_ERROR;
			}

	}

	if (changed)
	{
		conn->errornumber = CONN_OPTION_VALUE_CHANGED;
		conn->errormsg = "Requested value changed.";
		return SQL_SUCCESS_WITH_INFO;
	}
	else
		return SQL_SUCCESS;
}

/*		-		-		-		-		-		-		-		-		- */

/* This function just can tell you whether you are in Autcommit mode or not */
RETCODE SQL_API
SQLGetConnectOption(
					HDBC hdbc,
					UWORD fOption,
					PTR pvParam)
{
	static char *func = "SQLGetConnectOption";
	ConnectionClass *conn = (ConnectionClass *) hdbc;

	mylog("%s: entering...\n", func);

	if (!conn)
	{
		CC_log_error(func, "", NULL);
		return SQL_INVALID_HANDLE;
	}

	switch (fOption)
	{
		case SQL_ACCESS_MODE:	/* NOT SUPPORTED */
			*((UDWORD *) pvParam) = SQL_MODE_READ_WRITE;
			break;

		case SQL_AUTOCOMMIT:
			*((UDWORD *) pvParam) = (UDWORD) (CC_is_in_autocommit(conn) ?
								 SQL_AUTOCOMMIT_ON : SQL_AUTOCOMMIT_OFF);
			break;

		case SQL_CURRENT_QUALIFIER:		/* don't use qualifiers */
			if (pvParam)
				strcpy(pvParam, "");

			break;

		case SQL_LOGIN_TIMEOUT:/* NOT SUPPORTED */
			*((UDWORD *) pvParam) = 0;
			break;

		case SQL_PACKET_SIZE:	/* NOT SUPPORTED */
			*((UDWORD *) pvParam) = globals.socket_buffersize;
			break;

		case SQL_QUIET_MODE:	/* NOT SUPPORTED */
			*((UDWORD *) pvParam) = (UDWORD) NULL;
			break;

		case SQL_TXN_ISOLATION:/* NOT SUPPORTED */
			*((UDWORD *) pvParam) = SQL_TXN_SERIALIZABLE;
			break;

			/* These options should be handled by driver manager */
		case SQL_ODBC_CURSORS:
		case SQL_OPT_TRACE:
		case SQL_OPT_TRACEFILE:
		case SQL_TRANSLATE_DLL:
		case SQL_TRANSLATE_OPTION:
			CC_log_error(func, "This connect option (Get) is only used by the Driver Manager", conn);
			break;

		default:
			{
				char		option[64];

				conn->errormsg = "Unknown connect option (Get)";
				conn->errornumber = CONN_UNSUPPORTED_OPTION;
				sprintf(option, "fOption=%d", fOption);
				CC_log_error(func, option, conn);
				return SQL_ERROR;
				break;
			}

	}

	return SQL_SUCCESS;
}

/*		-		-		-		-		-		-		-		-		- */

RETCODE SQL_API
SQLSetStmtOption(
				 HSTMT hstmt,
				 UWORD fOption,
				 UDWORD vParam)
{
	static char *func = "SQLSetStmtOption";
	StatementClass *stmt = (StatementClass *) hstmt;

	mylog("%s: entering...\n", func);

	/* thought we could fake Access out by just returning SQL_SUCCESS */
	/* all the time, but it tries to set a huge value for SQL_MAX_LENGTH */
	/* and expects the driver to reduce it to the real value */

	if (!stmt)
	{
		SC_log_error(func, "", NULL);
		return SQL_INVALID_HANDLE;
	}

	return set_statement_option(NULL, stmt, fOption, vParam);
}


/*		-		-		-		-		-		-		-		-		- */

RETCODE SQL_API
SQLGetStmtOption(
				 HSTMT hstmt,
				 UWORD fOption,
				 PTR pvParam)
{
	static char *func = "SQLGetStmtOption";
	StatementClass *stmt = (StatementClass *) hstmt;
	QResultClass *res;

	mylog("%s: entering...\n", func);

	/* thought we could fake Access out by just returning SQL_SUCCESS */
	/* all the time, but it tries to set a huge value for SQL_MAX_LENGTH */
	/* and expects the driver to reduce it to the real value */

	if (!stmt)
	{
		SC_log_error(func, "", NULL);
		return SQL_INVALID_HANDLE;
	}

	switch (fOption)
	{
		case SQL_GET_BOOKMARK:
		case SQL_ROW_NUMBER:

			res = stmt->result;

			if (stmt->manual_result || !globals.use_declarefetch)
			{
				/* make sure we're positioned on a valid row */
				if ((stmt->currTuple < 0) ||
					(stmt->currTuple >= QR_get_num_tuples(res)))
				{
					stmt->errormsg = "Not positioned on a valid row.";
					stmt->errornumber = STMT_INVALID_CURSOR_STATE_ERROR;
					SC_log_error(func, "", stmt);
					return SQL_ERROR;
				}
			}
			else
			{
				if (stmt->currTuple == -1 || !res || !res->tupleField)
				{
					stmt->errormsg = "Not positioned on a valid row.";
					stmt->errornumber = STMT_INVALID_CURSOR_STATE_ERROR;
					SC_log_error(func, "", stmt);
					return SQL_ERROR;
				}
			}

			if (fOption == SQL_GET_BOOKMARK && stmt->options.use_bookmarks == SQL_UB_OFF)
			{
				stmt->errormsg = "Operation invalid because use bookmarks not enabled.";
				stmt->errornumber = STMT_OPERATION_INVALID;
				SC_log_error(func, "", stmt);
				return SQL_ERROR;
			}

			*((UDWORD *) pvParam) = SC_get_bookmark(stmt);

			break;

		case SQL_ASYNC_ENABLE:	/* NOT SUPPORTED */
			*((SDWORD *) pvParam) = SQL_ASYNC_ENABLE_OFF;
			break;

		case SQL_BIND_TYPE:
			*((SDWORD *) pvParam) = stmt->options.bind_size;
			break;

		case SQL_CONCURRENCY:	/* NOT REALLY SUPPORTED */
			mylog("GetStmtOption(): SQL_CONCURRENCY\n");
			*((SDWORD *) pvParam) = stmt->options.scroll_concurrency;
			break;

		case SQL_CURSOR_TYPE:	/* PARTIAL SUPPORT */
			mylog("GetStmtOption(): SQL_CURSOR_TYPE\n");
			*((SDWORD *) pvParam) = stmt->options.cursor_type;
			break;

		case SQL_KEYSET_SIZE:	/* NOT SUPPORTED, but saved */
			mylog("GetStmtOption(): SQL_KEYSET_SIZE\n");
			*((SDWORD *) pvParam) = stmt->options.keyset_size;
			break;

		case SQL_MAX_LENGTH:	/* NOT SUPPORTED, but saved */
			*((SDWORD *) pvParam) = stmt->options.maxLength;
			break;

		case SQL_MAX_ROWS:		/* NOT SUPPORTED, but saved */
			*((SDWORD *) pvParam) = stmt->options.maxRows;
			mylog("GetSmtOption: MAX_ROWS, returning %d\n", stmt->options.maxRows);
			break;

		case SQL_NOSCAN:		/* NOT SUPPORTED */
			*((SDWORD *) pvParam) = SQL_NOSCAN_ON;
			break;

		case SQL_QUERY_TIMEOUT:/* NOT SUPPORTED */
			*((SDWORD *) pvParam) = 0;
			break;

		case SQL_RETRIEVE_DATA:/* NOT SUPPORTED, but saved */
			*((SDWORD *) pvParam) = stmt->options.retrieve_data;
			break;

		case SQL_ROWSET_SIZE:
			*((SDWORD *) pvParam) = stmt->options.rowset_size;
			break;

		case SQL_SIMULATE_CURSOR:		/* NOT SUPPORTED */
			*((SDWORD *) pvParam) = SQL_SC_NON_UNIQUE;
			break;

		case SQL_USE_BOOKMARKS:
			*((SDWORD *) pvParam) = stmt->options.use_bookmarks;
			break;

		default:
			{
				char		option[64];

				stmt->errornumber = STMT_NOT_IMPLEMENTED_ERROR;
				stmt->errormsg = "Unknown statement option (Get)";
				sprintf(option, "fOption=%d", fOption);
				SC_log_error(func, option, stmt);
				return SQL_ERROR;
			}
	}

	return SQL_SUCCESS;
}

/*		-		-		-		-		-		-		-		-		- */
