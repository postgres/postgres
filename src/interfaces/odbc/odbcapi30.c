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

/*	new function */
RETCODE		SQL_API
SQLCopyDesc(SQLHDESC SourceDescHandle,
			SQLHDESC TargetDescHandle)
{
	mylog("[[SQLCopyDesc]]\n");
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
	return SQL_ERROR;			/* SQLSTATE HY092 ("Invalid
								 * attribute/option identifier") */

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
			FetchOffset += *((Int4 *) stmt->options.bookmark_ptr);
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
	RETCODE		ret;

	mylog("[[SQLGetDiagRec]]\n");
	switch (HandleType)
	{
		case SQL_HANDLE_ENV:
			ret = PGAPI_Error(Handle, NULL, NULL, Sqlstate, NativeError,
							  MessageText, BufferLength, TextLength);
			break;
		case SQL_HANDLE_DBC:
			ret = PGAPI_Error(NULL, Handle, NULL, Sqlstate, NativeError,
							  MessageText, BufferLength, TextLength);
			break;
		case SQL_HANDLE_STMT:
			ret = PGAPI_Error(NULL, NULL, Handle, Sqlstate, NativeError,
							  MessageText, BufferLength, TextLength);
			break;
		default:
			ret = SQL_ERROR;
	}
	if (ret == SQL_SUCCESS_WITH_INFO &&
		BufferLength == 0 &&
		*TextLength)
	{
		SQLSMALLINT BufferLength = *TextLength + 4;
		SQLCHAR    *MessageText = malloc(BufferLength);

		ret = SQLGetDiagRec(HandleType, Handle, RecNumber, Sqlstate,
							NativeError, MessageText, BufferLength,
							TextLength);
		free(MessageText);
	}
	return ret;
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
			*((unsigned int *) Value) = SQL_OV_ODBC3;
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

	mylog("[[%s]] %d\n", func, Attribute);
	switch (Attribute)
	{
		case SQL_ATTR_FETCH_BOOKMARK_PTR:		/* 16 */
			Value = stmt->options.bookmark_ptr;

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
			*((HSTMT *) Value) = StatementHandle;		/* this is useless */
			len = 4;
			break;
		case SQL_ATTR_APP_PARAM_DESC:	/* 10011 */
			*((HSTMT *) Value) = StatementHandle;		/* this is useless */
			len = 4;
			break;
		case SQL_ATTR_IMP_ROW_DESC:		/* 10012 */
			*((HSTMT *) Value) = StatementHandle;		/* this is useless */
			len = 4;
			break;
		case SQL_ATTR_IMP_PARAM_DESC:	/* 10013 */
			*((HSTMT *) Value) = StatementHandle;		/* this is useless */
			len = 4;
			break;
		case SQL_ATTR_AUTO_IPD:	/* 10001 */
			/* case SQL_ATTR_ROW_BIND_TYPE: ** == SQL_BIND_TYPE(ODBC2.0) */
		case SQL_ATTR_PARAMSET_SIZE:	/* 22 */
		case SQL_ATTR_PARAM_STATUS_PTR: /* 20 */
		case SQL_ATTR_PARAMS_PROCESSED_PTR:		/* 21 */

		case SQL_ATTR_CURSOR_SCROLLABLE:		/* -1 */
		case SQL_ATTR_CURSOR_SENSITIVITY:		/* -2 */

		case SQL_ATTR_ENABLE_AUTO_IPD:	/* 15 */
		case SQL_ATTR_METADATA_ID:		/* 10014 */

			/*
			 * case SQL_ATTR_PREDICATE_PTR: case
			 * SQL_ATTR_PREDICATE_OCTET_LENGTH_PTR:
			 */
		case SQL_ATTR_PARAM_BIND_OFFSET_PTR:	/* 17 */
		case SQL_ATTR_PARAM_BIND_TYPE:	/* 18 */
		case SQL_ATTR_PARAM_OPERATION_PTR:		/* 19 */
		case SQL_ATTR_ROW_BIND_OFFSET_PTR:		/* 23 */
		case SQL_ATTR_ROW_OPERATION_PTR:		/* 24 */
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

/*	new function */
RETCODE		SQL_API
SQLSetDescField(SQLHDESC DescriptorHandle,
				SQLSMALLINT RecNumber, SQLSMALLINT FieldIdentifier,
				PTR Value, SQLINTEGER BufferLength)
{
	mylog("[[SQLSetDescField]]\n");
	return SQL_ERROR;
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
	mylog("[[SQLsetDescRec]]\n");
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
	UDWORD		rowcount;

	mylog("[[%s]] %d,%u\n", func, Attribute, Value);
	switch (Attribute)
	{
		case SQL_ATTR_PARAMSET_SIZE:	/* 22 */
			return PGAPI_ParamOptions(StatementHandle, (UWORD) Value, &rowcount);
		case SQL_ATTR_PARAM_STATUS_PTR: /* 20 */
		case SQL_ATTR_PARAMS_PROCESSED_PTR:		/* 21 */

		case SQL_ATTR_CURSOR_SCROLLABLE:		/* -1 */
		case SQL_ATTR_CURSOR_SENSITIVITY:		/* -2 */

		case SQL_ATTR_ENABLE_AUTO_IPD:	/* 15 */

		case SQL_ATTR_APP_ROW_DESC:		/* 10010 */
		case SQL_ATTR_APP_PARAM_DESC:	/* 10011 */
		case SQL_ATTR_AUTO_IPD:	/* 10001 */
			/* case SQL_ATTR_ROW_BIND_TYPE: ** == SQL_BIND_TYPE(ODBC2.0) */
		case SQL_ATTR_IMP_ROW_DESC:		/* 10012 */
		case SQL_ATTR_IMP_PARAM_DESC:	/* 10013 */
		case SQL_ATTR_METADATA_ID:		/* 10014 */

			/*
			 * case SQL_ATTR_PREDICATE_PTR: case
			 * SQL_ATTR_PREDICATE_OCTET_LENGTH_PTR:
			 */
		case SQL_ATTR_PARAM_BIND_OFFSET_PTR:	/* 17 */
		case SQL_ATTR_PARAM_BIND_TYPE:	/* 18 */
		case SQL_ATTR_PARAM_OPERATION_PTR:		/* 19 */
		case SQL_ATTR_ROW_BIND_OFFSET_PTR:		/* 23 */
		case SQL_ATTR_ROW_OPERATION_PTR:		/* 24 */
			stmt->errornumber = STMT_INVALID_OPTION_IDENTIFIER;
			stmt->errormsg = "Unsupported statement option (Set)";
			SC_log_error(func, "", stmt);
			return SQL_ERROR;

		case SQL_ATTR_FETCH_BOOKMARK_PTR:		/* 16 */
			stmt->options.bookmark_ptr = Value;

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

	/*
	 * SQL_FUNC_ESET(pfExists, SQL_API_SQLSETCONNECTIONOPTION); 50
	 * deprecated
	 */
	/* SQL_FUNC_ESET(pfExists, SQL_API_SQLSETSTMTOPTION); 51 deprecated */
	SQL_FUNC_ESET(pfExists, SQL_API_SQLSPECIALCOLUMNS); /* 52 */
	SQL_FUNC_ESET(pfExists, SQL_API_SQLSTATISTICS);		/* 53 */
	SQL_FUNC_ESET(pfExists, SQL_API_SQLTABLES); /* 54 */
	SQL_FUNC_ESET(pfExists, SQL_API_SQLBROWSECONNECT);	/* 55 */
	SQL_FUNC_ESET(pfExists, SQL_API_SQLCOLUMNPRIVILEGES);		/* 56 */
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
	SQL_FUNC_ESET(pfExists, SQL_API_SQLPROCEDURECOLUMNS);		/* 66 */
	SQL_FUNC_ESET(pfExists, SQL_API_SQLPROCEDURES);		/* 67 */
	SQL_FUNC_ESET(pfExists, SQL_API_SQLSETPOS); /* 68 */
	SQL_FUNC_ESET(pfExists, SQL_API_SQLSETSCROLLOPTIONS);		/* 69 deprecated */
	SQL_FUNC_ESET(pfExists, SQL_API_SQLTABLEPRIVILEGES);		/* 70 */
	/* SQL_FUNC_ESET(pfExists, SQL_API_SQLDRIVERS); */	/* 71 */
	SQL_FUNC_ESET(pfExists, SQL_API_SQLBINDPARAMETER);	/* 72 */

	SQL_FUNC_ESET(pfExists, SQL_API_SQLALLOCHANDLE);	/* 1001 */
	SQL_FUNC_ESET(pfExists, SQL_API_SQLBINDPARAM);		/* 1002 */
	SQL_FUNC_ESET(pfExists, SQL_API_SQLCLOSECURSOR);	/* 1003 */
	SQL_FUNC_ESET(pfExists, SQL_API_SQLCOPYDESC);		/* 1004 not implemented
														 * yet */
	SQL_FUNC_ESET(pfExists, SQL_API_SQLENDTRAN);		/* 1005 */
	SQL_FUNC_ESET(pfExists, SQL_API_SQLFREEHANDLE);		/* 1006 */
	SQL_FUNC_ESET(pfExists, SQL_API_SQLGETCONNECTATTR); /* 1007 */
	SQL_FUNC_ESET(pfExists, SQL_API_SQLGETDESCFIELD);	/* 1008 not implemented
														 * yet */
	SQL_FUNC_ESET(pfExists, SQL_API_SQLGETDESCREC);		/* 1009 not implemented
														 * yet */
	SQL_FUNC_ESET(pfExists, SQL_API_SQLGETDIAGFIELD);	/* 1010 not implemented
														 * yet */
	SQL_FUNC_ESET(pfExists, SQL_API_SQLGETDIAGREC);		/* 1011 */
	SQL_FUNC_ESET(pfExists, SQL_API_SQLGETENVATTR);		/* 1012 */
	SQL_FUNC_ESET(pfExists, SQL_API_SQLGETSTMTATTR);	/* 1014 */
	SQL_FUNC_ESET(pfExists, SQL_API_SQLSETCONNECTATTR); /* 1016 */
	SQL_FUNC_ESET(pfExists, SQL_API_SQLSETDESCFIELD);	/* 1017 not implemeted
														 * yet */
	SQL_FUNC_ESET(pfExists, SQL_API_SQLSETDESCREC);		/* 1018 not implemented
														 * yet */
	SQL_FUNC_ESET(pfExists, SQL_API_SQLSETENVATTR);		/* 1019 */
	SQL_FUNC_ESET(pfExists, SQL_API_SQLSETSTMTATTR);	/* 1020 */
	SQL_FUNC_ESET(pfExists, SQL_API_SQLFETCHSCROLL);	/* 1021 */
	SQL_FUNC_ESET(pfExists, SQL_API_SQLBULKOPERATIONS); /* 24 not implemented
														 * yet */

	return SQL_SUCCESS;
}
RETCODE		SQL_API
PGAPI_GetInfo30(HDBC hdbc, UWORD fInfoType, PTR rgbInfoValue,
				SWORD cbInfoValueMax, SWORD FAR * pcbInfoValue)
{
	static char *func = "PGAPI_GetInfo30";
	ConnectionClass *conn = (ConnectionClass *) hdbc;
	char	   *p = NULL;
	int			len = 0,
				value = 0;
	RETCODE		result;

	switch (fInfoType)
	{
		case SQL_DYNAMIC_CURSOR_ATTRIBUTES1:
			len = 4;
			value = 0;
			break;
		case SQL_DYNAMIC_CURSOR_ATTRIBUTES2:
			len = 4;
			value = 0;
			break;

		case SQL_FORWARD_ONLY_CURSOR_ATTRIBUTES1:
			len = 4;
			value = SQL_CA1_NEXT | SQL_CA1_ABSOLUTE |
				SQL_CA1_RELATIVE | SQL_CA1_BOOKMARK;
			break;
		case SQL_FORWARD_ONLY_CURSOR_ATTRIBUTES2:
			len = 4;
			value = 0;
			break;
		case SQL_KEYSET_CURSOR_ATTRIBUTES1:
			len = 4;
			value = SQL_CA1_NEXT | SQL_CA1_ABSOLUTE
				| SQL_CA1_RELATIVE | SQL_CA1_BOOKMARK
				| SQL_CA1_LOCK_NO_CHANGE | SQL_CA1_POS_POSITION
				| SQL_CA1_POS_UPDATE | SQL_CA1_POS_DELETE
				| SQL_CA1_POS_REFRESH
				| SQL_CA1_BULK_ADD
				| SQL_CA1_BULK_UPDATE_BY_BOOKMARK
				| SQL_CA1_BULK_DELETE_BY_BOOKMARK
				| SQL_CA1_BULK_FETCH_BY_BOOKMARK
				;
			break;
		case SQL_KEYSET_CURSOR_ATTRIBUTES2:
			len = 4;
			value = SQL_CA2_OPT_ROWVER_CONCURRENCY |
				SQL_CA2_SENSITIVITY_ADDITIONS |
				SQL_CA2_SENSITIVITY_DELETIONS |
				SQL_CA2_SENSITIVITY_UPDATES;
			break;

		case SQL_STATIC_CURSOR_ATTRIBUTES1:
			len = 4;
			value = SQL_CA1_NEXT | SQL_CA1_ABSOLUTE |
				SQL_CA1_RELATIVE | SQL_CA1_BOOKMARK |
				SQL_CA1_LOCK_NO_CHANGE | SQL_CA1_POS_POSITION |
				SQL_CA1_POS_UPDATE | SQL_CA1_POS_DELETE |
				SQL_CA1_POS_REFRESH;
			break;
		case SQL_STATIC_CURSOR_ATTRIBUTES2:
			len = 4;
			value = SQL_CA2_OPT_ROWVER_CONCURRENCY |
				SQL_CA2_SENSITIVITY_ADDITIONS |
				SQL_CA2_SENSITIVITY_DELETIONS |
				SQL_CA2_SENSITIVITY_UPDATES;
			break;
		default:
			/* unrecognized key */
			conn->errormsg = "Unrecognized key passed to SQLGetInfo.";
			conn->errornumber = CONN_NOT_IMPLEMENTED_ERROR;
			CC_log_error(func, "", conn);
			return SQL_ERROR;
	}
	result = SQL_SUCCESS;
	if (p)
	{
		/* char/binary data */
		len = strlen(p);

		if (rgbInfoValue)
		{
			strncpy_null((char *) rgbInfoValue, p, (size_t) cbInfoValueMax);

			if (len >= cbInfoValueMax)
			{
				result = SQL_SUCCESS_WITH_INFO;
				conn->errornumber = STMT_TRUNCATED;
				conn->errormsg = "The buffer was too small for tthe InfoValue.";
			}
		}
	}
	else
	{
		/* numeric data */
		if (rgbInfoValue)
		{
			if (len == 2)
				*((WORD *) rgbInfoValue) = (WORD) value;
			else if (len == 4)
				*((DWORD *) rgbInfoValue) = (DWORD) value;
		}
	}

	if (pcbInfoValue)
		*pcbInfoValue = len;
	return result;
}
