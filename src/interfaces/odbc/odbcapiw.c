/*-------
 * Module:			odbcapiw.c
 *
 * Description:		This module contains UNICODE routines
 *
 * Classes:			n/a
 *
 * API functions:	SQLColumnPrivilegesW, SQLColumnsW,
 			SQLConnectW, SQLDataSourcesW, SQLDescribeColW,
			SQLDriverConnectW, SQLExecDirectW,
			SQLForeignKeysW,
			SQLGetCursorNameW, SQLGetInfoW, SQLNativeSqlW,
			SQLPrepareW, SQLPrimaryKeysW, SQLProcedureColumnsW,
			SQLProceduresW, SQLSetCursorNameW,
			SQLSpecialColumnsW, SQLStatisticsW, SQLTablesW,
			SQLTablePrivilegesW
 *-------
 */

#include "psqlodbc.h"
#include <stdio.h>
#include <string.h>

#include "pgapifunc.h"
#include "connection.h"
#include "statement.h"

RETCODE  SQL_API SQLColumnsW(HSTMT StatementHandle,
           SQLWCHAR *CatalogName, SQLSMALLINT NameLength1,
           SQLWCHAR *SchemaName, SQLSMALLINT NameLength2,
           SQLWCHAR *TableName, SQLSMALLINT NameLength3,
           SQLWCHAR *ColumnName, SQLSMALLINT NameLength4)
{
	RETCODE	ret;
	char	*ctName, *scName, *tbName, *clName;
	UInt4	nmlen1, nmlen2, nmlen3, nmlen4;

	mylog("[SQLColumnsW]");
	ctName = ucs2_to_utf8(CatalogName, NameLength1, &nmlen1);
	scName = ucs2_to_utf8(SchemaName, NameLength2, &nmlen2);
	tbName = ucs2_to_utf8(TableName, NameLength3, &nmlen3);
	clName = ucs2_to_utf8(ColumnName, NameLength4, &nmlen4);
	ret = PGAPI_Columns(StatementHandle, ctName, (SWORD) nmlen1,
           	scName, (SWORD) nmlen2, tbName, (SWORD) nmlen3,
           	clName, (SWORD) nmlen4, 0);
	if (ctName)
		free(ctName);
	if (scName);
		free(scName);
	if (tbName)
		free(tbName);
	if (clName);
		free(clName);
	return ret;
}


RETCODE  SQL_API SQLConnectW(HDBC ConnectionHandle,
           SQLWCHAR *ServerName, SQLSMALLINT NameLength1,
           SQLWCHAR *UserName, SQLSMALLINT NameLength2,
           SQLWCHAR *Authentication, SQLSMALLINT NameLength3)
{
	char	*svName, *usName, *auth;
	UInt4	nmlen1, nmlen2, nmlen3;
	RETCODE	ret;
	
	mylog("[SQLConnectW]");
	((ConnectionClass *) ConnectionHandle)->unicode = 1;
	svName = ucs2_to_utf8(ServerName, NameLength1, &nmlen1);
	usName = ucs2_to_utf8(UserName, NameLength2, &nmlen2);
	auth = ucs2_to_utf8(Authentication, NameLength3, &nmlen3);
	ret = PGAPI_Connect(ConnectionHandle, svName, (SWORD) nmlen1,
           	usName, (SWORD) nmlen2, auth, (SWORD) nmlen3);
	if (svName);
		free(svName);
	if (usName);
		free(usName);
	if (auth);
		free(auth);
	return ret;
}

