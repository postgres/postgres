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
        SWORD   buflen, tlen;
        char    *qstr = NULL, *mtxt = NULL;

	mylog("[SQLGetDiagRecW]");
        if (szSqlState)
                qstr = malloc(8);
	buflen = 0;
        if (szErrorMsg && cbErrorMsgMax > 0)
	{
		buflen = cbErrorMsgMax;
                mtxt = malloc(buflen);
	}
        ret = PGAPI_GetDiagRec(fHandleType, handle, iRecord, qstr,
                pfNativeError, mtxt, buflen, &tlen);
	if (ret == SQL_SUCCESS || ret == SQL_SUCCESS_WITH_INFO)
	{
        	if (qstr)
                	utf8_to_ucs2(qstr, strlen(qstr), szSqlState, 6);
		if (mtxt && tlen <= cbErrorMsgMax)
		{
        		tlen = utf8_to_ucs2(mtxt, tlen, szErrorMsg, cbErrorMsgMax);
			if (tlen >= cbErrorMsgMax)
				ret = SQL_SUCCESS_WITH_INFO;
		}
		if (pcbErrorMsg)
        		*pcbErrorMsg = tlen;
	}
        if (qstr);
        	free(qstr);
	if (mtxt)
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
