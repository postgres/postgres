
/* Module:          options.c
 *
 * Description:     This module contains routines for getting/setting
 *                  connection and statement options.
 *
 * Classes:         n/a
 *
 * API functions:   SQLSetConnectOption, SQLSetStmtOption, SQLGetConnectOption,
 *                  SQLGetStmtOption
 *
 * Comments:        See "notice.txt" for copyright and license information.
 *
 */

#include "psqlodbc.h"
#include <windows.h>
#include <sql.h>
#include <sqlext.h>
#include "environ.h"
#include "connection.h"
#include "statement.h"


extern GLOBAL_VALUES globals;


/* Implements only SQL_AUTOCOMMIT */
RETCODE SQL_API SQLSetConnectOption(
        HDBC    hdbc,
        UWORD   fOption,
        UDWORD  vParam)
{
ConnectionClass *conn = (ConnectionClass *) hdbc;

	if ( ! conn) 
		return SQL_INVALID_HANDLE;

	switch (fOption) {
	case SQL_AUTOCOMMIT:

		/*  Since we are almost always in a transaction, this is now ok.
			Even if we were, the logic will handle it by sending a commit
			after the statement.
		
		if (CC_is_in_trans(conn)) {
			conn->errormsg = "Cannot switch commit mode while a transaction is in progres";
			conn->errornumber = CONN_TRANSACT_IN_PROGRES;
			return SQL_ERROR;
		}
		*/

		mylog("SQLSetConnectOption: AUTOCOMMIT: transact_status=%d, vparam=%d\n", conn->transact_status, vParam);

		switch(vParam) {
		case SQL_AUTOCOMMIT_OFF:
			CC_set_autocommit_off(conn);
			break;

		case SQL_AUTOCOMMIT_ON:
			CC_set_autocommit_on(conn);
			break;

		default:
			conn->errormsg = "Illegal parameter value for SQL_AUTOCOMMIT";
			conn->errornumber = CONN_INVALID_ARGUMENT_NO;
			return SQL_ERROR;
		}

		break;

	case SQL_LOGIN_TIMEOUT:
		break;

	case SQL_ACCESS_MODE:
		break;

	default:
		conn->errormsg = "This option is currently unsupported by the driver";
		conn->errornumber = CONN_UNSUPPORTED_OPTION;
		return SQL_ERROR;

	}    
	return SQL_SUCCESS;
}

//      -       -       -       -       -       -       -       -       -

/* This function just can tell you whether you are in Autcommit mode or not */
RETCODE SQL_API SQLGetConnectOption(
        HDBC    hdbc,
        UWORD   fOption,
        PTR     pvParam)
{
ConnectionClass *conn = (ConnectionClass *) hdbc;

	if (! conn) 
		return SQL_INVALID_HANDLE;

	switch (fOption) {
	case SQL_AUTOCOMMIT:
		*((UDWORD *)pvParam) = (UDWORD)( CC_is_in_autocommit(conn) ?
						SQL_AUTOCOMMIT_ON : SQL_AUTOCOMMIT_OFF);
		break;

	/* don't use qualifiers */
	case SQL_CURRENT_QUALIFIER:
		if(pvParam)
			strcpy(pvParam, "");

		break;

	default:
		conn->errormsg = "This option is currently unsupported by the driver";
		conn->errornumber = CONN_UNSUPPORTED_OPTION;
		return SQL_ERROR;
		break;

	}    

	return SQL_SUCCESS;
}

//      -       -       -       -       -       -       -       -       -