RETCODE SQL_API SQLDriverConnectW(HDBC hdbc,
                                 HWND hwnd,
                                 SQLWCHAR *szConnStrIn,
                                 SWORD cbConnStrIn,
                                 SQLWCHAR *szConnStrOut,
                                 SWORD cbConnStrOutMax,
                                 SWORD FAR *pcbConnStrOut,
                                 UWORD fDriverCompletion)
{
	char	*szIn, *szOut;
	UInt4	inlen, obuflen;
	SWORD	olen;
	RETCODE	ret;

	mylog("[SQLDriverConnectW]");
	((ConnectionClass *) hdbc)->unicode = 1;
	szIn = ucs2_to_utf8(szConnStrIn, cbConnStrIn, &inlen);
	obuflen = cbConnStrOutMax + 1;
	szOut = malloc(obuflen);
	ret = PGAPI_DriverConnect(hdbc, hwnd, szIn, (SWORD) inlen,
		szOut, cbConnStrOutMax, &olen, fDriverCompletion);
	if (ret != SQL_ERROR)
		*pcbConnStrOut = utf8_to_ucs2(szOut, olen, szConnStrOut, cbConnStrOutMax);
	free(szOut);
	if (szIn);
		free(szIn);
	return ret;
}
RETCODE SQL_API SQLBrowseConnectW(
    HDBC            hdbc,
    SQLWCHAR 		  *szConnStrIn,
    SQLSMALLINT        cbConnStrIn,
    SQLWCHAR 		  *szConnStrOut,
    SQLSMALLINT        cbConnStrOutMax,
    SQLSMALLINT       *pcbConnStrOut)
{
	char	*szIn, *szOut;
	UInt4	inlen, obuflen;
	SWORD	olen;
	RETCODE	ret;

	mylog("[SQLBrowseConnectW]");
	((ConnectionClass *) hdbc)->unicode = 1;
	szIn = ucs2_to_utf8(szConnStrIn, cbConnStrIn, &inlen);
	obuflen = cbConnStrOutMax + 1;
	szOut = malloc(obuflen);
	ret = PGAPI_BrowseConnect(hdbc, szIn, (SWORD) inlen,
		szOut, cbConnStrOutMax, &olen);
	if (ret != SQL_ERROR)
		*pcbConnStrOut = utf8_to_ucs2(szOut, olen, szConnStrOut, cbConnStrOutMax);
	free(szOut);
	if (szIn);
		free(szIn);
	return ret;
}

RETCODE  SQL_API SQLDataSourcesW(HENV EnvironmentHandle,
           SQLUSMALLINT Direction, SQLWCHAR *ServerName,
           SQLSMALLINT BufferLength1, SQLSMALLINT *NameLength1,
           SQLWCHAR *Description, SQLSMALLINT BufferLength2,
           SQLSMALLINT *NameLength2)
{
	mylog("[SQLDataSourcesW]");
	/*
	return PGAPI_DataSources(EnvironmentHandle, Direction, ServerName,
		 BufferLength1, NameLength1, Description, BufferLength2,
           	NameLength2);
	*/
	return SQL_ERROR;
}

RETCODE  SQL_API SQLDescribeColW(HSTMT StatementHandle,
           SQLUSMALLINT ColumnNumber, SQLWCHAR *ColumnName,
           SQLSMALLINT BufferLength, SQLSMALLINT *NameLength,
           SQLSMALLINT *DataType, SQLUINTEGER *ColumnSize,
           SQLSMALLINT *DecimalDigits, SQLSMALLINT *Nullable)
{
	RETCODE	ret;
	SWORD	nmlen;
	char	*clName;

	mylog("[SQLDescribeColW]");
	clName = malloc(BufferLength);
	ret = PGAPI_DescribeCol(StatementHandle, ColumnNumber,
		clName, BufferLength, &nmlen,
           	DataType, ColumnSize, DecimalDigits, Nullable);
	*NameLength = utf8_to_ucs2(clName, nmlen, ColumnName, BufferLength);
	free(clName); 
	return ret;
}

RETCODE  SQL_API SQLExecDirectW(HSTMT StatementHandle,
           SQLWCHAR *StatementText, SQLINTEGER TextLength)
{
	RETCODE	ret;
	char	*stxt;
	UInt4	slen;

	mylog("[SQLExecDirectW]");
	stxt = ucs2_to_utf8(StatementText, TextLength, &slen);
	ret = PGAPI_ExecDirect(StatementHandle, stxt, slen);
	if (stxt);
		free(stxt);
	return ret;
}

