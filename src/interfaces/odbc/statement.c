
/* Module:          statement.c
 *
 * Description:     This module contains functions related to creating
 *                  and manipulating a statement.
 *
 * Classes:         StatementClass (Functions prefix: "SC_")
 *
 * API functions:   SQLAllocStmt, SQLFreeStmt
 *
 * Comments:        See "notice.txt" for copyright and license information.
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "statement.h"
#include "bind.h"
#include "connection.h"
#include "qresult.h"
#include "convert.h"
#include "environ.h"
#include <stdio.h>
#include <string.h>

#ifdef HAVE_IODBC
#include "iodbc.h"
#include "isql.h"
#else
#include <windows.h>
#include <sql.h>
#endif

extern GLOBAL_VALUES globals;

#ifdef UNIX
#if !HAVE_STRICMP
#define stricmp(s1,s2) 		strcasecmp(s1,s2)
#define strnicmp(s1,s2,n)	strncasecmp(s1,s2,n)
#endif
#endif

/*	Map sql commands to statement types */
static struct {
	int  type;
	char *s;
} Statement_Type[] = {
	{ STMT_TYPE_SELECT, "SELECT" },
	{ STMT_TYPE_INSERT, "INSERT" },
	{ STMT_TYPE_UPDATE, "UPDATE" },
	{ STMT_TYPE_DELETE, "DELETE" },
	{ STMT_TYPE_CREATE, "CREATE" },
	{ STMT_TYPE_ALTER,  "ALTER"  },
	{ STMT_TYPE_DROP,   "DROP"   },
	{ STMT_TYPE_GRANT,  "GRANT"  },
	{ STMT_TYPE_REVOKE, "REVOKE" },
	{      0,            NULL    }
};


RETCODE SQL_API SQLAllocStmt(HDBC      hdbc,
                             HSTMT FAR *phstmt)
{
	return _SQLAllocStmt(hdbc, phstmt);
}

RETCODE SQL_API SQLFreeStmt(HSTMT     hstmt,
                            UWORD     fOption)
{
	return _SQLFreeStmt(hstmt, fOption);
}


RETCODE SQL_API _SQLAllocStmt(HDBC      hdbc,
                             HSTMT FAR *phstmt)
{
char *func="SQLAllocStmt";
ConnectionClass *conn = (ConnectionClass *) hdbc;
StatementClass *stmt;

	if( ! conn) {
		CC_log_error(func, "", NULL);
		return SQL_INVALID_HANDLE;
	}

	stmt = SC_Constructor();

	mylog("**** SQLAllocStmt: hdbc = %u, stmt = %u\n", hdbc, stmt);

	if ( ! stmt) {
		conn->errornumber = CONN_STMT_ALLOC_ERROR;
		conn->errormsg = "No more memory to allocate a further SQL-statement";
		*phstmt = SQL_NULL_HSTMT;
		CC_log_error(func, "", conn);
		return SQL_ERROR;
	}

    if ( ! CC_add_statement(conn, stmt)) {
        conn->errormsg = "Maximum number of connections exceeded.";
        conn->errornumber = CONN_STMT_ALLOC_ERROR;
		CC_log_error(func, "", conn);
        SC_Destructor(stmt);
		*phstmt = SQL_NULL_HSTMT;
        return SQL_ERROR;
    }

	*phstmt = (HSTMT) stmt;

    return SQL_SUCCESS;
}


RETCODE SQL_API _SQLFreeStmt(HSTMT     hstmt,
                            UWORD     fOption)
{
char *func="SQLFreeStmt";
StatementClass *stmt = (StatementClass *) hstmt;

	mylog("**** enter SQLFreeStmt: hstmt=%u, fOption=%d\n", hstmt, fOption);

	if ( ! stmt) {
		SC_log_error(func, "", NULL);
		return SQL_INVALID_HANDLE;
	}

	if (fOption == SQL_DROP) {
		ConnectionClass *conn = stmt->hdbc;

		/* Remove the statement from the connection's statement list */
		if ( conn) {
			if ( ! CC_remove_statement(conn, stmt)) {
				stmt->errornumber = STMT_SEQUENCE_ERROR;
				stmt->errormsg = "Statement is currently executing a transaction.";
				SC_log_error(func, "", stmt);
				return SQL_ERROR;  /* stmt may be executing a transaction */
			}

			/*	Free any cursors and discard any result info */
			if (stmt->result) {
				QR_Destructor(stmt->result);
				stmt->result = NULL;
			}
		}

		/* Destroy the statement and free any results, cursors, etc. */
		SC_Destructor(stmt);

    } else if (fOption == SQL_UNBIND) {
		SC_unbind_cols(stmt);

    } else if (fOption == SQL_CLOSE) {
		ConnectionClass *conn = stmt->hdbc;

		/* this should discard all the results, but leave the statement */
		/* itself in place (it can be executed again) */
        if (!SC_recycle_statement(stmt)) {
			//	errormsg passed in above
			SC_log_error(func, "", stmt);
            return SQL_ERROR;
		}

    } else if(fOption == SQL_RESET_PARAMS) {
		SC_free_params(stmt, STMT_FREE_PARAMS_ALL);

    } else {
        stmt->errormsg = "Invalid option passed to SQLFreeStmt.";
        stmt->errornumber = STMT_OPTION_OUT_OF_RANGE_ERROR;
		SC_log_error(func, "", stmt);
        return SQL_ERROR;
    }
    
    return SQL_SUCCESS;
}



