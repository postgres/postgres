/*-------
 * Module:			odbcapi.c
 *
 * Description:		This module contains routines related to
 *					preparing and executing an SQL statement.
 *
 * Classes:			n/a
 *
 * API functions:	SQLAllocConnect, SQLAllocEnv, SQLAllocStmt,
			SQLBindCol, SQLCancel, SQLColumns, SQLConnect,
			SQLDataSources, SQLDescribeCol, SQLDisconnect,
			SQLError, SQLExecDirect, SQLExecute, SQLFetch,
			SQLFreeConnect, SQLFreeEnv, SQLFreeStmt,
			SQLGetConnectOption, SQLGetCursorName, SQLGetData,
			SQLGetFunctions, SQLGetInfo, SQLGetStmtOption,
			SQLGetTypeInfo, SQLNumResultCols, SQLParamData,
			SQLPrepare, SQLPutData, SQLRowCount,
			SQLSetConnectOption, SQLSetCursorName, SQLSetParam,
			SQLSetStmtOption, SQLSpecialColumns, SQLStatistics,
			SQLTables, SQLTransact, SQLColAttributes,
 			SQLColumnPrivileges, SQLDescribeParam, SQLExtendedFetch,
 			SQLForeignKeys, SQLMoreResults, SQLNativeSql,
 			SQLNumParams, SQLParamOptions, SQLPrimaryKeys,
 			SQLProcedureColumns, SQLProcedures, SQLSetPos,
 			SQLTablePrivileges, SQLBindParameter
 *-------
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "psqlodbc.h"
#undef	ODBCVER
#define	ODBCVER	0x3000
#include <stdio.h>
#include <string.h>

#ifndef WIN32
#include "iodbc.h"
#include "isqlext.h"
#else
#include <windows.h>
#include <sqlext.h>
#endif
#include "pgapifunc.h"
#include "connection.h"
#include "statement.h"

SQLRETURN  SQL_API SQLAllocConnect(SQLHENV EnvironmentHandle,
           SQLHDBC *ConnectionHandle)
{
	mylog("[SQLAllocConnect]");
	return PGAPI_AllocConnect(EnvironmentHandle, ConnectionHandle);
}

SQLRETURN  SQL_API SQLAllocEnv(SQLHENV *EnvironmentHandle)
{
	mylog("[SQLAllocEnv]");
	return PGAPI_AllocEnv(EnvironmentHandle);
}

SQLRETURN  SQL_API SQLAllocStmt(SQLHDBC ConnectionHandle,
           SQLHSTMT *StatementHandle)
{
	mylog("[SQLAllocStmt]");
	return PGAPI_AllocStmt(ConnectionHandle, StatementHandle);
}

SQLRETURN  SQL_API SQLBindCol(SQLHSTMT StatementHandle, 
		   SQLUSMALLINT ColumnNumber, SQLSMALLINT TargetType, 
		   SQLPOINTER TargetValue, SQLINTEGER BufferLength, 
	   	   SQLINTEGER *StrLen_or_Ind)
{
	mylog("[SQLBindCol]");
	return PGAPI_BindCol(StatementHandle, ColumnNumber,
		  TargetType, TargetValue, BufferLength, StrLen_or_Ind);
}

SQLRETURN  SQL_API SQLCancel(SQLHSTMT StatementHandle)
{
	mylog("[SQLCancel]");
	return PGAPI_Cancel(StatementHandle);
}

SQLRETURN  SQL_API SQLColumns(SQLHSTMT StatementHandle,
           SQLCHAR *CatalogName, SQLSMALLINT NameLength1,
           SQLCHAR *SchemaName, SQLSMALLINT NameLength2,
           SQLCHAR *TableName, SQLSMALLINT NameLength3,
           SQLCHAR *ColumnName, SQLSMALLINT NameLength4)
{
	mylog("[SQLColumns]");
	return PGAPI_Columns(StatementHandle, CatalogName, NameLength1,
           	SchemaName, NameLength2, TableName, NameLength3,
           	ColumnName, NameLength4);
}


SQLRETURN  SQL_API SQLConnect(SQLHDBC ConnectionHandle,
           SQLCHAR *ServerName, SQLSMALLINT NameLength1,
           SQLCHAR *UserName, SQLSMALLINT NameLength2,
           SQLCHAR *Authentication, SQLSMALLINT NameLength3)
{
	mylog("[SQLConnect]");
	return PGAPI_Connect(ConnectionHandle, ServerName, NameLength1,
           	UserName, NameLength2, Authentication, NameLength3);
}

SQLRETURN SQL_API SQLDriverConnect(HDBC hdbc,
                                 HWND hwnd,
                                 UCHAR FAR *szConnStrIn,
                                 SWORD cbConnStrIn,
                                 UCHAR FAR *szConnStrOut,
                                 SWORD cbConnStrOutMax,
                                 SWORD FAR *pcbConnStrOut,
                                 UWORD fDriverCompletion)
{
	mylog("[SQLDriverConnect]");
	return PGAPI_DriverConnect(hdbc, hwnd, szConnStrIn, cbConnStrIn,
		szConnStrOut, cbConnStrOutMax, pcbConnStrOut, fDriverCompletion);
}
SQLRETURN SQL_API SQLBrowseConnect(
    SQLHDBC            hdbc,
    SQLCHAR 		  *szConnStrIn,
    SQLSMALLINT        cbConnStrIn,
    SQLCHAR 		  *szConnStrOut,
    SQLSMALLINT        cbConnStrOutMax,
    SQLSMALLINT       *pcbConnStrOut)
{
	mylog("[SQLBrowseConnect]");
	return PGAPI_BrowseConnect(hdbc, szConnStrIn, cbConnStrIn,
		szConnStrOut, cbConnStrOutMax, pcbConnStrOut);
}

SQLRETURN  SQL_API SQLDataSources(SQLHENV EnvironmentHandle,
           SQLUSMALLINT Direction, SQLCHAR *ServerName,
           SQLSMALLINT BufferLength1, SQLSMALLINT *NameLength1,
           SQLCHAR *Description, SQLSMALLINT BufferLength2,
           SQLSMALLINT *NameLength2)
{
	mylog("[SQLDataSources]");
	/*
	return PGAPI_DataSources(EnvironmentHandle, Direction, ServerName,
		 BufferLength1, NameLength1, Description, BufferLength2,
           	NameLength2);
	*/
	return SQL_ERROR;
}

