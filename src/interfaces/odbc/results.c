/*-------
 * Module:			results.c
 *
 * Description:		This module contains functions related to
 *					retrieving result information through the ODBC API.
 *
 * Classes:			n/a
 *
 * API functions:	SQLRowCount, SQLNumResultCols, SQLDescribeCol,
 *					SQLColAttributes, SQLGetData, SQLFetch, SQLExtendedFetch,
 *					SQLMoreResults(NI), SQLSetPos, SQLSetScrollOptions(NI),
 *					SQLSetCursorName, SQLGetCursorName
 *
 * Comments:		See "notice.txt" for copyright and license information.
 *-------
 */

#include "psqlodbc.h"

#include <string.h>
#include "dlg_specific.h"
#include "environ.h"
#include "connection.h"
#include "statement.h"
#include "bind.h"
#include "qresult.h"
#include "convert.h"
#include "pgtypes.h"

#include <stdio.h>

#include "pgapifunc.h"



RETCODE		SQL_API
PGAPI_RowCount(
			   HSTMT hstmt,
			   SDWORD FAR * pcrow)
{
	static char *func = "PGAPI_RowCount";
	StatementClass *stmt = (StatementClass *) hstmt;
	QResultClass *res;
	char	   *msg,
			   *ptr;
	ConnInfo   *ci;

	if (!stmt)
	{
		SC_log_error(func, "", NULL);
		return SQL_INVALID_HANDLE;
	}
	ci = &(SC_get_conn(stmt)->connInfo);
	if (stmt->manual_result)
	{
		if (pcrow)
			*pcrow = -1;
		return SQL_SUCCESS;
	}

	if (stmt->statement_type == STMT_TYPE_SELECT)
	{
		if (stmt->status == STMT_FINISHED)
		{
			res = SC_get_Result(stmt);

			if (res && pcrow)
			{
				*pcrow = SC_is_fetchcursor(stmt) ? -1 : QR_get_num_tuples(res);
				return SQL_SUCCESS;
			}
		}
	}
	else
	{
		res = SC_get_Result(stmt);
		if (res && pcrow)
		{
			msg = QR_get_command(res);
			mylog("*** msg = '%s'\n", msg);
			trim(msg);			/* get rid of trailing spaces */
			ptr = strrchr(msg, ' ');
			if (ptr)
			{
				*pcrow = atoi(ptr + 1);
				mylog("**** PGAPI_RowCount(): THE ROWS: *pcrow = %d\n", *pcrow);
			}
			else
			{
				*pcrow = -1;
				mylog("**** PGAPI_RowCount(): NO ROWS: *pcrow = %d\n", *pcrow);
			}

			return SQL_SUCCESS;
		}
	}

	SC_log_error(func, "Bad return value", stmt);
	return SQL_ERROR;
}


/*
 *	This returns the number of columns associated with the database
 *	attached to "hstmt".
 */
RETCODE		SQL_API
PGAPI_NumResultCols(
					HSTMT hstmt,
					SWORD FAR * pccol)
{
	static char *func = "PGAPI_NumResultCols";
	StatementClass *stmt = (StatementClass *) hstmt;
	QResultClass *result;
	char		parse_ok;
	ConnInfo   *ci;

	if (!stmt)
	{
		SC_log_error(func, "", NULL);
		return SQL_INVALID_HANDLE;
	}
	ci = &(SC_get_conn(stmt)->connInfo);

	SC_clear_error(stmt);

	parse_ok = FALSE;
	if (ci->drivers.parse && stmt->statement_type == STMT_TYPE_SELECT)
	{
		if (stmt->parse_status == STMT_PARSE_NONE)
		{
			mylog("PGAPI_NumResultCols: calling parse_statement on stmt=%u\n", stmt);
			parse_statement(stmt);
		}

		if (stmt->parse_status != STMT_PARSE_FATAL)
		{
			parse_ok = TRUE;
			*pccol = stmt->nfld;
			mylog("PARSE: PGAPI_NumResultCols: *pccol = %d\n", *pccol);
		}
	}

	if (!parse_ok)
	{
		SC_pre_execute(stmt);
		result = SC_get_Result(stmt);

		mylog("PGAPI_NumResultCols: result = %u, status = %d, numcols = %d\n", result, stmt->status, result != NULL ? QR_NumResultCols(result) : -1);
		if ((!result) || ((stmt->status != STMT_FINISHED) && (stmt->status != STMT_PREMATURE)))
		{
			/* no query has been executed on this statement */
			stmt->errornumber = STMT_SEQUENCE_ERROR;
			stmt->errormsg = "No query has been executed with that handle";
			SC_log_error(func, "", stmt);
			return SQL_ERROR;
		}

		*pccol = QR_NumResultCols(result);
		/* updatable cursors */
		if (ci->updatable_cursors &&
			stmt->options.scroll_concurrency != SQL_CONCUR_READ_ONLY)
			*pccol -= 2;
	}

	return SQL_SUCCESS;
}


/*
 *	Return information about the database column the user wants
 *	information about.
 */
