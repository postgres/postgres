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

#include "psqlodbc.h"
#include <stdio.h>
#include <string.h>

#include "pgapifunc.h"
#include "connection.h"
#include "statement.h"

RETCODE		SQL_API
SQLAllocConnect(HENV EnvironmentHandle,
				HDBC FAR * ConnectionHandle)
{
	mylog("[SQLAllocConnect]");
	return PGAPI_AllocConnect(EnvironmentHandle, ConnectionHandle);
}

RETCODE		SQL_API
SQLAllocEnv(HENV FAR * EnvironmentHandle)
{
	mylog("[SQLAllocEnv]");
	return PGAPI_AllocEnv(EnvironmentHandle);
}

RETCODE		SQL_API
SQLAllocStmt(HDBC ConnectionHandle,
			 HSTMT *StatementHandle)
{
	mylog("[SQLAllocStmt]");
	return PGAPI_AllocStmt(ConnectionHandle, StatementHandle);
}

RETCODE		SQL_API
SQLBindCol(HSTMT StatementHandle,
		   SQLUSMALLINT ColumnNumber, SQLSMALLINT TargetType,
		   PTR TargetValue, SQLINTEGER BufferLength,
		   SQLINTEGER *StrLen_or_Ind)
{
	mylog("[SQLBindCol]");
	return PGAPI_BindCol(StatementHandle, ColumnNumber,
				   TargetType, TargetValue, BufferLength, StrLen_or_Ind);
}

RETCODE		SQL_API
SQLCancel(HSTMT StatementHandle)
{
	mylog("[SQLCancel]");
	return PGAPI_Cancel(StatementHandle);
}

