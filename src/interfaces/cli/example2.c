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
 * B.2  Interactive Query
 *
 * This sample function uses the concise CLI functions to
 * interactively execute a SQL statement supplied as an argument.
 * In the case where the user types a SELECT statement, the function
 * fetches and displays all rows of the result set.
 *
 * This example illustrates the use of GetDiagField() to identify
 * the type of SQL statement executed and, for SQL statements where
 * the row count is defined on all implementations, the use of
 * GetDiagField() to obtain the row count.
 */

/*
 * Sample program - uses concise CLI functions to execute
 * interactively an ad hoc statement.
 */
#include <sqlcli.h>
#include <string.h>
#include <stdlib.h>

#define  MAXCOLS   100

#define  max(a,b) (a>b?a:b)

int print_err(SQLSMALLINT handletype, SQLINTEGER handle);
int build_indicator_message(SQLCHAR *errmsg,
			    SQLPOINTER *data,
			    SQLINTEGER collen,
			    SQLINTEGER *outlen,
			    SQLSMALLINT colnum);

SQLINTEGER display_length(SQLSMALLINT coltype,
			  SQLINTEGER collen,
			  SQLCHAR *colname);

example2(SQLCHAR *server, SQLCHAR *uid, SQLCHAR *authen, SQLCHAR *sqlstr)
{
  int         i;
  SQLHENV     henv;
  SQLHDBC     hdbc;
  SQLHSTMT    hstmt;
  SQLCHAR     errmsg[256];
  SQLCHAR     colname[32];
  SQLSMALLINT coltype;
  SQLSMALLINT colnamelen;
  SQLSMALLINT nullable;
  SQLINTEGER  collen[MAXCOLS];
  SQLSMALLINT scale;
  SQLINTEGER  outlen[MAXCOLS];
  SQLCHAR    *data[MAXCOLS];
  SQLSMALLINT nresultcols;
  SQLINTEGER  rowcount;
  SQLINTEGER  stmttype;
  SQLRETURN   rc;

  /* allocate an environment handle */
  SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &henv);

  /* allocate a connection handle */
  SQLAllocHandle(SQL_HANDLE_DBC, henv, &hdbc);

  /* connect to database */
  if (SQLConnect(hdbc, server, SQL_NTS, uid, SQL_NTS, authen, SQL_NTS)
      != SQL_SUCCESS )
    return(print_err(SQL_HANDLE_DBC, hdbc));

  /* allocate a statement handle */
  SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &hstmt);

  /* execute the SQL statement */
  if (SQLExecDirect(hstmt, sqlstr, SQL_NTS) != SQL_SUCCESS)
    return(print_err(SQL_HANDLE_STMT, hstmt));

  /* see what kind of statement it was */
  SQLGetDiagField(SQL_HANDLE_STMT, hstmt, 0,
		  SQL_DIAG_DYNAMIC_FUNCTION_CODE,
		  (SQLPOINTER)&stmttype, 0, (SQLSMALLINT *)NULL);

  switch (stmttype) {
    /* SELECT statement */
  case SQL_SELECT_CURSOR:
    /* determine number of result columns */
    SQLNumResultCols(hstmt, &nresultcols);
    /* display column names */
    for (i=0; i<nresultcols; i++) {
      SQLDescribeCol(hstmt, i+1, colname, sizeof(colname),
		     &colnamelen, &coltype, &collen[i], &scale, &nullable);

      /* assume there is a display_length function which
	 computes correct length given the data type  */
      collen[i] = display_length(coltype, collen[i], colname);
      (void)printf("%*.*s", collen[i], collen[i], colname);
      /* allocate memory to bind column */
      data[i] = (SQLCHAR *) malloc(collen[i]);

      /* bind columns to program vars, converting all types to CHAR */
      SQLBindCol(hstmt, i+1, SQL_CHAR, data[i], collen[i],
		 &outlen[i]);
    }
    /* display result rows */
    while ((rc=SQLFetch(hstmt))!=SQL_ERROR) {
      errmsg[0] = '\0';
      if (rc ==  SQL_SUCCESS_WITH_INFO) {
	for (i=0; i<nresultcols; i++) {
	  if (outlen[i] ==  SQL_NULL_DATA || outlen[i] >= collen[i])
	    build_indicator_message(errmsg,
                                    (SQLPOINTER *)&data[i], collen[i],
                                    &outlen[i], i);
	  (void)printf("%*.*s ", outlen[i], outlen[i],
		       data[i]);
	} /* for all columns in this row  */
	/* print any truncation messages */
	(void)printf("\n%s", errmsg);
      }
    } /* while rows to fetch */
    SQLClose(hstmt);
    break;

    /* searched DELETE, INSERT or searched UPDATE statement */
  case SQL_DELETE_WHERE:
  case SQL_INSERT:
  case SQL_UPDATE_WHERE:
    /* check rowcount */
    SQLGetDiagField(SQL_HANDLE_STMT, hstmt, 0,
		    SQL_DIAG_ROW_COUNT, (SQLPOINTER)&rowcount, 0,
		    (SQLSMALLINT *)NULL);
    if (SQLEndTran(SQL_HANDLE_ENV, henv, SQL_COMMIT)
	==  SQL_SUCCESS) {
      (void) printf("Operation successful\n");
    }
    else {
      (void) printf("Operation failed\n");
    }
    (void)printf("%ld rows affected\n", rowcount);
    break;

    /* other statements */
  case SQL_ALTER_TABLE:
  case SQL_CREATE_TABLE:
  case SQL_CREATE_VIEW:
  case SQL_DROP_TABLE:
  case SQL_DROP_VIEW:
  case SQL_DYNAMIC_DELETE_CURSOR:
  case SQL_DYNAMIC_UPDATE_CURSOR:
  case SQL_GRANT:
  case SQL_REVOKE:
    if (SQLEndTran(SQL_HANDLE_ENV, henv, SQL_COMMIT)
	==  SQL_SUCCESS) {
      (void) printf("Operation successful\n");
    }
    else {
      (void) printf("Operation failed\n");
    }
    break;

  /* implementation-defined statement */
  default:
    (void)printf("Statement type=%ld\n", stmttype);
    break;
  }

  /* free data buffers */
  for (i=0; i<nresultcols; i++)  {
    (void)free(data[i]);
  }

  /* free statement handle */
  SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
  /* disconnect from database */
  SQLDisconnect(hdbc);
  /* free connection handle */
  SQLFreeHandle(SQL_HANDLE_DBC, hdbc);
  /* free environment handle */
  SQLFreeHandle(SQL_HANDLE_ENV, henv);

  return(0);
}

