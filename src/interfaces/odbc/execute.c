
/* Module:          execute.c
 *
 * Description:     This module contains routines related to 
 *                  preparing and executing an SQL statement.
 *
 * Classes:         n/a
 *
 * API functions:   SQLPrepare, SQLExecute, SQLExecDirect, SQLTransact,
 *                  SQLCancel, SQLNativeSql, SQLParamData, SQLPutData
 *
 * Comments:        See "notice.txt" for copyright and license information.
 *
 */

#include "psqlodbc.h"
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <sqlext.h>

#include "connection.h"
#include "statement.h"
#include "qresult.h"
#include "convert.h"
#include "bind.h"


//      Perform a Prepare on the SQL statement
RETCODE SQL_API SQLPrepare(HSTMT     hstmt,
                           UCHAR FAR *szSqlStr,
                           SDWORD    cbSqlStr)
{
StatementClass *self = (StatementClass *) hstmt;

	if ( ! self)
		return SQL_INVALID_HANDLE;
    
  /* CC: According to the ODBC specs it is valid to call SQLPrepare mulitple times. In that case,
         the bound SQL statement is replaced by the new one */

	switch (self->status) {
	case STMT_PREMATURE:
		mylog("**** SQLPrepare: STMT_PREMATURE, recycle\n");

		SC_recycle_statement(self); /* recycle the statement, but do not remove parameter bindings */

		/* NO Break! -- Contiue the same way as with a newly allocated statement ! */

	case STMT_ALLOCATED:
		// it is not really necessary to do any conversion of the statement
		// here--just copy it, and deal with it when it's ready to be
		// executed.
		mylog("**** SQLPrepare: STMT_ALLOCATED, copy\n");

		self->statement = make_string(szSqlStr, cbSqlStr, NULL);
		if ( ! self->statement) {
			self->errornumber = STMT_NO_MEMORY_ERROR;
			self->errormsg = "No memory available to store statement";
			return SQL_ERROR;
		}

		self->statement_type = statement_type(self->statement);

		//	Check if connection is readonly (only selects are allowed)
		if ( CC_is_readonly(self->hdbc) && self->statement_type != STMT_TYPE_SELECT ) {
			self->errornumber = STMT_EXEC_ERROR;
			self->errormsg = "Connection is readonly, only select statements are allowed.";
			return SQL_ERROR;
		}

		self->prepare = TRUE;
		self->status = STMT_READY;

		return SQL_SUCCESS;
    
	case STMT_READY:  /* SQLPrepare has already been called -- Just changed the SQL statement that is assigned to the handle */
		mylog("**** SQLPrepare: STMT_READY, change SQL\n");

		if (self->statement)
			free(self->statement);

		self->statement = make_string(szSqlStr, cbSqlStr, NULL);
		if ( ! self->statement) {
			self->errornumber = STMT_NO_MEMORY_ERROR;
			self->errormsg = "No memory available to store statement";
			return SQL_ERROR;
		}

		self->prepare = TRUE;
		self->statement_type = statement_type(self->statement);

		//	Check if connection is readonly (only selects are allowed)
		if ( CC_is_readonly(self->hdbc) && self->statement_type != STMT_TYPE_SELECT ) {
			self->errornumber = STMT_EXEC_ERROR;
			self->errormsg = "Connection is readonly, only select statements are allowed.";
			return SQL_ERROR;
		}

		return SQL_SUCCESS;
                            
	case STMT_FINISHED:
		mylog("**** SQLPrepare: STMT_FINISHED\n");
		/* No BREAK:  continue as with STMT_EXECUTING */

	case STMT_EXECUTING:
		mylog("**** SQLPrepare: STMT_EXECUTING, error!\n");

		self->errornumber = STMT_SEQUENCE_ERROR;
		self->errormsg = "SQLPrepare(): The handle does not point to a statement that is ready to be executed";

		return SQL_ERROR;

	default:
		self->errornumber = STMT_INTERNAL_ERROR;
		self->errormsg = "An Internal Error has occured -- Unknown statement status.";
		return SQL_ERROR;
	}
}

//      -       -       -       -       -       -       -       -       -

//      Performs the equivalent of SQLPrepare, followed by SQLExecute.