RETCODE  SQL_API SQLGetCursorNameW(HSTMT StatementHandle,
           SQLWCHAR *CursorName, SQLSMALLINT BufferLength,
           SQLSMALLINT *NameLength)
{
	RETCODE	ret;
	char	*crName;
	SWORD	clen;

	mylog("[SQLGetCursorNameW]");
	crName = malloc(BufferLength);
	ret = PGAPI_GetCursorName(StatementHandle, crName, BufferLength,
           	&clen);
	*NameLength = utf8_to_ucs2(crName, (Int4) clen, CursorName, BufferLength);
	free(crName);
	return ret;
}

RETCODE  SQL_API SQLGetInfoW(HDBC ConnectionHandle,
           SQLUSMALLINT InfoType, PTR InfoValue,
           SQLSMALLINT BufferLength, SQLSMALLINT *StringLength)
{
	RETCODE	ret;
	((ConnectionClass *) ConnectionHandle)->unicode = 1;
#if (ODBCVER >= 0x0300)
	mylog("[SQLGetInfoW(30)]");
	if ((ret = PGAPI_GetInfo(ConnectionHandle, InfoType, InfoValue,
           	BufferLength, StringLength)) == SQL_ERROR)
	{
		if (((ConnectionClass *) ConnectionHandle)->driver_version >= 0x0300)
			return PGAPI_GetInfo30(ConnectionHandle, InfoType, InfoValue,
           			BufferLength, StringLength);
	}
	return ret;
#else
	mylog("[SQLGetInfoW]");
	return PGAPI_GetInfo(ConnectionHandle, InfoType, InfoValue,
           	BufferLength, StringLength);
#endif
}

RETCODE  SQL_API SQLPrepareW(HSTMT StatementHandle,
           SQLWCHAR *StatementText, SQLINTEGER TextLength)
{
	RETCODE	ret;
	char	*stxt;
	UInt4	slen;

	mylog("[SQLPrepareW]");
	stxt = ucs2_to_utf8(StatementText, TextLength, &slen);
	ret = PGAPI_Prepare(StatementHandle, stxt, slen);
	if (stxt);
		free(stxt);
	return ret;
}

RETCODE  SQL_API SQLSetCursorNameW(HSTMT StatementHandle,
           SQLWCHAR *CursorName, SQLSMALLINT NameLength)
{
	RETCODE	ret;
	char	*crName;
	UInt4	nlen;

	mylog("[SQLSetCursorNameW]");
	crName = ucs2_to_utf8(CursorName, NameLength, &nlen);
	ret = PGAPI_SetCursorName(StatementHandle, crName, (SWORD) nlen);
	if (crName);
		free(crName);
	return ret;
}

RETCODE  SQL_API SQLSpecialColumnsW(HSTMT StatementHandle,
           SQLUSMALLINT IdentifierType, SQLWCHAR *CatalogName,
           SQLSMALLINT NameLength1, SQLWCHAR *SchemaName,
           SQLSMALLINT NameLength2, SQLWCHAR *TableName,
           SQLSMALLINT NameLength3, SQLUSMALLINT Scope,
           SQLUSMALLINT Nullable)
{
	RETCODE	ret;
	char	*ctName, *scName, *tbName;
	UInt4	nmlen1, nmlen2, nmlen3;
	
	mylog("[SQLSpecialColumnsW]");
	ctName = ucs2_to_utf8(CatalogName, NameLength1, &nmlen1);
	scName = ucs2_to_utf8(SchemaName, NameLength2, &nmlen2);
	tbName = ucs2_to_utf8(TableName, NameLength3, &nmlen3);
	ret = PGAPI_SpecialColumns(StatementHandle, IdentifierType, ctName,
           (SWORD) nmlen1, scName, (SWORD) nmlen2, tbName, (SWORD) nmlen3,
		Scope, Nullable);
	if (ctName);
		free(ctName);
	if (scName);
		free(scName);
	if (tbName);
		free(tbName);
	return ret;
}