RETCODE		SQL_API
PGAPI_DescribeCol(
				  HSTMT hstmt,
				  UWORD icol,
				  UCHAR FAR * szColName,
				  SWORD cbColNameMax,
				  SWORD FAR * pcbColName,
				  SWORD FAR * pfSqlType,
				  UDWORD FAR * pcbColDef,
				  SWORD FAR * pibScale,
				  SWORD FAR * pfNullable)
{
	static char *func = "PGAPI_DescribeCol";

	/* gets all the information about a specific column */
	StatementClass *stmt = (StatementClass *) hstmt;
	QResultClass *res;
	char	   *col_name = NULL;
	Int4		fieldtype = 0;
	int			precision = 0,
				scale = 0;
	ConnInfo   *ci;
	char		parse_ok;
	char		buf[255];
	int			len = 0;
	RETCODE		result;

	mylog("%s: entering...\n", func);

	if (!stmt)
	{
		SC_log_error(func, "", NULL);
		return SQL_INVALID_HANDLE;
	}

	ci = &(SC_get_conn(stmt)->connInfo);

	SC_clear_error(stmt);

	/*
	 * Dont check for bookmark column. This is the responsibility of the
	 * driver manager.
	 */

	icol--;						/* use zero based column numbers */

	parse_ok = FALSE;
	if (ci->drivers.parse && stmt->statement_type == STMT_TYPE_SELECT)
	{
		if (stmt->parse_status == STMT_PARSE_NONE)
		{
			mylog("PGAPI_DescribeCol: calling parse_statement on stmt=%u\n", stmt);
			parse_statement(stmt);
		}

		mylog("PARSE: DescribeCol: icol=%d, stmt=%u, stmt->nfld=%d, stmt->fi=%u\n", icol, stmt, stmt->nfld, stmt->fi);

		if (stmt->parse_status != STMT_PARSE_FATAL && stmt->fi && stmt->fi[icol])
		{
			if (icol >= stmt->nfld)
			{
				stmt->errornumber = STMT_INVALID_COLUMN_NUMBER_ERROR;
				stmt->errormsg = "Invalid column number in DescribeCol.";
				SC_log_error(func, "", stmt);
				return SQL_ERROR;
			}
			mylog("DescribeCol: getting info for icol=%d\n", icol);

			fieldtype = stmt->fi[icol]->type;
			if (stmt->fi[icol]->alias[0])
				col_name = stmt->fi[icol]->alias;
			else
				col_name = stmt->fi[icol]->name;
			precision = stmt->fi[icol]->precision;
			scale = stmt->fi[icol]->scale;

			mylog("PARSE: fieldtype=%d, col_name='%s', precision=%d\n", fieldtype, col_name, precision);
			if (fieldtype > 0)
				parse_ok = TRUE;
		}
	}

	/*
	 * If couldn't parse it OR the field being described was not parsed
	 * (i.e., because it was a function or expression, etc, then do it the
	 * old fashioned way.
	 */
	if (!parse_ok)
	{
		SC_pre_execute(stmt);

		res = SC_get_Result(stmt);

		mylog("**** PGAPI_DescribeCol: res = %u, stmt->status = %d, !finished=%d, !premature=%d\n", res, stmt->status, stmt->status != STMT_FINISHED, stmt->status != STMT_PREMATURE);
		if ((NULL == res) || ((stmt->status != STMT_FINISHED) && (stmt->status != STMT_PREMATURE)))
		{
			/* no query has been executed on this statement */
			stmt->errornumber = STMT_SEQUENCE_ERROR;
			stmt->errormsg = "No query has been assigned to this statement.";
			SC_log_error(func, "", stmt);
			return SQL_ERROR;
		}

		if (icol >= QR_NumResultCols(res))
		{
			stmt->errornumber = STMT_INVALID_COLUMN_NUMBER_ERROR;
			stmt->errormsg = "Invalid column number in DescribeCol.";
			sprintf(buf, "Col#=%d, #Cols=%d", icol, QR_NumResultCols(res));
			SC_log_error(func, buf, stmt);
			return SQL_ERROR;
		}

		col_name = QR_get_fieldname(res, icol);
		fieldtype = QR_get_field_type(res, icol);

		/* atoi(ci->unknown_sizes) */
		precision = pgtype_precision(stmt, fieldtype, icol, ci->drivers.unknown_sizes);
		scale = pgtype_scale(stmt, fieldtype, icol);
	}

	mylog("describeCol: col %d fieldname = '%s'\n", icol, col_name);
	mylog("describeCol: col %d fieldtype = %d\n", icol, fieldtype);
	mylog("describeCol: col %d precision = %d\n", icol, precision);

	result = SQL_SUCCESS;

	/*
	 * COLUMN NAME
	 */
	len = strlen(col_name);

	if (pcbColName)
		*pcbColName = len;

	if (szColName)
	{
		strncpy_null(szColName, col_name, cbColNameMax);

		if (len >= cbColNameMax)
		{
			result = SQL_SUCCESS_WITH_INFO;
			stmt->errornumber = STMT_TRUNCATED;
			stmt->errormsg = "The buffer was too small for the colName.";
		}
	}

	/*
	 * SQL TYPE
	 */
	if (pfSqlType)
	{
		*pfSqlType = pgtype_to_sqltype(stmt, fieldtype);

		mylog("describeCol: col %d *pfSqlType = %d\n", icol, *pfSqlType);
	}

	/*
	 * PRECISION
	 */
	if (pcbColDef)
	{
		if (precision < 0)
			precision = 0;		/* "I dont know" */

		*pcbColDef = precision;

		mylog("describeCol: col %d  *pcbColDef = %d\n", icol, *pcbColDef);
	}

	/*
	 * SCALE
	 */
	if (pibScale)
	{
		if (scale < 0)
			scale = 0;

		*pibScale = scale;
		mylog("describeCol: col %d  *pibScale = %d\n", icol, *pibScale);
	}

	/*
	 * NULLABILITY
	 */
	if (pfNullable)
	{
		*pfNullable = (parse_ok) ? stmt->fi[icol]->nullable : pgtype_nullable(stmt, fieldtype);

		mylog("describeCol: col %d  *pfNullable = %d\n", icol, *pfNullable);
	}

	return result;
}


/*		Returns result column descriptor information for a result set. */
RETCODE		SQL_API
PGAPI_ColAttributes(
					HSTMT hstmt,
					UWORD icol,
					UWORD fDescType,
					PTR rgbDesc,
					SWORD cbDescMax,
					SWORD FAR * pcbDesc,
					SDWORD FAR * pfDesc)
{
	static char *func = "PGAPI_ColAttributes";
	StatementClass *stmt = (StatementClass *) hstmt;
	Int4		field_type = 0;
	ConnInfo   *ci;
	int			unknown_sizes;
	int			cols = 0;
	char		parse_ok;
	RETCODE		result;
	char	   *p = NULL;
	int			len = 0,
				value = 0;

	mylog("%s: entering...\n", func);

	if (!stmt)
	{
		SC_log_error(func, "", NULL);
		return SQL_INVALID_HANDLE;
	}

	ci = &(SC_get_conn(stmt)->connInfo);

	/*
	 * Dont check for bookmark column.	This is the responsibility of the
	 * driver manager.	For certain types of arguments, the column number
	 * is ignored anyway, so it may be 0.
	 */

	icol--;

	/* atoi(ci->unknown_sizes); */
	unknown_sizes = ci->drivers.unknown_sizes;

	/* not appropriate for SQLColAttributes() */
	if (unknown_sizes == UNKNOWNS_AS_DONTKNOW)
		unknown_sizes = UNKNOWNS_AS_MAX;

	parse_ok = FALSE;
	if (ci->drivers.parse && stmt->statement_type == STMT_TYPE_SELECT)
	{
		if (stmt->parse_status == STMT_PARSE_NONE)
		{
			mylog("PGAPI_ColAttributes: calling parse_statement\n");
			parse_statement(stmt);
		}

		cols = stmt->nfld;

		/*
		 * Column Count is a special case.	The Column number is ignored
		 * in this case.
		 */
		if (fDescType == SQL_COLUMN_COUNT)
		{
			if (pfDesc)
				*pfDesc = cols;

			return SQL_SUCCESS;
		}

		if (stmt->parse_status != STMT_PARSE_FATAL && stmt->fi && stmt->fi[icol])
		{
			if (icol >= cols)
			{
				stmt->errornumber = STMT_INVALID_COLUMN_NUMBER_ERROR;
				stmt->errormsg = "Invalid column number in ColAttributes.";
				SC_log_error(func, "", stmt);
				return SQL_ERROR;
			}
			field_type = stmt->fi[icol]->type;
			if (field_type > 0)
				parse_ok = TRUE;
		}
	}

	if (!parse_ok)
	{
		SC_pre_execute(stmt);

		mylog("**** PGAPI_ColAtt: result = %u, status = %d, numcols = %d\n", stmt->result, stmt->status, stmt->result != NULL ? QR_NumResultCols(stmt->result) : -1);

		if ((NULL == stmt->result) || ((stmt->status != STMT_FINISHED) && (stmt->status != STMT_PREMATURE)))
		{
			stmt->errormsg = "Can't get column attributes: no result found.";
			stmt->errornumber = STMT_SEQUENCE_ERROR;
			SC_log_error(func, "", stmt);
			return SQL_ERROR;
		}

		cols = QR_NumResultCols(stmt->result);

		/*
		 * Column Count is a special case.	The Column number is ignored
		 * in this case.
		 */
		if (fDescType == SQL_COLUMN_COUNT)
		{
			if (pfDesc)
				*pfDesc = cols;

			return SQL_SUCCESS;
		}

		if (icol >= cols)
		{
			stmt->errornumber = STMT_INVALID_COLUMN_NUMBER_ERROR;
			stmt->errormsg = "Invalid column number in ColAttributes.";
			SC_log_error(func, "", stmt);
			return SQL_ERROR;
		}

		field_type = QR_get_field_type(stmt->result, icol);
	}

	mylog("colAttr: col %d field_type = %d\n", icol, field_type);

	switch (fDescType)
	{
		case SQL_COLUMN_AUTO_INCREMENT:
			value = pgtype_auto_increment(stmt, field_type);
			if (value == -1)	/* non-numeric becomes FALSE (ODBC Doc) */
				value = FALSE;

			break;

		case SQL_COLUMN_CASE_SENSITIVE:
			value = pgtype_case_sensitive(stmt, field_type);
			break;

			/*
			 * This special case is handled above.
			 *
			 * case SQL_COLUMN_COUNT:
			 */
		case SQL_COLUMN_DISPLAY_SIZE:
			value = (parse_ok) ? stmt->fi[icol]->display_size : pgtype_display_size(stmt, field_type, icol, unknown_sizes);

			mylog("PGAPI_ColAttributes: col %d, display_size= %d\n", icol, value);

			break;

		case SQL_COLUMN_LABEL:
			if (parse_ok && stmt->fi[icol]->alias[0] != '\0')
			{
				p = stmt->fi[icol]->alias;

				mylog("PGAPI_ColAttr: COLUMN_LABEL = '%s'\n", p);
				break;

			}
			/* otherwise same as column name -- FALL THROUGH!!! */

		case SQL_COLUMN_NAME:
			p = (parse_ok) ? stmt->fi[icol]->name : QR_get_fieldname(stmt->result, icol);

			mylog("PGAPI_ColAttr: COLUMN_NAME = '%s'\n", p);
			break;

		case SQL_COLUMN_LENGTH:
			value = (parse_ok) ? stmt->fi[icol]->length : pgtype_length(stmt, field_type, icol, unknown_sizes);

			mylog("PGAPI_ColAttributes: col %d, length = %d\n", icol, value);
			break;

		case SQL_COLUMN_MONEY:
			value = pgtype_money(stmt, field_type);
			break;

		case SQL_COLUMN_NULLABLE:
			value = (parse_ok) ? stmt->fi[icol]->nullable : pgtype_nullable(stmt, field_type);
			break;

		case SQL_COLUMN_OWNER_NAME:
			p = "";
			break;

		case SQL_COLUMN_PRECISION:
			value = (parse_ok) ? stmt->fi[icol]->precision : pgtype_precision(stmt, field_type, icol, unknown_sizes);

			mylog("PGAPI_ColAttributes: col %d, precision = %d\n", icol, value);
			break;

		case SQL_COLUMN_QUALIFIER_NAME:
			p = "";
			break;

		case SQL_COLUMN_SCALE:
			value = pgtype_scale(stmt, field_type, icol);
			break;

		case SQL_COLUMN_SEARCHABLE:
			value = pgtype_searchable(stmt, field_type);
			break;

		case SQL_COLUMN_TABLE_NAME:
			p = (parse_ok && stmt->fi[icol]->ti) ? stmt->fi[icol]->ti->name : "";

			mylog("PGAPI_ColAttr: TABLE_NAME = '%s'\n", p);
			break;

		case SQL_COLUMN_TYPE:
			value = pgtype_to_sqltype(stmt, field_type);
			break;

		case SQL_COLUMN_TYPE_NAME:
			p = pgtype_to_name(stmt, field_type);
			break;

		case SQL_COLUMN_UNSIGNED:
			value = pgtype_unsigned(stmt, field_type);
			if (value == -1)	/* non-numeric becomes TRUE (ODBC Doc) */
				value = TRUE;

			break;

		case SQL_COLUMN_UPDATABLE:

			/*
			 * Neither Access or Borland care about this.
			 *
			 * if (field_type == PG_TYPE_OID) pfDesc = SQL_ATTR_READONLY;
			 * else
			 */
			value = SQL_ATTR_WRITE;

			mylog("PGAPI_ColAttr: UPDATEABLE = %d\n", value);
			break;
	}

	result = SQL_SUCCESS;

	if (p)
	{							/* char/binary data */
		len = strlen(p);

		if (rgbDesc)
		{
			strncpy_null((char *) rgbDesc, p, (size_t) cbDescMax);

			if (len >= cbDescMax)
			{
				result = SQL_SUCCESS_WITH_INFO;
				stmt->errornumber = STMT_TRUNCATED;
				stmt->errormsg = "The buffer was too small for the rgbDesc.";
			}
		}

		if (pcbDesc)
			*pcbDesc = len;
	}
	else
	{
		/* numeric data */
		if (pfDesc)
			*pfDesc = value;
	}

	return result;
}