RETCODE SQL_API SQLExecDirect(
        HSTMT     hstmt,
        UCHAR FAR *szSqlStr,
        SDWORD    cbSqlStr)
{
StatementClass *stmt = (StatementClass *) hstmt;
    
	if ( ! stmt)
		return SQL_INVALID_HANDLE;

	if (stmt->statement)
		free(stmt->statement);

	// keep a copy of the un-parametrized statement, in case
	// they try to execute this statement again
	stmt->statement = make_string(szSqlStr, cbSqlStr, NULL);
	if ( ! stmt->statement) {
		stmt->errornumber = STMT_NO_MEMORY_ERROR;
		stmt->errormsg = "No memory available to store statement";
		return SQL_ERROR;
	}

	mylog("**** SQLExecDirect: hstmt=%u, statement='%s'\n", hstmt, stmt->statement);

	stmt->prepare = FALSE;
	stmt->statement_type = statement_type(stmt->statement);

	//	Check if connection is readonly (only selects are allowed)
	if ( CC_is_readonly(stmt->hdbc) && stmt->statement_type != STMT_TYPE_SELECT ) {
		stmt->errornumber = STMT_EXEC_ERROR;
		stmt->errormsg = "Connection is readonly, only select statements are allowed.";
		return SQL_ERROR;
	}
	
	mylog("SQLExecDirect: calling SQLExecute\n");

	return SQLExecute(hstmt);
}

//      Execute a prepared SQL statement
RETCODE SQL_API SQLExecute(
        HSTMT   hstmt)
{
StatementClass *stmt = (StatementClass *) hstmt;
ConnectionClass *conn;
int i, retval;


	if ( ! stmt)
		return SQL_INVALID_HANDLE;

	/*  If the statement is premature, it means we already executed
		it from an SQLPrepare/SQLDescribeCol type of scenario.  So
		just return success.
	*/
	if ( stmt->prepare && stmt->status == STMT_PREMATURE) {
		stmt->status = STMT_FINISHED;       
		return stmt->errormsg == NULL ? SQL_SUCCESS : SQL_ERROR;
	}  

	SC_clear_error(stmt);

	conn = SC_get_conn(stmt);
	if (conn->status == CONN_EXECUTING) {
		stmt->errormsg = "Connection is already in use.";
		stmt->errornumber = STMT_SEQUENCE_ERROR;
		return SQL_ERROR;
	}

	if ( ! stmt->statement) {
		stmt->errornumber = STMT_NO_STMTSTRING;
		stmt->errormsg = "This handle does not have a SQL statement stored in it";
		return SQL_ERROR;
	}

	/*	If SQLExecute is being called again, recycle the statement.
		Note this should have been done by the application in a call
		to SQLFreeStmt(SQL_CLOSE) or SQLCancel.
	*/
	if (stmt->status == STMT_FINISHED) {
		SC_recycle_statement(stmt);
	}

	/*	Check if the statement is in the correct state */
	if ((stmt->prepare && stmt->status != STMT_READY) || 
		(stmt->status != STMT_ALLOCATED && stmt->status != STMT_READY)) {
		
		stmt->errornumber = STMT_STATUS_ERROR;
		stmt->errormsg = "The handle does not point to a statement that is ready to be executed";
		return SQL_ERROR;
	}


	/*	The bound parameters could have possibly changed since the last execute
		of this statement?  Therefore check for params and re-copy.
	*/
	stmt->data_at_exec = -1;
	for (i = 0; i < stmt->parameters_allocated; i++) {
		/*	Check for data at execution parameters */
		if ( stmt->parameters[i].data_at_exec == TRUE) {
			if (stmt->data_at_exec < 0)
				stmt->data_at_exec = 1;
			else
				stmt->data_at_exec++;
		}
	}
	//	If there are some data at execution parameters, return need data
	//	SQLParamData and SQLPutData will be used to send params and execute the statement.
	if (stmt->data_at_exec > 0)
		return SQL_NEED_DATA;


	mylog("SQLExecute: copying statement params: trans_status=%d, len=%d, stmt='%s'\n", conn->transact_status, strlen(stmt->statement), stmt->statement);

	//	Create the statement with parameters substituted.
	retval = copy_statement_with_parameters(stmt);
	if( retval != SQL_SUCCESS)
		/* error msg passed from above */
		return retval;

	mylog("   stmt_with_params = '%s'\n", stmt->stmt_with_params);


	return SC_execute(stmt);

}




