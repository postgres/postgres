/*-------
 * Module:			pgapifunc.h
 *
 *-------
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

SQLRETURN  SQL_API PGAPI_AllocConnect(SQLHENV EnvironmentHandle,
           SQLHDBC *ConnectionHandle);
SQLRETURN  SQL_API PGAPI_AllocEnv(SQLHENV *EnvironmentHandle);
SQLRETURN  SQL_API PGAPI_AllocStmt(SQLHDBC ConnectionHandle,
           SQLHSTMT *StatementHandle);
SQLRETURN  SQL_API PGAPI_BindCol(SQLHSTMT StatementHandle, 
		   SQLUSMALLINT ColumnNumber, SQLSMALLINT TargetType, 
		   SQLPOINTER TargetValue, SQLINTEGER BufferLength, 
	   	   SQLINTEGER *StrLen_or_Ind);
SQLRETURN  SQL_API PGAPI_Cancel(SQLHSTMT StatementHandle);
SQLRETURN  SQL_API PGAPI_Columns(SQLHSTMT StatementHandle,
           SQLCHAR *CatalogName, SQLSMALLINT NameLength1,
           SQLCHAR *SchemaName, SQLSMALLINT NameLength2,
           SQLCHAR *TableName, SQLSMALLINT NameLength3,
           SQLCHAR *ColumnName, SQLSMALLINT NameLength4);
SQLRETURN  SQL_API PGAPI_Connect(SQLHDBC ConnectionHandle,
           SQLCHAR *ServerName, SQLSMALLINT NameLength1,
           SQLCHAR *UserName, SQLSMALLINT NameLength2,
           SQLCHAR *Authentication, SQLSMALLINT NameLength3);
SQLRETURN  SQL_API PGAPI_DriverConnect(HDBC hdbc, HWND hwnd,
	   UCHAR FAR *szConnStrIn, SWORD cbConnStrIn,
	   UCHAR FAR *szConnStrOut, SWORD cbConnStrOutMax,
	   SWORD FAR *pcbConnStrOut, UWORD fDriverCompletion);
SQLRETURN SQL_API PGAPI_BrowseConnect(SQLHDBC hdbc,
	  SQLCHAR *szConnStrIn, SQLSMALLINT cbConnStrIn,
	  SQLCHAR *szConnStrOut, SQLSMALLINT cbConnStrOutMax,
	  SQLSMALLINT *pcbConnStrOut);
SQLRETURN  SQL_API PGAPI_DataSources(SQLHENV EnvironmentHandle,
           SQLUSMALLINT Direction, SQLCHAR *ServerName,
           SQLSMALLINT BufferLength1, SQLSMALLINT *NameLength1,
           SQLCHAR *Description, SQLSMALLINT BufferLength2,
           SQLSMALLINT *NameLength2);
SQLRETURN  SQL_API PGAPI_DescribeCol(SQLHSTMT StatementHandle,
           SQLUSMALLINT ColumnNumber, SQLCHAR *ColumnName,
           SQLSMALLINT BufferLength, SQLSMALLINT *NameLength,
           SQLSMALLINT *DataType, SQLUINTEGER *ColumnSize,
           SQLSMALLINT *DecimalDigits, SQLSMALLINT *Nullable);
SQLRETURN  SQL_API PGAPI_Disconnect(SQLHDBC ConnectionHandle);
SQLRETURN  SQL_API PGAPI_Error(SQLHENV EnvironmentHandle,
           SQLHDBC ConnectionHandle, SQLHSTMT StatementHandle,
           SQLCHAR *Sqlstate, SQLINTEGER *NativeError,
           SQLCHAR *MessageText, SQLSMALLINT BufferLength,
           SQLSMALLINT *TextLength);
SQLRETURN  SQL_API PGAPI_ExecDirect(SQLHSTMT StatementHandle,
           SQLCHAR *StatementText, SQLINTEGER TextLength);
SQLRETURN  SQL_API PGAPI_Execute(SQLHSTMT StatementHandle);
SQLRETURN  SQL_API PGAPI_Fetch(SQLHSTMT StatementHandle);
SQLRETURN  SQL_API PGAPI_FreeConnect(SQLHDBC ConnectionHandle);
SQLRETURN  SQL_API PGAPI_FreeEnv(SQLHENV EnvironmentHandle);
SQLRETURN  SQL_API PGAPI_FreeStmt(SQLHSTMT StatementHandle,
           SQLUSMALLINT Option);
SQLRETURN  SQL_API PGAPI_GetConnectOption(SQLHDBC ConnectionHandle,
           SQLUSMALLINT Option, SQLPOINTER Value);
SQLRETURN  SQL_API PGAPI_GetCursorName(SQLHSTMT StatementHandle,
           SQLCHAR *CursorName, SQLSMALLINT BufferLength,
           SQLSMALLINT *NameLength);
SQLRETURN  SQL_API PGAPI_GetData(SQLHSTMT StatementHandle,
           SQLUSMALLINT ColumnNumber, SQLSMALLINT TargetType,
           SQLPOINTER TargetValue, SQLINTEGER BufferLength,
           SQLINTEGER *StrLen_or_Ind);
SQLRETURN  SQL_API PGAPI_GetFunctions(SQLHDBC ConnectionHandle,
           SQLUSMALLINT FunctionId, SQLUSMALLINT *Supported);
SQLRETURN  SQL_API PGAPI_GetFunctions30(SQLHDBC ConnectionHandle,
           SQLUSMALLINT FunctionId, SQLUSMALLINT *Supported);
SQLRETURN  SQL_API PGAPI_GetInfo(SQLHDBC ConnectionHandle,
           SQLUSMALLINT InfoType, SQLPOINTER InfoValue,
           SQLSMALLINT BufferLength, SQLSMALLINT *StringLength);
SQLRETURN  SQL_API PGAPI_GetInfo30(SQLHDBC ConnectionHandle,
           SQLUSMALLINT InfoType, SQLPOINTER InfoValue,
           SQLSMALLINT BufferLength, SQLSMALLINT *StringLength);
SQLRETURN  SQL_API PGAPI_GetStmtOption(SQLHSTMT StatementHandle,
           SQLUSMALLINT Option, SQLPOINTER Value);
SQLRETURN  SQL_API PGAPI_GetTypeInfo(SQLHSTMT StatementHandle,
           SQLSMALLINT DataType);
SQLRETURN  SQL_API PGAPI_NumResultCols(SQLHSTMT StatementHandle,
           SQLSMALLINT *ColumnCount);
SQLRETURN  SQL_API PGAPI_ParamData(SQLHSTMT StatementHandle,
           SQLPOINTER *Value);
SQLRETURN  SQL_API PGAPI_Prepare(SQLHSTMT StatementHandle,
           SQLCHAR *StatementText, SQLINTEGER TextLength);
SQLRETURN  SQL_API PGAPI_PutData(SQLHSTMT StatementHandle,
           SQLPOINTER Data, SQLINTEGER StrLen_or_Ind);
SQLRETURN  SQL_API PGAPI_RowCount(SQLHSTMT StatementHandle, 
	   SQLINTEGER *RowCount);
SQLRETURN  SQL_API PGAPI_SetConnectOption(SQLHDBC ConnectionHandle,
           SQLUSMALLINT Option, SQLUINTEGER Value);
SQLRETURN  SQL_API PGAPI_SetCursorName(SQLHSTMT StatementHandle,
           SQLCHAR *CursorName, SQLSMALLINT NameLength);
SQLRETURN  SQL_API PGAPI_SetParam(SQLHSTMT StatementHandle,
           SQLUSMALLINT ParameterNumber, SQLSMALLINT ValueType,
           SQLSMALLINT ParameterType, SQLUINTEGER LengthPrecision,
           SQLSMALLINT ParameterScale, SQLPOINTER ParameterValue,
           SQLINTEGER *StrLen_or_Ind);
SQLRETURN  SQL_API PGAPI_SetStmtOption(SQLHSTMT StatementHandle,
           SQLUSMALLINT Option, SQLUINTEGER Value);
SQLRETURN  SQL_API PGAPI_SpecialColumns(SQLHSTMT StatementHandle,
           SQLUSMALLINT IdentifierType, SQLCHAR *CatalogName,
           SQLSMALLINT NameLength1, SQLCHAR *SchemaName,
           SQLSMALLINT NameLength2, SQLCHAR *TableName,
           SQLSMALLINT NameLength3, SQLUSMALLINT Scope,
           SQLUSMALLINT Nullable);
SQLRETURN  SQL_API PGAPI_Statistics(SQLHSTMT StatementHandle,
           SQLCHAR *CatalogName, SQLSMALLINT NameLength1,
           SQLCHAR *SchemaName, SQLSMALLINT NameLength2,
           SQLCHAR *TableName, SQLSMALLINT NameLength3,
           SQLUSMALLINT Unique, SQLUSMALLINT Reserved);
SQLRETURN  SQL_API PGAPI_Tables(SQLHSTMT StatementHandle,
           SQLCHAR *CatalogName, SQLSMALLINT NameLength1,
           SQLCHAR *SchemaName, SQLSMALLINT NameLength2,
           SQLCHAR *TableName, SQLSMALLINT NameLength3,
           SQLCHAR *TableType, SQLSMALLINT NameLength4);
SQLRETURN  SQL_API PGAPI_Transact(SQLHENV EnvironmentHandle,
           SQLHDBC ConnectionHandle, SQLUSMALLINT CompletionType);
SQLRETURN SQL_API PGAPI_ColAttributes(
	   SQLHSTMT hstmt,
	   SQLUSMALLINT icol,
	   SQLUSMALLINT fDescType,
	   SQLPOINTER  rgbDesc,
	   SQLSMALLINT cbDescMax,
	   SQLSMALLINT *pcbDesc,
	   SQLINTEGER *pfDesc);
SQLRETURN SQL_API PGAPI_ColumnPrivileges(
    SQLHSTMT           hstmt,
    SQLCHAR 		  *szCatalogName,
    SQLSMALLINT        cbCatalogName,
    SQLCHAR 		  *szSchemaName,
    SQLSMALLINT        cbSchemaName,
    SQLCHAR 		  *szTableName,
    SQLSMALLINT        cbTableName,
    SQLCHAR 		  *szColumnName,
    SQLSMALLINT        cbColumnName);
SQLRETURN SQL_API PGAPI_DescribeParam(
    SQLHSTMT           hstmt,
    SQLUSMALLINT       ipar,
    SQLSMALLINT 	  *pfSqlType,
    SQLUINTEGER 	  *pcbParamDef,
    SQLSMALLINT 	  *pibScale,
    SQLSMALLINT 	  *pfNullable);
SQLRETURN SQL_API PGAPI_ExtendedFetch(
    SQLHSTMT           hstmt,
    SQLUSMALLINT       fFetchType,
    SQLINTEGER         irow,
    SQLUINTEGER 	  *pcrow,
    SQLUSMALLINT 	  *rgfRowStatus);
SQLRETURN SQL_API PGAPI_ForeignKeys(
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
    SQLSMALLINT        cbFkTableName);
SQLRETURN SQL_API PGAPI_MoreResults(
    SQLHSTMT           hstmt);
SQLRETURN SQL_API PGAPI_NativeSql(
    SQLHDBC            hdbc,
    SQLCHAR 		  *szSqlStrIn,
    SQLINTEGER         cbSqlStrIn,
    SQLCHAR 		  *szSqlStr,
    SQLINTEGER         cbSqlStrMax,
    SQLINTEGER 		  *pcbSqlStr);
SQLRETURN SQL_API PGAPI_NumParams(
    SQLHSTMT           hstmt,
    SQLSMALLINT 	  *pcpar);
SQLRETURN SQL_API PGAPI_ParamOptions(
    SQLHSTMT           hstmt,
    SQLUINTEGER        crow,
    SQLUINTEGER 	  *pirow);
SQLRETURN SQL_API PGAPI_PrimaryKeys(
    SQLHSTMT           hstmt,
    SQLCHAR 		  *szCatalogName,
    SQLSMALLINT        cbCatalogName,
    SQLCHAR 		  *szSchemaName,
    SQLSMALLINT        cbSchemaName,
    SQLCHAR 		  *szTableName,
    SQLSMALLINT        cbTableName);
SQLRETURN SQL_API PGAPI_ProcedureColumns(
    SQLHSTMT           hstmt,
    SQLCHAR 		  *szCatalogName,
    SQLSMALLINT        cbCatalogName,
    SQLCHAR 		  *szSchemaName,
    SQLSMALLINT        cbSchemaName,
    SQLCHAR 		  *szProcName,
    SQLSMALLINT        cbProcName,
    SQLCHAR 		  *szColumnName,
    SQLSMALLINT        cbColumnName);
SQLRETURN SQL_API PGAPI_Procedures(
    SQLHSTMT           hstmt,
    SQLCHAR 		  *szCatalogName,
    SQLSMALLINT        cbCatalogName,
    SQLCHAR 		  *szSchemaName,
    SQLSMALLINT        cbSchemaName,
    SQLCHAR 		  *szProcName,
    SQLSMALLINT        cbProcName);
SQLRETURN SQL_API PGAPI_SetPos(
    SQLHSTMT           hstmt,
    SQLUSMALLINT       irow,
    SQLUSMALLINT       fOption,
    SQLUSMALLINT       fLock);
SQLRETURN SQL_API PGAPI_TablePrivileges(
    SQLHSTMT           hstmt,
    SQLCHAR 		  *szCatalogName,
    SQLSMALLINT        cbCatalogName,
    SQLCHAR 		  *szSchemaName,
    SQLSMALLINT        cbSchemaName,
    SQLCHAR 		  *szTableName,
    SQLSMALLINT        cbTableName);
SQLRETURN SQL_API PGAPI_BindParameter(
    SQLHSTMT           hstmt,
    SQLUSMALLINT       ipar,
    SQLSMALLINT        fParamType,
    SQLSMALLINT        fCType,
    SQLSMALLINT        fSqlType,
    SQLUINTEGER        cbColDef,
    SQLSMALLINT        ibScale,
    SQLPOINTER         rgbValue,
    SQLINTEGER         cbValueMax,
    SQLINTEGER 		  *pcbValue);

/* #include "pg_converr_check.h" */
