/*-------
 * Module:			odbcapi30.c
 *
 * Description:		This module contains routines related to ODBC 3.0
 *			most of their implementations are temporary
 *			and must be rewritten properly.
 *			2001/07/23	inoue
 *
 * Classes:			n/a
 *
 * API functions:	SQLAllocHandle, SQLBindParam, SQLCloseCursor,
			SQLColAttribute, SQLCopyDesc, SQLEndTran,
			SQLFetchScroll, SQLFreeHandle, SQLGetDescField,
			SQLGetDescRec, SQLGetDiagField, SQLGetDiagRec,
			SQLGetEnvAttr, SQLGetConnectAttr, SQLGetStmtAttr,
			SQLSetConnectAttr, SQLSetDescField, SQLSetDescRec,
			SQLSetEnvAttr, SQLSetStmtAttr, SQLBulkOperations
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

/*	SQLAllocConnect/SQLAllocEnv/SQLAllocStmt -> SQLAllocHandle */
RETCODE		SQL_API
SQLAllocHandle(SQLSMALLINT HandleType,
			   SQLHANDLE InputHandle, SQLHANDLE * OutputHandle)
{
	mylog("[[SQLAllocHandle]]");
	switch (HandleType)
	{
		case SQL_HANDLE_ENV:
			return PGAPI_AllocEnv(OutputHandle);
		case SQL_HANDLE_DBC:
			return PGAPI_AllocConnect(InputHandle, OutputHandle);
		case SQL_HANDLE_STMT:
			return PGAPI_AllocStmt(InputHandle, OutputHandle);
		default:
			break;
	}
	return SQL_ERROR;
}

/*	SQLBindParameter/SQLSetParam -> SQLBindParam */
RETCODE		SQL_API
SQLBindParam(HSTMT StatementHandle,
			 SQLUSMALLINT ParameterNumber, SQLSMALLINT ValueType,
			 SQLSMALLINT ParameterType, SQLUINTEGER LengthPrecision,
			 SQLSMALLINT ParameterScale, PTR ParameterValue,
			 SQLINTEGER *StrLen_or_Ind)
{
	int			BufferLength = 512;		/* Is it OK ? */

	mylog("[[SQLBindParam]]");
	return PGAPI_BindParameter(StatementHandle, ParameterNumber, SQL_PARAM_INPUT, ValueType, ParameterType, LengthPrecision, ParameterScale, ParameterValue, BufferLength, StrLen_or_Ind);
}

/*	New function */
RETCODE		SQL_API
SQLCloseCursor(HSTMT StatementHandle)
{
	mylog("[[SQLCloseCursor]]");
	return PGAPI_FreeStmt(StatementHandle, SQL_CLOSE);
}