//      -       -       -       -       -       -       -       -       -
RETCODE SQL_API SQLTransact(
        HENV    henv,
        HDBC    hdbc,
        UWORD   fType)
{
extern ConnectionClass *conns[];
ConnectionClass *conn;
QResultClass *res;
char ok, *stmt_string;
int lf;

mylog("**** SQLTransact: hdbc=%u, henv=%u\n", hdbc, henv);

	if (hdbc == SQL_NULL_HDBC && henv == SQL_NULL_HENV)
		return SQL_INVALID_HANDLE;

	/* If hdbc is null and henv is valid,
	it means transact all connections on that henv.  
	*/
	if (hdbc == SQL_NULL_HDBC && henv != SQL_NULL_HENV) {
		for (lf=0; lf <MAX_CONNECTIONS; lf++) {
			conn = conns[lf];

			if (conn && conn->henv == henv)
				if ( SQLTransact(henv, (HDBC) conn, fType) != SQL_SUCCESS)
					return SQL_ERROR;

		}
		return SQL_SUCCESS;       
	}

	conn = (ConnectionClass *) hdbc;

	if (fType == SQL_COMMIT) {
		stmt_string = "COMMIT";

	} else if (fType == SQL_ROLLBACK) {
		stmt_string = "ROLLBACK";

	} else {
		conn->errornumber = CONN_INVALID_ARGUMENT_NO;
		conn->errormsg ="SQLTransact can only be called with SQL_COMMIT or SQL_ROLLBACK as parameter";
		return SQL_ERROR;
	}    

	/*	If manual commit and in transaction, then proceed. */
	if ( ! CC_is_in_autocommit(conn) &&  CC_is_in_trans(conn)) {

		mylog("SQLTransact: sending on conn %d '%s'\n", conn, stmt_string);

		res = CC_send_query(conn, stmt_string, NULL, NULL);
		CC_set_no_trans(conn);

		if ( ! res)
			//	error msg will be in the connection
			return SQL_ERROR;

		ok = QR_command_successful(res);   
		QR_Destructor(res);

		if (!ok)
			return SQL_ERROR;
	}    
	return SQL_SUCCESS;
}

//      -       -       -       -       -       -       -       -       -


RETCODE SQL_API SQLCancel(
        HSTMT   hstmt)  // Statement to cancel.
{
StatementClass *stmt = (StatementClass *) hstmt;

	//	Check if this can handle canceling in the middle of a SQLPutData?
	if ( ! stmt)
		return SQL_INVALID_HANDLE;

	//	Not in the middle of SQLParamData/SQLPutData so cancel like a close.
	if (stmt->data_at_exec < 0)
		return SQLFreeStmt(hstmt, SQL_CLOSE);

	//	In the middle of SQLParamData/SQLPutData, so cancel that.
	//	Note, any previous data-at-exec buffers will be freed in the recycle
	//	if they call SQLExecDirect or SQLExecute again.

	stmt->data_at_exec = -1;
	stmt->current_exec_param = -1;
	stmt->put_data = FALSE;

}

//      -       -       -       -       -       -       -       -       -

//      Returns the SQL string as modified by the driver.

RETCODE SQL_API SQLNativeSql(
        HDBC      hdbc,
        UCHAR FAR *szSqlStrIn,
        SDWORD     cbSqlStrIn,
        UCHAR FAR *szSqlStr,
        SDWORD     cbSqlStrMax,
        SDWORD FAR *pcbSqlStr)
{

    strncpy_null(szSqlStr, szSqlStrIn, cbSqlStrMax);

    return SQL_SUCCESS;
}

//      -       -       -       -       -       -       -       -       -

//      Supplies parameter data at execution time.      Used in conjuction with
//      SQLPutData.

RETCODE SQL_API SQLParamData(
        HSTMT   hstmt,
        PTR FAR *prgbValue)
{
StatementClass *stmt = (StatementClass *) hstmt;
int i, retval;

	if ( ! stmt)
		return SQL_INVALID_HANDLE;

	if (stmt->data_at_exec < 0) {
		stmt->errornumber = STMT_SEQUENCE_ERROR;
		stmt->errormsg = "No execution-time parameters for this statement";
		return SQL_ERROR;
	}

	if (stmt->data_at_exec > stmt->parameters_allocated) {
		stmt->errornumber = STMT_SEQUENCE_ERROR;
		stmt->errormsg = "Too many execution-time parameters were present";
		return SQL_ERROR;
	}

	/*	Done, now copy the params and then execute the statement */
	if (stmt->data_at_exec == 0) {
		retval = copy_statement_with_parameters(stmt);
		if (retval != SQL_SUCCESS)
			return retval;

		return SC_execute(stmt);
	}

	/*	At least 1 data at execution parameter, so Fill in the token value */
	for (i = 0; i < stmt->parameters_allocated; i++) {
		if (stmt->parameters[i].data_at_exec == TRUE) {
			stmt->data_at_exec--;
			stmt->current_exec_param = i;
			stmt->put_data = FALSE;
			*prgbValue = stmt->parameters[i].buffer;	/* token */
		}
	}

	return SQL_NEED_DATA;
}

