
/* Module:          results.c
 *
 * Description:     This module contains functions related to 
 *                  retrieving result information through the ODBC API.
 *
 * Classes:         n/a
 *
 * API functions:   SQLRowCount, SQLNumResultCols, SQLDescribeCol, SQLColAttributes,
 *                  SQLGetData, SQLFetch, SQLExtendedFetch, 
 *                  SQLMoreResults(NI), SQLSetPos(NI), SQLSetScrollOptions(NI),
 *                  SQLSetCursorName(NI), SQLGetCursorName(NI)
 *
 * Comments:        See "notice.txt" for copyright and license information.
 *
 */

#include <string.h>
#include "psqlodbc.h"
#include "environ.h"
#include "connection.h"
#include "statement.h"
#include "bind.h"
#include "qresult.h"
#include "convert.h"
#include "pgtypes.h" 

#include <stdio.h>
#include <windows.h>
#include <sqlext.h>


RETCODE SQL_API SQLRowCount(
        HSTMT      hstmt,
        SDWORD FAR *pcrow)
{
StatementClass *stmt = (StatementClass *) hstmt;
QResultClass *res;
char *msg, *ptr;

	if ( ! stmt)
		return SQL_ERROR;

	if(stmt->statement_type == STMT_TYPE_SELECT) {
		if (stmt->status == STMT_FINISHED) {
			res = SC_get_Result(stmt);

			if(res && pcrow) {
				*pcrow = QR_get_num_tuples(res);
				return SQL_SUCCESS;
			}
		}
	} else {

		res = SC_get_Result(stmt);
		if (res && pcrow) {
			msg = QR_get_command(res);
			mylog("*** msg = '%s'\n", msg);
			trim(msg);	//	get rid of trailing spaces
			ptr = strrchr(msg, ' ');
			if (ptr) {
				*pcrow = atoi(ptr+1);
				mylog("**** SQLRowCount(): THE ROWS: *pcrow = %d\n", *pcrow);
			}
			else {
				*pcrow = -1;

				mylog("**** SQLRowCount(): NO ROWS: *pcrow = %d\n", *pcrow);
			}

		return SQL_SUCCESS;
		}
	}

	return SQL_ERROR;     
}


//      This returns the number of columns associated with the database
//      attached to "hstmt".


RETCODE SQL_API SQLNumResultCols(
        HSTMT     hstmt,
        SWORD FAR *pccol)
{       
StatementClass *stmt = (StatementClass *) hstmt;
QResultClass *result;

	if ( ! stmt)
		return SQL_INVALID_HANDLE;

	SC_clear_error(stmt);    

	/* CC: Now check for the "prepared, but not executed" situation, that enables us to
	deal with "SQLPrepare -- SQLDescribeCol -- ... -- SQLExecute" situations.
	(AutoCAD 13 ASE/ASI just _loves_ that ;-) )
	*/
	mylog("**** SQLNumResultCols: calling SC_pre_execute\n");

	SC_pre_execute(stmt);       

	result = SC_get_Result(stmt);

	mylog("SQLNumResultCols: result = %u, status = %d, numcols = %d\n", result, stmt->status, result != NULL ? QR_NumResultCols(result) : -1);
	if (( ! result) || ((stmt->status != STMT_FINISHED) && (stmt->status != STMT_PREMATURE)) ) {
		/* no query has been executed on this statement */
		stmt->errornumber = STMT_SEQUENCE_ERROR;
		stmt->errormsg = "No query has been executed with that handle";
		return SQL_ERROR;
	}

	*pccol = QR_NumResultCols(result);

	return SQL_SUCCESS;
}

//      -       -       -       -       -       -       -       -       -

