/*-------
 * Module:			pgapi30.c
 *
 * Description:		This module contains routines related to ODBC 3.0
 *			most of their implementations are temporary
 *			and must be rewritten properly.
 *			2001/07/23	inoue
 *
 * Classes:			n/a
 *
 * API functions:	PGAPI_ColAttribute, PGAPI_GetDiagRec,
			PGAPI_GetConnectAttr, PGAPI_GetStmtAttr,
			PGAPI_SetConnectAttr, PGAPI_SetStmtAttr
 *-------
 */

#ifndef ODBCVER
#define ODBCVER 0x0300
#endif
#include "psqlodbc.h"
#include <stdio.h>
#include <string.h>

#include "environ.h"
#include "connection.h"
#include "statement.h"
#include "pgapifunc.h"

/*	SQLError -> SQLDiagRec */
RETCODE		SQL_API
PGAPI_GetDiagRec(SQLSMALLINT HandleType, SQLHANDLE Handle,
		SQLSMALLINT RecNumber, SQLCHAR *Sqlstate,
		SQLINTEGER *NativeError, SQLCHAR *MessageText,
		SQLSMALLINT BufferLength, SQLSMALLINT *TextLength)
{
	RETCODE		ret;
	static const char *func = "PGAPI_GetDiagRec";

	mylog("%s entering rec=%d", func, RecNumber);
	switch (HandleType)
	{
		case SQL_HANDLE_ENV:
			ret = PGAPI_EnvError(Handle, RecNumber, Sqlstate,
					NativeError, MessageText,
					BufferLength, TextLength, 0);
			break;
		case SQL_HANDLE_DBC:
			ret = PGAPI_ConnectError(Handle, RecNumber, Sqlstate,
					NativeError, MessageText, BufferLength,
					TextLength, 0);
			break;
		case SQL_HANDLE_STMT:
			ret = PGAPI_StmtError(Handle, RecNumber, Sqlstate,
					NativeError, MessageText, BufferLength,
					TextLength, 0);
			break;
		default:
			ret = SQL_ERROR;
	}
	mylog("%s exiting %d\n", func, ret);
	return ret;
}

RETCODE		SQL_API
PGAPI_GetDiagField(SQLSMALLINT HandleType, SQLHANDLE Handle,
		SQLSMALLINT RecNumber, SQLSMALLINT DiagIdentifier,
		PTR DiagInfoPtr, SQLSMALLINT BufferLength,
		SQLSMALLINT *StringLengthPtr)
{
	RETCODE		ret = SQL_SUCCESS;
	static const char *func = "PGAPI_GetDiagField";

	mylog("%s entering rec=%d", func, RecNumber);
	switch (HandleType)
	{
		case SQL_HANDLE_ENV:
			switch (DiagIdentifier)
			{
				case SQL_DIAG_CLASS_ORIGIN:
				case SQL_DIAG_SUBCLASS_ORIGIN:
				case SQL_DIAG_CONNECTION_NAME:
				case SQL_DIAG_MESSAGE_TEXT:
				case SQL_DIAG_NATIVE:
				case SQL_DIAG_NUMBER:
				case SQL_DIAG_RETURNCODE:
				case SQL_DIAG_SERVER_NAME:
				case SQL_DIAG_SQLSTATE:
					break;
			}
			break;
		case SQL_HANDLE_DBC:
			switch (DiagIdentifier)
			{
				case SQL_DIAG_CLASS_ORIGIN:
				case SQL_DIAG_SUBCLASS_ORIGIN:
				case SQL_DIAG_CONNECTION_NAME:
				case SQL_DIAG_MESSAGE_TEXT:
				case SQL_DIAG_NATIVE:
				case SQL_DIAG_NUMBER:
				case SQL_DIAG_RETURNCODE:
				case SQL_DIAG_SERVER_NAME:
				case SQL_DIAG_SQLSTATE:
					break;
			}
			break;
		case SQL_HANDLE_STMT:
			switch (DiagIdentifier)
			{
				case SQL_DIAG_CLASS_ORIGIN:
				case SQL_DIAG_SUBCLASS_ORIGIN:
				case SQL_DIAG_CONNECTION_NAME:
				case SQL_DIAG_MESSAGE_TEXT:
				case SQL_DIAG_NATIVE:
				case SQL_DIAG_NUMBER:
				case SQL_DIAG_RETURNCODE:
				case SQL_DIAG_SERVER_NAME:
				case SQL_DIAG_SQLSTATE:
					break;
			}
			break;
		default:
			ret = SQL_ERROR;
	}
	mylog("%s exiting %d\n", func, ret);
	return ret;
}