SQLRETURN  SQL_API SQLDescribeCol(SQLHSTMT StatementHandle,
           SQLUSMALLINT ColumnNumber, SQLCHAR *ColumnName,
           SQLSMALLINT BufferLength, SQLSMALLINT *NameLength,
           SQLSMALLINT *DataType, SQLUINTEGER *ColumnSize,
           SQLSMALLINT *DecimalDigits, SQLSMALLINT *Nullable)
{
	mylog("[SQLDescribeCol]");
	return PGAPI_DescribeCol(StatementHandle, ColumnNumber,
		ColumnName, BufferLength, NameLength,
           	DataType, ColumnSize, DecimalDigits, Nullable);
}

SQLRETURN  SQL_API SQLDisconnect(SQLHDBC ConnectionHandle)
{
	mylog("[SQLDisconnect]");
	return PGAPI_Disconnect(ConnectionHandle);
}

SQLRETURN  SQL_API SQLError(SQLHENV EnvironmentHandle,
           SQLHDBC ConnectionHandle, SQLHSTMT StatementHandle,
           SQLCHAR *Sqlstate, SQLINTEGER *NativeError,
           SQLCHAR *MessageText, SQLSMALLINT BufferLength,
           SQLSMALLINT *TextLength)
{
	mylog("[SQLError]");
	return PGAPI_Error(EnvironmentHandle, ConnectionHandle, StatementHandle,
           	Sqlstate, NativeError, MessageText, BufferLength, TextLength);
}

SQLRETURN  SQL_API SQLExecDirect(SQLHSTMT StatementHandle,
           SQLCHAR *StatementText, SQLINTEGER TextLength)
{
	mylog("[SQLExecDirect]");
	return PGAPI_ExecDirect(StatementHandle, StatementText, TextLength);
}

SQLRETURN  SQL_API SQLExecute(SQLHSTMT StatementHandle)
{
	mylog("[SQLExecute]");
	return PGAPI_Execute(StatementHandle);
}