//      Return information about the database column the user wants
//      information about.
/* CC: preliminary implementation */
RETCODE SQL_API SQLDescribeCol(
        HSTMT      hstmt,
        UWORD      icol,
        UCHAR  FAR *szColName,
        SWORD      cbColNameMax,
        SWORD  FAR *pcbColName,
        SWORD  FAR *pfSqlType,
        UDWORD FAR *pcbColDef,
        SWORD  FAR *pibScale,
        SWORD  FAR *pfNullable)
{
    /* gets all the information about a specific column */
StatementClass *stmt = (StatementClass *) hstmt;
QResultClass *result;
char *name;
Int4 fieldtype;

    if ( ! stmt)
        return SQL_INVALID_HANDLE;

    SC_clear_error(stmt);

    /* CC: Now check for the "prepared, but not executed" situation, that enables us to
           deal with "SQLPrepare -- SQLDescribeCol -- ... -- SQLExecute" situations.
           (AutoCAD 13 ASE/ASI just _loves_ that ;-) )
    */

	SC_pre_execute(stmt);       

        
    result = SC_get_Result(stmt);
	mylog("**** SQLDescribeCol: result = %u, stmt->status = %d, !finished=%d, !premature=%d\n", result, stmt->status, stmt->status != STMT_FINISHED, stmt->status != STMT_PREMATURE);
    if ( (NULL == result) || ((stmt->status != STMT_FINISHED) && (stmt->status != STMT_PREMATURE))) {
        /* no query has been executed on this statement */
        stmt->errornumber = STMT_SEQUENCE_ERROR;
        stmt->errormsg = "No query has been assigned to this statement.";
        return SQL_ERROR;
    }

    if (cbColNameMax >= 1) {
        name = QR_get_fieldname(result, (Int2) (icol-1));
		mylog("describeCol: col %d fieldname = '%s'\n", icol - 1, name);
        /* our indices start from 0 whereas ODBC defines indices starting from 1 */
        if (NULL != pcbColName)  {
            // we want to get the total number of bytes in the column name
            if (NULL == name) 
                *pcbColName = 0;
            else
                *pcbColName = strlen(name);
        }
        if (NULL != szColName) {
            // get the column name into the buffer if there is one
            if (NULL == name) 
                szColName[0] = '\0';
            else
                strncpy_null(szColName, name, cbColNameMax);
        }
    }

    fieldtype = QR_get_field_type(result, (Int2) (icol-1));
	mylog("describeCol: col %d fieldtype = %d\n", icol - 1, fieldtype);

    if (NULL != pfSqlType) {
        *pfSqlType = pgtype_to_sqltype(fieldtype);
		if (*pfSqlType == PG_UNKNOWN)
			*pfSqlType = SQL_CHAR;
	}

    if (NULL != pcbColDef)
		*pcbColDef = pgtype_precision(fieldtype);

    if (NULL != pibScale) {
        Int2 scale;
        scale = pgtype_scale(fieldtype);
        if(scale == -1) { scale = 0; }
        
        *pibScale = scale;
    }
    if (NULL != pfNullable) {
        *pfNullable = pgtype_nullable(fieldtype);
    }

    return SQL_SUCCESS;
}

//      Returns result column descriptor information for a result set.