RETCODE  SQL_API SQLStatisticsW(HSTMT StatementHandle,
           SQLWCHAR *CatalogName, SQLSMALLINT NameLength1,
           SQLWCHAR *SchemaName, SQLSMALLINT NameLength2,
           SQLWCHAR *TableName, SQLSMALLINT NameLength3,
           SQLUSMALLINT Unique, SQLUSMALLINT Reserved)
{
	RETCODE	ret;
	char	*ctName, *scName, *tbName;
	UInt4	nmlen1, nmlen2, nmlen3;

	mylog("[SQLStatisticsW]");
	ctName = ucs2_to_utf8(CatalogName, NameLength1, &nmlen1);
	scName = ucs2_to_utf8(SchemaName, NameLength2, &nmlen2);
	tbName = ucs2_to_utf8(TableName, NameLength3, &nmlen3);
	return PGAPI_Statistics(StatementHandle, ctName, (SWORD) nmlen1,
           scName, (SWORD) nmlen2, tbName, (SWORD) nmlen3, Unique,
		Reserved);
	if (ctName);
		free(ctName);
	if (scName);
		free(scName);
	if (tbName);
		free(tbName);
	return ret;
}

RETCODE  SQL_API SQLTablesW(HSTMT StatementHandle,
           SQLWCHAR *CatalogName, SQLSMALLINT NameLength1,
           SQLWCHAR *SchemaName, SQLSMALLINT NameLength2,
           SQLWCHAR *TableName, SQLSMALLINT NameLength3,
           SQLWCHAR *TableType, SQLSMALLINT NameLength4)
{
	RETCODE	ret;
	char	*ctName, *scName, *tbName, *tbType;
	UInt4	nmlen1, nmlen2, nmlen3, nmlen4;

	mylog("[SQLTablesW]");
	ctName = ucs2_to_utf8(CatalogName, NameLength1, &nmlen1);
	scName = ucs2_to_utf8(SchemaName, NameLength2, &nmlen2);
	tbName = ucs2_to_utf8(TableName, NameLength3, &nmlen3);
	tbType = ucs2_to_utf8(TableType, NameLength4, &nmlen4);
	return PGAPI_Tables(StatementHandle, ctName, (SWORD) nmlen1,
           scName, (SWORD) nmlen2, tbName, (SWORD) nmlen3,
           tbType, (SWORD) nmlen4);
	if (ctName);
		free(ctName);
	if (scName);
		free(scName);
	if (tbName);
		free(tbName);
	if (tbType);
		free(tbType);
	return ret;
}

RETCODE SQL_API SQLColumnPrivilegesW(
    HSTMT           hstmt,
    SQLWCHAR 		  *szCatalogName,
    SQLSMALLINT        cbCatalogName,
    SQLWCHAR 		  *szSchemaName,
    SQLSMALLINT        cbSchemaName,
    SQLWCHAR 		  *szTableName,
    SQLSMALLINT        cbTableName,
    SQLWCHAR 		  *szColumnName,
    SQLSMALLINT        cbColumnName)
{
	RETCODE	ret;
	char	*ctName, *scName, *tbName, *clName;
	UInt4	nmlen1, nmlen2, nmlen3, nmlen4;

	mylog("[SQLColumnPrivilegesW]");
	ctName = ucs2_to_utf8(szCatalogName, cbCatalogName, &nmlen1);
	scName = ucs2_to_utf8(szSchemaName, cbSchemaName, &nmlen2);
	tbName = ucs2_to_utf8(szTableName, cbTableName, &nmlen3);
	clName = ucs2_to_utf8(szColumnName, cbColumnName, &nmlen4);
	ret = PGAPI_ColumnPrivileges(hstmt, ctName, (SWORD) nmlen1,
		scName, (SWORD) nmlen2, tbName, (SWORD) nmlen3,
		clName, (SWORD) nmlen4);
	if (ctName);
		free(ctName);
	if (scName);
		free(scName);
	if (tbName);
		free(tbName);
	if (clName);
		free(clName);
	return ret;
}