SQLRETURN  SQL_API SQLFetch(SQLHSTMT StatementHandle)
{
        static char *func = "SQLFetch";
#if (ODBCVER >= 0x3000)
        StatementClass  *stmt = (StatementClass *) StatementHandle;
	ConnectionClass *conn = SC_get_conn(stmt);
	if (conn->driver_version >= 0x0300) 
	{
        	SQLUSMALLINT    *rowStatusArray = stmt->options.rowStatusArray;
        	SQLINTEGER      *pcRow = stmt->options.rowsFetched;

		mylog("[[%s]]", func);
        	return PGAPI_ExtendedFetch(StatementHandle, SQL_FETCH_NEXT, 0,
                        pcRow, rowStatusArray);
	}
#endif
	mylog("[%s]", func);
	return PGAPI_Fetch(StatementHandle);
}

SQLRETURN  SQL_API SQLFreeConnect(SQLHDBC ConnectionHandle)
{
	mylog("[SQLFreeStmt]");
	return PGAPI_FreeConnect(ConnectionHandle);
}

SQLRETURN  SQL_API SQLFreeEnv(SQLHENV EnvironmentHandle)
{
	mylog("[SQLFreeEnv]");
	return PGAPI_FreeEnv(EnvironmentHandle);
}

SQLRETURN  SQL_API SQLFreeStmt(SQLHSTMT StatementHandle,
           SQLUSMALLINT Option)
{
	mylog("[SQLFreeStmt]");
	return PGAPI_FreeStmt(StatementHandle, Option);
}

SQLRETURN  SQL_API SQLGetConnectOption(SQLHDBC ConnectionHandle,
           SQLUSMALLINT Option, SQLPOINTER Value)
{
	mylog("[SQLGetConnectOption]");
	return PGAPI_GetConnectOption(ConnectionHandle, Option, Value);
} 
SQLRETURN  SQL_API SQLGetCursorName(SQLHSTMT StatementHandle,
           SQLCHAR *CursorName, SQLSMALLINT BufferLength,
           SQLSMALLINT *NameLength)
{
	mylog("[SQLGetCursorName]");
	return PGAPI_GetCursorName(StatementHandle, CursorName, BufferLength,
           	NameLength);
}

SQLRETURN  SQL_API SQLGetData(SQLHSTMT StatementHandle,
           SQLUSMALLINT ColumnNumber, SQLSMALLINT TargetType,
           SQLPOINTER TargetValue, SQLINTEGER BufferLength,
           SQLINTEGER *StrLen_or_Ind)
{
	mylog("[SQLGetData]");
	return PGAPI_GetData(StatementHandle, ColumnNumber, TargetType,
           	TargetValue, BufferLength, StrLen_or_Ind);
}

SQLRETURN  SQL_API SQLGetFunctions(SQLHDBC ConnectionHandle,
           SQLUSMALLINT FunctionId, SQLUSMALLINT *Supported)
{
	mylog("[SQLGetFunctions");
#if (ODBCVER >= 0x3000)
	if (FunctionId == SQL_API_ODBC3_ALL_FUNCTIONS)
		return PGAPI_GetFunctions30(ConnectionHandle, FunctionId, Supported);
#endif
	return PGAPI_GetFunctions(ConnectionHandle, FunctionId, Supported);
}
SQLRETURN  SQL_API SQLGetInfo(SQLHDBC ConnectionHandle,
           SQLUSMALLINT InfoType, SQLPOINTER InfoValue,
           SQLSMALLINT BufferLength, SQLSMALLINT *StringLength)
{
	mylog("[SQLGetInfo]");
	return PGAPI_GetInfo(ConnectionHandle, InfoType, InfoValue,
           	BufferLength, StringLength);
}

SQLRETURN  SQL_API SQLGetStmtOption(SQLHSTMT StatementHandle,
           SQLUSMALLINT Option, SQLPOINTER Value)
{
	mylog("[SQLGetStmtOption]");
	return PGAPI_GetStmtOption(StatementHandle, Option, Value);
}

SQLRETURN  SQL_API SQLGetTypeInfo(SQLHSTMT StatementHandle,
           SQLSMALLINT DataType)
{
	mylog("[SQLGetTypeInfo]");
	return PGAPI_GetTypeInfo(StatementHandle,DataType);
}

SQLRETURN  SQL_API SQLNumResultCols(SQLHSTMT StatementHandle,
           SQLSMALLINT *ColumnCount)
{
	mylog("[SQLNumResultCols]");
	return PGAPI_NumResultCols(StatementHandle, ColumnCount);
}

SQLRETURN  SQL_API SQLParamData(SQLHSTMT StatementHandle,
           SQLPOINTER *Value)
{
	mylog("[SQLParamData]");
	return PGAPI_ParamData(StatementHandle, Value);
}