RETCODE SQL_API SQLColAttributes(
        HSTMT      hstmt,
        UWORD      icol,
        UWORD      fDescType,
        PTR        rgbDesc,
        SWORD      cbDescMax,
        SWORD  FAR *pcbDesc,
        SDWORD FAR *pfDesc)
{
StatementClass *stmt = (StatementClass *) hstmt;
char *value;
Int4 field_type;

    if( ! stmt) {
        return SQL_INVALID_HANDLE;
    }

    /* CC: Now check for the "prepared, but not executed" situation, that enables us to
           deal with "SQLPrepare -- SQLDescribeCol -- ... -- SQLExecute" situations.
           (AutoCAD 13 ASE/ASI just _loves_ that ;-) )
    */
    SC_pre_execute(stmt);       

	mylog("**** SQLColAtt: result = %u, status = %d, numcols = %d\n", stmt->result, stmt->status, stmt->result != NULL ? QR_NumResultCols(stmt->result) : -1);

    if ( (NULL == stmt->result) || ((stmt->status != STMT_FINISHED) && (stmt->status != STMT_PREMATURE)) ) {
        stmt->errormsg = "Can't get column attributes: no result found.";
        stmt->errornumber = STMT_SEQUENCE_ERROR;
        return SQL_ERROR;
    }

    if(icol < 1) {
        // we do not support bookmarks
        stmt->errormsg = "Bookmarks are not currently supported.";
        stmt->errornumber = STMT_NOT_IMPLEMENTED_ERROR;
        return SQL_ERROR;
    }

    icol -= 1;
    field_type = QR_get_field_type(stmt->result, icol);
	mylog("colAttr: col %d field_type = %d\n", icol, field_type);
    switch(fDescType) {
    case SQL_COLUMN_AUTO_INCREMENT:
        if (NULL != pfDesc) {
	    *pfDesc = pgtype_auto_increment(field_type);

	    if(*pfDesc == -1) { /* "not applicable" becomes false */
		*pfDesc = FALSE;
	    }
	}
        break;
    case SQL_COLUMN_CASE_SENSITIVE:
        if (NULL != pfDesc)    
          *pfDesc = pgtype_case_sensitive(field_type);
        break;
    case SQL_COLUMN_COUNT:
        if (NULL != pfDesc)    
          *pfDesc = QR_NumResultCols(stmt->result);
        break;
    case SQL_COLUMN_DISPLAY_SIZE:
		if (NULL != pfDesc)
			*pfDesc = pgtype_precision(field_type);

		mylog("colAttr: col %d fieldsize = %d\n", icol, *pfDesc);

        break;
    case SQL_COLUMN_LABEL:
    case SQL_COLUMN_NAME:
        value = QR_get_fieldname(stmt->result, icol);
        strncpy_null((char *)rgbDesc, value, cbDescMax);
        /* CC: Check for Nullpointesr */
        if (NULL != pcbDesc)
          *pcbDesc = strlen(value);
        break;
    case SQL_COLUMN_LENGTH:
        if (NULL != pfDesc)
          *pfDesc = pgtype_precision(field_type);
        return SQL_SUCCESS;
        break;
    case SQL_COLUMN_MONEY:
        if (NULL != pfDesc)    
          *pfDesc = pgtype_money(field_type);
        break;
    case SQL_COLUMN_NULLABLE:
        if (NULL != pfDesc)    
          *pfDesc = pgtype_nullable(field_type);
        break;
    case SQL_COLUMN_OWNER_NAME:
        return SQL_ERROR;
        break;
    case SQL_COLUMN_PRECISION:
        if (NULL != pfDesc)    
          *pfDesc = pgtype_precision(field_type);
        break;
    case SQL_COLUMN_QUALIFIER_NAME:
        strncpy_null((char *)rgbDesc, "", cbDescMax);
        if (NULL != pfDesc)        
          *pcbDesc = 1;
        break;
    case SQL_COLUMN_SCALE:
        if (NULL != pfDesc)    
          *pfDesc = pgtype_scale(field_type);
        break;
    case SQL_COLUMN_SEARCHABLE:
        if (NULL != pfDesc)    
          *pfDesc = pgtype_searchable(field_type);
        break;
    case SQL_COLUMN_TABLE_NAME:
        return SQL_ERROR;
        break;
    case SQL_COLUMN_TYPE:
        if (NULL != pfDesc) {
          *pfDesc = pgtype_to_sqltype(field_type);
		  if (*pfDesc == PG_UNKNOWN)
			  *pfDesc = SQL_CHAR;
		}
        break;
    case SQL_COLUMN_TYPE_NAME:
        value = pgtype_to_name(field_type);
        strncpy_null((char *)rgbDesc, value, cbDescMax);
        if (NULL != pcbDesc)        
          *pcbDesc = strlen(value);
        break;
    case SQL_COLUMN_UNSIGNED:
        if (NULL != pfDesc) {
	    *pfDesc = pgtype_unsigned(field_type);
	    if(*pfDesc == -1) {
		*pfDesc = FALSE;
	    }
	}
        break;
    case SQL_COLUMN_UPDATABLE:
        // everything should be updatable, I guess, unless access permissions
        // prevent it--are we supposed to check for that here?  seems kind
        // of complicated.  hmm...
        if (NULL != pfDesc)        
          *pfDesc = SQL_ATTR_WRITE;
        break;
    }

    return SQL_SUCCESS;
}