/**********************************************************************
 * StatementClass implementation
 */

StatementClass *
SC_Constructor()
{
StatementClass *rv;

	rv = (StatementClass *) malloc(sizeof(StatementClass));
	if (rv) {
		rv->hdbc = NULL;       /* no connection associated yet */
		rv->result = NULL;
		rv->manual_result = FALSE;
		rv->prepare = FALSE;
		rv->status = STMT_ALLOCATED;
		rv->maxRows = 0;			// driver returns all rows
		rv->rowset_size = 1;
		rv->keyset_size = 0;		// fully keyset driven is the default
		rv->scroll_concurrency = SQL_CONCUR_READ_ONLY;
		rv->cursor_type = SQL_CURSOR_FORWARD_ONLY;
		rv->errormsg = NULL;
		rv->errornumber = 0;
		rv->errormsg_created = FALSE;
		rv->statement = NULL;
		rv->stmt_with_params[0] = '\0';
		rv->statement_type = STMT_TYPE_UNKNOWN;
		rv->bindings = NULL;
		rv->bindings_allocated = 0;
		rv->parameters_allocated = 0;
		rv->parameters = 0;
		rv->currTuple = -1;
		rv->current_col = -1;
		rv->result = 0;
		rv->data_at_exec = -1;
		rv->current_exec_param = -1;
		rv->put_data = FALSE;
		rv->lobj_fd = -1;
		rv->internal = FALSE;
		rv->cursor_name[0] = '\0';

		rv->ti = NULL;
		rv->fi = NULL;
		rv->ntab = 0;
		rv->nfld = 0;
		rv->parse_status = STMT_PARSE_NONE;
	}
	return rv;
}

char
SC_Destructor(StatementClass *self)
{

	mylog("SC_Destructor: self=%u, self->result=%u, self->hdbc=%u\n", self, self->result, self->hdbc);
	if (STMT_EXECUTING == self->status) {
		self->errornumber = STMT_SEQUENCE_ERROR;
		self->errormsg = "Statement is currently executing a transaction.";
		return FALSE;
	}

	if (self->result) {
		if ( ! self->hdbc)
			self->result->conn = NULL;  /* prevent any dbase activity */

		QR_Destructor(self->result);
	}

	if (self->statement)
		free(self->statement);

	SC_free_params(self, STMT_FREE_PARAMS_ALL);

	/* the memory pointed to by the bindings is not deallocated by the driver */
	/* by by the application that uses that driver, so we don't have to care  */
	/* about that here. */
	if (self->bindings)
		free(self->bindings);


	/*	Free the parsed table information */
	if (self->ti) {
		int i;
		for (i = 0; i < self->ntab; i++) {
			free(self->ti[i]);
		}

		free(self->ti);
	}

	/*	Free the parsed field information */
	if (self->fi) {
		int i;
		for (i = 0; i < self->nfld; i++) {
			free(self->fi[i]);
		}
		free(self->fi);
	}


	free(self);

	mylog("SC_Destructor: EXIT\n");

	return TRUE;
}