SQLRETURN  SQL_API SQLPrepare(SQLHSTMT StatementHandle,
           SQLCHAR *StatementText, SQLINTEGER TextLength)
{
	mylog("[SQLPrepare]");
	return PGAPI_Prepare(StatementHandle, StatementText, TextLength);
}

SQLRETURN  SQL_API SQLPutData(SQLHSTMT StatementHandle,
           SQLPOINTER Data, SQLINTEGER StrLen_or_Ind)
{
	mylog("[SQLPutData]");
	return PGAPI_PutData(StatementHandle, Data, StrLen_or_Ind);
}

SQLRETURN  SQL_API SQLRowCount(SQLHSTMT StatementHandle, 
	   SQLINTEGER *RowCount)
{
	mylog("[SQLRowCount]");
	return PGAPI_RowCount(StatementHandle, RowCount);
}

SQLRETURN  SQL_API SQLSetConnectOption(SQLHDBC ConnectionHandle,
           SQLUSMALLINT Option, SQLUINTEGER Value)
{
	mylog("[SQLSetConnectionOption]");
	return PGAPI_SetConnectOption(ConnectionHandle, Option, Value);
}

SQLRETURN  SQL_API SQLSetCursorName(SQLHSTMT StatementHandle,
           SQLCHAR *CursorName, SQLSMALLINT NameLength)
{
	mylog("[SQLSetCursorName]");
	return PGAPI_SetCursorName(StatementHandle, CursorName, NameLength);
}

SQLRETURN  SQL_API SQLSetParam(SQLHSTMT StatementHandle,
           SQLUSMALLINT ParameterNumber, SQLSMALLINT ValueType,
           SQLSMALLINT ParameterType, SQLUINTEGER LengthPrecision,
           SQLSMALLINT ParameterScale, SQLPOINTER ParameterValue,
           SQLINTEGER *StrLen_or_Ind)
{
	mylog("[SQLSetParam]");
	/*
	return PGAPI_SetParam(StatementHandle, ParameterNumber, ValueType,
           ParameterType, LengthPrecision, ParameterScale, ParameterValue,
           StrLen_or_Ind);
	*/
        return SQL_ERROR;
}

SQLRETURN  SQL_API SQLSetStmtOption(SQLHSTMT StatementHandle,
           SQLUSMALLINT Option, SQLUINTEGER Value)
{
	mylog("[SQLSetStmtOption]");
	return PGAPI_SetStmtOption(StatementHandle, Option, Value);
}

SQLRETURN  SQL_API SQLSpecialColumns(SQLHSTMT StatementHandle,
           SQLUSMALLINT IdentifierType, SQLCHAR *CatalogName,
           SQLSMALLINT NameLength1, SQLCHAR *SchemaName,
           SQLSMALLINT NameLength2, SQLCHAR *TableName,
           SQLSMALLINT NameLength3, SQLUSMALLINT Scope,
           SQLUSMALLINT Nullable)
{
	mylog("[SQLSpecialColumns]");
	return PGAPI_SpecialColumns(StatementHandle, IdentifierType, CatalogName,
           NameLength1, SchemaName, NameLength2, TableName, NameLength3,
		Scope, Nullable);
}

SQLRETURN  SQL_API SQLStatistics(SQLHSTMT StatementHandle,
           SQLCHAR *CatalogName, SQLSMALLINT NameLength1,
           SQLCHAR *SchemaName, SQLSMALLINT NameLength2,
           SQLCHAR *TableName, SQLSMALLINT NameLength3,
           SQLUSMALLINT Unique, SQLUSMALLINT Reserved)
{
	mylog("[SQLStatistics]");
	return PGAPI_Statistics(StatementHandle, CatalogName, NameLength1,
           SchemaName, NameLength2, TableName, NameLength3, Unique,
		Reserved);
}

SQLRETURN  SQL_API SQLTables(SQLHSTMT StatementHandle,
           SQLCHAR *CatalogName, SQLSMALLINT NameLength1,
           SQLCHAR *SchemaName, SQLSMALLINT NameLength2,
           SQLCHAR *TableName, SQLSMALLINT NameLength3,
           SQLCHAR *TableType, SQLSMALLINT NameLength4)
{
	mylog("[SQLTables]");
	return PGAPI_Tables(StatementHandle, CatalogName, NameLength1,
           SchemaName, NameLength2, TableName, NameLength3,
           TableType, NameLength4);
}