RETCODE SQL_API SQLForeignKeysW(
    HSTMT           hstmt,
    SQLWCHAR 		  *szPkCatalogName,
    SQLSMALLINT        cbPkCatalogName,
    SQLWCHAR 		  *szPkSchemaName,
    SQLSMALLINT        cbPkSchemaName,
    SQLWCHAR 		  *szPkTableName,
    SQLSMALLINT        cbPkTableName,
    SQLWCHAR 		  *szFkCatalogName,
    SQLSMALLINT        cbFkCatalogName,
    SQLWCHAR 		  *szFkSchemaName,
    SQLSMALLINT        cbFkSchemaName,
    SQLWCHAR 		  *szFkTableName,
    SQLSMALLINT        cbFkTableName)
{
	RETCODE	ret;
	char	*ctName, *scName, *tbName, *fkctName, *fkscName, *fktbName;
	UInt4	nmlen1, nmlen2, nmlen3, nmlen4, nmlen5, nmlen6;

	mylog("[SQLForeignKeysW]");
	ctName = ucs2_to_utf8(szPkCatalogName, cbPkCatalogName, &nmlen1);
	scName = ucs2_to_utf8(szPkSchemaName, cbPkSchemaName, &nmlen2);
	tbName = ucs2_to_utf8(szPkTableName, cbPkTableName, &nmlen3);
	fkctName = ucs2_to_utf8(szFkCatalogName, cbFkCatalogName, &nmlen4);
	fkscName = ucs2_to_utf8(szFkSchemaName, cbFkSchemaName, &nmlen5);
	fktbName = ucs2_to_utf8(szFkTableName, cbFkTableName, &nmlen6);
	ret = PGAPI_ForeignKeys(hstmt, ctName, (SWORD) nmlen1,
		scName, (SWORD) nmlen2, tbName, (SWORD) nmlen3,
		fkctName, (SWORD) nmlen4, fkscName, (SWORD) nmlen5,
		fktbName, (SWORD) nmlen6);
	if (ctName);
		free(ctName);
	if (scName);
		free(scName);
	if (tbName);
		free(tbName);
	if (fkctName);
		free(fkctName);
	if (fkscName);
		free(fkscName);
	if (fktbName);
		free(fktbName);
	return ret;
}

RETCODE SQL_API SQLNativeSqlW(
    HDBC            hdbc,
    SQLWCHAR 		  *szSqlStrIn,
    SQLINTEGER         cbSqlStrIn,
    SQLWCHAR 		  *szSqlStr,
    SQLINTEGER         cbSqlStrMax,
    SQLINTEGER 		  *pcbSqlStr)
{
	RETCODE		ret;
	char		*szIn, *szOut;
	UInt4		slen;
	SQLINTEGER	olen;

	mylog("[SQLNativeSqlW]");
	((ConnectionClass *) hdbc)->unicode = 1;
	szIn = ucs2_to_utf8(szSqlStrIn, cbSqlStrIn, &slen);
	szOut = malloc(cbSqlStrMax);
	ret = PGAPI_NativeSql(hdbc, szIn, (SQLINTEGER) slen,
		szOut, cbSqlStrMax, &olen);
	if (szIn);
		free(szIn);
	*pcbSqlStr = utf8_to_ucs2(szOut, olen, szSqlStr, cbSqlStrMax);
	free(szOut);
	return ret;
}

RETCODE SQL_API SQLPrimaryKeysW(
    HSTMT           hstmt,
    SQLWCHAR 		  *szCatalogName,
    SQLSMALLINT        cbCatalogName,
    SQLWCHAR 		  *szSchemaName,
    SQLSMALLINT        cbSchemaName,
    SQLWCHAR 		  *szTableName,
    SQLSMALLINT        cbTableName)
{
	RETCODE	ret;
	char	*ctName, *scName, *tbName;
	UInt4	nmlen1, nmlen2, nmlen3;

	mylog("[SQLPrimaryKeysW]");
	ctName = ucs2_to_utf8(szCatalogName, cbCatalogName, &nmlen1);
	scName = ucs2_to_utf8(szSchemaName, cbSchemaName, &nmlen2);
	tbName = ucs2_to_utf8(szTableName, cbTableName, &nmlen3);
	return PGAPI_PrimaryKeys(hstmt, ctName, (SWORD) nmlen1,
		scName, (SWORD) nmlen2, tbName, (SWORD) nmlen3);
	if (ctName);
		free(ctName);
	if (scName);
		free(scName);
	if (tbName);
		free(tbName);
	return ret;
}