/*	Free parameters and free the memory from the 
	data-at-execution parameters that was allocated in SQLPutData.
*/
void
SC_free_params(StatementClass *self, char option)
{
int i;

	mylog("SC_free_params:  ENTER, self=%d\n", self);

	if( ! self->parameters)
		return;

	for (i = 0; i < self->parameters_allocated; i++) {
		if (self->parameters[i].data_at_exec == TRUE) {

			if (self->parameters[i].EXEC_used) {
				free(self->parameters[i].EXEC_used);
				self->parameters[i].EXEC_used = NULL;
			}

			if (self->parameters[i].EXEC_buffer) {
				free(self->parameters[i].EXEC_buffer);
				self->parameters[i].EXEC_buffer = NULL;
			}
		}
	}
	self->data_at_exec = -1;
	self->current_exec_param = -1;
	self->put_data = FALSE;

	if (option == STMT_FREE_PARAMS_ALL) {
		free(self->parameters);
		self->parameters = NULL;
		self->parameters_allocated = 0;
	}

	mylog("SC_free_params:  EXIT\n");
}


int 
statement_type(char *statement)
{
int i;

	for (i = 0; Statement_Type[i].s; i++)
		if ( ! strnicmp(statement, Statement_Type[i].s, strlen(Statement_Type[i].s)))
			return Statement_Type[i].type;

	return STMT_TYPE_OTHER;
}


/*	Called from SQLPrepare if STMT_PREMATURE, or
	from SQLExecute if STMT_FINISHED, or
	from SQLFreeStmt(SQL_CLOSE)
 */
char 
SC_recycle_statement(StatementClass *self)
{
ConnectionClass *conn;

	/*	This would not happen */    
	if (self->status == STMT_EXECUTING) {
		self->errornumber = STMT_SEQUENCE_ERROR;
		self->errormsg = "Statement is currently executing a transaction.";
		return FALSE;
	}

	self->errormsg = NULL;
	self->errornumber = 0;
	self->errormsg_created = FALSE;

	switch (self->status) {
	case STMT_ALLOCATED:
		/* this statement does not need to be recycled */
		return TRUE;

	case STMT_READY:
		break;

	case STMT_PREMATURE:
		/*	Premature execution of the statement might have caused the start of a transaction.
			If so, we have to rollback that transaction.
		*/
		conn = SC_get_conn(self);
		if ( ! CC_is_in_autocommit(conn) && CC_is_in_trans(conn)) {             

			CC_send_query(conn, "ABORT", NULL, NULL);
			CC_set_no_trans(conn);
		}
		break;

	case STMT_FINISHED:
		break;

	default:
		self->errormsg = "An internal error occured while recycling statements";
		self->errornumber = STMT_INTERNAL_ERROR;
		return FALSE;
	}

	/*	Free the parsed table information */
	if (self->ti) {
		int i;
		for (i = 0; i < self->ntab; i++) {
			free(self->ti[i]);
		}

		free(self->ti);
		self->ti = NULL;
		self->ntab = 0;
	}

	/*	Free the parsed field information */
	if (self->fi) {
		int i;
		for (i = 0; i < self->nfld; i++) {
			free(self->fi[i]);
		}
		free(self->fi);
		self->fi = NULL;
		self->nfld = 0;
	}
	self->parse_status = STMT_PARSE_NONE;

	/*	Free any cursors */
	if (self->result) {
		QR_Destructor(self->result);
		self->result = NULL;
	}

	self->status = STMT_READY;
	self->manual_result = FALSE;	// very important

	self->currTuple = -1;
	self->current_col = -1;

	self->errormsg = NULL;
	self->errornumber = 0;
	self->errormsg_created = FALSE;

	self->lobj_fd = -1;

	//	Free any data at exec params before the statement is executed
	//	again.  If not, then there will be a memory leak when
	//	the next SQLParamData/SQLPutData is called.
	SC_free_params(self, STMT_FREE_PARAMS_DATA_AT_EXEC_ONLY);

	return TRUE;
}

/* Pre-execute a statement (SQLPrepare/SQLDescribeCol) */
void 
SC_pre_execute(StatementClass *self)
{

	mylog("SC_pre_execute: status = %d\n", self->status);

	if (self->status == STMT_READY) {
		mylog("              preprocess: status = READY\n");

		SQLExecute(self);

		if (self->status == STMT_FINISHED) {
			mylog("              preprocess: after status = FINISHED, so set PREMATURE\n");
			self->status = STMT_PREMATURE;
		}
	}  
}

/* This is only called from SQLFreeStmt(SQL_UNBIND) */
char 
SC_unbind_cols(StatementClass *self)
{
Int2 lf;

	for(lf = 0; lf < self->bindings_allocated; lf++) {
		self->bindings[lf].buflen = 0;
		self->bindings[lf].buffer = NULL;
		self->bindings[lf].used = NULL;
		self->bindings[lf].returntype = SQL_C_CHAR;
	}

    return 1;
}