/*	Returns result data for a single column in the current row. */
RETCODE		SQL_API
PGAPI_GetData(
			  HSTMT hstmt,
			  UWORD icol,
			  SWORD fCType,
			  PTR rgbValue,
			  SDWORD cbValueMax,
			  SDWORD FAR * pcbValue)
{
	static char *func = "PGAPI_GetData";
	QResultClass *res;
	StatementClass *stmt = (StatementClass *) hstmt;
	int			num_cols,
				num_rows;
	Int4		field_type;
	void	   *value = NULL;
	int			result;
	char		get_bookmark = FALSE;
	ConnInfo   *ci;

	mylog("PGAPI_GetData: enter, stmt=%u\n", stmt);

	if (!stmt)
	{
		SC_log_error(func, "", NULL);
		return SQL_INVALID_HANDLE;
	}
	ci = &(SC_get_conn(stmt)->connInfo);
	res = stmt->result;

	if (STMT_EXECUTING == stmt->status)
	{
		stmt->errormsg = "Can't get data while statement is still executing.";
		stmt->errornumber = STMT_SEQUENCE_ERROR;
		SC_log_error(func, "", stmt);
		return SQL_ERROR;
	}

	if (stmt->status != STMT_FINISHED)
	{
		stmt->errornumber = STMT_STATUS_ERROR;
		stmt->errormsg = "GetData can only be called after the successful execution on a SQL statement";
		SC_log_error(func, "", stmt);
		return SQL_ERROR;
	}

	if (icol == 0)
	{
		if (stmt->options.use_bookmarks == SQL_UB_OFF)
		{
			stmt->errornumber = STMT_COLNUM_ERROR;
			stmt->errormsg = "Attempt to retrieve bookmark with bookmark usage disabled";
			SC_log_error(func, "", stmt);
			return SQL_ERROR;
		}

		/* Make sure it is the bookmark data type */
		if (fCType != SQL_C_BOOKMARK)
		{
			stmt->errormsg = "Column 0 is not of type SQL_C_BOOKMARK";
			stmt->errornumber = STMT_PROGRAM_TYPE_OUT_OF_RANGE;
			SC_log_error(func, "", stmt);
			return SQL_ERROR;
		}

		get_bookmark = TRUE;
	}
	else
	{
		/* use zero-based column numbers */
		icol--;

		/* make sure the column number is valid */
		num_cols = QR_NumResultCols(res);
		if (icol >= num_cols)
		{
			stmt->errormsg = "Invalid column number.";
			stmt->errornumber = STMT_INVALID_COLUMN_NUMBER_ERROR;
			SC_log_error(func, "", stmt);
			return SQL_ERROR;
		}
	}

	if (stmt->manual_result || !SC_is_fetchcursor(stmt))
	{
		/* make sure we're positioned on a valid row */
		num_rows = QR_get_num_tuples(res);
		if ((stmt->currTuple < 0) ||
			(stmt->currTuple >= num_rows))
		{
			stmt->errormsg = "Not positioned on a valid row for GetData.";
			stmt->errornumber = STMT_INVALID_CURSOR_STATE_ERROR;
			SC_log_error(func, "", stmt);
			return SQL_ERROR;
		}
		mylog("     num_rows = %d\n", num_rows);

		if (!get_bookmark)
		{
			if (stmt->manual_result)
				value = QR_get_value_manual(res, stmt->currTuple, icol);
			else
				value = QR_get_value_backend_row(res, stmt->currTuple, icol);
			mylog("     value = '%s'\n", value);
		}
	}
	else
	{
		/* it's a SOCKET result (backend data) */
		if (stmt->currTuple == -1 || !res || !res->tupleField)
		{
			stmt->errormsg = "Not positioned on a valid row for GetData.";
			stmt->errornumber = STMT_INVALID_CURSOR_STATE_ERROR;
			SC_log_error(func, "", stmt);
			return SQL_ERROR;
		}

		if (!get_bookmark)
			value = QR_get_value_backend(res, icol);

		mylog("  socket: value = '%s'\n", value);
	}

	if (get_bookmark)
	{
		*((UDWORD *) rgbValue) = SC_get_bookmark(stmt);

		if (pcbValue)
			*pcbValue = 4;

		return SQL_SUCCESS;
	}

	field_type = QR_get_field_type(res, icol);

	mylog("**** PGAPI_GetData: icol = %d, fCType = %d, field_type = %d, value = '%s'\n", icol, fCType, field_type, value);

	stmt->current_col = icol;

	result = copy_and_convert_field(stmt, field_type, value,
								 fCType, rgbValue, cbValueMax, pcbValue);

	stmt->current_col = -1;

	switch (result)
	{
		case COPY_OK:
			return SQL_SUCCESS;

		case COPY_UNSUPPORTED_TYPE:
			stmt->errormsg = "Received an unsupported type from Postgres.";
			stmt->errornumber = STMT_RESTRICTED_DATA_TYPE_ERROR;
			SC_log_error(func, "", stmt);
			return SQL_ERROR;

		case COPY_UNSUPPORTED_CONVERSION:
			stmt->errormsg = "Couldn't handle the necessary data type conversion.";
			stmt->errornumber = STMT_RESTRICTED_DATA_TYPE_ERROR;
			SC_log_error(func, "", stmt);
			return SQL_ERROR;

		case COPY_RESULT_TRUNCATED:
			stmt->errornumber = STMT_TRUNCATED;
			stmt->errormsg = "The buffer was too small for the GetData.";
			return SQL_SUCCESS_WITH_INFO;

		case COPY_GENERAL_ERROR:		/* error msg already filled in */
			SC_log_error(func, "", stmt);
			return SQL_ERROR;

		case COPY_NO_DATA_FOUND:
			/* SC_log_error(func, "no data found", stmt); */
			return SQL_NO_DATA_FOUND;

		default:
			stmt->errormsg = "Unrecognized return value from copy_and_convert_field.";
			stmt->errornumber = STMT_INTERNAL_ERROR;
			SC_log_error(func, "", stmt);
			return SQL_ERROR;
	}
}