//      -       -       -       -       -       -       -       -       -

//      Supplies parameter data at execution time.      Used in conjunction with
//      SQLParamData.

RETCODE SQL_API SQLPutData(
        HSTMT   hstmt,
        PTR     rgbValue,
        SDWORD  cbValue)
{
StatementClass *stmt = (StatementClass *) hstmt;
char *buffer;
SDWORD *used;
int old_pos;


	if ( ! stmt)
		return SQL_INVALID_HANDLE;

	
	if (stmt->current_exec_param < 0) {
		stmt->errornumber = STMT_SEQUENCE_ERROR;
		stmt->errormsg = "Previous call was not SQLPutData or SQLParamData";
		return SQL_ERROR;
	}

	if ( ! stmt->put_data) {	/* first call */

		mylog("SQLPutData: (1) cbValue = %d\n", cbValue);

		stmt->put_data = TRUE;

		used = (SDWORD *) malloc(sizeof(SDWORD));
		if ( ! used) {
			stmt->errornumber = STMT_NO_MEMORY_ERROR;
			stmt->errormsg = "Out of memory in SQLPutData (1)";
			return SQL_ERROR;
		}

		*used = cbValue;
		stmt->parameters[stmt->current_exec_param].EXEC_used = used;

		if (cbValue == SQL_NULL_DATA)
			return SQL_SUCCESS;

		if (cbValue == SQL_NTS) {
			buffer = strdup(rgbValue);
			if ( ! buffer) {
				stmt->errornumber = STMT_NO_MEMORY_ERROR;
				stmt->errormsg = "Out of memory in SQLPutData (2)";
				return SQL_ERROR;
			}
		}
		else {
			buffer = malloc(cbValue + 1);
			if ( ! buffer) {
				stmt->errornumber = STMT_NO_MEMORY_ERROR;
				stmt->errormsg = "Out of memory in SQLPutData (2)";
				return SQL_ERROR;
			}
			memcpy(buffer, rgbValue, cbValue);
			buffer[cbValue] = '\0';
		}

		stmt->parameters[stmt->current_exec_param].EXEC_buffer = buffer;
	}

	else {	/* calling SQLPutData more than once */

		mylog("SQLPutData: (>1) cbValue = %d\n", cbValue);

		used = stmt->parameters[stmt->current_exec_param].EXEC_used;
		buffer = stmt->parameters[stmt->current_exec_param].EXEC_buffer;

		if (cbValue == SQL_NTS) {
			buffer = realloc(buffer, strlen(buffer) + strlen(rgbValue) + 1);
			if ( ! buffer) {
				stmt->errornumber = STMT_NO_MEMORY_ERROR;
				stmt->errormsg = "Out of memory in SQLPutData (3)";
				return SQL_ERROR;
			}
			strcat(buffer, rgbValue);

			mylog("       cbValue = SQL_NTS: strlen(buffer) = %d\n", strlen(buffer));

			*used = cbValue;

		}
		else if (cbValue > 0) {

			old_pos = *used;

			*used += cbValue;

			mylog("        cbValue = %d, old_pos = %d, *used = %d\n", cbValue, old_pos, *used);

			buffer = realloc(buffer, *used + 1);
			if ( ! buffer) {
				stmt->errornumber = STMT_NO_MEMORY_ERROR;
				stmt->errormsg = "Out of memory in SQLPutData (3)";
				return SQL_ERROR;
			}

			memcpy(&buffer[old_pos], rgbValue, cbValue);
			buffer[*used] = '\0';

		}
		else
			return SQL_ERROR;
		

		/*	reassign buffer incase realloc moved it */
		stmt->parameters[stmt->current_exec_param].EXEC_buffer = buffer;

	}


	return SQL_SUCCESS;
}