//      Returns result data for a single column in the current row.

RETCODE SQL_API SQLGetData(
        HSTMT      hstmt,
        UWORD      icol,
        SWORD      fCType,
        PTR        rgbValue,
        SDWORD     cbValueMax,
        SDWORD FAR *pcbValue)
{
QResultClass *res;
StatementClass *stmt = (StatementClass *) hstmt;
int num_cols, num_rows;
Int4 field_type;
void *value;
int result;

    if( ! stmt) {
        return SQL_INVALID_HANDLE;
    }
	res = stmt->result;

    if (STMT_EXECUTING == stmt->status) {
        stmt->errormsg = "Can't get data while statement is still executing.";
        stmt->errornumber = STMT_SEQUENCE_ERROR;
        return 0;
    }

    if (stmt->status != STMT_FINISHED) {
        stmt->errornumber = STMT_STATUS_ERROR;
        stmt->errormsg = "GetData can only be called after the successful execution on a SQL statement";
        return 0;
    }

    if (icol == 0) {
        stmt->errormsg = "Bookmarks are not currently supported.";
        stmt->errornumber = STMT_NOT_IMPLEMENTED_ERROR;
        return SQL_ERROR;
    }

    // use zero-based column numbers
    icol--;

    // make sure the column number is valid
    num_cols = QR_NumResultCols(res);
    if (icol >= num_cols) {
        stmt->errormsg = "Invalid column number.";
        stmt->errornumber = STMT_INVALID_COLUMN_NUMBER_ERROR;
        return SQL_ERROR;
    }

	if ( stmt->manual_result) {
		// make sure we're positioned on a valid row
		num_rows = QR_get_num_tuples(res);
		if((stmt->currTuple < 0) ||
		   (stmt->currTuple >= num_rows)) {
			stmt->errormsg = "Not positioned on a valid row for GetData.";
			stmt->errornumber = STMT_INVALID_CURSOR_STATE_ERROR;
			return SQL_ERROR;
		}
		value = QR_get_value_manual(res, stmt->currTuple, icol);
	}
	else { /* its a SOCKET result (backend data) */
		if (stmt->currTuple == -1 || ! res || QR_end_tuples(res)) {
			stmt->errormsg = "Not positioned on a valid row for GetData.";
			stmt->errornumber = STMT_INVALID_CURSOR_STATE_ERROR;
			return SQL_ERROR;
		}

		value = QR_get_value_backend(res, icol);

	}

	field_type = QR_get_field_type(res, icol);

	mylog("**** SQLGetData: icol = %d, fCType = %d, field_type = %d, value = '%s'\n", icol, fCType, field_type, value);

    result = copy_and_convert_field(field_type, value, 
                                    fCType, rgbValue, cbValueMax, pcbValue);


    if(result == COPY_UNSUPPORTED_TYPE) {
        stmt->errormsg = "Received an unsupported type from Postgres.";
        stmt->errornumber = STMT_RESTRICTED_DATA_TYPE_ERROR;
        return SQL_ERROR;
    } else if(result == COPY_UNSUPPORTED_CONVERSION) {
        stmt->errormsg = "Couldn't handle the necessary data type conversion.";
        stmt->errornumber = STMT_RESTRICTED_DATA_TYPE_ERROR;
        return SQL_ERROR;
    } else if(result == COPY_RESULT_TRUNCATED) {
        stmt->errornumber = STMT_TRUNCATED;
        stmt->errormsg = "The buffer was too small for the result.";
        return SQL_SUCCESS_WITH_INFO;
    } else if(result != COPY_OK) {
        stmt->errormsg = "Unrecognized return value from copy_and_convert_field.";
        stmt->errornumber = STMT_INTERNAL_ERROR;
        return SQL_ERROR;
    }

    return SQL_SUCCESS;
}