void 
SC_clear_error(StatementClass *self)
{
	self->errornumber = 0;
	self->errormsg = NULL;
	self->errormsg_created = FALSE;
}


//	This function creates an error msg which is the concatenation
//	of the result, statement, connection, and socket messages.
char *
SC_create_errormsg(StatementClass *self)
{
QResultClass *res = self->result;
ConnectionClass *conn = self->hdbc;
int pos;
static char msg[4096];

	msg[0] = '\0';

	if (res && res->message)
		strcpy(msg, res->message);

	else if (self->errormsg)
		strcpy(msg, self->errormsg);

	if (conn) {
		SocketClass *sock = conn->sock;

		if (conn->errormsg && conn->errormsg[0] != '\0') {
			pos = strlen(msg);
			sprintf(&msg[pos], ";\n%s", conn->errormsg);
		}

		if (sock && sock->errormsg && sock->errormsg[0] != '\0') {
			pos = strlen(msg);
			sprintf(&msg[pos], ";\n%s", sock->errormsg);
		}
	}

	return msg;
}

char 
SC_get_error(StatementClass *self, int *number, char **message)
{
char rv;

	//	Create a very informative errormsg if it hasn't been done yet.
	if ( ! self->errormsg_created) {
		self->errormsg = SC_create_errormsg(self);
		self->errormsg_created = TRUE;
	}

	if ( self->errornumber) {
		*number = self->errornumber;
		*message = self->errormsg;
		self->errormsg = NULL;
	}

	rv = (self->errornumber != 0);
	self->errornumber = 0;

	return rv;
}

RETCODE SC_execute(StatementClass *self)
{
char *func="SC_execute";
ConnectionClass *conn;
QResultClass *res;
char ok, was_ok, was_nonfatal;
Int2 oldstatus, numcols;


	conn = SC_get_conn(self);

	/*	Begin a transaction if one is not already in progress */
	/*	The reason is because we can't use declare/fetch cursors without
		starting a transaction first.
	*/
	if ( ! CC_is_in_trans(conn) && (globals.use_declarefetch || STMT_UPDATE(self))) {

		mylog("   about to begin a transaction on statement = %u\n", self);
		res = CC_send_query(conn, "BEGIN", NULL, NULL);
		if ( ! res) {
			self->errormsg = "Could not begin a transaction";
			self->errornumber = STMT_EXEC_ERROR;
			SC_log_error(func, "", self);
			return SQL_ERROR;
		}
		
		ok = QR_command_successful(res);   
		
		mylog("SQLExecute: ok = %d, status = %d\n", ok, QR_get_status(res));
		
		QR_Destructor(res);
		
		if (!ok) {
			self->errormsg = "Could not begin a transaction";
			self->errornumber = STMT_EXEC_ERROR;
			SC_log_error(func, "", self);
			return SQL_ERROR;
		}
		else
			CC_set_in_trans(conn);
	}



	oldstatus = conn->status;
	conn->status = CONN_EXECUTING;
	self->status = STMT_EXECUTING;


	//	If its a SELECT statement, use a cursor.
	//	Note that the declare cursor has already been prepended to the statement
	//	in copy_statement...
	if (self->statement_type == STMT_TYPE_SELECT) {

		char fetch[128];

		mylog("       Sending SELECT statement on stmt=%u, cursor_name='%s'\n", self, self->cursor_name);


		/*	send the declare/select */
		self->result = CC_send_query(conn, self->stmt_with_params, NULL, NULL);

		if (globals.use_declarefetch && self->result != NULL) {
			/*	That worked, so now send the fetch to start getting data back */
			sprintf(fetch, "fetch %d in %s", globals.fetch_max, self->cursor_name);
			
			//	Save the cursor in the result for later use
			self->result = CC_send_query( conn, fetch, NULL, self->cursor_name);
		}

		mylog("     done sending the query:\n");


		
	}
	else  { // not a SELECT statement so don't use a cursor 		 
		mylog("      its NOT a select statement: stmt=%u\n", self);
		self->result = CC_send_query(conn, self->stmt_with_params, NULL, NULL);
		
		//	If we are in autocommit, we must send the commit.
		if (CC_is_in_autocommit(conn) && STMT_UPDATE(self)) {
			CC_send_query(conn, "COMMIT", NULL, NULL);
			CC_set_no_trans(conn);
		}
		
	}

	conn->status = oldstatus;
	self->status = STMT_FINISHED;

	/*	Check the status of the result */
	if (self->result) {

		was_ok = QR_command_successful(self->result);
		was_nonfatal = QR_command_nonfatal(self->result);
		
		if ( was_ok)
			self->errornumber = STMT_OK;
		else
			self->errornumber = was_nonfatal ? STMT_INFO_ONLY : STMT_ERROR_TAKEN_FROM_BACKEND;
		
		self->currTuple = -1; /* set cursor before the first tuple in the list */
		self->current_col = -1;
		
		/* see if the query did return any result columns */
		numcols = QR_NumResultCols(self->result);
		
		/* now allocate the array to hold the binding info */
		if (numcols > 0) {
			extend_bindings(self, numcols);
			if (self->bindings == NULL) {
				self->errornumber = STMT_NO_MEMORY_ERROR;
				self->errormsg = "Could not get enough free memory to store the binding information";
				SC_log_error(func, "", self);
				return SQL_ERROR;
			}
		}
		
	} else {		/* Bad Error -- The error message will be in the Connection */

		if (self->statement_type == STMT_TYPE_CREATE) {
			self->errornumber = STMT_CREATE_TABLE_ERROR;
			self->errormsg = "Error creating the table";
			/*	This would allow the table to already exists, thus appending
				rows to it.  BUT, if the table didn't have the same attributes,
				it would fail.
				return SQL_SUCCESS_WITH_INFO;
			*/
		}
		else {
			self->errornumber = STMT_EXEC_ERROR;
			self->errormsg = "Error while executing the query";
		}
		CC_abort(conn);
	}

	if (self->errornumber == STMT_OK)
		return SQL_SUCCESS;

	else if (self->errornumber == STMT_INFO_ONLY)
		return SQL_SUCCESS_WITH_INFO;

	else {
		SC_log_error(func, "", self);
		return SQL_ERROR;
	}
}

