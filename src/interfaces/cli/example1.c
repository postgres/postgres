/* -*- C -*- */
/* The first example illustrates creating a table, adding some data
 * to it, and selecting the inserted data. The second example shows
 * interactive ad hoc query processing.
 *
 * Actual applications include more complete error checking following
 * calls to SQL/CLI routines. That material is omitted from this
 * Appendix for the sake of clarity.
 *
 * This file is adapted for PostgreSQL
 * from the CLI Annex in the SQL98 August 1994 draft standard.
 * Thomas G. Lockhart 1999-06-16
 */

/*
 * B.1	Create table, insert, select
 *
 * This example function creates a table, inserts data into the table,
 * and selects the inserted data.
 *
 * This example illustrates the execution of SQL statement text
 * both using the Prepare() and Execute()  method and using the
 * ExecDirect() method. The example also illustrates both the case
 * where the application uses the automatically-generated descriptors
 * and the case where the application allocates a descriptor of its
 * own and associates this descriptor with the SQL statement.
 *
 * Code comments include the equivalent statements in embedded SQL
 * to show how embedded SQL operations correspond to SQL/CLI function
 * calls.
 */

#include "sqlcli.h"
#include <string.h>

#ifndef NULL
#define NULL   0
#endif

int			print_err(SQLSMALLINT handletype, SQLINTEGER handle);