/*	SQLGetConnectOption -> SQLGetconnectAttr */
RETCODE		SQL_API
PGAPI_GetConnectAttr(HDBC ConnectionHandle,
			SQLINTEGER Attribute, PTR Value,
			SQLINTEGER BufferLength, SQLINTEGER *StringLength)
{
	ConnectionClass *conn = (ConnectionClass *) ConnectionHandle;

	mylog("PGAPI_GetConnectAttr %d\n", Attribute);
	switch (Attribute)
	{
		case SQL_ATTR_ASYNC_ENABLE:
		case SQL_ATTR_AUTO_IPD:
		case SQL_ATTR_CONNECTION_DEAD:
		case SQL_ATTR_CONNECTION_TIMEOUT:
		case SQL_ATTR_METADATA_ID:
			conn->errornumber = STMT_INVALID_OPTION_IDENTIFIER;
			conn->errormsg = "Unsupported connect attribute (Get)";
			return SQL_ERROR;
	}
	return PGAPI_GetConnectOption(ConnectionHandle, (UWORD) Attribute, Value);
}

static HSTMT
descHandleFromStatementHandle(HSTMT StatementHandle, SQLINTEGER descType) 
{
	switch (descType)
	{
		case SQL_ATTR_APP_ROW_DESC:		/* 10010 */
			return StatementHandle;	/* this is bogus */
		case SQL_ATTR_APP_PARAM_DESC:	/* 10011 */
			return (HSTMT) ((SQLUINTEGER) StatementHandle + 1) ; /* this is bogus */
		case SQL_ATTR_IMP_ROW_DESC:		/* 10012 */
			return (HSTMT) ((SQLUINTEGER) StatementHandle + 2); /* this is bogus */
		case SQL_ATTR_IMP_PARAM_DESC:	/* 10013 */
			return (HSTMT) ((SQLUINTEGER) StatementHandle + 3); /* this is bogus */
	}
	return (HSTMT) 0;
}
static HSTMT
statementHandleFromDescHandle(HSTMT DescHandle, SQLINTEGER *descType) 
{
	SQLUINTEGER res = (SQLUINTEGER) DescHandle % 4;
	switch (res)
	{
		case 0: *descType = SQL_ATTR_APP_ROW_DESC; /* 10010 */
			break;
		case 1: *descType = SQL_ATTR_APP_PARAM_DESC; /* 10011 */
			break;
		case 2: *descType = SQL_ATTR_IMP_ROW_DESC; /* 10012 */
			break;
		case 3: *descType = SQL_ATTR_IMP_PARAM_DESC; /* 10013 */
			break;
	}
	return (HSTMT) ((SQLUINTEGER) DescHandle - res);
}