void
SC_log_error(char *func, char *desc, StatementClass *self)
{
	if (self) {
		qlog("STATEMENT ERROR: func=%s, desc='%s', errnum=%d, errmsg='%s'\n", func, desc, self->errornumber, self->errormsg);
		qlog("                 ------------------------------------------------------------\n");
		qlog("                 hdbc=%u, stmt=%u, result=%u\n", self->hdbc, self, self->result);
		qlog("                 manual_result=%d, prepare=%d, internal=%d\n", self->manual_result, self->prepare, self->internal);
		qlog("                 bindings=%u, bindings_allocated=%d\n", self->bindings, self->bindings_allocated);
		qlog("                 parameters=%u, parameters_allocated=%d\n", self->parameters, self->parameters_allocated);
		qlog("                 statement_type=%d, statement='%s'\n", self->statement_type, self->statement);
		qlog("                 stmt_with_params='%s'\n", self->stmt_with_params);
		qlog("                 data_at_exec=%d, current_exec_param=%d, put_data=%d\n", self->data_at_exec, self->current_exec_param, self->put_data);
		qlog("                 currTuple=%d, current_col=%d, lobj_fd=%d\n", self->currTuple, self->current_col, self->lobj_fd);
		qlog("                 maxRows=%d, rowset_size=%d, keyset_size=%d, cursor_type=%d, scroll_concurrency=%d\n", self->maxRows, self->rowset_size, self->keyset_size, self->cursor_type, self->scroll_concurrency);
		qlog("                 cursor_name='%s'\n", self->cursor_name);

		qlog("                 ----------------QResult Info -------------------------------\n");

		if (self->result) {
		QResultClass *res = self->result;
		qlog("                 fields=%u, manual_tuples=%u, backend_tuples=%u, tupleField=%d, conn=%u\n", res->fields, res->manual_tuples, res->backend_tuples, res->tupleField, res->conn);
		qlog("                 fetch_count=%d, fcount=%d, num_fields=%d, cursor='%s'\n", res->fetch_count, res->fcount, res->num_fields, res->cursor);
		qlog("                 message='%s', command='%s', notice='%s'\n", res->message, res->command, res->notice);
		qlog("                 status=%d, inTuples=%d\n", res->status, res->inTuples);
		}
	
		//	Log the connection error if there is one
		CC_log_error(func, desc, self->hdbc);
	}
	else
		qlog("INVALID STATEMENT HANDLE ERROR: func=%s, desc='%s'\n", func, desc);
}