int
example1(SQLCHAR * server, SQLCHAR * uid, SQLCHAR * authen)
{
	SQLHENV		henv;
	SQLHDBC		hdbc;
	SQLHDESC	hdesc;
	SQLHDESC	hdesc1;
	SQLHDESC	hdesc2;
	SQLHSTMT	hstmt;
	SQLINTEGER	id;
	SQLSMALLINT idind;
	SQLCHAR		name[51];
	SQLINTEGER	namelen;
	SQLSMALLINT nameind;

	/* EXEC SQL CONNECT TO :server USER :uid; */

	/* allocate an environment handle */
	SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &henv);
	/* allocate a connection handle */
	SQLAllocHandle(SQL_HANDLE_DBC, henv, &hdbc);

	/* connect to database */
	if (SQLConnect(hdbc, server, SQL_NTS, uid, SQL_NTS,
				   authen, SQL_NTS)
		!= SQL_SUCCESS)
		return (print_err(SQL_HANDLE_DBC, hdbc));

	/* allocate a statement handle */
	SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &hstmt);

	/* EXEC SQL CREATE TABLE NAMEID (ID integer, NAME varchar(50));  */
	{
		SQLCHAR		create[] = "CREATE TABLE NAMEID (ID integer,"
		" NAME varchar(50))";

		/* execute the CREATE TABLE statement */
		if (SQLExecDirect(hstmt, create, SQL_NTS) != SQL_SUCCESS)
			return (print_err(SQL_HANDLE_STMT, hstmt));
	}

	/* EXEC SQL COMMIT WORK; */
	/* commit CREATE TABLE */
	SQLEndTran(SQL_HANDLE_ENV, henv, SQL_COMMIT);
	/* EXEC SQL INSERT INTO NAMEID VALUES ( :id, :name ); */
	{
		SQLCHAR		insert[] = "INSERT INTO NAMEID VALUES (?, ?)";

		/* show the use of SQLPrepare/SQLExecute method */
		/* prepare the INSERT */
		if (SQLPrepare(hstmt, insert, SQL_NTS) != SQL_SUCCESS)
			return (print_err(SQL_HANDLE_STMT, hstmt));
		/* application parameter descriptor */
		SQLGetStmtAttr(hstmt, SQL_ATTR_APP_PARAM_
					   DESC, &hdesc1, 0L,
					   (SQLINTEGER *) NULL);
		SQLSetDescRec(hdesc1, 1, SQL_INTEGER, 0, 0L, 0, 0,
		   (SQLPOINTER) & id, (SQLINTEGER *) NULL, (SQLSMALLINT *) NULL);
		SQLSetDescRec(hdesc1, 2, SQL_CHAR, 0, 0L, 0, 0,
					  (SQLPOINTER) name, (SQLINTEGER *) NULL,
					  (SQLSMALLINT *) NULL);
		/* implementation parameter descriptor */
		SQLGetStmtAttr(hstmt, SQL_ATTR_IMP_PARAM_
					   DESC, &hdesc2, 0L,
					   (SQLINTEGER *) NULL);
		SQLSetDescRec(hdesc2, 1, SQL_INTEGER, 0, 0L, 0, 0,
					  (SQLPOINTER) NULL, (SQLINTEGER *) NULL,
					  (SQLSMALLINT *) NULL);
		SQLSetDescRec(hdesc2, 2, SQL_VARCHAR, 0, 50L, 0, 0,
					  (SQLPOINTER) NULL, (SQLINTEGER *) NULL,
					  (SQLSMALLINT *) NULL);

		/* assign parameter values and execute the INSERT */
		id = 500;
		(void) strcpy(name, "Babbage");
		if (SQLExecute(hstmt) != SQL_SUCCESS)
			return (print_err(SQL_HANDLE_STMT, hstmt));
	}
	/* EXEC SQL COMMIT WORK; */
	SQLEndTran(SQL_HANDLE_ENV, henv, SQL_COMMIT);
	/* commit inserts */

	/* EXEC SQL DECLARE c1 CURSOR FOR SELECT ID, NAME FROM NAMEID; */
	/* EXEC SQL OPEN c1; */
	/* The application doesn't specify "declare c1 cursor for" */
	{
		SQLCHAR		select[] = "select ID, NAME from NAMEID";

		if (SQLExecDirect(hstmt, select, SQL_NTS) != SQL_SUCCESS)
			return (print_err(SQL_HANDLE_STMT, hstmt));
	}

	/* EXEC SQL FETCH c1 INTO :id, :name; */
	/* this time, explicitly allocate an application row descriptor */
	SQLAllocHandle(SQL_HANDLE_DESC, hdbc, &hdesc);
	SQLSetDescRec(hdesc, 1, SQL_INTEGER, 0, 0L, 0, 0,
		(SQLPOINTER) & id, (SQLINTEGER *) NULL, (SQLSMALLINT *) & idind);

	SQLSetDescRec(hdesc, 2, SQL_
				  CHAR, 0, (SQLINTEGER) sizeof(name),
				  0, 0, (SQLPOINTER) & name, (SQLINTEGER *) & namelen,
				  (SQLSMALLINT *) & nameind);
	/* associate descriptor with statement handle */
	SQLSetStmtAttr(hstmt, SQL_ATTR_APP_ROW_DESC, &hdesc, 0);
	/* execute the fetch */
	SQLFetch(hstmt);

	/* EXEC SQL COMMIT WORK; */
	/* commit the transaction  */
	SQLEndTran(SQL_HANDLE_ENV, henv, SQL_COMMIT);

	/* EXEC SQL CLOSE c1; */
	SQLClose(hstmt);
	/* free the statement handle */
	SQLFreeHandle(SQL_HANDLE_STMT, hstmt);

	/* EXEC SQL DISCONNECT; */
	/* disconnect from the database */
	SQLDisconnect(hdbc);
	/* free descriptor handle */
	SQLFreeHandle(SQL_HANDLE_DESC, hdesc);
	/* free descriptor handle */
	SQLFreeHandle(SQL_HANDLE_DESC, hdesc1);
	/* free descriptor handle */
	SQLFreeHandle(SQL_HANDLE_DESC, hdesc2);
	/* free connection handle */
	SQLFreeHandle(SQL_HANDLE_DBC, hdbc);
	/* free environment handle */
	SQLFreeHandle(SQL_HANDLE_ENV, henv);

	return (0);
}