/*
 *		Returns data for bound columns in the current row ("hstmt->iCursor"),
 *		advances the cursor.
 */
RETCODE		SQL_API
PGAPI_Fetch(
			HSTMT hstmt)
{
	static char *func = "PGAPI_Fetch";
	StatementClass *stmt = (StatementClass *) hstmt;
	QResultClass *res;

	mylog("PGAPI_Fetch: stmt = %u, stmt->result= %u\n", stmt, stmt->result);

	if (!stmt)
	{
		SC_log_error(func, "", NULL);
		return SQL_INVALID_HANDLE;
	}

	SC_clear_error(stmt);

	if (!(res = stmt->result))
	{
		stmt->errormsg = "Null statement result in PGAPI_Fetch.";
		stmt->errornumber = STMT_SEQUENCE_ERROR;
		SC_log_error(func, "", stmt);
		return SQL_ERROR;
	}

	/* Not allowed to bind a bookmark column when using SQLFetch. */
	if (stmt->bookmark.buffer)
	{
		stmt->errornumber = STMT_COLNUM_ERROR;
		stmt->errormsg = "Not allowed to bind a bookmark column when using PGAPI_Fetch";
		SC_log_error(func, "", stmt);
		return SQL_ERROR;
	}

	if (stmt->status == STMT_EXECUTING)
	{
		stmt->errormsg = "Can't fetch while statement is still executing.";
		stmt->errornumber = STMT_SEQUENCE_ERROR;
		SC_log_error(func, "", stmt);
		return SQL_ERROR;
	}

	if (stmt->status != STMT_FINISHED)
	{
		stmt->errornumber = STMT_STATUS_ERROR;
		stmt->errormsg = "Fetch can only be called after the successful execution on a SQL statement";
		SC_log_error(func, "", stmt);
		return SQL_ERROR;
	}

	if (stmt->bindings == NULL)
	{
		/* just to avoid a crash if the user insists on calling this */
		/* function even if SQL_ExecDirect has reported an Error */
		stmt->errormsg = "Bindings were not allocated properly.";
		stmt->errornumber = STMT_SEQUENCE_ERROR;
		SC_log_error(func, "", stmt);
		return SQL_ERROR;
	}

	QR_set_rowset_size(res, 1);
	QR_inc_base(res, stmt->last_fetch_count);

	return SC_fetch(stmt);
}