//      Returns data for bound columns in the current row ("hstmt->iCursor"),
//      advances the cursor.

RETCODE SQL_API SQLFetch(
        HSTMT   hstmt)
{
StatementClass *stmt = (StatementClass *) hstmt;   
QResultClass *res;
int retval;
Int2 num_cols, lf;
Oid type;
char *value;
ColumnInfoClass *ci;


 if ( ! stmt)
     return SQL_INVALID_HANDLE;

 SC_clear_error(stmt);

 if ( ! (res = stmt->result)) {
     stmt->errormsg = "Null statement result in SQLFetch.";
     stmt->errornumber = STMT_SEQUENCE_ERROR;
     return SQL_ERROR;
 }

 ci = QR_get_fields(res);		/* the column info */

 if (stmt->status == STMT_EXECUTING) {
     stmt->errormsg = "Can't fetch while statement is still executing.";
     stmt->errornumber = STMT_SEQUENCE_ERROR;
     return SQL_ERROR;
 }


 if (stmt->status != STMT_FINISHED) {
     stmt->errornumber = STMT_STATUS_ERROR;
     stmt->errormsg = "Fetch can only be called after the successful execution on a SQL statement";
     return SQL_ERROR;
 }

 if (stmt->bindings == NULL) {
     // just to avoid a crash if the user insists on calling this
     // function even if SQL_ExecDirect has reported an Error
     stmt->errormsg = "Bindings were not allocated properly.";
     stmt->errornumber = STMT_SEQUENCE_ERROR;
     return SQL_ERROR;
 }

 
 if ( stmt->manual_result) {
	 if (QR_get_num_tuples(res) -1 == stmt->currTuple ||
		 (stmt->maxRows > 0 && stmt->currTuple == stmt->maxRows - 1))
		 /* if we are at the end of a tuple list, we return a "no data found" */
		 return SQL_NO_DATA_FOUND;
 
	 mylog("**** SQLFetch: manual_result\n");
	 (stmt->currTuple)++;
 }
 else {

	 // read from the cache or the physical next tuple
	 retval = QR_next_tuple(res);
	 if (retval < 0) {
		mylog("**** SQLFetch: end_tuples\n");
		return SQL_NO_DATA_FOUND;
	 }
	 else if (retval > 0)
		 (stmt->currTuple)++;		// all is well

	 else {
		mylog("SQLFetch: error\n");
		stmt->errornumber = STMT_EXEC_ERROR;
		stmt->errormsg = "Error fetching next row";
		 return SQL_ERROR;
	 }

 }

 num_cols = QR_NumResultCols(res);

 for (lf=0; lf < num_cols; lf++) {

	 mylog("fetch: cols=%d, lf=%d, buffer[] = %u\n", 
			 num_cols, lf, stmt->bindings[lf].buffer);

     if (stmt->bindings[lf].buffer != NULL) {
            // this column has a binding

            // type = QR_get_field_type(res, lf);
			type = CI_get_oid(ci, lf);		/* speed things up */

			if (stmt->manual_result)
				value = QR_get_value_manual(res, stmt->currTuple, lf);
			else
				value = QR_get_value_backend(res, lf);

            retval = copy_and_convert_field_bindinfo(type, value, &(stmt->bindings[lf]));

            // check whether the complete result was copied
            if(retval == COPY_UNSUPPORTED_TYPE) {
                stmt->errormsg = "Received an unsupported type from Postgres.";
                stmt->errornumber = STMT_RESTRICTED_DATA_TYPE_ERROR;
                return SQL_ERROR;

            } else if(retval == COPY_UNSUPPORTED_CONVERSION) {
                stmt->errormsg = "Couldn't handle the necessary data type conversion.";
                stmt->errornumber = STMT_RESTRICTED_DATA_TYPE_ERROR;
                return SQL_ERROR;

            } else if(retval == COPY_RESULT_TRUNCATED) {
                /* The result has been truncated during the copy */
                /* this will generate a SQL_SUCCESS_WITH_INFO result */
                stmt->errornumber = STMT_TRUNCATED;
                stmt->errormsg = "A buffer was too small for the return value to fit in";
				return SQL_SUCCESS_WITH_INFO;

            } else if(retval != COPY_OK) {
                stmt->errormsg = "Unrecognized return value from copy_and_convert_field.";
                stmt->errornumber = STMT_INTERNAL_ERROR;
                return SQL_ERROR;

            }
     }
 }

 return SQL_SUCCESS;
}

