
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "psqlodbc.h"
#include <stdio.h>
#include <string.h>

#ifndef WIN32
#include "iodbc.h"
#include "isqlext.h"
#else
#include <windows.h>
#include <sqlext.h>
#endif

#include "connection.h"
#include "statement.h"
#include "qresult.h"
#include "convert.h"
#include "bind.h"
#include "lobj.h"

extern GLOBAL_VALUES globals;


//      Perform a Prepare on the SQL statement
RETCODE SQL_API SQLPrepare(HSTMT     hstmt,
                           UCHAR FAR *szSqlStr,
                           SDWORD    cbSqlStr)
{
static char *func = "SQLPrepare";
StatementClass *self = (StatementClass *) hstmt;

	mylog( "%s: entering...\n", func);

	if ( ! self) {
		SC_log_error(func, "", NULL);
		return SQL_INVALID_HANDLE;
	}
    
	/*	According to the ODBC specs it is valid to call SQLPrepare mulitple times.
		In that case, the bound SQL statement is replaced by the new one 
	*/

	switch(self->status) {
	case STMT_PREMATURE:
		mylog("**** SQLPrepare: STMT_PREMATURE, recycle\n");
		SC_recycle_statement(self); /* recycle the statement, but do not remove parameter bindings */
		break;

	case STMT_FINISHED:
		mylog("**** SQLPrepare: STMT_FINISHED, recycle\n");
		SC_recycle_statement(self); /* recycle the statement, but do not remove parameter bindings */
		break;

	case STMT_ALLOCATED:
		mylog("**** SQLPrepare: STMT_ALLOCATED, copy\n");
		self->status = STMT_READY;
		break;

	case STMT_READY:
		mylog("**** SQLPrepare: STMT_READY, change SQL\n");
		break;

	case STMT_EXECUTING:
		mylog("**** SQLPrepare: STMT_EXECUTING, error!\n");

		self->errornumber = STMT_SEQUENCE_ERROR;
		self->errormsg = "SQLPrepare(): The handle does not point to a statement that is ready to be executed";
		SC_log_error(func, "", self);

		return SQL_ERROR;

	default:
		self->errornumber = STMT_INTERNAL_ERROR;
		self->errormsg = "An Internal Error has occured -- Unknown statement status.";
		SC_log_error(func, "", self);
		return SQL_ERROR;
	}

	if (self->statement)
		free(self->statement);

	self->statement = make_string(szSqlStr, cbSqlStr, NULL);
	if ( ! self->statement) {
		self->errornumber = STMT_NO_MEMORY_ERROR;
		self->errormsg = "No memory available to store statement";
		SC_log_error(func, "", self);
		return SQL_ERROR;
	}

	self->prepare = TRUE;
	self->statement_type = statement_type(self->statement);

	//	Check if connection is onlyread (only selects are allowed)
	if ( CC_is_onlyread(self->hdbc) && STMT_UPDATE(self)) {
		self->errornumber = STMT_EXEC_ERROR;
		self->errormsg = "Connection is readonly, only select statements are allowed.";
		SC_log_error(func, "", self);
		return SQL_ERROR;
	}

	return SQL_SUCCESS;


}

//      -       -       -       -       -       -       -       -       -

//      Performs the equivalent of SQLPrepare, followed by SQLExecute.

RETCODE SQL_API SQLExecDirect(
        HSTMT     hstmt,
        UCHAR FAR *szSqlStr,
        SDWORD    cbSqlStr)
{
StatementClass *stmt = (StatementClass *) hstmt;
RETCODE result;
static char *func = "SQLExecDirect";

	mylog( "%s: entering...\n", func);
    
	if ( ! stmt) {
		SC_log_error(func, "", NULL);
		return SQL_INVALID_HANDLE;
	}

	if (stmt->statement)
		free(stmt->statement);

	// keep a copy of the un-parametrized statement, in case
	// they try to execute this statement again
	stmt->statement = make_string(szSqlStr, cbSqlStr, NULL);
	if ( ! stmt->statement) {
		stmt->errornumber = STMT_NO_MEMORY_ERROR;
		stmt->errormsg = "No memory available to store statement";
		SC_log_error(func, "", stmt);
		return SQL_ERROR;
	}

	mylog("**** %s: hstmt=%u, statement='%s'\n", func, hstmt, stmt->statement);

	stmt->prepare = FALSE;
	stmt->statement_type = statement_type(stmt->statement);

	//	Check if connection is onlyread (only selects are allowed)
	if ( CC_is_onlyread(stmt->hdbc) && STMT_UPDATE(stmt)) {
		stmt->errornumber = STMT_EXEC_ERROR;
		stmt->errormsg = "Connection is readonly, only select statements are allowed.";
		SC_log_error(func, "", stmt);
		return SQL_ERROR;
	}
	
	mylog("%s: calling SQLExecute...\n", func);

	result = SQLExecute(hstmt);

	mylog("%s: returned %hd from SQLExecute\n", func, result);
	return result;
}