RETCODE SQL_API SQLProcedureColumnsW(
    HSTMT           hstmt,
    SQLWCHAR 		  *szCatalogName,
    SQLSMALLINT        cbCatalogName,
    SQLWCHAR 		  *szSchemaName,
    SQLSMALLINT        cbSchemaName,
    SQLWCHAR 		  *szProcName,
    SQLSMALLINT        cbProcName,
    SQLWCHAR 		  *szColumnName,
    SQLSMALLINT        cbColumnName)
{
	RETCODE	ret;
	char	*ctName, *scName, *prName, *clName;
	UInt4	nmlen1, nmlen2, nmlen3, nmlen4;

	mylog("[SQLProcedureColumnsW]");
	ctName = ucs2_to_utf8(szCatalogName, cbCatalogName, &nmlen1);
	scName = ucs2_to_utf8(szSchemaName, cbSchemaName, &nmlen2);
	prName = ucs2_to_utf8(szProcName, cbProcName, &nmlen3);
	clName = ucs2_to_utf8(szColumnName, cbColumnName, &nmlen4);
	ret = PGAPI_ProcedureColumns(hstmt, ctName, (SWORD) nmlen1,
		scName, (SWORD) nmlen2, prName, (SWORD) nmlen3,
		clName, (SWORD) nmlen4);
	if (ctName);
		free(ctName);
	if (scName);
		free(scName);
	if (prName);
		free(prName);
	if (clName);
		free(clName);
	return ret;
}

RETCODE SQL_API SQLProceduresW(
    HSTMT           hstmt,
    SQLWCHAR 		  *szCatalogName,
    SQLSMALLINT        cbCatalogName,
    SQLWCHAR 		  *szSchemaName,
    SQLSMALLINT        cbSchemaName,
    SQLWCHAR 		  *szProcName,
    SQLSMALLINT        cbProcName)
{
	RETCODE	ret;
	char	*ctName, *scName, *prName;
	UInt4	nmlen1, nmlen2, nmlen3;

	mylog("[SQLProceduresW]");
	ctName = ucs2_to_utf8(szCatalogName, cbCatalogName, &nmlen1);
	scName = ucs2_to_utf8(szSchemaName, cbSchemaName, &nmlen2);
	prName = ucs2_to_utf8(szProcName, cbProcName, &nmlen3);
	ret = PGAPI_Procedures(hstmt, ctName, (SWORD) nmlen1,
		scName, (SWORD) nmlen2, prName, (SWORD) nmlen3);
	if (ctName);
		free(ctName);
	if (scName);
		free(scName);
	if (prName);
		free(prName);
	return ret;
}

RETCODE SQL_API SQLTablePrivilegesW(
    HSTMT           hstmt,
    SQLWCHAR 		  *szCatalogName,
    SQLSMALLINT        cbCatalogName,
    SQLWCHAR 		  *szSchemaName,
    SQLSMALLINT        cbSchemaName,
    SQLWCHAR 		  *szTableName,
    SQLSMALLINT        cbTableName)
{
	RETCODE	ret;
	char	*ctName, *scName, *tbName;
	UInt4	nmlen1, nmlen2, nmlen3;

	mylog("[SQLTablePrivilegesW]");
	ctName = ucs2_to_utf8(szCatalogName, cbCatalogName, &nmlen1);
	scName = ucs2_to_utf8(szSchemaName, cbSchemaName, &nmlen2);
	tbName = ucs2_to_utf8(szTableName, cbTableName, &nmlen3);
	ret = PGAPI_TablePrivileges(hstmt, ctName, (SWORD) nmlen1,
		scName, (SWORD) nmlen2, tbName, (SWORD) nmlen3, 0);
	if (ctName);
		free(ctName);
	if (scName);
		free(scName);
	if (tbName);
		free(tbName);
	return ret;
}
