
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
#include "environ.h"
#include "connection.h"
#include "statement.h"

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

RETCODE SQL_API SQLSetStmtOption(
        HSTMT   hstmt,
        UWORD   fOption,
        UDWORD  vParam)
{
StatementClass *stmt = (StatementClass *) hstmt;

    // thought we could fake Access out by just returning SQL_SUCCESS
    // all the time, but it tries to set a huge value for SQL_MAX_LENGTH
    // and expects the driver to reduce it to the real value

    if( ! stmt) {
        return SQL_INVALID_HANDLE;
    }

    switch(fOption) {
    case SQL_QUERY_TIMEOUT:
		mylog("SetStmtOption: vParam = %d\n", vParam);
		/*
		stmt->errornumber = STMT_OPTION_VALUE_CHANGED;
		stmt->errormsg = "Query Timeout:  value changed to 0";
		return SQL_SUCCESS_WITH_INFO;
		*/
		return SQL_SUCCESS;
        break;
    case SQL_MAX_LENGTH:
/* CC: Some apps consider returning SQL_SUCCESS_WITH_INFO to be an error */
/* so if we're going to return SQL_SUCCESS, we better not set an */
/* error message.  (otherwise, if a subsequent function call returns */
/* SQL_ERROR without setting a message, things can get confused.) */

      /*
        stmt->errormsg = "Requested value changed.";
        stmt->errornumber = STMT_OPTION_VALUE_CHANGED;
       */

        return SQL_SUCCESS;
        break;
	case SQL_MAX_ROWS:
		mylog("SetStmtOption(): SQL_MAX_ROWS = %d, returning success\n", vParam);
		stmt->maxRows = vParam;
		return SQL_SUCCESS;
		break;
    default:
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
      /* CC 28.05.96: Do not set fOption, but pvParam */
        *((UDWORD *)pvParam) = (UDWORD)( CC_is_in_autocommit(conn) ?
                                        SQL_AUTOCOMMIT_ON : SQL_AUTOCOMMIT_OFF);
      break;
      /* we don't use qualifiers */
    case SQL_CURRENT_QUALIFIER:
      if(pvParam) {
	strcpy(pvParam, "");
      }
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

RETCODE SQL_API SQLGetStmtOption(
        HSTMT   hstmt,
        UWORD   fOption,
        PTR     pvParam)
{
StatementClass *stmt = (StatementClass *) hstmt;

    // thought we could fake Access out by just returning SQL_SUCCESS
    // all the time, but it tries to set a huge value for SQL_MAX_LENGTH
    // and expects the driver to reduce it to the real value

    if( ! stmt) {
        return SQL_INVALID_HANDLE;
    }

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
    default:
        return SQL_ERROR;
    }

    return SQL_SUCCESS;
}

//      -       -       -       -       -       -       -       -       -