RETCODE		SQL_API
SQLColumns(HSTMT StatementHandle,
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


RETCODE		SQL_API
SQLConnect(HDBC ConnectionHandle,
		   SQLCHAR *ServerName, SQLSMALLINT NameLength1,
		   SQLCHAR *UserName, SQLSMALLINT NameLength2,
		   SQLCHAR *Authentication, SQLSMALLINT NameLength3)
{
	mylog("[SQLConnect]");
	return PGAPI_Connect(ConnectionHandle, ServerName, NameLength1,
					 UserName, NameLength2, Authentication, NameLength3);
}

RETCODE		SQL_API
SQLDriverConnect(HDBC hdbc,
				 HWND hwnd,
				 UCHAR FAR * szConnStrIn,
				 SWORD cbConnStrIn,
				 UCHAR FAR * szConnStrOut,
				 SWORD cbConnStrOutMax,
				 SWORD FAR * pcbConnStrOut,
				 UWORD fDriverCompletion)
{
	mylog("[SQLDriverConnect]");
	return PGAPI_DriverConnect(hdbc, hwnd, szConnStrIn, cbConnStrIn,
		szConnStrOut, cbConnStrOutMax, pcbConnStrOut, fDriverCompletion);
}
RETCODE		SQL_API
SQLBrowseConnect(
				 HDBC hdbc,
				 SQLCHAR *szConnStrIn,
				 SQLSMALLINT cbConnStrIn,
				 SQLCHAR *szConnStrOut,
				 SQLSMALLINT cbConnStrOutMax,
				 SQLSMALLINT *pcbConnStrOut)
{
	mylog("[SQLBrowseConnect]");
	return PGAPI_BrowseConnect(hdbc, szConnStrIn, cbConnStrIn,
						   szConnStrOut, cbConnStrOutMax, pcbConnStrOut);
}

RETCODE		SQL_API
SQLDataSources(HENV EnvironmentHandle,
			   SQLUSMALLINT Direction, SQLCHAR *ServerName,
			   SQLSMALLINT BufferLength1, SQLSMALLINT *NameLength1,
			   SQLCHAR *Description, SQLSMALLINT BufferLength2,
			   SQLSMALLINT *NameLength2)
{
	mylog("[SQLDataSources]");

	/*
	 * return PGAPI_DataSources(EnvironmentHandle, Direction, ServerName,
	 * BufferLength1, NameLength1, Description, BufferLength2,
	 * NameLength2);
	 */
	return SQL_ERROR;
}

RETCODE		SQL_API
SQLDescribeCol(HSTMT StatementHandle,
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

RETCODE		SQL_API
SQLDisconnect(HDBC ConnectionHandle)
{
	mylog("[SQLDisconnect]");
	return PGAPI_Disconnect(ConnectionHandle);
}

RETCODE		SQL_API
SQLError(HENV EnvironmentHandle,
		 HDBC ConnectionHandle, HSTMT StatementHandle,
		 SQLCHAR *Sqlstate, SQLINTEGER *NativeError,
		 SQLCHAR *MessageText, SQLSMALLINT BufferLength,
		 SQLSMALLINT *TextLength)
{
	mylog("[SQLError]");
	return PGAPI_Error(EnvironmentHandle, ConnectionHandle, StatementHandle,
		   Sqlstate, NativeError, MessageText, BufferLength, TextLength);
}

RETCODE		SQL_API
SQLExecDirect(HSTMT StatementHandle,
			  SQLCHAR *StatementText, SQLINTEGER TextLength)
{
	mylog("[SQLExecDirect]");
	return PGAPI_ExecDirect(StatementHandle, StatementText, TextLength);
}

RETCODE		SQL_API
SQLExecute(HSTMT StatementHandle)
{
	mylog("[SQLExecute]");
	return PGAPI_Execute(StatementHandle);
}

RETCODE		SQL_API
SQLFetch(HSTMT StatementHandle)
{
	static char *func = "SQLFetch";

#if (ODBCVER >= 0x0300)
	StatementClass *stmt = (StatementClass *) StatementHandle;
	ConnectionClass *conn = SC_get_conn(stmt);

	if (conn->driver_version >= 0x0300)
	{
		SQLUSMALLINT *rowStatusArray = stmt->options.rowStatusArray;
		SQLINTEGER *pcRow = stmt->options.rowsFetched;

		mylog("[[%s]]", func);
		return PGAPI_ExtendedFetch(StatementHandle, SQL_FETCH_NEXT, 0,
								   pcRow, rowStatusArray);
	}
#endif
	mylog("[%s]", func);
	return PGAPI_Fetch(StatementHandle);
}

RETCODE		SQL_API
SQLFreeConnect(HDBC ConnectionHandle)
{
	mylog("[SQLFreeStmt]");
	return PGAPI_FreeConnect(ConnectionHandle);
}

RETCODE		SQL_API
SQLFreeEnv(HENV EnvironmentHandle)
{
	mylog("[SQLFreeEnv]");
	return PGAPI_FreeEnv(EnvironmentHandle);
}

RETCODE		SQL_API
SQLFreeStmt(HSTMT StatementHandle,
			SQLUSMALLINT Option)
{
	mylog("[SQLFreeStmt]");
	return PGAPI_FreeStmt(StatementHandle, Option);
}

RETCODE		SQL_API
SQLGetConnectOption(HDBC ConnectionHandle,
					SQLUSMALLINT Option, PTR Value)
{
	mylog("[SQLGetConnectOption]");
	return PGAPI_GetConnectOption(ConnectionHandle, Option, Value);
}
RETCODE		SQL_API
SQLGetCursorName(HSTMT StatementHandle,
				 SQLCHAR *CursorName, SQLSMALLINT BufferLength,
				 SQLSMALLINT *NameLength)
{
	mylog("[SQLGetCursorName]");
	return PGAPI_GetCursorName(StatementHandle, CursorName, BufferLength,
							   NameLength);
}

RETCODE		SQL_API
SQLGetData(HSTMT StatementHandle,
		   SQLUSMALLINT ColumnNumber, SQLSMALLINT TargetType,
		   PTR TargetValue, SQLINTEGER BufferLength,
		   SQLINTEGER *StrLen_or_Ind)
{
	mylog("[SQLGetData]");
	return PGAPI_GetData(StatementHandle, ColumnNumber, TargetType,
						 TargetValue, BufferLength, StrLen_or_Ind);
}

RETCODE		SQL_API
SQLGetFunctions(HDBC ConnectionHandle,
				SQLUSMALLINT FunctionId, SQLUSMALLINT *Supported)
{
	mylog("[SQLGetFunctions]");
#if (ODBCVER >= 0x0300)
	if (FunctionId == SQL_API_ODBC3_ALL_FUNCTIONS)
		return PGAPI_GetFunctions30(ConnectionHandle, FunctionId, Supported);
#endif
	return PGAPI_GetFunctions(ConnectionHandle, FunctionId, Supported);
}
RETCODE		SQL_API
SQLGetInfo(HDBC ConnectionHandle,
		   SQLUSMALLINT InfoType, PTR InfoValue,
		   SQLSMALLINT BufferLength, SQLSMALLINT *StringLength)
{
#if (ODBCVER >= 0x0300)
	RETCODE		ret;

	mylog("[SQLGetInfo(30)]");
	if ((ret = PGAPI_GetInfo(ConnectionHandle, InfoType, InfoValue,
							 BufferLength, StringLength)) == SQL_ERROR)
	{
		if (((ConnectionClass *) ConnectionHandle)->driver_version >= 0x0300)
			return PGAPI_GetInfo30(ConnectionHandle, InfoType, InfoValue,
								   BufferLength, StringLength);
	}
	return ret;
#else
	mylog("[SQLGetInfo]");
	return PGAPI_GetInfo(ConnectionHandle, InfoType, InfoValue,
						 BufferLength, StringLength);
#endif
}

RETCODE		SQL_API
SQLGetStmtOption(HSTMT StatementHandle,
				 SQLUSMALLINT Option, PTR Value)
{
	mylog("[SQLGetStmtOption]");
	return PGAPI_GetStmtOption(StatementHandle, Option, Value);
}

RETCODE		SQL_API
SQLGetTypeInfo(HSTMT StatementHandle,
			   SQLSMALLINT DataType)
{
	mylog("[SQLGetTypeInfo]");
	return PGAPI_GetTypeInfo(StatementHandle, DataType);
}

RETCODE		SQL_API
SQLNumResultCols(HSTMT StatementHandle,
				 SQLSMALLINT *ColumnCount)
{
	mylog("[SQLNumResultCols]");
	return PGAPI_NumResultCols(StatementHandle, ColumnCount);
}

RETCODE		SQL_API
SQLParamData(HSTMT StatementHandle,
			 PTR *Value)
{
	mylog("[SQLParamData]");
	return PGAPI_ParamData(StatementHandle, Value);
}

RETCODE		SQL_API
SQLPrepare(HSTMT StatementHandle,
		   SQLCHAR *StatementText, SQLINTEGER TextLength)
{
	mylog("[SQLPrepare]");
	return PGAPI_Prepare(StatementHandle, StatementText, TextLength);
}

RETCODE		SQL_API
SQLPutData(HSTMT StatementHandle,
		   PTR Data, SQLINTEGER StrLen_or_Ind)
{
	mylog("[SQLPutData]");
	return PGAPI_PutData(StatementHandle, Data, StrLen_or_Ind);
}

RETCODE		SQL_API
SQLRowCount(HSTMT StatementHandle,
			SQLINTEGER *RowCount)
{
	mylog("[SQLRowCount]");
	return PGAPI_RowCount(StatementHandle, RowCount);
}

RETCODE		SQL_API
SQLSetConnectOption(HDBC ConnectionHandle,
					SQLUSMALLINT Option, SQLUINTEGER Value)
{
	mylog("[SQLSetConnectionOption]");
	return PGAPI_SetConnectOption(ConnectionHandle, Option, Value);
}

RETCODE		SQL_API
SQLSetCursorName(HSTMT StatementHandle,
				 SQLCHAR *CursorName, SQLSMALLINT NameLength)
{
	mylog("[SQLSetCursorName]");
	return PGAPI_SetCursorName(StatementHandle, CursorName, NameLength);
}

RETCODE		SQL_API
SQLSetParam(HSTMT StatementHandle,
			SQLUSMALLINT ParameterNumber, SQLSMALLINT ValueType,
			SQLSMALLINT ParameterType, SQLUINTEGER LengthPrecision,
			SQLSMALLINT ParameterScale, PTR ParameterValue,
			SQLINTEGER *StrLen_or_Ind)
{
	mylog("[SQLSetParam]");

	/*
	 * return PGAPI_SetParam(StatementHandle, ParameterNumber, ValueType,
	 * ParameterType, LengthPrecision, ParameterScale, ParameterValue,
	 * StrLen_or_Ind);
	 */
	return SQL_ERROR;
}

RETCODE		SQL_API
SQLSetStmtOption(HSTMT StatementHandle,
				 SQLUSMALLINT Option, SQLUINTEGER Value)
{
	mylog("[SQLSetStmtOption]");
	return PGAPI_SetStmtOption(StatementHandle, Option, Value);
}

RETCODE		SQL_API
SQLSpecialColumns(HSTMT StatementHandle,
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

RETCODE		SQL_API
SQLStatistics(HSTMT StatementHandle,
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

RETCODE		SQL_API
SQLTables(HSTMT StatementHandle,
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

RETCODE		SQL_API
SQLTransact(HENV EnvironmentHandle,
			HDBC ConnectionHandle, SQLUSMALLINT CompletionType)
{
	mylog("[SQLTransact]");
	return PGAPI_Transact(EnvironmentHandle, ConnectionHandle, CompletionType);
}

RETCODE		SQL_API
SQLColAttributes(
				 HSTMT hstmt,
				 SQLUSMALLINT icol,
				 SQLUSMALLINT fDescType,
				 PTR rgbDesc,
				 SQLSMALLINT cbDescMax,
				 SQLSMALLINT *pcbDesc,
				 SQLINTEGER *pfDesc)
{
	mylog("[SQLColAttributes]");
	return PGAPI_ColAttributes(hstmt, icol, fDescType, rgbDesc,
							   cbDescMax, pcbDesc, pfDesc);
}

RETCODE		SQL_API
SQLColumnPrivileges(
					HSTMT hstmt,
					SQLCHAR *szCatalogName,
					SQLSMALLINT cbCatalogName,
					SQLCHAR *szSchemaName,
					SQLSMALLINT cbSchemaName,
					SQLCHAR *szTableName,
					SQLSMALLINT cbTableName,
					SQLCHAR *szColumnName,
					SQLSMALLINT cbColumnName)
{
	mylog("[SQLColumnPrivileges]");
	return PGAPI_ColumnPrivileges(hstmt, szCatalogName, cbCatalogName,
					szSchemaName, cbSchemaName, szTableName, cbTableName,
								  szColumnName, cbColumnName);
}

RETCODE		SQL_API
SQLDescribeParam(
				 HSTMT hstmt,
				 SQLUSMALLINT ipar,
				 SQLSMALLINT *pfSqlType,
				 SQLUINTEGER *pcbParamDef,
				 SQLSMALLINT *pibScale,
				 SQLSMALLINT *pfNullable)
{
	mylog("[SQLDescribeParam]");
	return PGAPI_DescribeParam(hstmt, ipar, pfSqlType, pcbParamDef,
							   pibScale, pfNullable);
}

RETCODE		SQL_API
SQLExtendedFetch(
				 HSTMT hstmt,
				 SQLUSMALLINT fFetchType,
				 SQLINTEGER irow,
				 SQLUINTEGER *pcrow,
				 SQLUSMALLINT *rgfRowStatus)
{
	mylog("[SQLExtendedFetch]");
	return PGAPI_ExtendedFetch(hstmt, fFetchType, irow, pcrow, rgfRowStatus);
}

RETCODE		SQL_API
SQLForeignKeys(
			   HSTMT hstmt,
			   SQLCHAR *szPkCatalogName,
			   SQLSMALLINT cbPkCatalogName,
			   SQLCHAR *szPkSchemaName,
			   SQLSMALLINT cbPkSchemaName,
			   SQLCHAR *szPkTableName,
			   SQLSMALLINT cbPkTableName,
			   SQLCHAR *szFkCatalogName,
			   SQLSMALLINT cbFkCatalogName,
			   SQLCHAR *szFkSchemaName,
			   SQLSMALLINT cbFkSchemaName,
			   SQLCHAR *szFkTableName,
			   SQLSMALLINT cbFkTableName)
{
	mylog("[SQLForeignKeys]");
	return PGAPI_ForeignKeys(hstmt, szPkCatalogName, cbPkCatalogName,
						   szPkSchemaName, cbPkSchemaName, szPkTableName,
						 cbPkTableName, szFkCatalogName, cbFkCatalogName,
		   szFkSchemaName, cbFkSchemaName, szFkTableName, cbFkTableName);
}

RETCODE		SQL_API
SQLMoreResults(HSTMT hstmt)
{
	mylog("[SQLMoreResults]");
	return PGAPI_MoreResults(hstmt);
}

RETCODE		SQL_API
SQLNativeSql(
			 HDBC hdbc,
			 SQLCHAR *szSqlStrIn,
			 SQLINTEGER cbSqlStrIn,
			 SQLCHAR *szSqlStr,
			 SQLINTEGER cbSqlStrMax,
			 SQLINTEGER *pcbSqlStr)
{
	mylog("[SQLNativeSql]");
	return PGAPI_NativeSql(hdbc, szSqlStrIn, cbSqlStrIn, szSqlStr,
						   cbSqlStrMax, pcbSqlStr);
}

RETCODE		SQL_API
SQLNumParams(
			 HSTMT hstmt,
			 SQLSMALLINT *pcpar)
{
	mylog("[SQLNumParams]");
	return PGAPI_NumParams(hstmt, pcpar);
}

RETCODE		SQL_API
SQLParamOptions(
				HSTMT hstmt,
				SQLUINTEGER crow,
				SQLUINTEGER *pirow)
{
	mylog("[SQLParamOptions]");
	return PGAPI_ParamOptions(hstmt, crow, pirow);
}

RETCODE		SQL_API
SQLPrimaryKeys(
			   HSTMT hstmt,
			   SQLCHAR *szCatalogName,
			   SQLSMALLINT cbCatalogName,
			   SQLCHAR *szSchemaName,
			   SQLSMALLINT cbSchemaName,
			   SQLCHAR *szTableName,
			   SQLSMALLINT cbTableName)
{
	mylog("[SQLPrimaryKeys]");
	return PGAPI_PrimaryKeys(hstmt, szCatalogName, cbCatalogName,
				   szSchemaName, cbSchemaName, szTableName, cbTableName);
}

RETCODE		SQL_API
SQLProcedureColumns(
					HSTMT hstmt,
					SQLCHAR *szCatalogName,
					SQLSMALLINT cbCatalogName,
					SQLCHAR *szSchemaName,
					SQLSMALLINT cbSchemaName,
					SQLCHAR *szProcName,
					SQLSMALLINT cbProcName,
					SQLCHAR *szColumnName,
					SQLSMALLINT cbColumnName)
{
	mylog("[SQLProcedureColumns]");
	return PGAPI_ProcedureColumns(hstmt, szCatalogName, cbCatalogName,
					  szSchemaName, cbSchemaName, szProcName, cbProcName,
								  szColumnName, cbColumnName);
}

RETCODE		SQL_API
SQLProcedures(
			  HSTMT hstmt,
			  SQLCHAR *szCatalogName,
			  SQLSMALLINT cbCatalogName,
			  SQLCHAR *szSchemaName,
			  SQLSMALLINT cbSchemaName,
			  SQLCHAR *szProcName,
			  SQLSMALLINT cbProcName)
{
	mylog("[SQLProcedures]");
	return PGAPI_Procedures(hstmt, szCatalogName, cbCatalogName,
					 szSchemaName, cbSchemaName, szProcName, cbProcName);
}

RETCODE		SQL_API
SQLSetPos(
		  HSTMT hstmt,
		  SQLUSMALLINT irow,
		  SQLUSMALLINT fOption,
		  SQLUSMALLINT fLock)
{
	mylog("[SQLSetPos]");
	return PGAPI_SetPos(hstmt, irow, fOption, fLock);
}

RETCODE		SQL_API
SQLTablePrivileges(
				   HSTMT hstmt,
				   SQLCHAR *szCatalogName,
				   SQLSMALLINT cbCatalogName,
				   SQLCHAR *szSchemaName,
				   SQLSMALLINT cbSchemaName,
				   SQLCHAR *szTableName,
				   SQLSMALLINT cbTableName)
{
	mylog("[SQLTablePrivileges]");
	return PGAPI_TablePrivileges(hstmt, szCatalogName, cbCatalogName,
				   szSchemaName, cbSchemaName, szTableName, cbTableName);
}

RETCODE		SQL_API
SQLBindParameter(
				 HSTMT hstmt,
				 SQLUSMALLINT ipar,
				 SQLSMALLINT fParamType,
				 SQLSMALLINT fCType,
				 SQLSMALLINT fSqlType,
				 SQLUINTEGER cbColDef,
				 SQLSMALLINT ibScale,
				 PTR rgbValue,
				 SQLINTEGER cbValueMax,
				 SQLINTEGER *pcbValue)
{
	mylog("[SQLBindParameter]");
	return PGAPI_BindParameter(hstmt, ipar, fParamType, fCType,
					   fSqlType, cbColDef, ibScale, rgbValue, cbValueMax,
							   pcbValue);
}