/*	This fetchs a block of data (rowset). */
RETCODE		SQL_API
PGAPI_ExtendedFetch(
					HSTMT hstmt,
					UWORD fFetchType,
					SDWORD irow,
					UDWORD FAR * pcrow,
					UWORD FAR * rgfRowStatus)
{
	static char *func = "PGAPI_ExtendedFetch";
	StatementClass *stmt = (StatementClass *) hstmt;
	QResultClass *res;
	int			num_tuples,
				i,
				save_rowset_size;
	RETCODE		result;
	char		truncated,
				error;
	ConnInfo   *ci;

	mylog("PGAPI_ExtendedFetch: stmt=%u\n", stmt);

	if (!stmt)
	{
		SC_log_error(func, "", NULL);
		return SQL_INVALID_HANDLE;
	}
	ci = &(SC_get_conn(stmt)->connInfo);

	if (SC_is_fetchcursor(stmt) && !stmt->manual_result)
	{
		if (fFetchType != SQL_FETCH_NEXT)
		{
			stmt->errornumber = STMT_NOT_IMPLEMENTED_ERROR;
			stmt->errormsg = "Unsupported fetch type for PGAPI_ExtendedFetch with UseDeclareFetch option.";
			return SQL_ERROR;
		}
	}

	SC_clear_error(stmt);

	if (!(res = stmt->result))
	{
		stmt->errormsg = "Null statement result in PGAPI_ExtendedFetch.";
		stmt->errornumber = STMT_SEQUENCE_ERROR;
		SC_log_error(func, "", stmt);
		return SQL_ERROR;
	}

	/*
	 * If a bookmark colunmn is bound but bookmark usage is off, then
	 * error
	 */
	if (stmt->bookmark.buffer && stmt->options.use_bookmarks == SQL_UB_OFF)
	{
		stmt->errornumber = STMT_COLNUM_ERROR;
		stmt->errormsg = "Attempt to retrieve bookmark with bookmark usage disabled";
		SC_log_error(func, "", stmt);
		return SQL_ERROR;
	}

	if (stmt->status == STMT_EXECUTING)
	{
		stmt->errormsg = "Can't fetch while statement is still executing.";
		stmt->errornumber = STMT_SEQUENCE_ERROR;
		SC_log_error(func, "", stmt);
		return SQL_ERROR;
	}

	if (stmt->status != STMT_FINISHED)
	{
		stmt->errornumber = STMT_STATUS_ERROR;
		stmt->errormsg = "ExtendedFetch can only be called after the successful execution on a SQL statement";
		SC_log_error(func, "", stmt);
		return SQL_ERROR;
	}

	if (stmt->bindings == NULL)
	{
		/* just to avoid a crash if the user insists on calling this */
		/* function even if SQL_ExecDirect has reported an Error */
		stmt->errormsg = "Bindings were not allocated properly.";
		stmt->errornumber = STMT_SEQUENCE_ERROR;
		SC_log_error(func, "", stmt);
		return SQL_ERROR;
	}

	/* Initialize to no rows fetched */
	if (rgfRowStatus)
		for (i = 0; i < stmt->options.rowset_size; i++)
			*(rgfRowStatus + i) = SQL_ROW_NOROW;

	if (pcrow)
		*pcrow = 0;

	num_tuples = QR_get_num_tuples(res);

	/* Save and discard the saved rowset size */
	save_rowset_size = stmt->save_rowset_size;
	stmt->save_rowset_size = -1;

	switch (fFetchType)
	{
		case SQL_FETCH_NEXT:

			/*
			 * From the odbc spec... If positioned before the start of the
			 * RESULT SET, then this should be equivalent to
			 * SQL_FETCH_FIRST.
			 */

			if (stmt->rowset_start < 0)
				stmt->rowset_start = 0;

			else
				stmt->rowset_start += (save_rowset_size > 0 ? save_rowset_size : stmt->options.rowset_size);

			mylog("SQL_FETCH_NEXT: num_tuples=%d, currtuple=%d\n", num_tuples, stmt->currTuple);
			break;

		case SQL_FETCH_PRIOR:
			mylog("SQL_FETCH_PRIOR: num_tuples=%d, currtuple=%d\n", num_tuples, stmt->currTuple);

			/*
			 * From the odbc spec... If positioned after the end of the
			 * RESULT SET, then this should be equivalent to
			 * SQL_FETCH_LAST.
			 */
			if (stmt->rowset_start >= num_tuples)
			{
				if (stmt->options.rowset_size > num_tuples)
				{
					stmt->errornumber = STMT_POS_BEFORE_RECORDSET;
					stmt->errormsg = "fetch prior from eof and before the beggining";
				}
				stmt->rowset_start = num_tuples <= 0 ? 0 : (num_tuples - stmt->options.rowset_size);

			}
			else
			{
				if (stmt->rowset_start < stmt->options.rowset_size)
				{
					stmt->errormsg = "fetch prior and before the beggining";
					stmt->errornumber = STMT_POS_BEFORE_RECORDSET;
				}
				stmt->rowset_start -= stmt->options.rowset_size;
			}
			break;

		case SQL_FETCH_FIRST:
			mylog("SQL_FETCH_FIRST: num_tuples=%d, currtuple=%d\n", num_tuples, stmt->currTuple);

			stmt->rowset_start = 0;
			break;

		case SQL_FETCH_LAST:
			mylog("SQL_FETCH_LAST: num_tuples=%d, currtuple=%d\n", num_tuples, stmt->currTuple);

			stmt->rowset_start = num_tuples <= 0 ? 0 : (num_tuples - stmt->options.rowset_size);
			break;

		case SQL_FETCH_ABSOLUTE:
			mylog("SQL_FETCH_ABSOLUTE: num_tuples=%d, currtuple=%d, irow=%d\n", num_tuples, stmt->currTuple, irow);

			/* Position before result set, but dont fetch anything */
			if (irow == 0)
			{
				stmt->rowset_start = -1;
				stmt->currTuple = -1;
				return SQL_NO_DATA_FOUND;
			}
			/* Position before the desired row */
			else if (irow > 0)
				stmt->rowset_start = irow - 1;
			/* Position with respect to the end of the result set */
			else
				stmt->rowset_start = num_tuples + irow;
			break;

		case SQL_FETCH_RELATIVE:

			/*
			 * Refresh the current rowset -- not currently implemented,
			 * but lie anyway
			 */
			if (irow == 0)
				break;

			stmt->rowset_start += irow;
			break;

		case SQL_FETCH_BOOKMARK:
			stmt->rowset_start = irow - 1;
			break;

		default:
			SC_log_error(func, "Unsupported PGAPI_ExtendedFetch Direction", stmt);
			return SQL_ERROR;
	}

	/*
	 * CHECK FOR PROPER CURSOR STATE
	 */

	/*
	 * Handle Declare Fetch style specially because the end is not really
	 * the end...
	 */
	if (SC_is_fetchcursor(stmt) && !stmt->manual_result)
	{
		if (QR_end_tuples(res))
			return SQL_NO_DATA_FOUND;
	}
	else
	{
		/* If *new* rowset is after the result_set, return no data found */
		if (stmt->rowset_start >= num_tuples)
		{
			stmt->rowset_start = num_tuples;
			return SQL_NO_DATA_FOUND;
		}
	}

	/* If *new* rowset is prior to result_set, return no data found */
	if (stmt->rowset_start < 0)
	{
		if (stmt->rowset_start + stmt->options.rowset_size <= 0)
		{
			stmt->rowset_start = -1;
			return SQL_NO_DATA_FOUND;
		}
		else
		{						/* overlap with beginning of result set,
								 * so get first rowset */
			stmt->rowset_start = 0;
		}
	}

	/* currTuple is always 1 row prior to the rowset */
	stmt->currTuple = stmt->rowset_start - 1;

	/* increment the base row in the tuple cache */
	QR_set_rowset_size(res, stmt->options.rowset_size);
	/* QR_inc_base(res, stmt->last_fetch_count); */
	/* Is inc_base right ? */
	res->base = stmt->rowset_start;

	/* Physical Row advancement occurs for each row fetched below */

	mylog("PGAPI_ExtendedFetch: new currTuple = %d\n", stmt->currTuple);

	truncated = error = FALSE;
	for (i = 0; i < stmt->options.rowset_size; i++)
	{
		stmt->bind_row = i;		/* set the binding location */
		result = SC_fetch(stmt);

		/* Determine Function status */
		if (result == SQL_NO_DATA_FOUND)
			break;
		else if (result == SQL_SUCCESS_WITH_INFO)
			truncated = TRUE;
		else if (result == SQL_ERROR)
			error = TRUE;

		/* Determine Row Status */
		if (rgfRowStatus)
		{
			if (result == SQL_ERROR)
				*(rgfRowStatus + i) = SQL_ROW_ERROR;
#ifdef	DRIVER_CURSOR_IMPLEMENT
			/* this should be refined */
			else if (result > 10 && result < 20)
				*(rgfRowStatus + i) = result - 10;
#endif   /* DRIVER_CURSOR_IMPLEMENT */
			else
				*(rgfRowStatus + i) = SQL_ROW_SUCCESS;
		}
	}

	/* Save the fetch count for SQLSetPos */
	stmt->last_fetch_count = i;

	/* Reset next binding row */
	stmt->bind_row = 0;

	/* Move the cursor position to the first row in the result set. */
	stmt->currTuple = stmt->rowset_start;

	/* For declare/fetch, need to reset cursor to beginning of rowset */
	if (SC_is_fetchcursor(stmt) && !stmt->manual_result)
		QR_set_position(res, 0);

	/* Set the number of rows retrieved */
	if (pcrow)
		*pcrow = i;

	if (i == 0)
		/* Only DeclareFetch should wind up here */
		return SQL_NO_DATA_FOUND;
	else if (error)
		return SQL_ERROR;
	else if (truncated)
		return SQL_SUCCESS_WITH_INFO;
	else if (stmt->errornumber == STMT_POS_BEFORE_RECORDSET)
		return SQL_SUCCESS_WITH_INFO;
	else
		return SQL_SUCCESS;
}


/*
 *		This determines whether there are more results sets available for
 *		the "hstmt".
 */
/* CC: return SQL_NO_DATA_FOUND since we do not support multiple result sets */
RETCODE		SQL_API
PGAPI_MoreResults(
				  HSTMT hstmt)
{
	return SQL_NO_DATA_FOUND;
}


#ifdef	DRIVER_CURSOR_IMPLEMENT
/*
 *	Stuff for updatable cursors.
 */
static QResultClass *
positioned_load(StatementClass *stmt, BOOL latest, int res_cols, UInt4 oid, const char *tidval)
{
	int			i;
	QResultClass *qres;
	char		selstr[4096];

	sprintf(selstr, "select");
	for (i = 0; i < res_cols; i++)
		sprintf(selstr, "%s \"%s\",", selstr, stmt->fi[i]->name);
	sprintf(selstr, "%s CTID, OID from \"%s\" where", selstr, stmt->ti[0]->name);
	if (tidval)
	{
		if (latest)
			sprintf(selstr, "%s ctid = currtid2('%s', '%s') and",
					selstr, stmt->ti[0]->name, tidval);
		else
			sprintf(selstr, "%s ctid = '%s' and", selstr, tidval);
	}
	sprintf(selstr, "%s oid = %u", selstr, oid),
		mylog("selstr=%s\n", selstr);
	qres = CC_send_query(SC_get_conn(stmt), selstr, NULL);
	if (qres && QR_aborted(qres))
	{
		QR_Destructor(qres);
		qres = (QResultClass *) 0;
	}
	return qres;
}