//      Execute a prepared SQL statement
RETCODE SQL_API SQLExecute(
        HSTMT   hstmt)
{
static char *func="SQLExecute";
StatementClass *stmt = (StatementClass *) hstmt;
ConnectionClass *conn;
int i, retval;


	mylog("%s: entering...\n", func);

	if ( ! stmt) {
		SC_log_error(func, "", NULL);
		mylog("%s: NULL statement so return SQL_INVALID_HANDLE\n", func);
		return SQL_INVALID_HANDLE;
	}

	/*  If the statement is premature, it means we already executed
		it from an SQLPrepare/SQLDescribeCol type of scenario.  So
		just return success.
	*/
	if ( stmt->prepare && stmt->status == STMT_PREMATURE) {
		stmt->status = STMT_FINISHED;       
		if (stmt->errormsg == NULL) {
			mylog("%s: premature statement but return SQL_SUCCESS\n", func);
			return SQL_SUCCESS;
		}
		else {
			SC_log_error(func, "", stmt);
			mylog("%s: premature statement so return SQL_ERROR\n", func);
			return SQL_ERROR;
		}
	}  

	mylog("%s: clear errors...\n", func);

	SC_clear_error(stmt);

	conn = SC_get_conn(stmt);
	if (conn->status == CONN_EXECUTING) {
		stmt->errormsg = "Connection is already in use.";
		stmt->errornumber = STMT_SEQUENCE_ERROR;
		SC_log_error(func, "", stmt);
		mylog("%s: problem with connection\n", func);
		return SQL_ERROR;
	}

	if ( ! stmt->statement) {
		stmt->errornumber = STMT_NO_STMTSTRING;
		stmt->errormsg = "This handle does not have a SQL statement stored in it";
		SC_log_error(func, "", stmt);
		mylog("%s: problem with handle\n", func);
		return SQL_ERROR;
	}

	/*	If SQLExecute is being called again, recycle the statement.
		Note this should have been done by the application in a call
		to SQLFreeStmt(SQL_CLOSE) or SQLCancel.
	*/
	if (stmt->status == STMT_FINISHED) {
		mylog("%s: recycling statement (should have been done by app)...\n", func);
		SC_recycle_statement(stmt);
	}

	/*	Check if the statement is in the correct state */
	if ((stmt->prepare && stmt->status != STMT_READY) || 
		(stmt->status != STMT_ALLOCATED && stmt->status != STMT_READY)) {
		
		stmt->errornumber = STMT_STATUS_ERROR;
		stmt->errormsg = "The handle does not point to a statement that is ready to be executed";
		SC_log_error(func, "", stmt);
		mylog("%s: problem with statement\n", func);
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


	mylog("%s: copying statement params: trans_status=%d, len=%d, stmt='%s'\n", func, conn->transact_status, strlen(stmt->statement), stmt->statement);

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
static char *func = "SQLTransact";
extern ConnectionClass *conns[];
ConnectionClass *conn;
QResultClass *res;
char ok, *stmt_string;
int lf;

	mylog("entering %s: hdbc=%u, henv=%u\n", func, hdbc, henv);

	if (hdbc == SQL_NULL_HDBC && henv == SQL_NULL_HENV) {
		CC_log_error(func, "", NULL);
		return SQL_INVALID_HANDLE;
	}

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
		CC_log_error(func, "", conn);
		return SQL_ERROR;
	}    

	/*	If manual commit and in transaction, then proceed. */
	if ( ! CC_is_in_autocommit(conn) &&  CC_is_in_trans(conn)) {

		mylog("SQLTransact: sending on conn %d '%s'\n", conn, stmt_string);

		res = CC_send_query(conn, stmt_string, NULL);
		CC_set_no_trans(conn);

		if ( ! res) {
			//	error msg will be in the connection
			CC_log_error(func, "", conn);
			return SQL_ERROR;
		}

		ok = QR_command_successful(res);   
		QR_Destructor(res);

		if (!ok) {
			CC_log_error(func, "", conn);
			return SQL_ERROR;
		}
	}    
	return SQL_SUCCESS;
}