//      This fetchs a block of data (rowset).

RETCODE SQL_API SQLExtendedFetch(
        HSTMT      hstmt,
        UWORD      fFetchType,
        SDWORD     irow,
        UDWORD FAR *pcrow,
        UWORD  FAR *rgfRowStatus)
{
StatementClass *stmt = (StatementClass *) hstmt;

       if ( ! stmt)
          return SQL_INVALID_HANDLE;

	   /*	Currently, only for manual results can this be done 
			because not all the tuples are read in ahead of time.
	   */
	   if ( ! stmt->manual_result)
		   return SQL_ERROR;

       // CC: we currently only support fetches in one row bits
       if (NULL != pcrow)
         *pcrow = 1;
       if (NULL != rgfRowStatus)  
         *rgfRowStatus = SQL_ROW_SUCCESS;

       switch (fFetchType)  {
       case SQL_FETCH_NEXT:
          return SQLFetch(hstmt);
       case SQL_FETCH_PRIOR:
          if (stmt->currTuple <= 0)
              return SQL_ERROR;
          stmt->currTuple--;
          return SQLFetch(hstmt);
       case SQL_FETCH_FIRST:
          stmt->currTuple = -1;
          return SQLFetch(hstmt);
       case SQL_FETCH_LAST:
          stmt->currTuple = QR_get_num_tuples(stmt->result)-1;
          return SQLFetch(hstmt);
       case SQL_FETCH_ABSOLUTE:
          if (irow == 0) {
              stmt->currTuple = stmt->currTuple > 0 ? stmt->currTuple-2 : -1;
          } else if (irow > 0) {
              stmt->currTuple = irow-2;
              return SQLFetch(hstmt);
          } else {
              // CC: ??? not sure about the specification in that case
              return SQL_ERROR;
          }    
        default:
          return SQL_ERROR;   
        }           
        return SQL_SUCCESS;
}

//      This determines whether there are more results sets available for
//      the "hstmt".

/* CC: return SQL_NO_DATA_FOUND since we do not support multiple result sets */
RETCODE SQL_API SQLMoreResults(
        HSTMT   hstmt)
{
          return SQL_NO_DATA_FOUND;
}

//      This positions the cursor within a block of data.

RETCODE SQL_API SQLSetPos(
        HSTMT   hstmt,
        UWORD   irow,
        UWORD   fOption,
        UWORD   fLock)
{
        return SQL_ERROR;
}

//      Sets options that control the behavior of cursors.

RETCODE SQL_API SQLSetScrollOptions(
        HSTMT      hstmt,
        UWORD      fConcurrency,
        SDWORD  crowKeyset,
        UWORD      crowRowset)
{
        return SQL_ERROR;
}


//      Set the cursor name on a statement handle

RETCODE SQL_API SQLSetCursorName(
        HSTMT     hstmt,
        UCHAR FAR *szCursor,
        SWORD     cbCursor)
{
        return SQL_SUCCESS;
}

//      Return the cursor name for a statement handle

RETCODE SQL_API SQLGetCursorName(
        HSTMT     hstmt,
        UCHAR FAR *szCursor,
        SWORD     cbCursorMax,
        SWORD FAR *pcbCursor)
{
        return SQL_ERROR;
}