RETCODE		SQL_API
SC_pos_reload(StatementClass *stmt, UWORD irow, UWORD *count)
{
	int			i,
				res_cols;
	UWORD		rcnt,
				global_ridx;
	UInt4		oid;
	QResultClass *res,
			   *qres;
	RETCODE		ret = SQL_ERROR;
	char	   *tidval,
			   *oidval;

	mylog("positioned load fi=%x ti=%x\n", stmt->fi, stmt->ti);
	rcnt = 0;
	if (count)
		*count = 0;
	if (!(res = stmt->result))
		return SQL_ERROR;
	if (!stmt->ti)
		parse_statement(stmt);	/* not preferable */
	if (!stmt->ti || stmt->ntab != 1)
	{
		stmt->options.scroll_concurrency = SQL_CONCUR_READ_ONLY;
		return SQL_ERROR;
	}
	global_ridx = irow + stmt->rowset_start;
	res_cols = QR_NumResultCols(res);
	if (!(oidval = QR_get_value_backend_row(res, global_ridx, res_cols - 1)))
		return SQL_SUCCESS_WITH_INFO;
	sscanf(oidval, "%u", &oid);
	tidval = QR_get_value_backend_row(res, global_ridx, res_cols - 2);
	res_cols -= 2;
	if (qres = positioned_load(stmt, TRUE, res_cols, oid, tidval), qres)
	{
		TupleField *tupleo,
				   *tuplen;

		rcnt = QR_get_num_tuples(qres);
		tupleo = res->backend_tuples + res->num_fields * global_ridx;
		if (rcnt == 1)
		{
			QR_set_position(qres, 0);
			tuplen = res->tupleField;
			for (i = 0; i < res->num_fields; i++)
			{
				if (tupleo[i].value)
					free(tupleo[i].value);
				tupleo[i].len = tuplen[i].len;
				tuplen[i].len = 0;
				tupleo[i].value = tuplen[i].value;
				tuplen[i].value = NULL;
			}
			ret = SQL_SUCCESS;
		}
		else
		{
			stmt->errornumber = STMT_ROW_VERSION_CHANGED;
			stmt->errormsg = "the content was deleted after last fetch";
			ret = SQL_SUCCESS_WITH_INFO;
			if (stmt->options.cursor_type == SQL_CURSOR_KEYSET_DRIVEN)
			{
				if (tupleo[res_cols + 1].value)
					free(tupleo[res_cols + 1].value);
				tupleo[res_cols + 1].value = NULL;
				tupleo[res_cols + 1].len = 0;
			}
		}
		QR_Destructor(qres);
	}
	else if (stmt->errornumber == 0)
		stmt->errornumber = STMT_ERROR_TAKEN_FROM_BACKEND;
	if (count)
		*count = rcnt;
	return ret;
}

RETCODE		SQL_API
SC_pos_newload(StatementClass *stmt, UInt4 oid, const char *tidval)
{
	int			i;
	QResultClass *res,
			   *qres;
	RETCODE		ret = SQL_ERROR;

	mylog("positioned new fi=%x ti=%x\n", stmt->fi, stmt->ti);
	if (!(res = stmt->result))
		return SQL_ERROR;
	if (!stmt->ti)
		parse_statement(stmt);	/* not preferable */
	if (!stmt->ti || stmt->ntab != 1)
	{
		stmt->options.scroll_concurrency = SQL_CONCUR_READ_ONLY;
		return SQL_ERROR;
	}
	if (qres = positioned_load(stmt, TRUE, QR_NumResultCols(res) - 2, oid, tidval), qres)
	{
		TupleField *tupleo,
				   *tuplen;
		int			count = QR_get_num_tuples(qres);

		QR_set_position(qres, 0);
		if (count == 1)
		{
			tuplen = qres->tupleField;
			if (res->fcount >= res->count_allocated)
			{
				int			tuple_size;

				if (!res->count_allocated)
					tuple_size = TUPLE_MALLOC_INC;
				else
					tuple_size = res->count_allocated * 2;
				res->backend_tuples = (TupleField *) realloc(
													 res->backend_tuples,
					  res->num_fields * sizeof(TupleField) * tuple_size);
				if (!res->backend_tuples)
				{
					stmt->errornumber = res->status = PGRES_FATAL_ERROR;
					stmt->errormsg = "Out of memory while reading tuples.";
					QR_Destructor(qres);
					return SQL_ERROR;
				}
				res->count_allocated = tuple_size;
			}
			tupleo = res->backend_tuples + res->num_fields * res->fcount;
			for (i = 0; i < res->num_fields; i++)
			{
				tupleo[i].len = tuplen[i].len;
				tuplen[i].len = 0;
				tupleo[i].value = tuplen[i].value;
				tuplen[i].value = NULL;
			}
			res->fcount++;
			ret = SQL_SUCCESS;
		}
		else
		{
			stmt->errornumber = STMT_ROW_VERSION_CHANGED;
			stmt->errormsg = "the content was changed before updation";
			ret = SQL_SUCCESS_WITH_INFO;
		}
		QR_Destructor(qres);
		/* stmt->currTuple = stmt->rowset_start + irow; */
	}
	return ret;
}