static RETCODE SQL_API
ARDSetField(StatementClass *stmt, SQLSMALLINT RecNumber,
		SQLSMALLINT FieldIdentifier, PTR Value, SQLINTEGER BufferLength)
{
	RETCODE		ret = SQL_SUCCESS;
	PTR		tptr;
	switch (FieldIdentifier)
	{
		case SQL_DESC_ARRAY_SIZE:
			stmt->options.rowset_size = (SQLUINTEGER) Value;
			break; 
		case SQL_DESC_ARRAY_STATUS_PTR:
			stmt->options.row_operation_ptr = Value;
			break;
		case SQL_DESC_BIND_OFFSET_PTR:
			stmt->options.row_offset_ptr = Value;
			break;
		case SQL_DESC_BIND_TYPE:
			stmt->options.bind_size = (SQLUINTEGER) Value;
			break;

		case SQL_DESC_DATA_PTR:
			if (!RecNumber)
				stmt->bookmark.buffer = Value;
			else
				stmt->bindings[RecNumber - 1].buffer = Value;
			break;
		case SQL_DESC_INDICATOR_PTR:
			if (!RecNumber)
				tptr = stmt->bookmark.used;
			else
				tptr = stmt->bindings[RecNumber - 1].used;
			if (Value != tptr)
			{
				ret = SQL_ERROR;
				stmt->errornumber = STMT_INVALID_OPTION_IDENTIFIER; 
				stmt->errormsg = "INDICATOR != OCTET_LENGTH_PTR"; 
			}
			break;
		case SQL_DESC_OCTET_LENGTH_PTR:
			if (!RecNumber)
				stmt->bookmark.used = Value;
			else
				stmt->bindings[RecNumber - 1].used = Value;
			break;
		default:ret = SQL_ERROR;
			stmt->errornumber = STMT_INVALID_OPTION_IDENTIFIER; 
			stmt->errormsg = "not implemedted yet"; 
	}
	return ret;
}

static RETCODE SQL_API
APDSetField(StatementClass *stmt, SQLSMALLINT RecNumber,
		SQLSMALLINT FieldIdentifier, PTR Value, SQLINTEGER BufferLength)
{
	RETCODE		ret = SQL_SUCCESS;
	switch (FieldIdentifier)
	{
		case SQL_DESC_ARRAY_SIZE:
			stmt->options.paramset_size = (SQLUINTEGER) Value;
			break; 
		case SQL_DESC_ARRAY_STATUS_PTR:
			stmt->options.param_operation_ptr = Value;
			break;
		case SQL_DESC_BIND_OFFSET_PTR:
			stmt->options.param_offset_ptr = Value;
			break;
		case SQL_DESC_BIND_TYPE:
			stmt->options.param_bind_type = (SQLUINTEGER) Value;
			break;

		case SQL_DESC_DATA_PTR:
			if (stmt->parameters_allocated < RecNumber)
				PGAPI_BindParameter(stmt, RecNumber, 0, 0, 0, 0, 0, 0, 0, 0);
			stmt->parameters[RecNumber - 1].buffer = Value;
			break;
		case SQL_DESC_INDICATOR_PTR:
			if (stmt->parameters_allocated < RecNumber ||
			    Value != stmt->parameters[RecNumber - 1].used)
			{
				ret = SQL_ERROR;
				stmt->errornumber = STMT_INVALID_OPTION_IDENTIFIER; 
				stmt->errormsg = "INDICATOR != OCTET_LENGTH_PTR"; 
			}
			break;
		case SQL_DESC_OCTET_LENGTH_PTR:
			if (stmt->parameters_allocated < RecNumber)
				PGAPI_BindParameter(stmt, RecNumber, 0, 0, 0, 0, 0, 0, 0, 0);
			stmt->parameters[RecNumber - 1].used = Value;
			break;
		default:ret = SQL_ERROR;
			stmt->errornumber = STMT_INVALID_OPTION_IDENTIFIER; 
	}
	return ret;
}

static RETCODE SQL_API
IRDSetField(StatementClass *stmt, SQLSMALLINT RecNumber,
		SQLSMALLINT FieldIdentifier, PTR Value, SQLINTEGER BufferLength)
{
	RETCODE		ret = SQL_SUCCESS;
	switch (FieldIdentifier)
	{
		case SQL_DESC_ARRAY_STATUS_PTR:
			stmt->options.rowStatusArray = (SQLUSMALLINT *) Value;
			break;
		case SQL_DESC_ROWS_PROCESSED_PTR:
			stmt->options.rowsFetched = (SQLUINTEGER *) Value;
			break;
		default:ret = SQL_ERROR;
			stmt->errornumber = STMT_INVALID_OPTION_IDENTIFIER; 
	}
	return ret;
}