RETCODE SQL_API SQLSetStmtOption(
        HSTMT   hstmt,
        UWORD   fOption,
        UDWORD  vParam)
{
StatementClass *stmt = (StatementClass *) hstmt;
char changed = FALSE;

    // thought we could fake Access out by just returning SQL_SUCCESS
    // all the time, but it tries to set a huge value for SQL_MAX_LENGTH
    // and expects the driver to reduce it to the real value

	if( ! stmt)
		return SQL_INVALID_HANDLE;

	switch(fOption) {
	case SQL_QUERY_TIMEOUT:
		mylog("SetStmtOption: vParam = %d\n", vParam);
		//	"0" returned in SQLGetStmtOption
		break;

	case SQL_MAX_LENGTH:
		//	"4096" returned in SQLGetStmtOption
		break;

	case SQL_MAX_ROWS:
		mylog("SetStmtOption(): SQL_MAX_ROWS = %d, returning success\n", vParam);
		stmt->maxRows = vParam;
		return SQL_SUCCESS;
		break;

	case SQL_ROWSET_SIZE:
		mylog("SetStmtOption(): SQL_ROWSET_SIZE = %d\n", vParam);

		stmt->rowset_size = 1;		// only support 1 row at a time
		if (vParam != 1) 
			changed = TRUE;

		break;

	case SQL_KEYSET_SIZE:
		mylog("SetStmtOption(): SQL_KEYSET_SIZE = %d\n", vParam);
		if (globals.lie)
			stmt->keyset_size = vParam;
		else {
			stmt->errornumber = STMT_NOT_IMPLEMENTED_ERROR;
			stmt->errormsg = "Driver does not support keyset size option";
			return SQL_ERROR;
		}
		break;

	case SQL_CONCURRENCY:
		//	positioned update isn't supported so cursor concurrency is read-only
		mylog("SetStmtOption(): SQL_CONCURRENCY = %d\n", vParam);

		if (globals.lie)
			stmt->scroll_concurrency = vParam;
		else {
			stmt->scroll_concurrency = SQL_CONCUR_READ_ONLY;
			if (vParam != SQL_CONCUR_READ_ONLY)
				changed = TRUE;
		}
		break;
		
	case SQL_CURSOR_TYPE:
		//	if declare/fetch, then type can only be forward.
		//	otherwise, it can only be forward or static.
		mylog("SetStmtOption(): SQL_CURSOR_TYPE = %d\n", vParam);

		if (globals.lie)
			stmt->cursor_type = vParam;
		else {
			if (globals.use_declarefetch) {
				stmt->cursor_type = SQL_CURSOR_FORWARD_ONLY;
				if (vParam != SQL_CURSOR_FORWARD_ONLY) 
					changed = TRUE;
			}
			else {
				if (vParam == SQL_CURSOR_FORWARD_ONLY || vParam == SQL_CURSOR_STATIC)
					stmt->cursor_type = vParam;		// valid type
				else {
					stmt->cursor_type = SQL_CURSOR_STATIC;
					changed = TRUE;
				}
			}
		}
		break;

	case SQL_SIMULATE_CURSOR:
		stmt->errornumber = STMT_NOT_IMPLEMENTED_ERROR;
		stmt->errormsg = "Simulated positioned update/delete not supported.  Use the cursor library.";
		return SQL_ERROR;

    default:
		stmt->errornumber = STMT_NOT_IMPLEMENTED_ERROR;
		stmt->errormsg = "Driver does not support this statement option";
        return SQL_ERROR;
    }

	if (changed) {
		stmt->errormsg = "Requested value changed.";
		stmt->errornumber = STMT_OPTION_VALUE_CHANGED;
		return SQL_SUCCESS_WITH_INFO;
	}
	else
		return SQL_SUCCESS;
}


//      -       -       -       -       -       -       -       -       -

RETCODE SQL_API SQLGetStmtOption(
        HSTMT   hstmt,
        UWORD   fOption,
        PTR     pvParam)
{
StatementClass *stmt = (StatementClass *) hstmt;

    // thought we could fake Access out by just returning SQL_SUCCESS
    // all the time, but it tries to set a huge value for SQL_MAX_LENGTH
    // and expects the driver to reduce it to the real value

	if( ! stmt)
		return SQL_INVALID_HANDLE;

	switch(fOption) {
	case SQL_QUERY_TIMEOUT:
		// how long we wait on a query before returning to the
		// application (0 == forever)
		*((SDWORD *)pvParam) = 0;
		break;

	case SQL_MAX_LENGTH:
		// what is the maximum length that will be returned in
		// a single column
		*((SDWORD *)pvParam) = 4096;
		break;

	case SQL_MAX_ROWS:
		*((SDWORD *)pvParam) = stmt->maxRows;
		mylog("GetSmtOption: MAX_ROWS, returning %d\n", stmt->maxRows);
		break;

	case SQL_ROWSET_SIZE:
		mylog("GetStmtOption(): SQL_ROWSET_SIZE\n");
		*((SDWORD *)pvParam) = stmt->rowset_size;
		break;

	case SQL_KEYSET_SIZE:
		mylog("GetStmtOption(): SQL_KEYSET_SIZE\n");
		*((SDWORD *)pvParam) = stmt->keyset_size;
		break;

	case SQL_CONCURRENCY:
		mylog("GetStmtOption(): SQL_CONCURRENCY\n");
		*((SDWORD *)pvParam) = stmt->scroll_concurrency;
		break;

	case SQL_CURSOR_TYPE:
		mylog("GetStmtOption(): SQL_CURSOR_TYPE\n");
		*((SDWORD *)pvParam) = stmt->cursor_type;
		break;

	case SQL_SIMULATE_CURSOR:
		stmt->errornumber = STMT_NOT_IMPLEMENTED_ERROR;
		stmt->errormsg = "Simulated positioned update/delete not supported. Use the cursor library.";
		return SQL_ERROR;

	default:
		stmt->errornumber = STMT_NOT_IMPLEMENTED_ERROR;
		stmt->errormsg = "Driver does not support this statement option";
		return SQL_ERROR;
	}

	return SQL_SUCCESS;
}

//      -       -       -       -       -       -       -       -       -