//      -       -       -       -       -       -       -       -       -

RETCODE SQL_API SQLCancel(
        HSTMT   hstmt)  // Statement to cancel.
{
static char *func="SQLCancel";
StatementClass *stmt = (StatementClass *) hstmt;
RETCODE result;
#ifdef WIN32
HMODULE hmodule;
FARPROC addr;
#endif

	mylog( "%s: entering...\n", func);

	//	Check if this can handle canceling in the middle of a SQLPutData?
	if ( ! stmt) {
		SC_log_error(func, "", NULL);
		return SQL_INVALID_HANDLE;
	}

	//	Not in the middle of SQLParamData/SQLPutData so cancel like a close.
	if (stmt->data_at_exec < 0) {


		/*	MAJOR HACK for Windows to reset the driver manager's cursor state:
			Because of what seems like a bug in the Odbc driver manager,
			SQLCancel does not act like a SQLFreeStmt(CLOSE), as many
			applications depend on this behavior.  So, this 
			brute force method calls the driver manager's function on
			behalf of the application.  
		*/

#ifdef WIN32
		if (globals.cancel_as_freestmt) {
			hmodule = GetModuleHandle("ODBC32");
			addr = GetProcAddress(hmodule, "SQLFreeStmt");
			result = addr( (char *) (stmt->phstmt) - 96, SQL_CLOSE);
		}
		else {
			result = SQLFreeStmt( hstmt, SQL_CLOSE);
		}
#else
		result = SQLFreeStmt( hstmt, SQL_CLOSE);
#endif

		mylog("SQLCancel:  SQLFreeStmt returned %d\n", result);

		SC_clear_error(hstmt);
		return SQL_SUCCESS;
	}

	//	In the middle of SQLParamData/SQLPutData, so cancel that.
	//	Note, any previous data-at-exec buffers will be freed in the recycle
	//	if they call SQLExecDirect or SQLExecute again.

	stmt->data_at_exec = -1;
	stmt->current_exec_param = -1;
	stmt->put_data = FALSE;

	return SQL_SUCCESS;

}

//      -       -       -       -       -       -       -       -       -

//      Returns the SQL string as modified by the driver.
//		Currently, just copy the input string without modification
//		observing buffer limits and truncation.
RETCODE SQL_API SQLNativeSql(
        HDBC      hdbc,
        UCHAR FAR *szSqlStrIn,
        SDWORD     cbSqlStrIn,
        UCHAR FAR *szSqlStr,
        SDWORD     cbSqlStrMax,
        SDWORD FAR *pcbSqlStr)
{
static char *func="SQLNativeSql";
int len = 0;
char *ptr;
ConnectionClass *conn = (ConnectionClass *) hdbc;
RETCODE result;

	mylog( "%s: entering...cbSqlStrIn=%d\n", func, cbSqlStrIn);

	ptr = (cbSqlStrIn == 0) ? "" : make_string(szSqlStrIn, cbSqlStrIn, NULL);
	if ( ! ptr) {
		conn->errornumber = CONN_NO_MEMORY_ERROR;
		conn->errormsg = "No memory available to store native sql string";
		CC_log_error(func, "", conn);
		return SQL_ERROR;
	}

	result = SQL_SUCCESS;
	len = strlen(ptr);

	if (szSqlStr) {
		strncpy_null(szSqlStr, ptr, cbSqlStrMax);

		if (len >= cbSqlStrMax)  {
			result = SQL_SUCCESS_WITH_INFO;
			conn->errornumber = STMT_TRUNCATED;
			conn->errormsg = "The buffer was too small for the result.";
		}
	}

	if (pcbSqlStr)
		*pcbSqlStr = len;

	free(ptr);

    return result;
}

