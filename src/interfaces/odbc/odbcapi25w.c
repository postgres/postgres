/*-------
 * Module:			odbcapi25w.c
 *
 * Description:		This module contains UNICODE routines
 *
 * Classes:			n/a
 *
 * API functions:	SQLColAttributesW, SQLErrorW, SQLGetConnectOptionW,
			SQLSetConnectOptionW
 *-------
 */

#include "psqlodbc.h"
#include <stdio.h>
#include <string.h>

#include "pgapifunc.h"
#include "connection.h"
#include "statement.h"

RETCODE  SQL_API SQLErrorW(HENV EnvironmentHandle,
           HDBC ConnectionHandle, HSTMT StatementHandle,
           SQLWCHAR *Sqlstate, SQLINTEGER *NativeError,
           SQLWCHAR *MessageText, SQLSMALLINT BufferLength,
           SQLSMALLINT *TextLength)
{
	RETCODE	ret;
	SWORD	tlen;
	char	*qst = NULL, *mtxt = NULL;

	mylog("[SQLErrorW]");
	if (Sqlstate)
		qst = malloc(8);
	if (MessageText)
		mtxt = malloc(BufferLength);	
	ret = PGAPI_Error(EnvironmentHandle, ConnectionHandle, StatementHandle,
           	qst, NativeError, mtxt, BufferLength, &tlen);
	if (qst)
		utf8_to_ucs2(qst, strlen(qst), Sqlstate, 5);
	if (TextLength)
		*TextLength = utf8_to_ucs2(mtxt, tlen, MessageText, BufferLength);
	free(qst);
	free(mtxt);
	return ret;
}

RETCODE  SQL_API SQLGetConnectOptionW(HDBC ConnectionHandle,
           SQLUSMALLINT Option, PTR Value)
{
	mylog("[SQLGetConnectOptionW]");
	((ConnectionClass *) ConnectionHandle)->unicode = 1;
	return PGAPI_GetConnectOption(ConnectionHandle, Option, Value);
} 

RETCODE  SQL_API SQLSetConnectOptionW(HDBC ConnectionHandle,
           SQLUSMALLINT Option, SQLUINTEGER Value)
{
	mylog("[SQLSetConnectionOptionW]");
	((ConnectionClass *) ConnectionHandle)->unicode = 1;
	return PGAPI_SetConnectOption(ConnectionHandle, Option, Value);
}

RETCODE SQL_API SQLColAttributesW(
    HSTMT           hstmt,
    SQLUSMALLINT       icol,
    SQLUSMALLINT       fDescType,
    PTR         rgbDesc,
    SQLSMALLINT        cbDescMax,
    SQLSMALLINT 	  *pcbDesc,
    SQLINTEGER 		  *pfDesc)
{
	mylog("[SQLColAttributesW]");
	return PGAPI_ColAttributes(hstmt, icol, fDescType, rgbDesc,
		cbDescMax, pcbDesc, pfDesc);
}