static RETCODE SQL_API
IPDSetField(StatementClass *stmt, SQLSMALLINT RecNumber,
		SQLSMALLINT FieldIdentifier, PTR Value, SQLINTEGER BufferLength)
{
	RETCODE		ret = SQL_SUCCESS;
	switch (FieldIdentifier)
	{
		case SQL_DESC_ARRAY_STATUS_PTR:
			stmt->options.param_status_ptr = (SQLUSMALLINT *) Value;
			break;
		case SQL_DESC_ROWS_PROCESSED_PTR:
			stmt->options.param_processed_ptr = (SQLUINTEGER *) Value;
			break;
		default:ret = SQL_ERROR;
			stmt->errornumber = STMT_INVALID_OPTION_IDENTIFIER; 
	}
	return ret;
}

/*	SQLGetStmtOption -> SQLGetStmtAttr */
RETCODE		SQL_API
PGAPI_GetStmtAttr(HSTMT StatementHandle,
		SQLINTEGER Attribute, PTR Value,
		SQLINTEGER BufferLength, SQLINTEGER *StringLength)
{
	static char *func = "PGAPI_GetStmtAttr";
	StatementClass *stmt = (StatementClass *) StatementHandle;
	RETCODE		ret = SQL_SUCCESS;
	int			len = 0;

	mylog("%s Handle=%u %d\n", func, StatementHandle, Attribute);
	switch (Attribute)
	{
		case SQL_ATTR_FETCH_BOOKMARK_PTR:		/* 16 */
			Value = stmt->options.bookmark_ptr;
			len = 4;
			break;
		case SQL_ATTR_PARAM_BIND_OFFSET_PTR:	/* 17 */
			Value = stmt->options.param_offset_ptr;
			len = 4;
			break;
		case SQL_ATTR_PARAM_BIND_TYPE:	/* 18 */
			*((SQLUINTEGER *) Value) = stmt->options.param_bind_type;
			len = 4;
			break;
		case SQL_ATTR_PARAM_OPERATION_PTR:		/* 19 */
			Value = stmt->options.param_operation_ptr;
			len = 4;
			break;
		case SQL_ATTR_PARAM_STATUS_PTR: /* 20 */
			Value = stmt->options.param_status_ptr;
			len = 4;
			break;
		case SQL_ATTR_PARAMS_PROCESSED_PTR:		/* 21 */
			Value = stmt->options.param_processed_ptr;
			len = 4;
			break;
		case SQL_ATTR_PARAMSET_SIZE:	/* 22 */
			*((SQLUINTEGER *) Value) = stmt->options.paramset_size;
			len = 4;
			break;
		case SQL_ATTR_ROW_BIND_OFFSET_PTR:		/* 23 */
			Value = stmt->options.row_offset_ptr;
			len = 4;
			break;
		case SQL_ATTR_ROW_OPERATION_PTR:		/* 24 */
			Value = stmt->options.row_operation_ptr;
			len = 4;
			break;
		case SQL_ATTR_ROW_STATUS_PTR:	/* 25 */
			Value = stmt->options.rowStatusArray;
			len = 4;
			break;
		case SQL_ATTR_ROWS_FETCHED_PTR: /* 26 */
			Value = stmt->options.rowsFetched;
			len = 4;
			break;
		case SQL_ATTR_ROW_ARRAY_SIZE:	/* 27 */
			*((SQLUINTEGER *) Value) = stmt->options.rowset_size;
			len = 4;
			break;
		case SQL_ATTR_APP_ROW_DESC:		/* 10010 */
		case SQL_ATTR_APP_PARAM_DESC:	/* 10011 */
		case SQL_ATTR_IMP_ROW_DESC:		/* 10012 */
		case SQL_ATTR_IMP_PARAM_DESC:	/* 10013 */
			len = 4;
			*((HSTMT *) Value) = descHandleFromStatementHandle(StatementHandle, Attribute); 
			break;
		case SQL_ATTR_AUTO_IPD:	/* 10001 */
			/* case SQL_ATTR_ROW_BIND_TYPE: ** == SQL_BIND_TYPE(ODBC2.0) */

		case SQL_ATTR_CURSOR_SCROLLABLE:		/* -1 */
		case SQL_ATTR_CURSOR_SENSITIVITY:		/* -2 */
		case SQL_ATTR_ENABLE_AUTO_IPD:	/* 15 */
		case SQL_ATTR_METADATA_ID:		/* 10014 */

			/*
			 * case SQL_ATTR_PREDICATE_PTR: case
			 * SQL_ATTR_PREDICATE_OCTET_LENGTH_PTR:
			 */
			stmt->errornumber = STMT_INVALID_OPTION_IDENTIFIER;
			stmt->errormsg = "Unsupported statement option (Get)";
			SC_log_error(func, "", stmt);
			return SQL_ERROR;
		default:
			len = 4;
			ret = PGAPI_GetStmtOption(StatementHandle, (UWORD) Attribute, Value);
	}
	if (ret == SQL_SUCCESS && StringLength)
		*StringLength = len;
	return ret;
}