RETCODE		SQL_API
SC_pos_update(StatementClass *stmt,
			  UWORD irow)
{
	int			i,
				res_cols,
				num_cols,
				upd_cols;
	UWORD		global_ridx;
	QResultClass *res;
	BindInfoClass *bindings = stmt->bindings;
	char		updstr[4096];
	RETCODE		ret;
	char	   *tidval,
			   *oidval;

	mylog("POS UPDATE %d+%d fi=%x ti=%x\n", irow, stmt->result->base, stmt->fi, stmt->ti);
	if (!(res = stmt->result))
		return SQL_ERROR;
	if (!stmt->ti)
		parse_statement(stmt);	/* not preferable */
	if (!stmt->ti || stmt->ntab != 1)
	{
		stmt->options.scroll_concurrency = SQL_CONCUR_READ_ONLY;
		return SQL_ERROR;
	}
	global_ridx = irow + stmt->rowset_start;
	res_cols = QR_NumResultCols(res);
	if (!(oidval = QR_get_value_backend_row(res, global_ridx, res_cols - 1)))
	{
		stmt->errormsg = "The row is already deleted";
		return SQL_ERROR;
	}
	tidval = QR_get_value_backend_row(res, global_ridx, res_cols - 2);

	sprintf(updstr, "update \"%s\" set", stmt->ti[0]->name);
	num_cols = stmt->nfld;
	for (i = upd_cols = 0; i < num_cols; i++)
	{
		if (bindings[i].used)
		{
			mylog("%d used=%d\n", i, *bindings[i].used);
			if (*bindings[i].used != SQL_IGNORE)
			{
				if (upd_cols)
					sprintf(updstr, "%s, \"%s\" = ?", updstr, stmt->fi[i]->name);
				else
					sprintf(updstr, "%s \"%s\" = ?", updstr, stmt->fi[i]->name);
				upd_cols++;
			}
		}
		else
			mylog("%d null bind\n", i);
	}
	if (upd_cols > 0)
	{
		HSTMT		hstmt;
		int			j;
		int			res_cols = QR_NumResultCols(res);
		StatementClass *qstmt;

		sprintf(updstr, "%s where ctid = '%s' and oid = %s", updstr,
				tidval, oidval);
		mylog("updstr=%s\n", updstr);
		if (PGAPI_AllocStmt(SC_get_conn(stmt), &hstmt) != SQL_SUCCESS)
			return SQL_ERROR;
		qstmt = (StatementClass *) hstmt;
		for (i = j = 0; i < num_cols; i++)
		{
			if (bindings[i].used)
			{
				mylog("%d used=%d\n", i, *bindings[i].used);
				if (*bindings[i].used != SQL_IGNORE)
				{
					PGAPI_BindParameter(hstmt, (SQLUSMALLINT) ++j,
								 SQL_PARAM_INPUT, bindings[i].returntype,
					  pgtype_to_sqltype(stmt, QR_get_field_type(res, i)),
										QR_get_fieldsize(res, i),
									(SQLSMALLINT) stmt->fi[i]->precision,
										bindings[i].buffer,
										bindings[i].buflen,
										bindings[i].used);
				}
			}
		}
		ret = PGAPI_ExecDirect(hstmt, updstr, strlen(updstr));
		if (ret == SQL_ERROR)
		{
			stmt->errornumber = qstmt->errornumber;
			stmt->errormsg = qstmt->errormsg;
		}
		else if (ret == SQL_NEED_DATA)	/* must be fixed */
		{
			stmt->options.scroll_concurrency = SQL_CONCUR_READ_ONLY;
			stmt->errornumber = STMT_INVALID_CURSOR_STATE_ERROR;
			stmt->errormsg = "SetPos with data_at_exec not yet supported";
			ret = SQL_ERROR;
		}
		if (ret != SQL_ERROR)
		{
			int			updcnt;
			const char *cmdstr = QR_get_command(qstmt->result);

			if (cmdstr &&
				sscanf(cmdstr, "UPDATE %d", &updcnt) == 1)
			{
				if (updcnt == 1)
					SC_pos_reload(stmt, irow, (UWORD *) 0);
				else if (updcnt == 0)
				{
					stmt->errornumber = STMT_ROW_VERSION_CHANGED;
					stmt->errormsg = "the content was changed before updation";
					ret = SQL_ERROR;
					if (stmt->options.cursor_type == SQL_CURSOR_KEYSET_DRIVEN)
						SC_pos_reload(stmt, irow, (UWORD *) 0);
				}
				else
					ret = SQL_ERROR;
				stmt->currTuple = stmt->rowset_start + irow;
			}
			else
				ret = SQL_ERROR;
			if (ret == SQL_ERROR && stmt->errornumber == 0)
			{
				stmt->errornumber = STMT_ERROR_TAKEN_FROM_BACKEND;
				stmt->errormsg = "SetPos update return error";
			}
		}
		PGAPI_FreeStmt(hstmt, SQL_DROP);
	}
	else
		ret = SQL_SUCCESS_WITH_INFO;
	return ret;
}
RETCODE		SQL_API
SC_pos_delete(StatementClass *stmt,
			  UWORD irow)
{
	int			res_cols;
	UWORD		global_ridx;
	QResultClass *res,
			   *qres;
	BindInfoClass *bindings = stmt->bindings;
	char		dltstr[4096];
	RETCODE		ret;
	char	   *oidval;

	mylog("POS DELETE fi=%x ti=%x\n", stmt->fi, stmt->ti);
	if (!(res = stmt->result))
		return SQL_ERROR;
	if (!stmt->ti)
		parse_statement(stmt);	/* not preferable */
	if (!stmt->ti || stmt->ntab != 1)
	{
		stmt->options.scroll_concurrency = SQL_CONCUR_READ_ONLY;
		return SQL_ERROR;
	}
	res_cols = QR_NumResultCols(res);
	global_ridx = irow + stmt->rowset_start;
	if (!(oidval = QR_get_value_backend_row(res, global_ridx, res_cols - 1)))
	{
		stmt->errormsg = "The row is already deleted";
		return SQL_ERROR;
	}
	sprintf(dltstr, "delete from \"%s\" where ctid = '%s' and oid = %s",
			stmt->ti[0]->name,
	   QR_get_value_backend_row(stmt->result, global_ridx, res_cols - 2),
			oidval);

	mylog("dltstr=%s\n", dltstr);
	qres = CC_send_query(SC_get_conn(stmt), dltstr, NULL);
	if (qres && QR_command_successful(qres))
	{
		int			dltcnt;
		const char *cmdstr = QR_get_command(qres);

		if (cmdstr &&
			sscanf(cmdstr, "DELETE %d", &dltcnt) == 1)
		{
			if (dltcnt == 1)
				SC_pos_reload(stmt, irow, (UWORD *) 0);
			else if (dltcnt == 0)
			{
				stmt->errornumber = STMT_ROW_VERSION_CHANGED;
				stmt->errormsg = "the content was changed before deletion";
				ret = SQL_ERROR;
				if (stmt->options.cursor_type == SQL_CURSOR_KEYSET_DRIVEN)
					SC_pos_reload(stmt, irow, (UWORD *) 0);
			}
			else
				ret = SQL_ERROR;
			stmt->currTuple = stmt->rowset_start + irow;
		}
		else
			ret = SQL_ERROR;
	}
	else
		ret = SQL_ERROR;
	if (ret == SQL_ERROR && stmt->errornumber == 0)
	{
		stmt->errornumber = STMT_ERROR_TAKEN_FROM_BACKEND;
		stmt->errormsg = "SetPos delete return error";
	}
	if (qres)
		QR_Destructor(qres);
	return ret;
}
RETCODE		SQL_API
SC_pos_add(StatementClass *stmt,
		   UWORD irow)
{
	int			num_cols,
				add_cols,
				i;
	HSTMT		hstmt;
	QResultClass *res;
	BindInfoClass *bindings = stmt->bindings;
	char		addstr[4096];
	RETCODE		ret;

	mylog("POS ADD fi=%x ti=%x\n", stmt->fi, stmt->ti);
	if (!(res = stmt->result))
		return SQL_ERROR;
	if (!stmt->ti)
		parse_statement(stmt);	/* not preferable */
	if (!stmt->ti || stmt->ntab != 1)
	{
		stmt->options.scroll_concurrency = SQL_CONCUR_READ_ONLY;
		return SQL_ERROR;
	}
	num_cols = stmt->nfld;
	sprintf(addstr, "insert into \"%s\" (", stmt->ti[0]->name);
	if (PGAPI_AllocStmt(SC_get_conn(stmt), &hstmt) != SQL_SUCCESS)
		return SQL_ERROR;
	for (i = add_cols = 0; i < num_cols; i++)
	{
		if (bindings[i].used)
		{
			mylog("%d used=%d\n", i, *bindings[i].used);
			if (*bindings[i].used != SQL_IGNORE)
			{
				if (add_cols)
					sprintf(addstr, "%s, \"%s\"", addstr, stmt->fi[i]->name);
				else
					sprintf(addstr, "%s\"%s\"", addstr, stmt->fi[i]->name);
				PGAPI_BindParameter(hstmt, (SQLUSMALLINT) ++add_cols,
								 SQL_PARAM_INPUT, bindings[i].returntype,
					  pgtype_to_sqltype(stmt, QR_get_field_type(res, i)),
									QR_get_fieldsize(res, i),
									(SQLSMALLINT) stmt->fi[i]->precision,
									bindings[i].buffer,
									bindings[i].buflen,
									bindings[i].used);
			}
		}
		else
			mylog("%d null bind\n", i);
	}
	if (add_cols > 0)
	{
		StatementClass *qstmt = (StatementClass *) hstmt;

		sprintf(addstr, "%s) values (", addstr);
		for (i = 0; i < add_cols; i++)
		{
			if (i)
				strcat(addstr, ", ?");
			else
				strcat(addstr, "?");
		}
		strcat(addstr, ")");
		mylog("addstr=%s\n", addstr);
		ret = PGAPI_ExecDirect(hstmt, addstr, strlen(addstr));
		if (ret == SQL_NEED_DATA)		/* must be fixed */
		{
			stmt->options.scroll_concurrency = SQL_CONCUR_READ_ONLY;
			stmt->errornumber = STMT_INVALID_CURSOR_STATE_ERROR;
			stmt->errormsg = "SetPos with data_at_exec not yet supported";
			ret = SQL_ERROR;
		}
		if (ret == SQL_ERROR)
		{
			stmt->errornumber = qstmt->errornumber;
			stmt->errormsg = qstmt->errormsg;
		}
		else
		{
			int			addcnt;
			UInt4		oid;
			const char *cmdstr = QR_get_command(qstmt->result);

			if (cmdstr &&
				sscanf(cmdstr, "INSERT %u %d", &oid, &addcnt) == 2 &&
				addcnt == 1)
			{
				SC_pos_newload(stmt, oid, NULL);
				if (stmt->bookmark.buffer)
				{
					char		buf[32];

					sprintf(buf, "%ld", res->fcount);
					copy_and_convert_field(stmt, 0, buf,
									  SQL_C_ULONG, stmt->bookmark.buffer,
										   0, stmt->bookmark.used);
				}
			}
			else
			{
				stmt->errornumber = STMT_ERROR_TAKEN_FROM_BACKEND;
				stmt->errormsg = "SetPos insert return error";
				ret = SQL_ERROR;
			}
		}
	}
	else
		ret = SQL_SUCCESS_WITH_INFO;
	PGAPI_FreeStmt(hstmt, SQL_DROP);
	return ret;
}

