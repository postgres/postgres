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
 *					SQLMoreResults, SQLSetPos, SQLSetScrollOptions(NI),
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

	mylog("%s: entering...\n", func);
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
			res = SC_get_Curres(stmt);

			if (res && pcrow)
			{
				*pcrow = SC_is_fetchcursor(stmt) ? -1 : QR_get_num_tuples(res);
				return SQL_SUCCESS;
			}
		}
	}
	else
	{
		res = SC_get_Curres(stmt);
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

	mylog("%s: entering...\n", func);
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
			*pccol = SC_get_IRD(stmt)->nfields;
			mylog("PARSE: PGAPI_NumResultCols: *pccol = %d\n", *pccol);
		}
	}

	if (!parse_ok)
	{
		SC_pre_execute(stmt);
		result = SC_get_Curres(stmt);

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
		if (result->keyset)
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
	ConnectionClass *conn;
	IRDFields	*irdflds;
	QResultClass *res;
	char	   *col_name = NULL;
	Int4		fieldtype = 0;
	int			column_size = 0,
				decimal_digits = 0;
	ConnInfo   *ci;
	char		parse_ok;
	char		buf[255];
	int			len = 0;
	RETCODE		result;

	mylog("%s: entering.%d..\n", func, icol);

	if (!stmt)
	{
		SC_log_error(func, "", NULL);
		return SQL_INVALID_HANDLE;
	}

	conn = SC_get_conn(stmt);
	ci = &(conn->connInfo);

	SC_clear_error(stmt);

	irdflds = SC_get_IRD(stmt);
#if (ODBCVER >= 0x0300)
	if (0 == icol) /* bookmark column */
	{
		SQLSMALLINT	fType = stmt->options.use_bookmarks == SQL_UB_VARIABLE ? SQL_BINARY : SQL_INTEGER;

		if (szColName && cbColNameMax > 0)
			*szColName = '\0';
		if (pcbColName)
			*pcbColName = 0;
		if (pfSqlType)
			*pfSqlType = fType;
		if (pcbColDef)
			*pcbColDef = 10;
		if (pibScale)
			*pibScale = 0;
		if (pfNullable)
			*pfNullable = SQL_NO_NULLS;
		return SQL_SUCCESS;
	}
#endif /* ODBCVER */
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

		mylog("PARSE: DescribeCol: icol=%d, stmt=%u, stmt->nfld=%d, stmt->fi=%u\n", icol, stmt, irdflds->nfields, irdflds->fi);

		if (stmt->parse_status != STMT_PARSE_FATAL && irdflds->fi && irdflds->fi[icol])
		{
			if (icol >= irdflds->nfields)
			{
				stmt->errornumber = STMT_INVALID_COLUMN_NUMBER_ERROR;
				stmt->errormsg = "Invalid column number in DescribeCol.";
				SC_log_error(func, "", stmt);
				return SQL_ERROR;
			}
			mylog("DescribeCol: getting info for icol=%d\n", icol);

			fieldtype = irdflds->fi[icol]->type;
			if (irdflds->fi[icol]->alias[0])
				col_name = irdflds->fi[icol]->alias;
			else
				col_name = irdflds->fi[icol]->name;
			column_size = irdflds->fi[icol]->column_size;
			decimal_digits = irdflds->fi[icol]->decimal_digits;

			mylog("PARSE: fieldtype=%d, col_name='%s', column_size=%d\n", fieldtype, col_name, column_size);
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

		res = SC_get_Curres(stmt);

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
		column_size = pgtype_column_size(stmt, fieldtype, icol, ci->drivers.unknown_sizes);
		decimal_digits = pgtype_decimal_digits(stmt, fieldtype, icol);
	}

	mylog("describeCol: col %d fieldname = '%s'\n", icol, col_name);
	mylog("describeCol: col %d fieldtype = %d\n", icol, fieldtype);
	mylog("describeCol: col %d column_size = %d\n", icol, column_size);

	result = SQL_SUCCESS;

	/*
	 * COLUMN NAME
	 */
	len = strlen(col_name);

	if (pcbColName)
		*pcbColName = len;

	if (szColName && cbColNameMax > 0)
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
	 * CONCISE(SQL) TYPE
	 */
	if (pfSqlType)
	{
		*pfSqlType = pgtype_to_concise_type(stmt, fieldtype);

		mylog("describeCol: col %d *pfSqlType = %d\n", icol, *pfSqlType);
	}

	/*
	 * COLUMN SIZE(PRECISION in 2.x)
	 */
	if (pcbColDef)
	{
		if (column_size < 0)
			column_size = 0;		/* "I dont know" */

		*pcbColDef = column_size;

		mylog("describeCol: col %d  *pcbColDef = %d\n", icol, *pcbColDef);
	}

	/*
	 * DECIMAL DIGITS(SCALE in 2.x)
	 */
	if (pibScale)
	{
		if (decimal_digits < 0)
			decimal_digits = 0;

		*pibScale = decimal_digits;
		mylog("describeCol: col %d  *pibScale = %d\n", icol, *pibScale);
	}

	/*
	 * NULLABILITY
	 */
	if (pfNullable)
	{
		*pfNullable = (parse_ok) ? irdflds->fi[icol]->nullable : pgtype_nullable(stmt, fieldtype);

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
	IRDFields	*irdflds;
	Int4		col_idx, field_type = 0;
	ConnectionClass	*conn;
	ConnInfo	*ci;
	int			unknown_sizes;
	int			cols = 0;
	char		parse_ok;
	RETCODE		result;
	const char   *p = NULL;
	int			len = 0,
				value = 0;
	const	FIELD_INFO	*fi = NULL;

	mylog("%s: entering..col=%d %d len=%d.\n", func, icol, fDescType,
				cbDescMax);

	if (!stmt)
	{
		SC_log_error(func, "", NULL);
		return SQL_INVALID_HANDLE;
	}

	if (pcbDesc)
		*pcbDesc = 0;
	irdflds = SC_get_IRD(stmt);
	conn = SC_get_conn(stmt);
	ci = &(conn->connInfo);

	/*
	 * Dont check for bookmark column.	This is the responsibility of the
	 * driver manager.	For certain types of arguments, the column number
	 * is ignored anyway, so it may be 0.
	 */

#if (ODBCVER >= 0x0300)
	if (0 == icol && SQL_DESC_COUNT != fDescType) /* bookmark column */
	{
		switch (fDescType)
		{
			case SQL_DESC_OCTET_LENGTH:
				if (pfDesc)
					*pfDesc = 4;
				break;
			case SQL_DESC_TYPE:
				if (pfDesc)
					*pfDesc = stmt->options.use_bookmarks == SQL_UB_VARIABLE ? SQL_BINARY : SQL_INTEGER;
				break;
		}
		return SQL_SUCCESS;
	}
#endif /* ODBCVER */
	col_idx = icol - 1;

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

		cols = irdflds->nfields;

		/*
		 * Column Count is a special case.	The Column number is ignored
		 * in this case.
		 */
#if (ODBCVER >= 0x0300)
		if (fDescType == SQL_DESC_COUNT)
#else
		if (fDescType == SQL_COLUMN_COUNT)
#endif /* ODBCVER */
		{
			if (pfDesc)
				*pfDesc = cols;

			return SQL_SUCCESS;
		}

		if (stmt->parse_status != STMT_PARSE_FATAL && irdflds->fi)
		{
			if (col_idx >= cols)
			{
				stmt->errornumber = STMT_INVALID_COLUMN_NUMBER_ERROR;
				stmt->errormsg = "Invalid column number in ColAttributes.";
				SC_log_error(func, "", stmt);
				return SQL_ERROR;
			}
			if (irdflds->fi[col_idx])
			{
				field_type = irdflds->fi[col_idx]->type;
				if (field_type > 0)
					parse_ok = TRUE;
			}
		}
	}

	if (parse_ok)
		fi = irdflds->fi[col_idx];
	else
	{
		SC_pre_execute(stmt);

		mylog("**** PGAPI_ColAtt: result = %u, status = %d, numcols = %d\n", SC_get_Curres(stmt), stmt->status, SC_get_Curres(stmt) != NULL ? QR_NumResultCols(SC_get_Curres(stmt)) : -1);

		if ((NULL == SC_get_Curres(stmt)) || ((stmt->status != STMT_FINISHED) && (stmt->status != STMT_PREMATURE)))
		{
			stmt->errormsg = "Can't get column attributes: no result found.";
			stmt->errornumber = STMT_SEQUENCE_ERROR;
			SC_log_error(func, "", stmt);
			return SQL_ERROR;
		}

		cols = QR_NumResultCols(SC_get_Curres(stmt));

		/*
		 * Column Count is a special case.	The Column number is ignored
		 * in this case.
		 */
#if (ODBCVER >= 0x0300)
		if (fDescType == SQL_DESC_COUNT)
#else
		if (fDescType == SQL_COLUMN_COUNT)
#endif /* ODBCVER */
		{
			if (pfDesc)
				*pfDesc = cols;

			return SQL_SUCCESS;
		}

		if (col_idx >= cols)
		{
			stmt->errornumber = STMT_INVALID_COLUMN_NUMBER_ERROR;
			stmt->errormsg = "Invalid column number in ColAttributes.";
			SC_log_error(func, "", stmt);
			return SQL_ERROR;
		}

		field_type = QR_get_field_type(SC_get_Curres(stmt), col_idx);
		if (stmt->parse_status != STMT_PARSE_FATAL && irdflds->fi && irdflds->fi[col_idx])
			fi = irdflds->fi[col_idx];
	}

	mylog("colAttr: col %d field_type = %d\n", col_idx, field_type);

	switch (fDescType)
	{
		case SQL_COLUMN_AUTO_INCREMENT: /* == SQL_DESC_AUTO_UNIQUE_VALUE */
			value = pgtype_auto_increment(stmt, field_type);
			if (value == -1)	/* non-numeric becomes FALSE (ODBC Doc) */
				value = FALSE;
inolog("AUTO_INCREMENT=%d\n", value);

			break;

		case SQL_COLUMN_CASE_SENSITIVE: /* == SQL_DESC_CASE_SENSITIVE */
			value = pgtype_case_sensitive(stmt, field_type);
			break;

			/*
			 * This special case is handled above.
			 *
			 * case SQL_COLUMN_COUNT:
			 */
		case SQL_COLUMN_DISPLAY_SIZE: /* == SQL_DESC_DISPLAY_SIZE */
			value = fi ? fi->display_size : pgtype_display_size(stmt, field_type, col_idx, unknown_sizes);

			mylog("PGAPI_ColAttributes: col %d, display_size= %d\n", col_idx, value);

			break;

		case SQL_COLUMN_LABEL: /* == SQL_DESC_LABEL */
			if (fi && fi->alias[0] != '\0')
			{
				p = fi->alias;

				mylog("PGAPI_ColAttr: COLUMN_LABEL = '%s'\n", p);
				break;

			}
			/* otherwise same as column name -- FALL THROUGH!!! */

#if (ODBCVER >= 0x0300)
		case SQL_DESC_NAME:
#else
		case SQL_COLUMN_NAME:
#endif /* ODBCVER */
			p = fi ? (fi->alias[0] ? fi->alias : fi->name) : QR_get_fieldname(SC_get_Curres(stmt), col_idx);

			mylog("PGAPI_ColAttr: COLUMN_NAME = '%s'\n", p);
			break;

		case SQL_COLUMN_LENGTH:
			value = (fi && fi->length > 0) ? fi->length : pgtype_buffer_length(stmt, field_type, col_idx, unknown_sizes);
			if (value < 0)
				value = 0;

			mylog("PGAPI_ColAttributes: col %d, length = %d\n", col_idx, value);
			break;

		case SQL_COLUMN_MONEY: /* == SQL_DESC_FIXED_PREC_SCALE */
			value = pgtype_money(stmt, field_type);
inolog("COLUMN_MONEY=%d\n", value);
			break;

#if (ODBCVER >= 0x0300)
		case SQL_DESC_NULLABLE:
#else
		case SQL_COLUMN_NULLABLE:
#endif /* ODBCVER */
			value = fi ? fi->nullable : pgtype_nullable(stmt, field_type);
inolog("COLUMN_NULLABLE=%d\n", value);
			break;

		case SQL_COLUMN_OWNER_NAME: /* == SQL_DESC_SCHEMA_NAME */
			p = fi && (fi->ti) ? fi->ti->schema : "";
			break;

		case SQL_COLUMN_PRECISION: /* in 2.x */
			value = (fi && fi->column_size > 0) ? fi->column_size : pgtype_column_size(stmt, field_type, col_idx, unknown_sizes);
			if (value < 0)
				value = 0;

			mylog("PGAPI_ColAttributes: col %d, column_size = %d\n", col_idx, value);
			break;

		case SQL_COLUMN_QUALIFIER_NAME: /* == SQL_DESC_CATALOG_NAME */
			p = "";
			break;

		case SQL_COLUMN_SCALE: /* in 2.x */
			value = pgtype_decimal_digits(stmt, field_type, col_idx);
inolog("COLUMN_SCALE=%d\n", value);
			if (value < 0)
				value = 0;
			break;

		case SQL_COLUMN_SEARCHABLE: /* SQL_DESC_SEARCHABLE */
			value = pgtype_searchable(stmt, field_type);
			break;

		case SQL_COLUMN_TABLE_NAME: /* == SQL_DESC_TABLE_NAME */
			p = fi && (fi->ti) ? fi->ti->name : "";

			mylog("PGAPI_ColAttr: TABLE_NAME = '%s'\n", p);
			break;

		case SQL_COLUMN_TYPE: /* == SQL_DESC_CONCISE_TYPE */
			value = pgtype_to_concise_type(stmt, field_type);
inolog("COLUMN_TYPE=%d\n", value);
			break;

		case SQL_COLUMN_TYPE_NAME: /* == SQL_DESC_TYPE_NAME */
			p = pgtype_to_name(stmt, field_type);
			break;

		case SQL_COLUMN_UNSIGNED: /* == SQL_DESC_UNSINGED */
			value = pgtype_unsigned(stmt, field_type);
			if (value == -1)	/* non-numeric becomes TRUE (ODBC Doc) */
				value = TRUE;

			break;

		case SQL_COLUMN_UPDATABLE: /* == SQL_DESC_UPDATABLE */

			/*
			 * Neither Access or Borland care about this.
			 *
			 * if (field_type == PG_TYPE_OID) pfDesc = SQL_ATTR_READONLY;
			 * else
			 */
			value = fi ? (fi->updatable ? SQL_ATTR_WRITE : SQL_ATTR_READONLY) : SQL_ATTR_READWRITE_UNKNOWN;

			mylog("PGAPI_ColAttr: UPDATEABLE = %d\n", value);
			break;
#if (ODBCVER >= 0x0300)
		case SQL_DESC_BASE_COLUMN_NAME:

			p = fi ? fi->name : QR_get_fieldname(SC_get_Curres(stmt), col_idx);

			mylog("PGAPI_ColAttr: BASE_COLUMN_NAME = '%s'\n", p);
			break;
		case SQL_DESC_BASE_TABLE_NAME: /* the same as TABLE_NAME ok ? */
			p = (fi && (fi->ti)) ? fi->ti->name : "";

			mylog("PGAPI_ColAttr: BASE_TABLE_NAME = '%s'\n", p);
			break;
		case SQL_DESC_LENGTH: /* different from SQL_COLUMN_LENGTH */
			value = (fi && fi->length > 0) ? fi->length : pgtype_desclength(stmt, field_type, col_idx, unknown_sizes);
			if (value < 0)
				value = 0;

			mylog("PGAPI_ColAttributes: col %d, length = %d\n", col_idx, value);
			break;
		case SQL_DESC_OCTET_LENGTH:
			value = (fi && fi->length > 0) ? fi->length : pgtype_transfer_octet_length(stmt, field_type, col_idx, unknown_sizes);
			if (value < 0)
				value = 0;
			mylog("PGAPI_ColAttributes: col %d, octet_length = %d\n", col_idx, value);
			break;
		case SQL_DESC_PRECISION: /* different from SQL_COLUMN_PRECISION */
			value = (fi && fi->column_size > 0) ? fi->column_size : pgtype_precision(stmt, field_type, col_idx, unknown_sizes);
			if (value < 0)
				value = 0;

			mylog("PGAPI_ColAttributes: col %d, desc_precision = %d\n", col_idx, value);
			break;
		case SQL_DESC_SCALE: /* different from SQL_COLUMN_SCALE */
			value = pgtype_scale(stmt, field_type, col_idx);
			if (value < 0)
				value = 0;
			break;
		case SQL_DESC_LOCAL_TYPE_NAME:
			p = pgtype_to_name(stmt, field_type);
			break;
		case SQL_DESC_TYPE:
			value = pgtype_to_sqldesctype(stmt, field_type);
			break;
		case SQL_DESC_NUM_PREC_RADIX:
			value = pgtype_radix(stmt, field_type);
			break;
		case SQL_DESC_LITERAL_PREFIX:
			p = pgtype_literal_prefix(stmt, field_type);
			break;
		case SQL_DESC_LITERAL_SUFFIX:
			p = pgtype_literal_suffix(stmt, field_type);
			break;
		case SQL_DESC_UNNAMED:
			value = (fi && !fi->name[0] && !fi->alias[0]) ? SQL_UNNAMED : SQL_NAMED;
			break;
#endif /* ODBCVER */
		case 1212:
			stmt->errornumber = STMT_OPTION_NOT_FOR_THE_DRIVER;
			stmt->errormsg = "this request may be for MS SQL Server";
			return SQL_ERROR;
		default:
			stmt->errornumber = STMT_INVALID_OPTION_IDENTIFIER;
			stmt->errormsg = "ColAttribute for this type not implemented yet";
			SC_log_error(func, "", stmt);
			return SQL_ERROR;
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
	res = SC_get_Curres(stmt);

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
		switch (fCType)
		{
			case SQL_C_BOOKMARK:
#if (ODBCVER >= 0x0300)
			case SQL_C_VARBOOKMARK:
#endif /* ODBCVER */
				break;
			default:
				stmt->errormsg = "Column 0 is not of type SQL_C_BOOKMARK";
inolog("Column 0 is type %d not of type SQL_C_BOOKMARK", fCType);
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
	ARDFields	*opts;
	QResultClass *res;

	mylog("PGAPI_Fetch: stmt = %u, stmt->result= %u\n", stmt, SC_get_Curres(stmt));

	if (!stmt)
	{
		SC_log_error(func, "", NULL);
		return SQL_INVALID_HANDLE;
	}

	SC_clear_error(stmt);

	if (!(res = SC_get_Curres(stmt)))
	{
		stmt->errormsg = "Null statement result in PGAPI_Fetch.";
		stmt->errornumber = STMT_SEQUENCE_ERROR;
		SC_log_error(func, "", stmt);
		return SQL_ERROR;
	}

	/* Not allowed to bind a bookmark column when using SQLFetch. */
	opts = SC_get_ARD(stmt);
	if (opts->bookmark->buffer)
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

	if (opts->bindings == NULL)
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
	ARDFields	*opts;
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

	if (!(res = SC_get_Curres(stmt)))
	{
		stmt->errormsg = "Null statement result in PGAPI_ExtendedFetch.";
		stmt->errornumber = STMT_SEQUENCE_ERROR;
		SC_log_error(func, "", stmt);
		return SQL_ERROR;
	}

	opts = SC_get_ARD(stmt);
	/*
	 * If a bookmark colunmn is bound but bookmark usage is off, then
	 * error
	 */
	if (opts->bookmark->buffer && stmt->options.use_bookmarks == SQL_UB_OFF)
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

	if (opts->bindings == NULL)
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
		for (i = 0; i < opts->rowset_size; i++)
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
				stmt->rowset_start += (save_rowset_size > 0 ? save_rowset_size : opts->rowset_size);

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
				if (opts->rowset_size > num_tuples)
				{
					stmt->errornumber = STMT_POS_BEFORE_RECORDSET;
					stmt->errormsg = "fetch prior from eof and before the beggining";
				}
				stmt->rowset_start = num_tuples <= 0 ? 0 : (num_tuples - opts->rowset_size);

			}
			else
			{
				if (stmt->rowset_start < opts->rowset_size)
				{
					stmt->errormsg = "fetch prior and before the beggining";
					stmt->errornumber = STMT_POS_BEFORE_RECORDSET;
				}
				stmt->rowset_start -= opts->rowset_size;
			}
			break;

		case SQL_FETCH_FIRST:
			mylog("SQL_FETCH_FIRST: num_tuples=%d, currtuple=%d\n", num_tuples, stmt->currTuple);

			stmt->rowset_start = 0;
			break;

		case SQL_FETCH_LAST:
			mylog("SQL_FETCH_LAST: num_tuples=%d, currtuple=%d\n", num_tuples, stmt->currTuple);

			stmt->rowset_start = num_tuples <= 0 ? 0 : (num_tuples - opts->rowset_size);
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
		if (stmt->rowset_start + opts->rowset_size <= 0)
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
	QR_set_rowset_size(res, opts->rowset_size);
	if (SC_is_fetchcursor(stmt))
		QR_inc_base(res, stmt->last_fetch_count);
	else
		res->base = stmt->rowset_start;

	/* Physical Row advancement occurs for each row fetched below */

	mylog("PGAPI_ExtendedFetch: new currTuple = %d\n", stmt->currTuple);

	truncated = error = FALSE;
	for (i = 0; i < opts->rowset_size; i++)
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
			else if (res->keyset)
			{
				DWORD	currp = stmt->rowset_start + i;
				UWORD	pstatus = res->keyset[currp].status & KEYSET_INFO_PUBLIC;
				if (pstatus != 0)
				{
					rgfRowStatus[i] = pstatus;
					/* refresh the status */
					if (SQL_ROW_DELETED != pstatus)
						res->keyset[currp].status &= (~KEYSET_INFO_PUBLIC);
				}
				else
					rgfRowStatus[i] = SQL_ROW_SUCCESS;
			}
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
	const char *func = "PGAPI_MoreResults";
	StatementClass	*stmt = (StatementClass *) hstmt;
	QResultClass	*res;

	mylog("%s: entering...\n", func);
	if (stmt && (res = SC_get_Curres(stmt)))
		SC_set_Curres(stmt, res->next);
	if (SC_get_Curres(stmt))
		return SQL_SUCCESS; 
	return SQL_NO_DATA_FOUND;
}


#ifdef	DRIVER_CURSOR_IMPLEMENT
/*
 *	Stuff for updatable cursors.
 */
static Int2	getNumResultCols(const QResultClass *res)
{
	Int2	res_cols = QR_NumResultCols(res);
	return res->keyset ? res_cols - 2 : res_cols;
}
static UInt4	getOid(const QResultClass *res, int index)
{
	return res->keyset[index].oid;
}
static void getTid(const QResultClass *res, int index, UInt4 *blocknum, UInt2 *offset)
{
	*blocknum = res->keyset[index].blocknum;
	*offset = res->keyset[index].offset;
}
static void KeySetSet(const TupleField *tuple, int num_fields, KeySet *keyset)
{
	sscanf(tuple[num_fields - 2].value, "(%u,%hu)",
			&keyset->blocknum, &keyset->offset);
	sscanf(tuple[num_fields - 1].value, "%u", &keyset->oid);
}

static void AddRollback(ConnectionClass *conn, QResultClass *res, int index, const KeySet *keyset)
{
	Rollback *rollback;

	if (!res->rollback)
	{
		res->rb_count = 0;
		res->rb_alloc = 10;
		rollback = res->rollback = malloc(sizeof(Rollback) * res->rb_alloc);
	}
	else
	{
		if (res->rb_count >= res->rb_alloc)
		{
			res->rb_alloc *= 2; 
			if (rollback = realloc(res->rollback, sizeof(Rollback) * res->rb_alloc), !rollback)
			{
				res->rb_alloc = res->rb_count = 0;
				return;
			}
			res->rollback = rollback; 
		}
		rollback = res->rollback + res->rb_count;
	}
	rollback->index = index;
	if (keyset)
	{
		rollback->blocknum = keyset[index].blocknum;
		rollback->offset = keyset[index].offset;
	}
	else
	{
		rollback->offset = 0;
		rollback->blocknum = 0;
	}

	conn->result_uncommitted = 1;
	res->rb_count++;	
}

static void DiscardRollback(QResultClass *res)
{
	int	i, index;
	UWORD	status;
	Rollback *rollback;
	KeySet	*keyset;

	if (0 == res->rb_count || NULL == res->rollback)
		return;
	rollback = res->rollback;
	keyset = res->keyset;
	for (i = 0; i < res->rb_count; i++)
	{
		index = rollback[i].index;
		status = keyset[index].status;
		keyset[index].status &= ~(CURS_SELF_DELETING | CURS_SELF_UPDATING | CURS_SELF_ADDING);
		keyset[index].status |= ((status & (CURS_SELF_DELETING | CURS_SELF_UPDATING | CURS_SELF_ADDING)) << 3);
	}
	free(rollback);
	res->rollback = NULL;
	res->rb_count = res->rb_alloc = 0;
}

static void UndoRollback(QResultClass *res)
{
	int	i, index;
	UWORD	status;
	Rollback *rollback;
	KeySet	*keyset;

	if (0 == res->rb_count || NULL == res->rollback)
		return;
	rollback = res->rollback;
	keyset = res->keyset;
	for (i = res->rb_count - 1; i >= 0; i--)
	{
		index = rollback[i].index;
		status = keyset[index].status;
		if ((status & CURS_SELF_ADDING) != 0)
		{
			if (index < res->fcount)
				res->fcount = index;
		}
		else
		{
			keyset[index].status &= ~(CURS_SELF_DELETING | CURS_SELF_UPDATING | CURS_SELF_ADDING | KEYSET_INFO_PUBLIC);
			keyset[index].blocknum = rollback[i].blocknum;
			keyset[index].offset = rollback[i].offset;
		}
	}
	free(rollback);
	res->rollback = NULL;
	res->rb_count = res->rb_alloc = 0;
}

void	ProcessRollback(ConnectionClass *conn, BOOL undo) 
{
	int	i;
	StatementClass	*stmt;
	QResultClass	*res;

	for (i = 0; i < conn->num_stmts; i++)
	{
		if (stmt = conn->stmts[i], !stmt)
			continue;
		for (res = SC_get_Result(stmt); res; res = res->next)
		{
			if (undo)
				UndoRollback(res);
			else
				DiscardRollback(res);
		}
	}
}

#define	LATEST_TUPLE_LOAD	1L
#define	USE_INSERTED_TID	(1L << 1)
static QResultClass *
positioned_load(StatementClass *stmt, UInt4 flag, UInt4 oid, const char *tidval)
{
	QResultClass *qres;
	char	*selstr;
	BOOL	latest = ((flag & LATEST_TUPLE_LOAD) != 0);
	UInt4	len;

	len = strlen(stmt->load_statement);
	if (tidval)
	{
		len += 100;
		selstr = malloc(len);
		if (latest)
			sprintf(selstr, "%s where ctid = currtid2('%s', '%s') and oid  = %u", stmt->load_statement, stmt->ti[0]->name, tidval, oid);
		else 
			sprintf(selstr, "%s where ctid = '%s' and oid = %u", stmt->load_statement, tidval, oid); 
	}
	else if ((flag & USE_INSERTED_TID) != 0)
	{
		len += 50;
		selstr = malloc(len);
		sprintf(selstr, "%s where ctid = currtid(0, '(,)') and oid = %u", stmt->load_statement, oid);
	} 
	else
	{
		len += 20;
		selstr = malloc(len);
		sprintf(selstr, "%s where oid = %u", stmt->load_statement, oid);
	} 

	mylog("selstr=%s\n", selstr);
	qres = CC_send_query(SC_get_conn(stmt), selstr, NULL, CLEAR_RESULT_ON_ABORT);
	free(selstr);
	return qres;
}

RETCODE		SQL_API
SC_pos_reload(StatementClass *stmt, UWORD irow, UDWORD global_ridx, UWORD *count)
{
	int			i,
				res_cols;
	UWORD		rcnt, offset;
	UInt4		oid, blocknum;
	QResultClass *res,
			   *qres;
	IRDFields	*irdflds = SC_get_IRD(stmt);
	RETCODE		ret = SQL_ERROR;
	char		tidval[32];

	mylog("positioned load fi=%x ti=%x\n", irdflds->fi, stmt->ti);
	rcnt = 0;
	if (count)
		*count = 0;
	if (!(res = SC_get_Curres(stmt)))
		return SQL_ERROR;
	if (!stmt->ti)
		parse_statement(stmt);	/* not preferable */
	if (!stmt->updatable)
	{
		stmt->options.scroll_concurrency = SQL_CONCUR_READ_ONLY;
		return SQL_ERROR;
	}
	global_ridx = irow + stmt->rowset_start;
	if (!(oid = getOid(res, global_ridx)))
		return SQL_SUCCESS_WITH_INFO;
	getTid(res, global_ridx, &blocknum, &offset);
	sprintf(tidval, "(%u, %u)", blocknum, offset);
	res_cols = getNumResultCols(res);
	if (qres = positioned_load(stmt, LATEST_TUPLE_LOAD, oid, tidval), qres)
	{
		TupleField *tupleo, *tuplen;

		rcnt = QR_get_num_tuples(qres);
		tupleo = res->backend_tuples + res->num_fields * global_ridx;
		if (rcnt == 1)
		{
			int	effective_fields = res_cols;

			QR_set_position(qres, 0);
			tuplen = qres->tupleField;
			if (res->keyset)
			{
				if (SQL_CURSOR_KEYSET_DRIVEN == stmt->options.cursor_type &&
					strcmp(tuplen[qres->num_fields - 2].value, tidval))
					res->keyset[global_ridx].status |= SQL_ROW_UPDATED;
				KeySetSet(tuplen, qres->num_fields, res->keyset + global_ridx);
			}
			for (i = 0; i < effective_fields; i++)
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
				res->keyset[global_ridx].blocknum = 0;
				res->keyset[global_ridx].offset = 0;
				res->keyset[global_ridx].status |= SQL_ROW_DELETED;
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
SC_pos_newload(StatementClass *stmt, UInt4 oid, BOOL tidRef)
{
	int			i;
	QResultClass *res, *qres;
	RETCODE		ret = SQL_ERROR;

	mylog("positioned new ti=%x\n", stmt->ti);
	if (!(res = SC_get_Curres(stmt)))
		return SQL_ERROR;
	if (!stmt->ti)
		parse_statement(stmt);	/* not preferable */
	if (!stmt->updatable)
	{
		stmt->options.scroll_concurrency = SQL_CONCUR_READ_ONLY;
		return SQL_ERROR;
	}
	if (qres = positioned_load(stmt, tidRef ? USE_INSERTED_TID : 0, oid, NULL), qres)
	{
		TupleField *tupleo, *tuplen;
		int			count = QR_get_num_tuples(qres);

		QR_set_position(qres, 0);
		if (count == 1)
		{
			int	effective_fields = res->num_fields;

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
				if (res->haskeyset)
					res->keyset = (KeySet *) realloc(res->keyset, sizeof(KeySet) * tuple_size);	
				res->count_allocated = tuple_size;
			}
			tupleo = res->backend_tuples + res->num_fields * res->fcount;
			KeySetSet(tuplen, qres->num_fields, res->keyset + res->fcount);
			for (i = 0; i < effective_fields; i++)
			{
				tupleo[i].len = tuplen[i].len;
				tuplen[i].len = 0;
				tupleo[i].value = tuplen[i].value;
				tuplen[i].value = NULL;
			}
			for (; i < res->num_fields; i++)
			{
				tupleo[i].len = 0;
				tupleo[i].value = NULL;
			}
			res->fcount++;
			ret = SQL_SUCCESS;
		}
		else if (0 == count)
			ret = SQL_NO_DATA_FOUND;
		else
		{
			stmt->errornumber = STMT_ROW_VERSION_CHANGED;
			stmt->errormsg = "the driver cound't identify inserted rows";
			ret = SQL_ERROR;
		}
		QR_Destructor(qres);
		/* stmt->currTuple = stmt->rowset_start + irow; */
	}
	return ret;
}

static RETCODE SQL_API
irow_update(RETCODE ret, StatementClass *stmt, StatementClass *ustmt, UWORD irow, UDWORD global_ridx)
{
	if (ret != SQL_ERROR)
	{
		int			updcnt;
		const char *cmdstr = QR_get_command(SC_get_Curres(ustmt));

		if (cmdstr &&
			sscanf(cmdstr, "UPDATE %d", &updcnt) == 1)
		{
			if (updcnt == 1)
				SC_pos_reload(stmt, irow, global_ridx, (UWORD *) 0);
			else if (updcnt == 0)
			{
				stmt->errornumber = STMT_ROW_VERSION_CHANGED;
				stmt->errormsg = "the content was changed before updation";
				ret = SQL_ERROR;
				if (stmt->options.cursor_type == SQL_CURSOR_KEYSET_DRIVEN)
					SC_pos_reload(stmt, irow, global_ridx, (UWORD *) 0);
			}
			else
				ret = SQL_ERROR;
		}
		else
			ret = SQL_ERROR;
		if (ret == SQL_ERROR && stmt->errornumber == 0)
		{
			stmt->errornumber = STMT_ERROR_TAKEN_FROM_BACKEND;
			stmt->errormsg = "SetPos update return error";
		}
	}
	return ret;
}
RETCODE
SC_pos_update(StatementClass *stmt,
			  UWORD irow, UDWORD global_ridx)
{
	int			i,
				num_cols,
				upd_cols;
	QResultClass *res;
	ConnectionClass	*conn = SC_get_conn(stmt);
	ARDFields	*opts = SC_get_ARD(stmt);
	IRDFields	*irdflds = SC_get_IRD(stmt);
	BindInfoClass *bindings = opts->bindings;
	FIELD_INFO	**fi = SC_get_IRD(stmt)->fi;
	char		updstr[4096];
	RETCODE		ret;
	UInt4	oid, offset, blocknum;
	UInt2	pgoffset;
	Int4	*used, bind_size = opts->bind_size;

	mylog("POS UPDATE %d+%d fi=%x ti=%x\n", irow, SC_get_Curres(stmt)->base,fi, stmt->ti);
	if (!(res = SC_get_Curres(stmt)))
		return SQL_ERROR;
	if (!stmt->ti)
		parse_statement(stmt);	/* not preferable */
	if (!stmt->updatable)
	{
		stmt->options.scroll_concurrency = SQL_CONCUR_READ_ONLY;
		return SQL_ERROR;
	}
	if (!(oid = getOid(res, global_ridx)))
	{
		stmt->errormsg = "The row is already deleted";
		return SQL_ERROR;
	}
	getTid(res, global_ridx, &blocknum, &pgoffset);

	sprintf(updstr, "update \"%s\" set", stmt->ti[0]->name);
	num_cols = irdflds->nfields;
	offset = opts->row_offset_ptr ? *opts->row_offset_ptr : 0;
	for (i = upd_cols = 0; i < num_cols; i++)
	{
		if (used = bindings[i].used, used != NULL)
		{
			used += (offset >> 2);
			if (bind_size > 0)
				used += (bind_size * irow / 4);
			else	
				used += irow; 
			mylog("%d used=%d,%x\n", i, *used, used);
			if (*used != SQL_IGNORE && fi[i]->updatable)
			{
				if (upd_cols)
					sprintf(updstr, "%s, \"%s\" = ?", updstr, fi[i]->name);
				else
					sprintf(updstr, "%s \"%s\" = ?", updstr, fi[i]->name);
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
		APDFields	*apdopts;

		/*sprintf(updstr, "%s where ctid = '%s' and oid = %s", updstr,
				tidval, oidval);*/
		sprintf(updstr, "%s where ctid = '(%u, %u)' and oid = %u", updstr,
				blocknum, pgoffset, oid);
		mylog("updstr=%s\n", updstr);
		if (PGAPI_AllocStmt(conn, &hstmt) != SQL_SUCCESS)
			return SQL_ERROR;
		qstmt = (StatementClass *) hstmt;
		apdopts = SC_get_APD(qstmt);
		apdopts->param_bind_type = opts->bind_size;
		apdopts->param_offset_ptr = opts->row_offset_ptr;
		for (i = j = 0; i < num_cols; i++)
		{
			if (used = bindings[i].used, used != NULL)
			{
				used += (offset >> 2);
				if (bind_size > 0)
					used += (bind_size * irow / 4);
				else
					used += irow;
				mylog("%d used=%d\n", i, *used);
				if (*used != SQL_IGNORE && fi[i]->updatable)
				{
					PGAPI_BindParameter(hstmt, (SQLUSMALLINT) ++j,
								 SQL_PARAM_INPUT, bindings[i].returntype,
					  pgtype_to_concise_type(stmt, QR_get_field_type(res, i)),
										QR_get_fieldsize(res, i),
									(SQLSMALLINT) fi[i]->decimal_digits,
										bindings[i].buffer,
										bindings[i].buflen,
										bindings[i].used);
				}
			}
		}
		qstmt->exec_start_row = qstmt->exec_end_row = irow; 
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
		ret = irow_update(ret, stmt, qstmt, irow, global_ridx);
		PGAPI_FreeStmt(hstmt, SQL_DROP);
	}
	else
	{
		ret = SQL_SUCCESS_WITH_INFO;
		stmt->errormsg = "update list null";
	}
	if (SQL_SUCCESS == ret && res->keyset)
	{
		if (CC_is_in_trans(conn))
		{
			AddRollback(conn, res, global_ridx, res->keyset);
			res->keyset[global_ridx].status |= (SQL_ROW_UPDATED  | CURS_SELF_UPDATING);
		}
		else
			res->keyset[global_ridx].status |= (SQL_ROW_UPDATED  | CURS_SELF_UPDATED);
	}
#if (ODBCVER >= 0x0300)
	if (irdflds->rowStatusArray)
	{
		switch (ret)
		{
			case SQL_SUCCESS:
				irdflds->rowStatusArray[irow] = SQL_ROW_UPDATED;
				break;
			default:
				irdflds->rowStatusArray[irow] = ret;
		}
	}
#endif /* ODBCVER */

	return ret;
}
RETCODE
SC_pos_delete(StatementClass *stmt,
			  UWORD irow, UDWORD global_ridx)
{
	UWORD		offset;
	QResultClass *res, *qres;
	ConnectionClass	*conn = SC_get_conn(stmt);
	ARDFields	*opts = SC_get_ARD(stmt);
	IRDFields	*irdflds = SC_get_IRD(stmt);
	BindInfoClass *bindings = opts->bindings;
	char		dltstr[4096];
	RETCODE		ret;
	UInt4		oid, blocknum;

	mylog("POS DELETE ti=%x\n", stmt->ti);
	if (!(res = SC_get_Curres(stmt)))
		return SQL_ERROR;
	if (!stmt->ti)
		parse_statement(stmt);	/* not preferable */
	if (!stmt->updatable)
	{
		stmt->options.scroll_concurrency = SQL_CONCUR_READ_ONLY;
		return SQL_ERROR;
	}
	if (!(oid = getOid(res, global_ridx)))
	{
		stmt->errormsg = "The row is already deleted";
		return SQL_ERROR;
	}
	getTid(res, global_ridx, &blocknum, &offset);
	/*sprintf(dltstr, "delete from \"%s\" where ctid = '%s' and oid = %s",*/
	sprintf(dltstr, "delete from \"%s\" where ctid = '(%u, %u)' and oid = %u",
			stmt->ti[0]->name, blocknum, offset, oid);

	mylog("dltstr=%s\n", dltstr);
	qres = CC_send_query(conn, dltstr, NULL, CLEAR_RESULT_ON_ABORT);
	ret = SQL_SUCCESS;
	if (qres && QR_command_successful(qres))
	{
		int			dltcnt;
		const char *cmdstr = QR_get_command(qres);

		if (cmdstr &&
			sscanf(cmdstr, "DELETE %d", &dltcnt) == 1)
		{
			if (dltcnt == 1)
				SC_pos_reload(stmt, irow, global_ridx, (UWORD *) 0);
			else if (dltcnt == 0)
			{
				stmt->errornumber = STMT_ROW_VERSION_CHANGED;
				stmt->errormsg = "the content was changed before deletion";
				ret = SQL_ERROR;
				if (stmt->options.cursor_type == SQL_CURSOR_KEYSET_DRIVEN)
					SC_pos_reload(stmt, irow, global_ridx, (UWORD *) 0);
			}
			else
				ret = SQL_ERROR;
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
	if (SQL_SUCCESS == ret && res->keyset)
	{
		if (CC_is_in_trans(conn))
		{
			AddRollback(conn, res, global_ridx, res->keyset);
			res->keyset[global_ridx].status |= (SQL_ROW_DELETED | CURS_SELF_DELETING);
		}
		else
			res->keyset[global_ridx].status |= (SQL_ROW_DELETED | CURS_SELF_DELETED);
	}
#if (ODBCVER >= 0x0300)
	if (irdflds->rowStatusArray)
	{
		switch (ret)
		{
			case SQL_SUCCESS:
				irdflds->rowStatusArray[irow] = SQL_ROW_DELETED;
				break;
			default:
				irdflds->rowStatusArray[irow] = ret;
		}
	}
#endif /* ODBCVER */
	return ret;
}

static RETCODE SQL_API
irow_insert(RETCODE ret, StatementClass *stmt, StatementClass *istmt, int addpos)
{
	if (ret != SQL_ERROR)
	{
		int		addcnt;
		UInt4		oid;
		ARDFields	*opts = SC_get_ARD(stmt);
		QResultClass	*ires = SC_get_Curres(istmt);
		const char *cmdstr;

		cmdstr = QR_get_command((ires->next ? ires->next : ires));
		if (cmdstr &&
			sscanf(cmdstr, "INSERT %u %d", &oid, &addcnt) == 2 &&
			addcnt == 1)
		{
			ConnectionClass	*conn = SC_get_conn(stmt);
			RETCODE	qret;

			qret = SQL_NO_DATA_FOUND;
			if (PG_VERSION_GE(conn, 7.2))
			{
				qret = SC_pos_newload(stmt, oid, TRUE);
				if (SQL_ERROR == qret)
					return qret;
			}
			if (SQL_NO_DATA_FOUND == qret)
			{
				qret = SC_pos_newload(stmt, oid, FALSE);
				if (SQL_ERROR == qret)
					return qret;
			}
			if (opts->bookmark->buffer)
			{
				char	buf[32];
				UInt4	offset = opts->row_offset_ptr ? *opts->row_offset_ptr : 0;

				sprintf(buf, "%ld", addpos + 1);
				copy_and_convert_field(stmt, 0, buf,
                         		SQL_C_ULONG, opts->bookmark->buffer + offset,
					0, opts->bookmark->used ? opts->bookmark->used
					+ (offset >> 2) : NULL);
			}
		}
		else
		{
			stmt->errornumber = STMT_ERROR_TAKEN_FROM_BACKEND;
			stmt->errormsg = "SetPos insert return error";
		}
	}
	return ret;
}
RETCODE
SC_pos_add(StatementClass *stmt,
		   UWORD irow)
{
	int			num_cols,
				add_cols,
				i;
	HSTMT		hstmt;
	StatementClass *qstmt;
	ConnectionClass	*conn;
	QResultClass *res;
	ARDFields	*opts = SC_get_ARD(stmt);
	IRDFields	*irdflds = SC_get_IRD(stmt);
	APDFields	*apdopts;
	BindInfoClass *bindings = opts->bindings;
	FIELD_INFO	**fi = SC_get_IRD(stmt)->fi;
	char		addstr[4096];
	RETCODE		ret;
	UInt4		offset;
	Int4		*used, bind_size = opts->bind_size;

	mylog("POS ADD fi=%x ti=%x\n", fi, stmt->ti);
	if (!(res = SC_get_Curres(stmt)))
		return SQL_ERROR;
	if (!stmt->ti)
		parse_statement(stmt);	/* not preferable */
	if (!stmt->updatable)
	{
		stmt->options.scroll_concurrency = SQL_CONCUR_READ_ONLY;
		return SQL_ERROR;
	}
	num_cols = irdflds->nfields;
	conn = SC_get_conn(stmt);
	sprintf(addstr, "insert into \"%s\" (", stmt->ti[0]->name);
	if (PGAPI_AllocStmt(conn, &hstmt) != SQL_SUCCESS)
		return SQL_ERROR;
	if (opts->row_offset_ptr)
		offset = *opts->row_offset_ptr;
	else
		offset = 0;
	qstmt = (StatementClass *) hstmt;
	apdopts = SC_get_APD(qstmt);
	apdopts->param_bind_type = opts->bind_size;
	apdopts->param_offset_ptr = opts->row_offset_ptr;
	for (i = add_cols = 0; i < num_cols; i++)
	{
		if (used = bindings[i].used, used != NULL)
		{
			used += (offset >> 2);
			if (bind_size > 0)
				used += (bind_size * irow / 4);
			else
				used += irow;
			mylog("%d used=%d\n", i, *used);
			if (*used != SQL_IGNORE && fi[i]->updatable)
			{
				if (add_cols)
					sprintf(addstr, "%s, \"%s\"", addstr, fi[i]->name);
				else
					sprintf(addstr, "%s\"%s\"", addstr, fi[i]->name);
				PGAPI_BindParameter(hstmt, (SQLUSMALLINT) ++add_cols,
								 SQL_PARAM_INPUT, bindings[i].returntype,
					  pgtype_to_concise_type(stmt, QR_get_field_type(res, i)),
									QR_get_fieldsize(res, i),
									(SQLSMALLINT) fi[i]->decimal_digits,
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
		int	brow_save;

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
		qstmt->exec_start_row = qstmt->exec_end_row = irow; 
		ret = PGAPI_ExecDirect(hstmt, addstr, strlen(addstr));
		if (ret == SQL_ERROR)
		{
			stmt->errornumber = qstmt->errornumber;
			stmt->errormsg = qstmt->errormsg;
		}
		else if (ret == SQL_NEED_DATA)		/* must be fixed */
		{
			stmt->options.scroll_concurrency = SQL_CONCUR_READ_ONLY;
			stmt->errornumber = STMT_INVALID_CURSOR_STATE_ERROR;
			stmt->errormsg = "SetPos with data_at_exec not yet supported";
			ret = SQL_ERROR;
		}
		brow_save = stmt->bind_row; 
		stmt->bind_row = irow; 
		ret = irow_insert(ret, stmt, qstmt, res->fcount);
		stmt->bind_row = brow_save; 
	}
	else
	{
		ret = SQL_SUCCESS_WITH_INFO;
		stmt->errormsg = "insert list null";
	}
	PGAPI_FreeStmt(hstmt, SQL_DROP);
	if (SQL_SUCCESS == ret && res->keyset)
	{
		int	global_ridx = res->fcount - 1;
		if (CC_is_in_trans(conn))
		{

			AddRollback(conn, res, global_ridx, NULL);
			res->keyset[global_ridx].status |= (SQL_ROW_ADDED | CURS_SELF_ADDING);
		}
		else
			res->keyset[global_ridx].status |= (SQL_ROW_ADDED | CURS_SELF_ADDED);
	}
#if (ODBCVER >= 0x0300)
	if (irdflds->rowStatusArray)
	{
		switch (ret)
		{
			case SQL_SUCCESS:
				irdflds->rowStatusArray[irow] = SQL_ROW_ADDED;
				break;
			default:
				irdflds->rowStatusArray[irow] = ret;
		}
	}
#endif /* ODBCVER */

	return ret;
}

/*
 *	Stuff for updatable cursors end.
 */
#endif   /* DRIVER_CURSOR_IMPLEMENT */

RETCODE
SC_pos_refresh(StatementClass *stmt, UWORD irow , UDWORD global_ridx)
{
	RETCODE	ret;
#if (ODBCVER >= 0x0300)
	IRDFields	*irdflds = SC_get_IRD(stmt);
#endif /* ODBCVER */
	/* save the last_fetch_count */
	int		last_fetch = stmt->last_fetch_count;
	int		bind_save = stmt->bind_row;

#ifdef	DRIVER_CURSOR_IMPLEMENT
	if (stmt->options.cursor_type == SQL_CURSOR_KEYSET_DRIVEN)
		SC_pos_reload(stmt, irow, global_ridx, (UWORD *) 0);
#endif   /* DRIVER_CURSOR_IMPLEMENT */
	stmt->bind_row = irow;
	ret = SC_fetch(stmt);
	/* restore the last_fetch_count */
	stmt->last_fetch_count = last_fetch;
	stmt->bind_row = bind_save;
#if (ODBCVER >= 0x0300)
	if (irdflds->rowStatusArray)
	{
		switch (ret)
		{
			case SQL_ERROR:
				irdflds->rowStatusArray[irow] = SQL_ROW_ERROR;
				break;
			case SQL_SUCCESS:
				irdflds->rowStatusArray[irow] = SQL_ROW_SUCCESS;
				break;
			default:
				irdflds->rowStatusArray[irow] = ret;
				break;
		}
	}
#endif /* ODBCVER */

	return SQL_SUCCESS;
}

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
	RETCODE	ret;
	StatementClass *stmt = (StatementClass *) hstmt;
	ConnectionClass	*conn = SC_get_conn(stmt);
	QResultClass *res;
	int		num_cols, i, start_row, end_row, processed;
	ARDFields	*opts;
	BindInfoClass *bindings;
	UDWORD		global_ridx, fcount;
	BOOL		auto_commit_needed = FALSE;

	if (!stmt)
	{
		SC_log_error(func, "", NULL);
		return SQL_INVALID_HANDLE;
	}

	opts = SC_get_ARD(stmt);
	bindings = opts->bindings;
#ifdef	DRIVER_CURSOR_IMPLEMENT
	mylog("%s fOption=%d irow=%d lock=%d currt=%d\n", func, fOption, irow, fLock, stmt->currTuple);
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

	if (!(res = SC_get_Curres(stmt)))
	{
		stmt->errormsg = "Null statement result in PGAPI_SetPos.";
		stmt->errornumber = STMT_SEQUENCE_ERROR;
		SC_log_error(func, "", stmt);
		return SQL_ERROR;
	}

	if (irow == 0) /* bulk operation */
	{
		if (SQL_POSITION == fOption)
		{
			stmt->errornumber = STMT_ROW_OUT_OF_RANGE;
			stmt->errormsg = "Bulk Fresh operations not allowed.";
			SC_log_error(func, "", stmt);
			return SQL_ERROR;
		}
		start_row = 0;
		end_row = opts->rowset_size - 1;
	}
	else
	{
		if (irow > stmt->last_fetch_count)
		{
			stmt->errornumber = STMT_ROW_OUT_OF_RANGE;
			stmt->errormsg = "Row value out of range";
			SC_log_error(func, "", stmt);
			return SQL_ERROR;
		}
		start_row = end_row = irow - 1;
	}

	num_cols = QR_NumResultCols(res);
	/* Reset for SQLGetData */
	if (bindings)
		for (i = 0; i < num_cols; i++)
			bindings[i].data_left = -1;
	ret = SQL_SUCCESS;
	fcount = res->fcount;
#ifdef	DRIVER_CURSOR_IMPLEMENT
	switch (fOption)
	{
		case SQL_UPDATE:
		case SQL_DELETE:
		case SQL_ADD:
			if (auto_commit_needed = CC_is_in_autocommit(conn), auto_commit_needed)
				PGAPI_SetConnectOption(conn, SQL_AUTOCOMMIT, SQL_AUTOCOMMIT_OFF);
			break;
	}
#endif   /* DRIVER_CURSOR_IMPLEMENT */
	for (i = start_row, processed = 0; i <= end_row; i++)
	{
#if (ODBCVER >= 0x0300)
		if (0 != irow || !opts->row_operation_ptr || opts->row_operation_ptr[i] == SQL_ROW_PROCEED)
		{
#endif /* ODBCVER */
			global_ridx = i + stmt->rowset_start;
			switch (fOption)
			{
#ifdef	DRIVER_CURSOR_IMPLEMENT
				case SQL_UPDATE:
					ret = SC_pos_update(stmt, (UWORD) i, global_ridx);
					break;
				case SQL_DELETE:
					ret = SC_pos_delete(stmt, (UWORD) i, global_ridx);
					break;
				case SQL_ADD:
					ret = SC_pos_add(stmt, (UWORD) i);
					break;
#endif   /* DRIVER_CURSOR_IMPLEMENT */
				case SQL_REFRESH:
					ret = SC_pos_refresh(stmt, (UWORD) i, global_ridx);
					break;
			}
			processed++;
			if (SQL_ERROR == ret)
				break;
#if (ODBCVER >= 0x0300)
		}
#endif /* ODBCVER */
	}
	if (SQL_ERROR == ret)
		res->fcount = fcount; /* restore the count */
	if (auto_commit_needed)
		PGAPI_SetConnectOption(conn, SQL_AUTOCOMMIT, SQL_AUTOCOMMIT_ON);
	if (irow > 0)
	{
		if (SQL_ADD != fOption) /* for SQLGetData */
		{ 
			stmt->currTuple = stmt->rowset_start + irow - 1;
			QR_set_position(res, irow - 1);
		}
	}
	else if (SC_get_IRD(stmt)->rowsFetched)
			*SC_get_IRD(stmt)->rowsFetched = processed;
inolog("rowset=%d processed=%d ret=%d\n", opts->rowset_size, processed, ret);
	return ret; 
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