/***********************************************************
 The following functions are given for completeness, but are
 not relevant for understanding the database processing
 nature of CLI
***********************************************************/

#define MAX_NUM_PRECISION 15
/*#define max length of char string representation of no. as:

  = max(precision) + leading sign + E + exp sign + max exp length
  =  15            + 1            + 1 + 1        + 2
  =  15 + 5
*/
#define MAX_NUM_STRING_SIZE (MAX_NUM_PRECISION + 5)

SQLINTEGER  display_length(SQLSMALLINT coltype, SQLINTEGER collen,
			   SQLCHAR *colname)
{
  switch (coltype) {

  case SQL_VARCHAR:
  case SQL_CHAR:
    return(max(collen,strlen((char *)colname)));
    break;

  case SQL_FLOAT:
  case SQL_DOUBLE:
  case SQL_NUMERIC:
  case SQL_REAL:
  case SQL_DECIMAL:
    return(max(MAX_NUM_STRING_SIZE,strlen((char *)colname)));
    break;

  case SQL_DATETIME:
    return(max(SQL_TIMESTAMP_LEN,strlen((char *)colname)));
    break;

  case SQL_INTEGER:
    return(max(10,strlen((char *)colname)));
    break;

  case SQL_SMALLINT:
    return(max(5,strlen((char *)colname)));
    break;

  default:
    (void)printf("Unknown datatype, %d\n", coltype);
    return(0);
    break;
  }
}

int build_indicator_message(SQLCHAR *errmsg, SQLPOINTER *data,
			    SQLINTEGER collen, SQLINTEGER *outlen, SQLSMALLINT colnum)
{
  if (*outlen ==  SQL_NULL_DATA) {
    (void)strcpy((char *)data, "NULL");
    *outlen=4;
  }
  else {
    sprintf((char *)errmsg+strlen((char *)errmsg),
	    "%d chars truncated, col %d\n", *outlen-collen+1,
	    colnum);
    *outlen=255;
  }
}