/*
 *	Stuff for updatable cursors end.
 */
#endif   /* DRIVER_CURSOR_IMPLEMENT */

/*
 *	This positions the cursor within a rowset, that was positioned using SQLExtendedFetch.
 *	This will be useful (so far) only when using SQLGetData after SQLExtendedFetch.
 */
RETCODE		SQL_API
PGAPI_SetPos(
			 HSTMT hstmt,
			 UWORD irow,
			 UWORD fOption,
			 UWORD fLock)
{
	static char *func = "PGAPI_SetPos";
	StatementClass *stmt = (StatementClass *) hstmt;
	QResultClass *res;
	int			num_cols,
				i;
	BindInfoClass *bindings = stmt->bindings;

	if (!stmt)
	{
		SC_log_error(func, "", NULL);
		return SQL_INVALID_HANDLE;
	}

#ifdef	DRIVER_CURSOR_IMPLEMENT
	mylog("SetPos fOption=%d irow=%d lock=%d currt=%d\n", fOption, irow, fLock, stmt->currTuple);
	if (stmt->options.scroll_concurrency != SQL_CONCUR_READ_ONLY)
		;
	else
#endif   /* DRIVER_CURSOR_IMPLEMENT */
	if (fOption != SQL_POSITION && fOption != SQL_REFRESH)
	{
		stmt->errornumber = STMT_NOT_IMPLEMENTED_ERROR;
		stmt->errormsg = "Only SQL_POSITION/REFRESH is supported for PGAPI_SetPos";
		SC_log_error(func, "", stmt);
		return SQL_ERROR;
	}

	if (!(res = stmt->result))
	{
		stmt->errormsg = "Null statement result in PGAPI_SetPos.";
		stmt->errornumber = STMT_SEQUENCE_ERROR;
		SC_log_error(func, "", stmt);
		return SQL_ERROR;
	}
	num_cols = QR_NumResultCols(res);

	if (irow == 0)
	{
		stmt->errornumber = STMT_ROW_OUT_OF_RANGE;
		stmt->errormsg = "Driver does not support Bulk operations.";
		SC_log_error(func, "", stmt);
		return SQL_ERROR;
	}

	if (irow > stmt->last_fetch_count)
	{
		stmt->errornumber = STMT_ROW_OUT_OF_RANGE;
		stmt->errormsg = "Row value out of range";
		SC_log_error(func, "", stmt);
		return SQL_ERROR;
	}

	irow--;

#ifdef	DRIVER_CURSOR_IMPLEMENT
	switch (fOption)
	{
		case SQL_UPDATE:
			return SC_pos_update(stmt, irow);
		case SQL_DELETE:
			return SC_pos_delete(stmt, irow);
		case SQL_ADD:
			return SC_pos_add(stmt, irow);
	}
#endif   /* DRIVER_CURSOR_IMPLEMENT */
	/* Reset for SQLGetData */
	for (i = 0; i < num_cols; i++)
		bindings[i].data_left = -1;

	if (fOption == SQL_REFRESH)
	{
		/* save the last_fetch_count */
		int			last_fetch = stmt->last_fetch_count;
		int			bind_save = stmt->bind_row;

#ifdef	DRIVER_CURSOR_IMPLEMENT
		if (stmt->options.cursor_type == SQL_CURSOR_KEYSET_DRIVEN)
			SC_pos_reload(stmt, irow, (UWORD *) 0);
#endif   /* DRIVER_CURSOR_IMPLEMENT */
		stmt->currTuple = stmt->rowset_start + irow - 1;
		stmt->bind_row = irow;
		SC_fetch(stmt);
		/* restore the last_fetch_count */
		stmt->last_fetch_count = last_fetch;
		stmt->bind_row = bind_save;
	}
	else
		stmt->currTuple = stmt->rowset_start + irow;
	QR_set_position(res, irow);

	return SQL_SUCCESS;
}


/*		Sets options that control the behavior of cursors. */
RETCODE		SQL_API
PGAPI_SetScrollOptions(
					   HSTMT hstmt,
					   UWORD fConcurrency,
					   SDWORD crowKeyset,
					   UWORD crowRowset)
{
	static char *func = "PGAPI_SetScrollOptions";
	StatementClass *stmt = (StatementClass *) hstmt;

	mylog("PGAPI_SetScrollOptions fConcurrency=%d crowKeyset=%d crowRowset=%d\n",
		  fConcurrency, crowKeyset, crowRowset);
	stmt->errornumber = STMT_NOT_IMPLEMENTED_ERROR;
	stmt->errormsg = "SetScroll option not implemeted";

	SC_log_error(func, "Function not implemented", hstmt);
	return SQL_ERROR;
}


/*	Set the cursor name on a statement handle */
RETCODE		SQL_API
PGAPI_SetCursorName(
					HSTMT hstmt,
					UCHAR FAR * szCursor,
					SWORD cbCursor)
{
	static char *func = "PGAPI_SetCursorName";
	StatementClass *stmt = (StatementClass *) hstmt;
	int			len;

	mylog("PGAPI_SetCursorName: hstmt=%u, szCursor=%u, cbCursorMax=%d\n", hstmt, szCursor, cbCursor);

	if (!stmt)
	{
		SC_log_error(func, "", NULL);
		return SQL_INVALID_HANDLE;
	}

	len = (cbCursor == SQL_NTS) ? strlen(szCursor) : cbCursor;

	if (len <= 0 || len > sizeof(stmt->cursor_name) - 1)
	{
		stmt->errornumber = STMT_INVALID_CURSOR_NAME;
		stmt->errormsg = "Invalid Cursor Name";
		SC_log_error(func, "", stmt);
		return SQL_ERROR;
	}

	strncpy_null(stmt->cursor_name, szCursor, len + 1);
	return SQL_SUCCESS;
}


/*	Return the cursor name for a statement handle */
RETCODE		SQL_API
PGAPI_GetCursorName(
					HSTMT hstmt,
					UCHAR FAR * szCursor,
					SWORD cbCursorMax,
					SWORD FAR * pcbCursor)
{
	static char *func = "PGAPI_GetCursorName";
	StatementClass *stmt = (StatementClass *) hstmt;
	int			len = 0;
	RETCODE		result;

	mylog("PGAPI_GetCursorName: hstmt=%u, szCursor=%u, cbCursorMax=%d, pcbCursor=%u\n", hstmt, szCursor, cbCursorMax, pcbCursor);

	if (!stmt)
	{
		SC_log_error(func, "", NULL);
		return SQL_INVALID_HANDLE;
	}

	if (stmt->cursor_name[0] == '\0')
	{
		stmt->errornumber = STMT_NO_CURSOR_NAME;
		stmt->errormsg = "No Cursor name available";
		SC_log_error(func, "", stmt);
		return SQL_ERROR;
	}

	result = SQL_SUCCESS;
	len = strlen(stmt->cursor_name);

	if (szCursor)
	{
		strncpy_null(szCursor, stmt->cursor_name, cbCursorMax);

		if (len >= cbCursorMax)
		{
			result = SQL_SUCCESS_WITH_INFO;
			stmt->errornumber = STMT_TRUNCATED;
			stmt->errormsg = "The buffer was too small for the GetCursorName.";
		}
	}

	if (pcbCursor)
		*pcbCursor = len;

	return result;
}