/*	SQLSetConnectOption -> SQLSetConnectAttr */
RETCODE		SQL_API
PGAPI_SetConnectAttr(HDBC ConnectionHandle,
			SQLINTEGER Attribute, PTR Value,
			SQLINTEGER StringLength)
{
	ConnectionClass *conn = (ConnectionClass *) ConnectionHandle;

	mylog("PGAPI_SetConnectAttr %d\n", Attribute);
	switch (Attribute)
	{
		case SQL_ATTR_ASYNC_ENABLE:
		case SQL_ATTR_AUTO_IPD:
		case SQL_ATTR_CONNECTION_DEAD:
		case SQL_ATTR_CONNECTION_TIMEOUT:
		case SQL_ATTR_METADATA_ID:
			conn->errornumber = STMT_INVALID_OPTION_IDENTIFIER;
			conn->errormsg = "Unsupported connect attribute (Set)";
			return SQL_ERROR;
	}
	return PGAPI_SetConnectOption(ConnectionHandle, (UWORD) Attribute, (UDWORD) Value);
}

/*	new function */
RETCODE		SQL_API
PGAPI_SetDescField(SQLHDESC DescriptorHandle,
			SQLSMALLINT RecNumber, SQLSMALLINT FieldIdentifier,
			PTR Value, SQLINTEGER BufferLength)
{
	RETCODE		ret = SQL_SUCCESS;
	HSTMT		hstmt;
	SQLUINTEGER	descType;
	StatementClass *stmt;
	static const char *func = "PGAPI_SetDescField";

	mylog("%s h=%u rec=%d field=%d val=%x\n", func, DescriptorHandle, RecNumber, FieldIdentifier, Value);
	hstmt = statementHandleFromDescHandle(DescriptorHandle, &descType);
	mylog("stmt=%x type=%d\n", hstmt, descType);
	stmt = (StatementClass *) hstmt;
	switch (descType)
	{
		case SQL_ATTR_APP_ROW_DESC:
			ret = ARDSetField(stmt, RecNumber, FieldIdentifier, Value, BufferLength);
			break;
		case SQL_ATTR_APP_PARAM_DESC:
			ret = APDSetField(stmt, RecNumber, FieldIdentifier, Value, BufferLength);
			break;
		case SQL_ATTR_IMP_ROW_DESC:
			ret = IRDSetField(stmt, RecNumber, FieldIdentifier, Value, BufferLength);
			break;
		case SQL_ATTR_IMP_PARAM_DESC:
			ret = IPDSetField(stmt, RecNumber, FieldIdentifier, Value, BufferLength);
			break;
		default:ret = SQL_ERROR;
			stmt->errornumber = STMT_INTERNAL_ERROR; 
			stmt->errormsg = "Error not implemented";
	}
	if (ret == SQL_ERROR)
		SC_log_error(func, "", stmt);
	return ret;
}