SQLRETURN  SQL_API SQLTransact(SQLHENV EnvironmentHandle,
           SQLHDBC ConnectionHandle, SQLUSMALLINT CompletionType)
{
	mylog("[SQLTransact]");
	return PGAPI_Transact(EnvironmentHandle, ConnectionHandle, CompletionType);
}

SQLRETURN SQL_API SQLColAttributes(
    SQLHSTMT           hstmt,
    SQLUSMALLINT       icol,
    SQLUSMALLINT       fDescType,
    SQLPOINTER         rgbDesc,
    SQLSMALLINT        cbDescMax,
    SQLSMALLINT 	  *pcbDesc,
    SQLINTEGER 		  *pfDesc)
{
	mylog("[SQLColAttributes]");
	return PGAPI_ColAttributes(hstmt, icol, fDescType, rgbDesc,
		cbDescMax, pcbDesc, pfDesc);
}

SQLRETURN SQL_API SQLColumnPrivileges(
    SQLHSTMT           hstmt,
    SQLCHAR 		  *szCatalogName,
    SQLSMALLINT        cbCatalogName,
    SQLCHAR 		  *szSchemaName,
    SQLSMALLINT        cbSchemaName,
    SQLCHAR 		  *szTableName,
    SQLSMALLINT        cbTableName,
    SQLCHAR 		  *szColumnName,
    SQLSMALLINT        cbColumnName)
{
	mylog("[SQLColumnPrivileges]");
	return PGAPI_ColumnPrivileges(hstmt, szCatalogName, cbCatalogName,
		szSchemaName, cbSchemaName, szTableName, cbTableName,
		szColumnName, cbColumnName);
}

SQLRETURN SQL_API SQLDescribeParam(
    SQLHSTMT           hstmt,
    SQLUSMALLINT       ipar,
    SQLSMALLINT 	  *pfSqlType,
    SQLUINTEGER 	  *pcbParamDef,
    SQLSMALLINT 	  *pibScale,
    SQLSMALLINT 	  *pfNullable)
{
	mylog("[SQLDescribeParam]");
	return PGAPI_DescribeParam(hstmt, ipar, pfSqlType, pcbParamDef,
		pibScale, pfNullable);
}

SQLRETURN SQL_API SQLExtendedFetch(
    SQLHSTMT           hstmt,
    SQLUSMALLINT       fFetchType,
    SQLINTEGER         irow,
    SQLUINTEGER 	  *pcrow,
    SQLUSMALLINT 	  *rgfRowStatus)
{
	mylog("[SQLExtendedFetch]");
	return PGAPI_ExtendedFetch(hstmt, fFetchType, irow, pcrow, rgfRowStatus);
}

SQLRETURN SQL_API SQLForeignKeys(
    SQLHSTMT           hstmt,
    SQLCHAR 		  *szPkCatalogName,
    SQLSMALLINT        cbPkCatalogName,
    SQLCHAR 		  *szPkSchemaName,
    SQLSMALLINT        cbPkSchemaName,
    SQLCHAR 		  *szPkTableName,
    SQLSMALLINT        cbPkTableName,
    SQLCHAR 		  *szFkCatalogName,
    SQLSMALLINT        cbFkCatalogName,
    SQLCHAR 		  *szFkSchemaName,
    SQLSMALLINT        cbFkSchemaName,
    SQLCHAR 		  *szFkTableName,
    SQLSMALLINT        cbFkTableName)
{
	mylog("[SQLForeignKeys]");
	return PGAPI_ForeignKeys(hstmt, szPkCatalogName, cbPkCatalogName,
		szPkSchemaName, cbPkSchemaName, szPkTableName,
		cbPkTableName, szFkCatalogName, cbFkCatalogName,
 		szFkSchemaName, cbFkSchemaName, szFkTableName, cbFkTableName);
}

SQLRETURN SQL_API SQLMoreResults(SQLHSTMT hstmt)
{
	mylog("[SQLMoreResults]");
	return PGAPI_MoreResults(hstmt);
}
 
SQLRETURN SQL_API SQLNativeSql(
    SQLHDBC            hdbc,
    SQLCHAR 		  *szSqlStrIn,
    SQLINTEGER         cbSqlStrIn,
    SQLCHAR 		  *szSqlStr,
    SQLINTEGER         cbSqlStrMax,
    SQLINTEGER 		  *pcbSqlStr)
{
	mylog("[SQLNativeSql]");
	return PGAPI_NativeSql(hdbc, szSqlStrIn, cbSqlStrIn, szSqlStr,
		cbSqlStrMax, pcbSqlStr);
}