/*	SQLColAttributes -> SQLColAttribute */
RETCODE		SQL_API
SQLColAttribute(HSTMT StatementHandle,
				SQLUSMALLINT ColumnNumber, SQLUSMALLINT FieldIdentifier,
				PTR CharacterAttribute, SQLSMALLINT BufferLength,
				SQLSMALLINT *StringLength, PTR NumericAttribute)
{
	mylog("[[SQLColAttribute]]");
	return PGAPI_ColAttributes(StatementHandle, ColumnNumber,
					   FieldIdentifier, CharacterAttribute, BufferLength,
							   StringLength, NumericAttribute);
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

/*	new function */
RETCODE		SQL_API
SQLCopyDesc(SQLHDESC SourceDescHandle,
			SQLHDESC TargetDescHandle)
{
	mylog("[[SQLCopyDesc]]\n");
	mylog("Error not implemented\n");
	return SQL_ERROR;
}

/*	SQLTransact -> SQLEndTran */
RETCODE		SQL_API
SQLEndTran(SQLSMALLINT HandleType, SQLHANDLE Handle,
		   SQLSMALLINT CompletionType)
{
	mylog("[[SQLEndTran]]");
	switch (HandleType)
	{
		case SQL_HANDLE_ENV:
			return PGAPI_Transact(Handle, SQL_NULL_HDBC, CompletionType);
		case SQL_HANDLE_DBC:
			return PGAPI_Transact(SQL_NULL_HENV, Handle, CompletionType);
		default:
			break;
	}
	return SQL_ERROR;
}

/*	SQLExtendedFetch -> SQLFetchScroll */
RETCODE		SQL_API
SQLFetchScroll(HSTMT StatementHandle,
			   SQLSMALLINT FetchOrientation, SQLINTEGER FetchOffset)
{
	static char *func = "SQLFetchScroll";
	StatementClass *stmt = (StatementClass *) StatementHandle;
	RETCODE		ret;
	SQLUSMALLINT *rowStatusArray = stmt->options.rowStatusArray;
	SQLINTEGER *pcRow = stmt->options.rowsFetched;

	mylog("[[%s]] %d,%d\n", func, FetchOrientation, FetchOffset);
	if (FetchOrientation == SQL_FETCH_BOOKMARK)
	{
		if (stmt->options.bookmark_ptr)
{
			FetchOffset += *((Int4 *) stmt->options.bookmark_ptr);
mylog("real FetchOffset = %d\n", FetchOffset);
}
		else
		{
			stmt->errornumber = STMT_SEQUENCE_ERROR;
			stmt->errormsg = "Bookmark isn't specifed yet";
			SC_log_error(func, "", stmt);
			return SQL_ERROR;
		}
	}
	ret = PGAPI_ExtendedFetch(StatementHandle, FetchOrientation, FetchOffset,
							  pcRow, rowStatusArray);
	if (ret != SQL_SUCCESS)
		mylog("%s return = %d\n", func, ret);
	return ret;
}

/*	SQLFree(Connect/Env/Stmt) -> SQLFreeHandle */
RETCODE		SQL_API
SQLFreeHandle(SQLSMALLINT HandleType, SQLHANDLE Handle)
{
	mylog("[[SQLFreeHandle]]");
	switch (HandleType)
	{
		case SQL_HANDLE_ENV:
			return PGAPI_FreeEnv(Handle);
		case SQL_HANDLE_DBC:
			return PGAPI_FreeConnect(Handle);
		case SQL_HANDLE_STMT:
			return PGAPI_FreeStmt(Handle, SQL_DROP);
		default:
			break;
	}
	return SQL_ERROR;
}

/*	new function */
RETCODE		SQL_API
SQLGetDescField(SQLHDESC DescriptorHandle,
				SQLSMALLINT RecNumber, SQLSMALLINT FieldIdentifier,
				PTR Value, SQLINTEGER BufferLength,
				SQLINTEGER *StringLength)
{
	mylog("[[SQLGetDescField]]\n");
	mylog("Error not implemented\n");
	return SQL_ERROR;
}

/*	new function */
RETCODE		SQL_API
SQLGetDescRec(SQLHDESC DescriptorHandle,
			  SQLSMALLINT RecNumber, SQLCHAR *Name,
			  SQLSMALLINT BufferLength, SQLSMALLINT *StringLength,
			  SQLSMALLINT *Type, SQLSMALLINT *SubType,
			  SQLINTEGER *Length, SQLSMALLINT *Precision,
			  SQLSMALLINT *Scale, SQLSMALLINT *Nullable)
{
	mylog("[[SQLGetDescRec]]\n");
	mylog("Error not implemented\n");
	return SQL_ERROR;
}

/*	new function */
RETCODE		SQL_API
SQLGetDiagField(SQLSMALLINT HandleType, SQLHANDLE Handle,
				SQLSMALLINT RecNumber, SQLSMALLINT DiagIdentifier,
				PTR DiagInfo, SQLSMALLINT BufferLength,
				SQLSMALLINT *StringLength)
{
	mylog("[[SQLGetDiagField]]\n");
	return SQL_ERROR;
}

/*	SQLError -> SQLDiagRec */
RETCODE		SQL_API
SQLGetDiagRec(SQLSMALLINT HandleType, SQLHANDLE Handle,
			  SQLSMALLINT RecNumber, SQLCHAR *Sqlstate,
			  SQLINTEGER *NativeError, SQLCHAR *MessageText,
			  SQLSMALLINT BufferLength, SQLSMALLINT *TextLength)
{
	mylog("[[SQLGetDiagRec]]\n");
	return PGAPI_GetDiagRec(HandleType, Handle, RecNumber, Sqlstate,
			NativeError, MessageText, BufferLength, TextLength);
}

/*	new function */
RETCODE		SQL_API
SQLGetEnvAttr(HENV EnvironmentHandle,
			  SQLINTEGER Attribute, PTR Value,
			  SQLINTEGER BufferLength, SQLINTEGER *StringLength)
{
	EnvironmentClass *env = (EnvironmentClass *) EnvironmentHandle;

	mylog("[[SQLGetEnvAttr]] %d\n", Attribute);
	switch (Attribute)
	{
		case SQL_ATTR_CONNECTION_POOLING:
			*((unsigned int *) Value) = SQL_CP_OFF;
			break;
		case SQL_ATTR_CP_MATCH:
			*((unsigned int *) Value) = SQL_CP_RELAXED_MATCH;
			break;
		case SQL_ATTR_ODBC_VERSION:
			*((unsigned int *) Value) = EN_is_odbc2(env) ? SQL_OV_ODBC2 : SQL_OV_ODBC3;
			break;
		case SQL_ATTR_OUTPUT_NTS:
			*((unsigned int *) Value) = SQL_TRUE;
			break;
		default:
			env->errornumber = CONN_INVALID_ARGUMENT_NO;
			return SQL_ERROR;
	}
	return SQL_SUCCESS;
}

/*	SQLGetConnectOption -> SQLGetconnectAttr */
RETCODE		SQL_API
SQLGetConnectAttr(HDBC ConnectionHandle,
				  SQLINTEGER Attribute, PTR Value,
				  SQLINTEGER BufferLength, SQLINTEGER *StringLength)
{
	ConnectionClass *conn = (ConnectionClass *) ConnectionHandle;

	mylog("[[SQLGetConnectAttr]] %d\n", Attribute);
	switch (Attribute)
	{
		case SQL_ATTR_ASYNC_ENABLE:
		case SQL_ATTR_AUTO_IPD:
		case SQL_ATTR_CONNECTION_DEAD:
		case SQL_ATTR_CONNECTION_TIMEOUT:
		case SQL_ATTR_METADATA_ID:
			conn->errornumber = STMT_INVALID_OPTION_IDENTIFIER;
			conn->errormsg = "Unsupported connection option (Set)";
			return SQL_ERROR;
	}
	return PGAPI_GetConnectOption(ConnectionHandle, (UWORD) Attribute, Value);
}

/*	SQLGetStmtOption -> SQLGetStmtAttr */
RETCODE		SQL_API
SQLGetStmtAttr(HSTMT StatementHandle,
			   SQLINTEGER Attribute, PTR Value,
			   SQLINTEGER BufferLength, SQLINTEGER *StringLength)
{
	static char *func = "SQLGetStmtAttr";
	StatementClass *stmt = (StatementClass *) StatementHandle;
	RETCODE		ret = SQL_SUCCESS;
	int			len = 0;

	mylog("[[%s]] Handle=%u %d\n", func, StatementHandle, Attribute);
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
SQLSetConnectAttr(HDBC ConnectionHandle,
				  SQLINTEGER Attribute, PTR Value,
				  SQLINTEGER StringLength)
{
	ConnectionClass *conn = (ConnectionClass *) ConnectionHandle;

	mylog("[[SQLSetConnectAttr]] %d\n", Attribute);
	switch (Attribute)
	{
		case SQL_ATTR_ASYNC_ENABLE:
		case SQL_ATTR_AUTO_IPD:
		case SQL_ATTR_CONNECTION_DEAD:
		case SQL_ATTR_CONNECTION_TIMEOUT:
		case SQL_ATTR_METADATA_ID:
			conn->errornumber = STMT_INVALID_OPTION_IDENTIFIER;
			conn->errormsg = "Unsupported connection option (Set)";
			return SQL_ERROR;
	}
	return PGAPI_SetConnectOption(ConnectionHandle, (UWORD) Attribute, (UDWORD) Value);
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

/*	new function */
RETCODE		SQL_API
SQLSetDescField(SQLHDESC DescriptorHandle,
				SQLSMALLINT RecNumber, SQLSMALLINT FieldIdentifier,
				PTR Value, SQLINTEGER BufferLength)
{
	RETCODE		ret = SQL_SUCCESS;
	HSTMT		hstmt;
	SQLUINTEGER	descType;
	StatementClass *stmt;
	static const char *func = "SQLSetDescField";

	mylog("[[SQLSetDescField]] h=%u rec=%d field=%d val=%x\n", DescriptorHandle, RecNumber, FieldIdentifier, Value);
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

/*	new fucntion */
RETCODE		SQL_API
SQLSetDescRec(SQLHDESC DescriptorHandle,
			  SQLSMALLINT RecNumber, SQLSMALLINT Type,
			  SQLSMALLINT SubType, SQLINTEGER Length,
			  SQLSMALLINT Precision, SQLSMALLINT Scale,
			  PTR Data, SQLINTEGER *StringLength,
			  SQLINTEGER *Indicator)
{
	const char *func = "SQLSetDescField";

	mylog("[[SQLSetDescRec]]\n");
	mylog("Error not implemented\n");
	return SQL_ERROR;
}

/*	new function */
RETCODE		SQL_API
SQLSetEnvAttr(HENV EnvironmentHandle,
			  SQLINTEGER Attribute, PTR Value,
			  SQLINTEGER StringLength)
{
	EnvironmentClass *env = (EnvironmentClass *) EnvironmentHandle;

	mylog("[[SQLSetEnvAttr]] att=%d,%u\n", Attribute, Value);
	switch (Attribute)
	{
		case SQL_ATTR_CONNECTION_POOLING:
			if ((SQLUINTEGER) Value == SQL_CP_OFF)
				return SQL_SUCCESS;
			break;
		case SQL_ATTR_CP_MATCH:
			/* *((unsigned int *) Value) = SQL_CP_RELAXED_MATCH; */
			return SQL_SUCCESS;
		case SQL_ATTR_ODBC_VERSION:
			if ((SQLUINTEGER) Value == SQL_OV_ODBC2)
				EN_set_odbc2(env);
			else
				EN_set_odbc3(env);
			return SQL_SUCCESS;
			break;
		case SQL_ATTR_OUTPUT_NTS:
			if ((SQLUINTEGER) Value == SQL_TRUE)
				return SQL_SUCCESS;
			break;
		default:
			env->errornumber = CONN_INVALID_ARGUMENT_NO;
			return SQL_ERROR;
	}
	env->errornumber = CONN_OPTION_VALUE_CHANGED;
	env->errormsg = "SetEnv changed to ";
	return SQL_SUCCESS_WITH_INFO;
}

/*	SQLSet(Param/Scroll/Stmt)Option -> SQLSetStmtAttr */
RETCODE		SQL_API
SQLSetStmtAttr(HSTMT StatementHandle,
			   SQLINTEGER Attribute, PTR Value,
			   SQLINTEGER StringLength)
{
	static char *func = "SQLSetStmtAttr";
	StatementClass *stmt = (StatementClass *) StatementHandle;

	mylog("[[%s]] Handle=%u %d,%u\n", func, StatementHandle, Attribute, Value);
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

#define SQL_FUNC_ESET(pfExists, uwAPI) \
		(*(((UWORD*) (pfExists)) + ((uwAPI) >> 4)) \
			|= (1 << ((uwAPI) & 0x000F)) \
				)
RETCODE		SQL_API
PGAPI_GetFunctions30(HDBC hdbc, UWORD fFunction, UWORD FAR * pfExists)
{
	ConnectionClass	*conn = (ConnectionClass *) hdbc;
	ConnInfo	*ci = &(conn->connInfo);

	if (fFunction != SQL_API_ODBC3_ALL_FUNCTIONS)
		return SQL_ERROR;
	memset(pfExists, 0, sizeof(UWORD) * SQL_API_ODBC3_ALL_FUNCTIONS_SIZE);

	/* SQL_FUNC_ESET(pfExists, SQL_API_SQLALLOCCONNECT); 1 deprecated */
	/* SQL_FUNC_ESET(pfExists, SQL_API_SQLALLOCENV); 2 deprecated */
	/* SQL_FUNC_ESET(pfExists, SQL_API_SQLALLOCSTMT); 3 deprecated */

	/*
	 * for (i = SQL_API_SQLBINDCOL; i <= 23; i++) SQL_FUNC_ESET(pfExists,
	 * i);
	 */
	SQL_FUNC_ESET(pfExists, SQL_API_SQLBINDCOL);		/* 4 */
	SQL_FUNC_ESET(pfExists, SQL_API_SQLCANCEL); /* 5 */
	SQL_FUNC_ESET(pfExists, SQL_API_SQLCOLATTRIBUTE);	/* 6 */
	SQL_FUNC_ESET(pfExists, SQL_API_SQLCONNECT);		/* 7 */
	SQL_FUNC_ESET(pfExists, SQL_API_SQLDESCRIBECOL);	/* 8 */
	SQL_FUNC_ESET(pfExists, SQL_API_SQLDISCONNECT);		/* 9 */
	/* SQL_FUNC_ESET(pfExists, SQL_API_SQLERROR);  10 deprecated */
	SQL_FUNC_ESET(pfExists, SQL_API_SQLEXECDIRECT);		/* 11 */
	SQL_FUNC_ESET(pfExists, SQL_API_SQLEXECUTE);		/* 12 */
	SQL_FUNC_ESET(pfExists, SQL_API_SQLFETCH);	/* 13 */
	/* SQL_FUNC_ESET(pfExists, SQL_API_SQLFREECONNECT); 14 deprecated */
	/* SQL_FUNC_ESET(pfExists, SQL_API_SQLFREEENV); 15 deprecated */
	SQL_FUNC_ESET(pfExists, SQL_API_SQLFREESTMT);		/* 16 */
	SQL_FUNC_ESET(pfExists, SQL_API_SQLGETCURSORNAME);	/* 17 */
	SQL_FUNC_ESET(pfExists, SQL_API_SQLNUMRESULTCOLS);	/* 18 */
	SQL_FUNC_ESET(pfExists, SQL_API_SQLPREPARE);		/* 19 */
	SQL_FUNC_ESET(pfExists, SQL_API_SQLROWCOUNT);		/* 20 */
	SQL_FUNC_ESET(pfExists, SQL_API_SQLSETCURSORNAME);	/* 21 */
	/* SQL_FUNC_ESET(pfExists, SQL_API_SQLSETPARAM); 22 deprecated */
	/* SQL_FUNC_ESET(pfExists, SQL_API_SQLTRANSACT); 23 deprecated */

	/*
	 * for (i = 40; i < SQL_API_SQLEXTENDEDFETCH; i++)
	 * SQL_FUNC_ESET(pfExists, i);
	 */
	SQL_FUNC_ESET(pfExists, SQL_API_SQLCOLUMNS);		/* 40 */
	SQL_FUNC_ESET(pfExists, SQL_API_SQLDRIVERCONNECT);	/* 41 */
	/* SQL_FUNC_ESET(pfExists, SQL_API_SQLGETCONNECTOPTION); 42 deprecated */
	SQL_FUNC_ESET(pfExists, SQL_API_SQLGETDATA);		/* 43 */
	SQL_FUNC_ESET(pfExists, SQL_API_SQLGETFUNCTIONS);	/* 44 */
	SQL_FUNC_ESET(pfExists, SQL_API_SQLGETINFO);		/* 45 */
	/* SQL_FUNC_ESET(pfExists, SQL_API_SQLGETSTMTOPTION); 46 deprecated */
	SQL_FUNC_ESET(pfExists, SQL_API_SQLGETTYPEINFO);	/* 47 */
	SQL_FUNC_ESET(pfExists, SQL_API_SQLPARAMDATA);		/* 48 */
	SQL_FUNC_ESET(pfExists, SQL_API_SQLPUTDATA);		/* 49 */

	/* SQL_FUNC_ESET(pfExists, SQL_API_SQLSETCONNECTIONOPTION); 50 deprecated */
	/* SQL_FUNC_ESET(pfExists, SQL_API_SQLSETSTMTOPTION); 51 deprecated */
	SQL_FUNC_ESET(pfExists, SQL_API_SQLSPECIALCOLUMNS);	/* 52 */
	SQL_FUNC_ESET(pfExists, SQL_API_SQLSTATISTICS);		/* 53 */
	SQL_FUNC_ESET(pfExists, SQL_API_SQLTABLES); /* 54 */
	SQL_FUNC_ESET(pfExists, SQL_API_SQLBROWSECONNECT);	/* 55 */
	if (ci->drivers.lie)
		SQL_FUNC_ESET(pfExists, SQL_API_SQLCOLUMNPRIVILEGES); /* 56 not implemented yet */ 
	SQL_FUNC_ESET(pfExists, SQL_API_SQLDATASOURCES);	/* 57 */
	SQL_FUNC_ESET(pfExists, SQL_API_SQLDESCRIBEPARAM);	/* 58 */
	/* SQL_FUNC_ESET(pfExists, SQL_API_SQLEXTENDEDFETCH); 59 deprecated */

	/*
	 * for (++i; i < SQL_API_SQLBINDPARAMETER; i++)
	 * SQL_FUNC_ESET(pfExists, i);
	 */
	SQL_FUNC_ESET(pfExists, SQL_API_SQLFOREIGNKEYS);	/* 60 */
	SQL_FUNC_ESET(pfExists, SQL_API_SQLMORERESULTS);	/* 61 */
	SQL_FUNC_ESET(pfExists, SQL_API_SQLNATIVESQL);		/* 62 */
	SQL_FUNC_ESET(pfExists, SQL_API_SQLNUMPARAMS);		/* 63 */
	/* SQL_FUNC_ESET(pfExists, SQL_API_SQLPARAMOPTIONS); 64 deprecated */
	SQL_FUNC_ESET(pfExists, SQL_API_SQLPRIMARYKEYS);	/* 65 */
	if (ci->drivers.lie)
		SQL_FUNC_ESET(pfExists, SQL_API_SQLPROCEDURECOLUMNS); /* 66 not implemeted yet */ 
	SQL_FUNC_ESET(pfExists, SQL_API_SQLPROCEDURES);		/* 67 */
	SQL_FUNC_ESET(pfExists, SQL_API_SQLSETPOS);		/* 68 */
	/* SQL_FUNC_ESET(pfExists, SQL_API_SQLSETSCROLLOPTIONS); 69 deprecated */
	SQL_FUNC_ESET(pfExists, SQL_API_SQLTABLEPRIVILEGES);		/* 70 */
	/* SQL_FUNC_ESET(pfExists, SQL_API_SQLDRIVERS); */	/* 71 */
	SQL_FUNC_ESET(pfExists, SQL_API_SQLBINDPARAMETER);	/* 72 */

	SQL_FUNC_ESET(pfExists, SQL_API_SQLALLOCHANDLE);	/* 1001 */
	SQL_FUNC_ESET(pfExists, SQL_API_SQLBINDPARAM);		/* 1002 */
	SQL_FUNC_ESET(pfExists, SQL_API_SQLCLOSECURSOR);	/* 1003 */
	if (ci->drivers.lie)
		SQL_FUNC_ESET(pfExists, SQL_API_SQLCOPYDESC); /* 1004 not implemented yet */
	SQL_FUNC_ESET(pfExists, SQL_API_SQLENDTRAN);		/* 1005 */
	SQL_FUNC_ESET(pfExists, SQL_API_SQLFREEHANDLE);		/* 1006 */
	SQL_FUNC_ESET(pfExists, SQL_API_SQLGETCONNECTATTR);	/* 1007 */
	if (ci->drivers.lie)
	{
		SQL_FUNC_ESET(pfExists, SQL_API_SQLGETDESCFIELD); /* 1008 not implemented yet */
		SQL_FUNC_ESET(pfExists, SQL_API_SQLGETDESCREC); /* 1009 not implemented yet */
		SQL_FUNC_ESET(pfExists, SQL_API_SQLGETDIAGFIELD); /* 1010 not implemented yet */
	}
	SQL_FUNC_ESET(pfExists, SQL_API_SQLGETDIAGREC);		/* 1011 */
	SQL_FUNC_ESET(pfExists, SQL_API_SQLGETENVATTR);		/* 1012 */
	SQL_FUNC_ESET(pfExists, SQL_API_SQLGETSTMTATTR);	/* 1014 */
	SQL_FUNC_ESET(pfExists, SQL_API_SQLSETCONNECTATTR);	/* 1016 */
	SQL_FUNC_ESET(pfExists, SQL_API_SQLSETDESCFIELD);	/* 1017 */
	if (ci->drivers.lie)
	{
		SQL_FUNC_ESET(pfExists, SQL_API_SQLSETDESCREC); /* 1018 not implemented yet */
	}
	SQL_FUNC_ESET(pfExists, SQL_API_SQLSETENVATTR);		/* 1019 */
	SQL_FUNC_ESET(pfExists, SQL_API_SQLSETSTMTATTR);	/* 1020 */
	SQL_FUNC_ESET(pfExists, SQL_API_SQLFETCHSCROLL);	/* 1021 */
	if (ci->drivers.lie)
		SQL_FUNC_ESET(pfExists, SQL_API_SQLBULKOPERATIONS); /* 24 not implemented yet */

	return SQL_SUCCESS;
}