/*	SQLSet(Param/Scroll/Stmt)Option -> SQLSetStmtAttr */
RETCODE		SQL_API
PGAPI_SetStmtAttr(HSTMT StatementHandle,
		SQLINTEGER Attribute, PTR Value,
		SQLINTEGER StringLength)
{
	static char *func = "PGAPI_SetStmtAttr";
	StatementClass *stmt = (StatementClass *) StatementHandle;

	mylog("%s Handle=%u %d,%u\n", func, StatementHandle, Attribute, Value);
	switch (Attribute)
	{
		case SQL_ATTR_CURSOR_SCROLLABLE:		/* -1 */
		case SQL_ATTR_CURSOR_SENSITIVITY:		/* -2 */

		case SQL_ATTR_ENABLE_AUTO_IPD:	/* 15 */

		case SQL_ATTR_APP_ROW_DESC:		/* 10010 */
		case SQL_ATTR_APP_PARAM_DESC:	/* 10011 */
		case SQL_ATTR_AUTO_IPD:	/* 10001 */
		/* case SQL_ATTR_ROW_BIND_TYPE: ** == SQL_BIND_TYPE(ODBC2.0) */
		case SQL_ATTR_IMP_ROW_DESC:	/* 10012 (read-only) */
		case SQL_ATTR_IMP_PARAM_DESC:	/* 10013 (read-only) */
		case SQL_ATTR_METADATA_ID:		/* 10014 */

			/*
			 * case SQL_ATTR_PREDICATE_PTR: case
			 * SQL_ATTR_PREDICATE_OCTET_LENGTH_PTR:
			 */
			stmt->errornumber = STMT_INVALID_OPTION_IDENTIFIER;
			stmt->errormsg = "Unsupported statement option (Set)";
			SC_log_error(func, "", stmt);
			return SQL_ERROR;

		case SQL_ATTR_FETCH_BOOKMARK_PTR:		/* 16 */
			stmt->options.bookmark_ptr = Value;
			break;
		case SQL_ATTR_PARAM_BIND_OFFSET_PTR:	/* 17 */
			stmt->options.param_offset_ptr = (SQLUINTEGER *) Value;
			break;
		case SQL_ATTR_PARAM_BIND_TYPE:	/* 18 */
			stmt->options.param_bind_type = (SQLUINTEGER) Value;
			break;
		case SQL_ATTR_PARAM_OPERATION_PTR:		/* 19 */
			stmt->options.param_operation_ptr = Value;
			break;
		case SQL_ATTR_PARAM_STATUS_PTR:			/* 20 */
			stmt->options.param_status_ptr = (SQLUSMALLINT *) Value;
			break;
		case SQL_ATTR_PARAMS_PROCESSED_PTR:		/* 21 */
			stmt->options.param_processed_ptr = (SQLUINTEGER *) Value;
			break;
		case SQL_ATTR_PARAMSET_SIZE:	/* 22 */
			stmt->options.paramset_size = (SQLUINTEGER) Value;
			break;
		case SQL_ATTR_ROW_BIND_OFFSET_PTR:		/* 23 */
			stmt->options.row_offset_ptr = (SQLUINTEGER *) Value;
			break;
		case SQL_ATTR_ROW_OPERATION_PTR:		/* 24 */
			stmt->options.row_operation_ptr = Value;
			break;
		case SQL_ATTR_ROW_STATUS_PTR:	/* 25 */
			stmt->options.rowStatusArray = (SQLUSMALLINT *) Value;
			break;
		case SQL_ATTR_ROWS_FETCHED_PTR: /* 26 */
			stmt->options.rowsFetched = (SQLUINTEGER *) Value;
			break;
		case SQL_ATTR_ROW_ARRAY_SIZE:	/* 27 */
			stmt->options.rowset_size = (SQLUINTEGER) Value;
			break;
		default:
			return PGAPI_SetStmtOption(StatementHandle, (UWORD) Attribute, (UDWORD) Value);
	}
	return SQL_SUCCESS;
}
