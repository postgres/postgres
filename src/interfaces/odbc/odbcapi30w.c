/*-------
 * Module:			odbcapi30w.c
 *
 * Description:		This module contains UNICODE routines
 *
 * Classes:			n/a
 *
 * API functions:	SQLColAttributeW, SQLGetStmtW, SQLSetStmtW,
 			SQLSetConnectAttrW, SQLGetConnectAttrW,
			SQLGetDescFieldW, SQLGetDescRecW, SQLGetDiagFieldW,
			SQLGetDiagRecW,
 *-------
 */

#include "psqlodbc.h"
#include <stdio.h>
#include <string.h>

#include "pgapifunc.h"
#include "connection.h"
#include "statement.h"


RETCODE	SQL_API	SQLGetStmtAttrW(SQLHSTMT hstmt,
		SQLINTEGER	fAttribute,
		PTR		rgbValue,
		SQLINTEGER	cbValueMax,
		SQLINTEGER	*pcbValue)
{
	RETCODE	ret;

	mylog("[SQLGetStmtAttrW]");
	ret = PGAPI_GetStmtAttr(hstmt, fAttribute, rgbValue,
		cbValueMax, pcbValue);
	return ret;
}

RETCODE SQL_API	SQLSetStmtAttrW(SQLHSTMT hstmt,
		SQLINTEGER	fAttribute,
		PTR		rgbValue,
		SQLINTEGER	cbValueMax)
{
	RETCODE	ret;

	mylog("[SQLSetStmtAttrW]");
	ret = PGAPI_SetStmtAttr(hstmt, fAttribute, rgbValue,
		cbValueMax);
	return ret;
}

RETCODE SQL_API	SQLGetConnectAttrW(HDBC hdbc,
		SQLINTEGER	fAttribute,
		PTR		rgbValue,
		SQLINTEGER	cbValueMax,
		SQLINTEGER	*pcbValue)
{
	RETCODE	ret;

	mylog("[SQLGetConnectAttrW]");
	ret = PGAPI_GetConnectAttr(hdbc, fAttribute, rgbValue,
		cbValueMax, pcbValue);
	return ret;
}

RETCODE SQL_API	SQLSetConnectAttrW(HDBC hdbc,
		SQLINTEGER	fAttribute,
		PTR		rgbValue,
		SQLINTEGER	cbValue)
{
	RETCODE	ret;

	mylog("[SQLSetConnectAttrW]");
	ret = PGAPI_SetConnectAttr(hdbc, fAttribute, rgbValue,
		cbValue);
	return ret;
}

RETCODE SQL_API	SQLGetDiagRecW(SWORD fHandleType,
		SQLHANDLE	handle,
		SQLSMALLINT	iRecord,
		SQLWCHAR	*szSqlState,
		SQLINTEGER	*pfNativeError,
		SQLWCHAR	*szErrorMsg,
		SQLSMALLINT	cbErrorMsgMax,
		SQLSMALLINT	*pcbErrorMsg)
{
	RETCODE	ret;
        SWORD   tlen;
        char    *qst = NULL, *mtxt = NULL;

	mylog("[SQLGetDiagRecW]");
        if (szSqlState)
                qst = malloc(8);
        if (szErrorMsg)
                mtxt = malloc(cbErrorMsgMax);
        ret = PGAPI_GetDiagRec(fHandleType, handle, iRecord, qst,
                pfNativeError, mtxt, cbErrorMsgMax, &tlen);
        if (qst)
                utf8_to_ucs2(qst, strlen(qst), szSqlState, 5);
	if (pcbErrorMsg)
        	*pcbErrorMsg = utf8_to_ucs2(mtxt, tlen, szErrorMsg, cbErrorMsgMax);
        free(qst);
        free(mtxt);
        return ret;
}

RETCODE SQL_API SQLColAttributeW(
    HSTMT           hstmt,
    SQLUSMALLINT       icol,
    SQLUSMALLINT       fDescType,
    PTR			rgbDesc,
    SQLSMALLINT        cbDescMax,
    SQLSMALLINT 	  *pcbDesc,
    SQLINTEGER 		  *pfDesc)
{
	RETCODE	ret;

	mylog("[SQLColAttributeW]");
	ret = PGAPI_ColAttributes(hstmt, icol, fDescType, rgbDesc,
		cbDescMax, pcbDesc, pfDesc);
	return ret;
}