//      -       -       -       -       -       -       -       -       -

//      Supplies parameter data at execution time.      Used in conjuction with
//      SQLPutData.

RETCODE SQL_API SQLParamData(
        HSTMT   hstmt,
        PTR FAR *prgbValue)
{
static char *func = "SQLParamData";
StatementClass *stmt = (StatementClass *) hstmt;
int i, retval;

	mylog( "%s: entering...\n", func);

	if ( ! stmt) {
		SC_log_error(func, "", NULL);
		return SQL_INVALID_HANDLE;
	}

	mylog("%s: data_at_exec=%d, params_alloc=%d\n", func, stmt->data_at_exec, stmt->parameters_allocated);

	if (stmt->data_at_exec < 0) {
		stmt->errornumber = STMT_SEQUENCE_ERROR;
		stmt->errormsg = "No execution-time parameters for this statement";
		SC_log_error(func, "", stmt);
		return SQL_ERROR;
	}

	if (stmt->data_at_exec > stmt->parameters_allocated) {
		stmt->errornumber = STMT_SEQUENCE_ERROR;
		stmt->errormsg = "Too many execution-time parameters were present";
		SC_log_error(func, "", stmt);
		return SQL_ERROR;
	}

	/* close the large object */
	if ( stmt->lobj_fd >= 0) {
		lo_close(stmt->hdbc, stmt->lobj_fd);
		stmt->lobj_fd = -1;
	}


	/*	Done, now copy the params and then execute the statement */
	if (stmt->data_at_exec == 0) {
		retval = copy_statement_with_parameters(stmt);
		if (retval != SQL_SUCCESS)
			return retval;

		stmt->current_exec_param = -1;

		return SC_execute(stmt);
	}

	/*	Set beginning param;  if first time SQLParamData is called , start at 0.
		Otherwise, start at the last parameter + 1.
	*/
	i = stmt->current_exec_param >= 0 ? stmt->current_exec_param+1 : 0;

	/*	At least 1 data at execution parameter, so Fill in the token value */
	for ( ; i < stmt->parameters_allocated; i++) {
		if (stmt->parameters[i].data_at_exec == TRUE) {
			stmt->data_at_exec--;
			stmt->current_exec_param = i;
			stmt->put_data = FALSE;
			*prgbValue = stmt->parameters[i].buffer;	/* token */
			break;
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
static char *func = "SQLPutData";
StatementClass *stmt = (StatementClass *) hstmt;
int old_pos, retval;
ParameterInfoClass *current_param;
char *buffer;

	mylog( "%s: entering...\n", func);

	if ( ! stmt) {
		SC_log_error(func, "", NULL);
		return SQL_INVALID_HANDLE;
	}

	
	if (stmt->current_exec_param < 0) {
		stmt->errornumber = STMT_SEQUENCE_ERROR;
		stmt->errormsg = "Previous call was not SQLPutData or SQLParamData";
		SC_log_error(func, "", stmt);
		return SQL_ERROR;
	}

	current_param = &(stmt->parameters[stmt->current_exec_param]);

	if ( ! stmt->put_data) {	/* first call */

		mylog("SQLPutData: (1) cbValue = %d\n", cbValue);

		stmt->put_data = TRUE;

		current_param->EXEC_used = (SDWORD *) malloc(sizeof(SDWORD));
		if ( ! current_param->EXEC_used) {
			stmt->errornumber = STMT_NO_MEMORY_ERROR;
			stmt->errormsg = "Out of memory in SQLPutData (1)";
			SC_log_error(func, "", stmt);
			return SQL_ERROR;
		}

		*current_param->EXEC_used = cbValue;

		if (cbValue == SQL_NULL_DATA)
			return SQL_SUCCESS;


		/*	Handle Long Var Binary with Large Objects */
		if ( current_param->SQLType == SQL_LONGVARBINARY) {

			/*	store the oid */
			current_param->lobj_oid = lo_creat(stmt->hdbc, INV_READ | INV_WRITE);
			if (current_param->lobj_oid == 0) {
				stmt->errornumber = STMT_EXEC_ERROR;
				stmt->errormsg = "Couldnt create large object.";
				SC_log_error(func, "", stmt);
				return SQL_ERROR;
			}

			/*	major hack -- to allow convert to see somethings there */
			/*					have to modify convert to handle this better */
			current_param->EXEC_buffer = (char *) &current_param->lobj_oid;

			/*	store the fd */
			stmt->lobj_fd = lo_open(stmt->hdbc, current_param->lobj_oid, INV_WRITE);
			if ( stmt->lobj_fd < 0) {
				stmt->errornumber = STMT_EXEC_ERROR;
				stmt->errormsg = "Couldnt open large object for writing.";
				SC_log_error(func, "", stmt);
				return SQL_ERROR;
			}

			retval = lo_write(stmt->hdbc, stmt->lobj_fd, rgbValue, cbValue);
			mylog("lo_write: cbValue=%d, wrote %d bytes\n", cbValue, retval);

		}
		else {	/* for handling text fields and small binaries */

			if (cbValue == SQL_NTS) {
				current_param->EXEC_buffer = strdup(rgbValue);
				if ( ! current_param->EXEC_buffer) {
					stmt->errornumber = STMT_NO_MEMORY_ERROR;
					stmt->errormsg = "Out of memory in SQLPutData (2)";
					SC_log_error(func, "", stmt);
					return SQL_ERROR;
				}
			}
			else {
				current_param->EXEC_buffer = malloc(cbValue + 1);
				if ( ! current_param->EXEC_buffer) {
					stmt->errornumber = STMT_NO_MEMORY_ERROR;
					stmt->errormsg = "Out of memory in SQLPutData (2)";
					SC_log_error(func, "", stmt);
					return SQL_ERROR;
				}
				memcpy(current_param->EXEC_buffer, rgbValue, cbValue);
				current_param->EXEC_buffer[cbValue] = '\0';
			}
		}
	}

	else {	/* calling SQLPutData more than once */

		mylog("SQLPutData: (>1) cbValue = %d\n", cbValue);

		if (current_param->SQLType == SQL_LONGVARBINARY) {

			/* the large object fd is in EXEC_buffer */
			retval = lo_write(stmt->hdbc, stmt->lobj_fd, rgbValue, cbValue);
			mylog("lo_write(2): cbValue = %d, wrote %d bytes\n", cbValue, retval);

			*current_param->EXEC_used += cbValue;

		} else {

			buffer = current_param->EXEC_buffer;

			if (cbValue == SQL_NTS) {
				buffer = realloc(buffer, strlen(buffer) + strlen(rgbValue) + 1);
				if ( ! buffer) {
					stmt->errornumber = STMT_NO_MEMORY_ERROR;
					stmt->errormsg = "Out of memory in SQLPutData (3)";
					SC_log_error(func, "", stmt);
					return SQL_ERROR;
				}
				strcat(buffer, rgbValue);

				mylog("       cbValue = SQL_NTS: strlen(buffer) = %d\n", strlen(buffer));

				*current_param->EXEC_used = cbValue;

				/*	reassign buffer incase realloc moved it */
				current_param->EXEC_buffer = buffer;

			}
			else if (cbValue > 0) {

				old_pos = *current_param->EXEC_used;

				*current_param->EXEC_used += cbValue;

				mylog("        cbValue = %d, old_pos = %d, *used = %d\n", cbValue, old_pos, *current_param->EXEC_used);

				/* dont lose the old pointer in case out of memory */
				buffer = realloc(current_param->EXEC_buffer, *current_param->EXEC_used + 1);
				if ( ! buffer) {
					stmt->errornumber = STMT_NO_MEMORY_ERROR;
					stmt->errormsg = "Out of memory in SQLPutData (3)";
					SC_log_error(func, "", stmt);
					return SQL_ERROR;
				}

				memcpy(&buffer[old_pos], rgbValue, cbValue);
				buffer[*current_param->EXEC_used] = '\0';

				/*	reassign buffer incase realloc moved it */
				current_param->EXEC_buffer = buffer;
				
			}
			else {
				SC_log_error(func, "bad cbValue", stmt);
				return SQL_ERROR;
			}

		}
	}


	return SQL_SUCCESS;
}