SQLRETURN SQL_API SQLNumParams(
    SQLHSTMT           hstmt,
    SQLSMALLINT 	  *pcpar)
{
	mylog("[SQLNumParams]");
	return PGAPI_NumParams(hstmt, pcpar);
}

SQLRETURN SQL_API SQLParamOptions(
    SQLHSTMT           hstmt,
    SQLUINTEGER        crow,
    SQLUINTEGER 	  *pirow)
{
	mylog("[SQLParamOptions]");
	return PGAPI_ParamOptions(hstmt, crow, pirow);
}

SQLRETURN SQL_API SQLPrimaryKeys(
    SQLHSTMT           hstmt,
    SQLCHAR 		  *szCatalogName,
    SQLSMALLINT        cbCatalogName,
    SQLCHAR 		  *szSchemaName,
    SQLSMALLINT        cbSchemaName,
    SQLCHAR 		  *szTableName,
    SQLSMALLINT        cbTableName)
{
	mylog("[SQLPrimaryKeys]");
	return PGAPI_PrimaryKeys(hstmt, szCatalogName, cbCatalogName,
		szSchemaName, cbSchemaName, szTableName, cbTableName);
}

SQLRETURN SQL_API SQLProcedureColumns(
    SQLHSTMT           hstmt,
    SQLCHAR 		  *szCatalogName,
    SQLSMALLINT        cbCatalogName,
    SQLCHAR 		  *szSchemaName,
    SQLSMALLINT        cbSchemaName,
    SQLCHAR 		  *szProcName,
    SQLSMALLINT        cbProcName,
    SQLCHAR 		  *szColumnName,
    SQLSMALLINT        cbColumnName)
{
	mylog("[SQLProcedureColumns]");
	return PGAPI_ProcedureColumns(hstmt, szCatalogName, cbCatalogName,
		szSchemaName, cbSchemaName, szProcName, cbProcName,
		szColumnName, cbColumnName);
}

SQLRETURN SQL_API SQLProcedures(
    SQLHSTMT           hstmt,
    SQLCHAR 		  *szCatalogName,
    SQLSMALLINT        cbCatalogName,
    SQLCHAR 		  *szSchemaName,
    SQLSMALLINT        cbSchemaName,
    SQLCHAR 		  *szProcName,
    SQLSMALLINT        cbProcName)
{
	mylog("[SQLProcedures]");
	return PGAPI_Procedures(hstmt, szCatalogName, cbCatalogName,
		szSchemaName, cbSchemaName, szProcName, cbProcName);
}

SQLRETURN SQL_API SQLSetPos(
    SQLHSTMT           hstmt,
    SQLUSMALLINT       irow,
    SQLUSMALLINT       fOption,
    SQLUSMALLINT       fLock)
{
	mylog("[SQLSetPos]");
	return PGAPI_SetPos(hstmt, irow, fOption, fLock);
}

SQLRETURN SQL_API SQLTablePrivileges(
    SQLHSTMT           hstmt,
    SQLCHAR 		  *szCatalogName,
    SQLSMALLINT        cbCatalogName,
    SQLCHAR 		  *szSchemaName,
    SQLSMALLINT        cbSchemaName,
    SQLCHAR 		  *szTableName,
    SQLSMALLINT        cbTableName)
{
	mylog("[SQLTablePrivileges]");
	return PGAPI_TablePrivileges(hstmt, szCatalogName, cbCatalogName,
		szSchemaName, cbSchemaName, szTableName, cbTableName);
}

SQLRETURN SQL_API SQLBindParameter(
    SQLHSTMT           hstmt,
    SQLUSMALLINT       ipar,
    SQLSMALLINT        fParamType,
    SQLSMALLINT        fCType,
    SQLSMALLINT        fSqlType,
    SQLUINTEGER        cbColDef,
    SQLSMALLINT        ibScale,
    SQLPOINTER         rgbValue,
    SQLINTEGER         cbValueMax,
    SQLINTEGER 		  *pcbValue)
{
	mylog("[SQLBindParameter]");
	return PGAPI_BindParameter(hstmt, ipar, fParamType, fCType,
		fSqlType, cbColDef, ibScale, rgbValue, cbValueMax,
		pcbValue);
}
