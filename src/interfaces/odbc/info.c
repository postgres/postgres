/*--------
 * Module:			info.c
 *
 * Description:		This module contains routines related to
 *					ODBC informational functions.
 *
 * Classes:			n/a
 *
 * API functions:	SQLGetInfo, SQLGetTypeInfo, SQLGetFunctions,
 *					SQLTables, SQLColumns, SQLStatistics, SQLSpecialColumns,
 *					SQLPrimaryKeys, SQLForeignKeys,
 *					SQLProcedureColumns(NI), SQLProcedures(NI),
 *					SQLTablePrivileges(NI), SQLColumnPrivileges(NI)
 *
 * Comments:		See "notice.txt" for copyright and license information.
 *--------
 */

#include "psqlodbc.h"

#include <string.h>
#include <stdio.h>

#ifndef WIN32
#include <ctype.h>
#endif

#include "tuple.h"
#include "pgtypes.h"

#include "environ.h"
#include "connection.h"
#include "statement.h"
#include "qresult.h"
#include "bind.h"
#include "misc.h"
#include "pgtypes.h"
#include "pgapifunc.h"


/*	Trigger related stuff for SQLForeign Keys */
#define TRIGGER_SHIFT 3
#define TRIGGER_MASK   0x03
#define TRIGGER_DELETE 0x01
#define TRIGGER_UPDATE 0x02


/* extern GLOBAL_VALUES globals; */



RETCODE		SQL_API
PGAPI_GetInfo(
			  HDBC hdbc,
			  UWORD fInfoType,
			  PTR rgbInfoValue,
			  SWORD cbInfoValueMax,
			  SWORD FAR * pcbInfoValue)
{
	static char *func = "PGAPI_GetInfo";
	ConnectionClass *conn = (ConnectionClass *) hdbc;
	ConnInfo   *ci;
	char	   *p = NULL,
				tmp[MAX_INFO_STRING];
	int			len = 0,
				value = 0;
	RETCODE		result;

	mylog("%s: entering...fInfoType=%d\n", func, fInfoType);

	if (!conn)
	{
		CC_log_error(func, "", NULL);
		return SQL_INVALID_HANDLE;
	}

	ci = &(conn->connInfo);

	switch (fInfoType)
	{
		case SQL_ACCESSIBLE_PROCEDURES: /* ODBC 1.0 */
			p = "N";
			break;

		case SQL_ACCESSIBLE_TABLES:		/* ODBC 1.0 */
			p = "N";
			break;

		case SQL_ACTIVE_CONNECTIONS:	/* ODBC 1.0 */
			len = 2;
			value = MAX_CONNECTIONS;
			break;

		case SQL_ACTIVE_STATEMENTS:		/* ODBC 1.0 */
			len = 2;
			value = 0;
			break;

		case SQL_ALTER_TABLE:	/* ODBC 2.0 */
			len = 4;
			value = SQL_AT_ADD_COLUMN;
			break;

		case SQL_BOOKMARK_PERSISTENCE:	/* ODBC 2.0 */
			/* very simple bookmark support */
			len = 4;
			value = ci->drivers.use_declarefetch ? 0 : (SQL_BP_SCROLL);
			break;

		case SQL_COLUMN_ALIAS:	/* ODBC 2.0 */
			p = "N";
			break;

		case SQL_CONCAT_NULL_BEHAVIOR:	/* ODBC 1.0 */
			len = 2;
			value = SQL_CB_NON_NULL;
			break;

		case SQL_CONVERT_BIGINT:
		case SQL_CONVERT_BINARY:
		case SQL_CONVERT_BIT:
		case SQL_CONVERT_CHAR:
		case SQL_CONVERT_DATE:
		case SQL_CONVERT_DECIMAL:
		case SQL_CONVERT_DOUBLE:
		case SQL_CONVERT_FLOAT:
		case SQL_CONVERT_INTEGER:
		case SQL_CONVERT_LONGVARBINARY:
		case SQL_CONVERT_LONGVARCHAR:
		case SQL_CONVERT_NUMERIC:
		case SQL_CONVERT_REAL:
		case SQL_CONVERT_SMALLINT:
		case SQL_CONVERT_TIME:
		case SQL_CONVERT_TIMESTAMP:
		case SQL_CONVERT_TINYINT:
		case SQL_CONVERT_VARBINARY:
		case SQL_CONVERT_VARCHAR:		/* ODBC 1.0 */
			len = 4;
			value = fInfoType;
			break;

		case SQL_CONVERT_FUNCTIONS:		/* ODBC 1.0 */
			len = 4;
			value = 0;
			break;

		case SQL_CORRELATION_NAME:		/* ODBC 1.0 */

			/*
			 * Saying no correlation name makes Query not work right.
			 * value = SQL_CN_NONE;
			 */
			len = 2;
			value = SQL_CN_ANY;
			break;

		case SQL_CURSOR_COMMIT_BEHAVIOR:		/* ODBC 1.0 */
			len = 2;
			value = SQL_CB_CLOSE;
			if (ci->updatable_cursors)
				if (!ci->drivers.use_declarefetch)
					value = SQL_CB_PRESERVE;
			break;

		case SQL_CURSOR_ROLLBACK_BEHAVIOR:		/* ODBC 1.0 */
			len = 2;
			value = SQL_CB_CLOSE;
			if (ci->updatable_cursors)
				if (!ci->drivers.use_declarefetch)
					value = SQL_CB_PRESERVE;
			break;

		case SQL_DATA_SOURCE_NAME:		/* ODBC 1.0 */
			p = CC_get_DSN(conn);
			break;

		case SQL_DATA_SOURCE_READ_ONLY: /* ODBC 1.0 */
			p = CC_is_onlyread(conn) ? "Y" : "N";
			break;

		case SQL_DATABASE_NAME:	/* Support for old ODBC 1.0 Apps */

			/*
			 * Returning the database name causes problems in MS Query. It
			 * generates query like: "SELECT DISTINCT a FROM byronnbad3
			 * bad3"
			 *
			 * p = CC_get_database(conn);
			 */
			p = "";
			break;

		case SQL_DBMS_NAME:		/* ODBC 1.0 */
			p = DBMS_NAME;
			break;

		case SQL_DBMS_VER:		/* ODBC 1.0 */

			/*
			 * The ODBC spec wants ##.##.#### ...whatever... so prepend
			 * the driver
			 */
			/* version number to the dbms version string */
			sprintf(tmp, "%s %s", POSTGRESDRIVERVERSION, conn->pg_version);
			p = tmp;
			break;

		case SQL_DEFAULT_TXN_ISOLATION: /* ODBC 1.0 */
			len = 4;
			value = SQL_TXN_READ_COMMITTED;		/* SQL_TXN_SERIALIZABLE; */
			break;

		case SQL_DRIVER_NAME:	/* ODBC 1.0 */
			p = DRIVER_FILE_NAME;
			break;

		case SQL_DRIVER_ODBC_VER:
			p = DRIVER_ODBC_VER;
			break;

		case SQL_DRIVER_VER:	/* ODBC 1.0 */
			p = POSTGRESDRIVERVERSION;
			break;

		case SQL_EXPRESSIONS_IN_ORDERBY:		/* ODBC 1.0 */
			p = "N";
			break;

		case SQL_FETCH_DIRECTION:		/* ODBC 1.0 */
			len = 4;
			value = ci->drivers.use_declarefetch ? (SQL_FD_FETCH_NEXT) : (SQL_FD_FETCH_NEXT |
													 SQL_FD_FETCH_FIRST |
													  SQL_FD_FETCH_LAST |
													 SQL_FD_FETCH_PRIOR |
												  SQL_FD_FETCH_ABSOLUTE |
												  SQL_FD_FETCH_RELATIVE |
												  SQL_FD_FETCH_BOOKMARK);
			break;

		case SQL_FILE_USAGE:	/* ODBC 2.0 */
			len = 2;
			value = SQL_FILE_NOT_SUPPORTED;
			break;

		case SQL_GETDATA_EXTENSIONS:	/* ODBC 2.0 */
			len = 4;
			value = (SQL_GD_ANY_COLUMN | SQL_GD_ANY_ORDER | SQL_GD_BOUND | SQL_GD_BLOCK);
			break;

		case SQL_GROUP_BY:		/* ODBC 2.0 */
			len = 2;
			value = SQL_GB_GROUP_BY_EQUALS_SELECT;
			break;

		case SQL_IDENTIFIER_CASE:		/* ODBC 1.0 */

			/*
			 * are identifiers case-sensitive (yes, but only when quoted.
			 * If not quoted, they default to lowercase)
			 */
			len = 2;
			value = SQL_IC_LOWER;
			break;

		case SQL_IDENTIFIER_QUOTE_CHAR: /* ODBC 1.0 */
			/* the character used to quote "identifiers" */
			p = PG_VERSION_LE(conn, 6.2) ? " " : "\"";
			break;

		case SQL_KEYWORDS:		/* ODBC 2.0 */
			p = "";
			break;

		case SQL_LIKE_ESCAPE_CLAUSE:	/* ODBC 2.0 */

			/*
			 * is there a character that escapes '%' and '_' in a LIKE
			 * clause? not as far as I can tell
			 */
			p = "N";
			break;

		case SQL_LOCK_TYPES:	/* ODBC 2.0 */
			len = 4;
			value = ci->drivers.lie ? (SQL_LCK_NO_CHANGE | SQL_LCK_EXCLUSIVE | SQL_LCK_UNLOCK) : SQL_LCK_NO_CHANGE;
			break;

		case SQL_MAX_BINARY_LITERAL_LEN:		/* ODBC 2.0 */
			len = 4;
			value = 0;
			break;

		case SQL_MAX_CHAR_LITERAL_LEN:	/* ODBC 2.0 */
			len = 4;
			value = 0;
			break;

		case SQL_MAX_COLUMN_NAME_LEN:	/* ODBC 1.0 */
			len = 2;
			value = MAX_COLUMN_LEN;
			break;

		case SQL_MAX_COLUMNS_IN_GROUP_BY:		/* ODBC 2.0 */
			len = 2;
			value = 0;
			break;

		case SQL_MAX_COLUMNS_IN_INDEX:	/* ODBC 2.0 */
			len = 2;
			value = 0;
			break;

		case SQL_MAX_COLUMNS_IN_ORDER_BY:		/* ODBC 2.0 */
			len = 2;
			value = 0;
			break;

		case SQL_MAX_COLUMNS_IN_SELECT: /* ODBC 2.0 */
			len = 2;
			value = 0;
			break;

		case SQL_MAX_COLUMNS_IN_TABLE:	/* ODBC 2.0 */
			len = 2;
			value = 0;
			break;

		case SQL_MAX_CURSOR_NAME_LEN:	/* ODBC 1.0 */
			len = 2;
			value = MAX_CURSOR_LEN;
			break;

		case SQL_MAX_INDEX_SIZE:		/* ODBC 2.0 */
			len = 4;
			value = 0;
			break;

		case SQL_MAX_OWNER_NAME_LEN:	/* ODBC 1.0 */
			len = 2;
			value = 0;
			break;

		case SQL_MAX_PROCEDURE_NAME_LEN:		/* ODBC 1.0 */
			len = 2;
			value = 0;
			break;

		case SQL_MAX_QUALIFIER_NAME_LEN:		/* ODBC 1.0 */
			len = 2;
			value = 0;
			break;

		case SQL_MAX_ROW_SIZE:	/* ODBC 2.0 */
			len = 4;
			if (PG_VERSION_GE(conn, 7.1))
			{
				/* Large Rowa in 7.1+ */
				value = MAX_ROW_SIZE;
			}
			else
			{
				/* Without the Toaster we're limited to the blocksize */
				value = BLCKSZ;
			}
			break;

		case SQL_MAX_ROW_SIZE_INCLUDES_LONG:	/* ODBC 2.0 */

			/*
			 * does the preceding value include LONGVARCHAR and
			 * LONGVARBINARY fields?   Well, it does include longvarchar,
			 * but not longvarbinary.
			 */
			p = "Y";
			break;

		case SQL_MAX_STATEMENT_LEN:		/* ODBC 2.0 */
			/* maybe this should be 0? */
			len = 4;
			value = CC_get_max_query_len(conn);
			break;

		case SQL_MAX_TABLE_NAME_LEN:	/* ODBC 1.0 */
			len = 2;
			value = MAX_TABLE_LEN;
			break;

		case SQL_MAX_TABLES_IN_SELECT:	/* ODBC 2.0 */
			len = 2;
			value = 0;
			break;

		case SQL_MAX_USER_NAME_LEN:
			len = 2;
			value = 0;
			break;

		case SQL_MULT_RESULT_SETS:		/* ODBC 1.0 */
			/* Don't support multiple result sets but say yes anyway? */
			p = "Y";
			break;

		case SQL_MULTIPLE_ACTIVE_TXN:	/* ODBC 1.0 */
			p = "Y";
			break;

		case SQL_NEED_LONG_DATA_LEN:	/* ODBC 2.0 */

			/*
			 * Don't need the length, SQLPutData can handle any size and
			 * multiple calls
			 */
			p = "N";
			break;

		case SQL_NON_NULLABLE_COLUMNS:	/* ODBC 1.0 */
			len = 2;
			value = SQL_NNC_NON_NULL;
			break;

		case SQL_NULL_COLLATION:		/* ODBC 2.0 */
			/* where are nulls sorted? */
			len = 2;
			value = SQL_NC_END;
			break;

		case SQL_NUMERIC_FUNCTIONS:		/* ODBC 1.0 */
			len = 4;
			value = 0;
			break;

		case SQL_ODBC_API_CONFORMANCE:	/* ODBC 1.0 */
			len = 2;
			value = SQL_OAC_LEVEL1;
			break;

		case SQL_ODBC_SAG_CLI_CONFORMANCE:		/* ODBC 1.0 */
			len = 2;
			value = SQL_OSCC_NOT_COMPLIANT;
			break;

		case SQL_ODBC_SQL_CONFORMANCE:	/* ODBC 1.0 */
			len = 2;
			value = SQL_OSC_CORE;
			break;

		case SQL_ODBC_SQL_OPT_IEF:		/* ODBC 1.0 */
			p = "N";
			break;

		case SQL_OJ_CAPABILITIES:		/* ODBC 2.01 */
			len = 4;
			if (PG_VERSION_GE(conn, 7.1))
			{
				/* OJs in 7.1+ */
				value = (SQL_OJ_LEFT |
						 SQL_OJ_RIGHT |
						 SQL_OJ_FULL |
						 SQL_OJ_NESTED |
						 SQL_OJ_NOT_ORDERED |
						 SQL_OJ_INNER |
						 SQL_OJ_ALL_COMPARISON_OPS);
			}
			else
				/* OJs not in <7.1 */
				value = 0;
			break;

		case SQL_ORDER_BY_COLUMNS_IN_SELECT:	/* ODBC 2.0 */
			p = (PG_VERSION_LE(conn, 6.3)) ? "Y" : "N";
			break;

		case SQL_OUTER_JOINS:	/* ODBC 1.0 */
			if (PG_VERSION_GE(conn, 7.1))
				/* OJs in 7.1+ */
				p = "Y";
			else
				/* OJs not in <7.1 */
				p = "N";
			break;

		case SQL_OWNER_TERM:	/* ODBC 1.0 */
			p = "owner";
			break;

		case SQL_OWNER_USAGE:	/* ODBC 2.0 */
			len = 4;
			value = 0;
			break;

		case SQL_POS_OPERATIONS:		/* ODBC 2.0 */
			len = 4;
			value = ci->drivers.lie ? (SQL_POS_POSITION | SQL_POS_REFRESH | SQL_POS_UPDATE | SQL_POS_DELETE | SQL_POS_ADD) : (SQL_POS_POSITION | SQL_POS_REFRESH);
			if (ci->updatable_cursors)
				value |= (SQL_POS_UPDATE | SQL_POS_DELETE | SQL_POS_ADD);
			break;

		case SQL_POSITIONED_STATEMENTS: /* ODBC 2.0 */
			len = 4;
			value = ci->drivers.lie ? (SQL_PS_POSITIONED_DELETE |
									   SQL_PS_POSITIONED_UPDATE |
									   SQL_PS_SELECT_FOR_UPDATE) : 0;
			break;

		case SQL_PROCEDURE_TERM:		/* ODBC 1.0 */
			p = "procedure";
			break;

		case SQL_PROCEDURES:	/* ODBC 1.0 */
			p = "Y";
			break;

		case SQL_QUALIFIER_LOCATION:	/* ODBC 2.0 */
			len = 2;
			value = SQL_QL_START;
			break;

		case SQL_QUALIFIER_NAME_SEPARATOR:		/* ODBC 1.0 */
			p = "";
			break;

		case SQL_QUALIFIER_TERM:		/* ODBC 1.0 */
			p = "";
			break;

		case SQL_QUALIFIER_USAGE:		/* ODBC 2.0 */
			len = 4;
			value = 0;
			break;

		case SQL_QUOTED_IDENTIFIER_CASE:		/* ODBC 2.0 */
			/* are "quoted" identifiers case-sensitive?  YES! */
			len = 2;
			value = SQL_IC_SENSITIVE;
			break;

		case SQL_ROW_UPDATES:	/* ODBC 1.0 */

			/*
			 * Driver doesn't support keyset-driven or mixed cursors, so
			 * not much point in saying row updates are supported
			 */
			p = (ci->drivers.lie || ci->updatable_cursors) ? "Y" : "N";
			break;

		case SQL_SCROLL_CONCURRENCY:	/* ODBC 1.0 */
			len = 4;
			value = ci->drivers.lie ? (SQL_SCCO_READ_ONLY |
									   SQL_SCCO_LOCK |
									   SQL_SCCO_OPT_ROWVER |
									   SQL_SCCO_OPT_VALUES) :
				(SQL_SCCO_READ_ONLY);
			if (ci->updatable_cursors)
				value |= SQL_SCCO_OPT_ROWVER;
			break;

		case SQL_SCROLL_OPTIONS:		/* ODBC 1.0 */
			len = 4;
			value = ci->drivers.lie ? (SQL_SO_FORWARD_ONLY |
									   SQL_SO_STATIC |
									   SQL_SO_KEYSET_DRIVEN |
									   SQL_SO_DYNAMIC |
									   SQL_SO_MIXED)
				: (ci->drivers.use_declarefetch ? SQL_SO_FORWARD_ONLY : (SQL_SO_FORWARD_ONLY | SQL_SO_STATIC));
			if (ci->updatable_cursors)
				value |= 0;		/* SQL_SO_KEYSET_DRIVEN in the furure */
			break;

		case SQL_SEARCH_PATTERN_ESCAPE: /* ODBC 1.0 */
			p = "";
			break;

		case SQL_SERVER_NAME:	/* ODBC 1.0 */
			p = CC_get_server(conn);
			break;

		case SQL_SPECIAL_CHARACTERS:	/* ODBC 2.0 */
			p = "_";
			break;

		case SQL_STATIC_SENSITIVITY:	/* ODBC 2.0 */
			len = 4;
			value = ci->drivers.lie ? (SQL_SS_ADDITIONS | SQL_SS_DELETIONS | SQL_SS_UPDATES) : 0;
			if (ci->updatable_cursors)
				value |= (SQL_SS_ADDITIONS | SQL_SS_DELETIONS | SQL_SS_UPDATES);
			break;

		case SQL_STRING_FUNCTIONS:		/* ODBC 1.0 */
			len = 4;
			value = (SQL_FN_STR_CONCAT |
					 SQL_FN_STR_LCASE |
					 SQL_FN_STR_LENGTH |
					 SQL_FN_STR_LOCATE |
					 SQL_FN_STR_LTRIM |
					 SQL_FN_STR_RTRIM |
					 SQL_FN_STR_SUBSTRING |
					 SQL_FN_STR_UCASE);
			break;

		case SQL_SUBQUERIES:	/* ODBC 2.0 */
			/* postgres 6.3 supports subqueries */
			len = 4;
			value = (SQL_SQ_QUANTIFIED |
					 SQL_SQ_IN |
					 SQL_SQ_EXISTS |
					 SQL_SQ_COMPARISON);
			break;

		case SQL_SYSTEM_FUNCTIONS:		/* ODBC 1.0 */
			len = 4;
			value = 0;
			break;

		case SQL_TABLE_TERM:	/* ODBC 1.0 */
			p = "table";
			break;

		case SQL_TIMEDATE_ADD_INTERVALS:		/* ODBC 2.0 */
			len = 4;
			value = 0;
			break;

		case SQL_TIMEDATE_DIFF_INTERVALS:		/* ODBC 2.0 */
			len = 4;
			value = 0;
			break;

		case SQL_TIMEDATE_FUNCTIONS:	/* ODBC 1.0 */
			len = 4;
			value = (SQL_FN_TD_NOW);
			break;

		case SQL_TXN_CAPABLE:	/* ODBC 1.0 */

			/*
			 * Postgres can deal with create or drop table statements in a
			 * transaction
			 */
			len = 2;
			value = SQL_TC_ALL;
			break;

		case SQL_TXN_ISOLATION_OPTION:	/* ODBC 1.0 */
			len = 4;
			value = SQL_TXN_READ_COMMITTED;		/* SQL_TXN_SERIALIZABLE; */
			break;

		case SQL_UNION: /* ODBC 2.0 */
			/* unions with all supported in postgres 6.3 */
			len = 4;
			value = (SQL_U_UNION | SQL_U_UNION_ALL);
			break;

		case SQL_USER_NAME:		/* ODBC 1.0 */
			p = CC_get_username(conn);
			break;

		default:
			/* unrecognized key */
			conn->errormsg = "Unrecognized key passed to PGAPI_GetInfo.";
			conn->errornumber = CONN_NOT_IMPLEMENTED_ERROR;
			CC_log_error(func, "", conn);
			return SQL_ERROR;
	}

	result = SQL_SUCCESS;

	mylog("%s: p='%s', len=%d, value=%d, cbMax=%d\n", func, p ? p : "<NULL>", len, value, cbInfoValueMax);

	/*
	 * NOTE, that if rgbInfoValue is NULL, then no warnings or errors
	 * should result and just pcbInfoValue is returned, which indicates
	 * what length would be required if a real buffer had been passed in.
	 */
	if (p)
	{
		/* char/binary data */
		len = strlen(p);

		if (rgbInfoValue)
		{
			strncpy_null((char *) rgbInfoValue, p, (size_t) cbInfoValueMax);

			if (len >= cbInfoValueMax)
			{
				result = SQL_SUCCESS_WITH_INFO;
				conn->errornumber = STMT_TRUNCATED;
				conn->errormsg = "The buffer was too small for tthe InfoValue.";
			}
		}
	}
	else
	{
		/* numeric data */
		if (rgbInfoValue)
		{
			if (len == 2)
				*((WORD *) rgbInfoValue) = (WORD) value;
			else if (len == 4)
				*((DWORD *) rgbInfoValue) = (DWORD) value;
		}
	}

	if (pcbInfoValue)
		*pcbInfoValue = len;

	return result;
}


RETCODE		SQL_API
PGAPI_GetTypeInfo(
				  HSTMT hstmt,
				  SWORD fSqlType)
{
	static char *func = "PGAPI_GetTypeInfo";
	StatementClass *stmt = (StatementClass *) hstmt;
	TupleNode  *row;
	int			i;

	/* Int4 type; */
	Int4		pgType;
	Int2		sqlType;

	mylog("%s: entering...fSqlType = %d\n", func, fSqlType);

	if (!stmt)
	{
		SC_log_error(func, "", NULL);
		return SQL_INVALID_HANDLE;
	}

	stmt->manual_result = TRUE;
	stmt->result = QR_Constructor();
	if (!stmt->result)
	{
		SC_log_error(func, "Error creating result.", stmt);
		return SQL_ERROR;
	}

	extend_bindings(stmt, 15);

	QR_set_num_fields(stmt->result, 15);
	QR_set_field_info(stmt->result, 0, "TYPE_NAME", PG_TYPE_TEXT, MAX_INFO_STRING);
	QR_set_field_info(stmt->result, 1, "DATA_TYPE", PG_TYPE_INT2, 2);
	QR_set_field_info(stmt->result, 2, "PRECISION", PG_TYPE_INT4, 4);
	QR_set_field_info(stmt->result, 3, "LITERAL_PREFIX", PG_TYPE_TEXT, MAX_INFO_STRING);
	QR_set_field_info(stmt->result, 4, "LITERAL_SUFFIX", PG_TYPE_TEXT, MAX_INFO_STRING);
	QR_set_field_info(stmt->result, 5, "CREATE_PARAMS", PG_TYPE_TEXT, MAX_INFO_STRING);
	QR_set_field_info(stmt->result, 6, "NULLABLE", PG_TYPE_INT2, 2);
	QR_set_field_info(stmt->result, 7, "CASE_SENSITIVE", PG_TYPE_INT2, 2);
	QR_set_field_info(stmt->result, 8, "SEARCHABLE", PG_TYPE_INT2, 2);
	QR_set_field_info(stmt->result, 9, "UNSIGNED_ATTRIBUTE", PG_TYPE_INT2, 2);
	QR_set_field_info(stmt->result, 10, "MONEY", PG_TYPE_INT2, 2);
	QR_set_field_info(stmt->result, 11, "AUTO_INCREMENT", PG_TYPE_INT2, 2);
	QR_set_field_info(stmt->result, 12, "LOCAL_TYPE_NAME", PG_TYPE_TEXT, MAX_INFO_STRING);
	QR_set_field_info(stmt->result, 13, "MINIMUM_SCALE", PG_TYPE_INT2, 2);
	QR_set_field_info(stmt->result, 14, "MAXIMUM_SCALE", PG_TYPE_INT2, 2);

	for (i = 0, sqlType = sqlTypes[0]; sqlType; sqlType = sqlTypes[++i])
	{
		pgType = sqltype_to_pgtype(stmt, sqlType);

		if (fSqlType == SQL_ALL_TYPES || fSqlType == sqlType)
		{
			row = (TupleNode *) malloc(sizeof(TupleNode) + (15 - 1) *sizeof(TupleField));

			/* These values can't be NULL */
			set_tuplefield_string(&row->tuple[0], pgtype_to_name(stmt, pgType));
			set_tuplefield_int2(&row->tuple[1], (Int2) sqlType);
			set_tuplefield_int2(&row->tuple[6], pgtype_nullable(stmt, pgType));
			set_tuplefield_int2(&row->tuple[7], pgtype_case_sensitive(stmt, pgType));
			set_tuplefield_int2(&row->tuple[8], pgtype_searchable(stmt, pgType));
			set_tuplefield_int2(&row->tuple[10], pgtype_money(stmt, pgType));

			/*
			 * Localized data-source dependent data type name (always
			 * NULL)
			 */
			set_tuplefield_null(&row->tuple[12]);

			/* These values can be NULL */
			set_nullfield_int4(&row->tuple[2], pgtype_precision(stmt, pgType, PG_STATIC, PG_STATIC));
			set_nullfield_string(&row->tuple[3], pgtype_literal_prefix(stmt, pgType));
			set_nullfield_string(&row->tuple[4], pgtype_literal_suffix(stmt, pgType));
			set_nullfield_string(&row->tuple[5], pgtype_create_params(stmt, pgType));
			set_nullfield_int2(&row->tuple[9], pgtype_unsigned(stmt, pgType));
			set_nullfield_int2(&row->tuple[11], pgtype_auto_increment(stmt, pgType));
			set_nullfield_int2(&row->tuple[13], pgtype_scale(stmt, pgType, PG_STATIC));
			set_nullfield_int2(&row->tuple[14], pgtype_scale(stmt, pgType, PG_STATIC));

			QR_add_tuple(stmt->result, row);
		}
	}

	stmt->status = STMT_FINISHED;
	stmt->currTuple = -1;
	stmt->rowset_start = -1;
	stmt->current_col = -1;

	return SQL_SUCCESS;
}


RETCODE		SQL_API
PGAPI_GetFunctions(
				   HDBC hdbc,
				   UWORD fFunction,
				   UWORD FAR * pfExists)
{
	static char *func = "PGAPI_GetFunctions";
	ConnectionClass *conn = (ConnectionClass *) hdbc;
	ConnInfo   *ci = &(conn->connInfo);

	mylog("%s: entering...%u\n", func, fFunction);

	if (fFunction == SQL_API_ALL_FUNCTIONS)
	{
#if (ODBCVER < 0x0300)
		if (ci->drivers.lie)
		{
			int			i;

			memset(pfExists, 0, sizeof(UWORD) * 100);

			pfExists[SQL_API_SQLALLOCENV] = TRUE;
			pfExists[SQL_API_SQLFREEENV] = TRUE;
			for (i = SQL_API_SQLALLOCCONNECT; i <= SQL_NUM_FUNCTIONS; i++)
				pfExists[i] = TRUE;
			for (i = SQL_EXT_API_START; i <= SQL_EXT_API_LAST; i++)
				pfExists[i] = TRUE;
		}
		else
#endif
		{
			memset(pfExists, 0, sizeof(UWORD) * 100);

			/* ODBC core functions */
			pfExists[SQL_API_SQLALLOCCONNECT] = TRUE;
			pfExists[SQL_API_SQLALLOCENV] = TRUE;
			pfExists[SQL_API_SQLALLOCSTMT] = TRUE;
			pfExists[SQL_API_SQLBINDCOL] = TRUE;
			pfExists[SQL_API_SQLCANCEL] = TRUE;
			pfExists[SQL_API_SQLCOLATTRIBUTES] = TRUE;
			pfExists[SQL_API_SQLCONNECT] = TRUE;
			pfExists[SQL_API_SQLDESCRIBECOL] = TRUE;	/* partial */
			pfExists[SQL_API_SQLDISCONNECT] = TRUE;
			pfExists[SQL_API_SQLERROR] = TRUE;
			pfExists[SQL_API_SQLEXECDIRECT] = TRUE;
			pfExists[SQL_API_SQLEXECUTE] = TRUE;
			pfExists[SQL_API_SQLFETCH] = TRUE;
			pfExists[SQL_API_SQLFREECONNECT] = TRUE;
			pfExists[SQL_API_SQLFREEENV] = TRUE;
			pfExists[SQL_API_SQLFREESTMT] = TRUE;
			pfExists[SQL_API_SQLGETCURSORNAME] = TRUE;
			pfExists[SQL_API_SQLNUMRESULTCOLS] = TRUE;
			pfExists[SQL_API_SQLPREPARE] = TRUE;		/* complete? */
			pfExists[SQL_API_SQLROWCOUNT] = TRUE;
			pfExists[SQL_API_SQLSETCURSORNAME] = TRUE;
			pfExists[SQL_API_SQLSETPARAM] = FALSE;		/* odbc 1.0 */
			pfExists[SQL_API_SQLTRANSACT] = TRUE;

			/* ODBC level 1 functions */
			pfExists[SQL_API_SQLBINDPARAMETER] = TRUE;
			pfExists[SQL_API_SQLCOLUMNS] = TRUE;
			pfExists[SQL_API_SQLDRIVERCONNECT] = TRUE;
			pfExists[SQL_API_SQLGETCONNECTOPTION] = TRUE;		/* partial */
			pfExists[SQL_API_SQLGETDATA] = TRUE;
			pfExists[SQL_API_SQLGETFUNCTIONS] = TRUE;
			pfExists[SQL_API_SQLGETINFO] = TRUE;
			pfExists[SQL_API_SQLGETSTMTOPTION] = TRUE;	/* partial */
			pfExists[SQL_API_SQLGETTYPEINFO] = TRUE;
			pfExists[SQL_API_SQLPARAMDATA] = TRUE;
			pfExists[SQL_API_SQLPUTDATA] = TRUE;
			pfExists[SQL_API_SQLSETCONNECTOPTION] = TRUE;		/* partial */
			pfExists[SQL_API_SQLSETSTMTOPTION] = TRUE;
			pfExists[SQL_API_SQLSPECIALCOLUMNS] = TRUE;
			pfExists[SQL_API_SQLSTATISTICS] = TRUE;
			pfExists[SQL_API_SQLTABLES] = TRUE;

			/* ODBC level 2 functions */
			pfExists[SQL_API_SQLBROWSECONNECT] = FALSE;
			pfExists[SQL_API_SQLCOLUMNPRIVILEGES] = FALSE;
			pfExists[SQL_API_SQLDATASOURCES] = FALSE;	/* only implemented by
														 * DM */
			pfExists[SQL_API_SQLDESCRIBEPARAM] = FALSE; /* not properly
														 * implemented */
			pfExists[SQL_API_SQLDRIVERS] = FALSE;		/* only implemented by
														 * DM */
			pfExists[SQL_API_SQLEXTENDEDFETCH] = TRUE;
			pfExists[SQL_API_SQLFOREIGNKEYS] = TRUE;
			pfExists[SQL_API_SQLMORERESULTS] = TRUE;
			pfExists[SQL_API_SQLNATIVESQL] = TRUE;
			pfExists[SQL_API_SQLNUMPARAMS] = TRUE;
			pfExists[SQL_API_SQLPARAMOPTIONS] = FALSE;
			pfExists[SQL_API_SQLPRIMARYKEYS] = TRUE;
			pfExists[SQL_API_SQLPROCEDURECOLUMNS] = FALSE;
			if (PG_VERSION_LT(conn, 6.5))
				pfExists[SQL_API_SQLPROCEDURES] = FALSE;
			else
				pfExists[SQL_API_SQLPROCEDURES] = TRUE;
			pfExists[SQL_API_SQLSETPOS] = TRUE;
			pfExists[SQL_API_SQLSETSCROLLOPTIONS] = TRUE;		/* odbc 1.0 */
			pfExists[SQL_API_SQLTABLEPRIVILEGES] = FALSE;
		}
	}
	else
	{
		if (ci->drivers.lie)
			*pfExists = TRUE;
		else
		{
			switch (fFunction)
			{
				case SQL_API_SQLALLOCCONNECT:
					*pfExists = TRUE;
					break;
				case SQL_API_SQLALLOCENV:
					*pfExists = TRUE;
					break;
				case SQL_API_SQLALLOCSTMT:
					*pfExists = TRUE;
					break;
				case SQL_API_SQLBINDCOL:
					*pfExists = TRUE;
					break;
				case SQL_API_SQLCANCEL:
					*pfExists = TRUE;
					break;
				case SQL_API_SQLCOLATTRIBUTES:
					*pfExists = TRUE;
					break;
				case SQL_API_SQLCONNECT:
					*pfExists = TRUE;
					break;
				case SQL_API_SQLDESCRIBECOL:
					*pfExists = TRUE;
					break;		/* partial */
				case SQL_API_SQLDISCONNECT:
					*pfExists = TRUE;
					break;
				case SQL_API_SQLERROR:
					*pfExists = TRUE;
					break;
				case SQL_API_SQLEXECDIRECT:
					*pfExists = TRUE;
					break;
				case SQL_API_SQLEXECUTE:
					*pfExists = TRUE;
					break;
				case SQL_API_SQLFETCH:
					*pfExists = TRUE;
					break;
				case SQL_API_SQLFREECONNECT:
					*pfExists = TRUE;
					break;
				case SQL_API_SQLFREEENV:
					*pfExists = TRUE;
					break;
				case SQL_API_SQLFREESTMT:
					*pfExists = TRUE;
					break;
				case SQL_API_SQLGETCURSORNAME:
					*pfExists = TRUE;
					break;
				case SQL_API_SQLNUMRESULTCOLS:
					*pfExists = TRUE;
					break;
				case SQL_API_SQLPREPARE:
					*pfExists = TRUE;
					break;
				case SQL_API_SQLROWCOUNT:
					*pfExists = TRUE;
					break;
				case SQL_API_SQLSETCURSORNAME:
					*pfExists = TRUE;
					break;
				case SQL_API_SQLSETPARAM:
					*pfExists = FALSE;
					break;		/* odbc 1.0 */
				case SQL_API_SQLTRANSACT:
					*pfExists = TRUE;
					break;

					/* ODBC level 1 functions */
				case SQL_API_SQLBINDPARAMETER:
					*pfExists = TRUE;
					break;
				case SQL_API_SQLCOLUMNS:
					*pfExists = TRUE;
					break;
				case SQL_API_SQLDRIVERCONNECT:
					*pfExists = TRUE;
					break;
				case SQL_API_SQLGETCONNECTOPTION:
					*pfExists = TRUE;
					break;		/* partial */
				case SQL_API_SQLGETDATA:
					*pfExists = TRUE;
					break;
				case SQL_API_SQLGETFUNCTIONS:
					*pfExists = TRUE;
					break;
				case SQL_API_SQLGETINFO:
					*pfExists = TRUE;
					break;
				case SQL_API_SQLGETSTMTOPTION:
					*pfExists = TRUE;
					break;		/* partial */
				case SQL_API_SQLGETTYPEINFO:
					*pfExists = TRUE;
					break;
				case SQL_API_SQLPARAMDATA:
					*pfExists = TRUE;
					break;
				case SQL_API_SQLPUTDATA:
					*pfExists = TRUE;
					break;
				case SQL_API_SQLSETCONNECTOPTION:
					*pfExists = TRUE;
					break;		/* partial */
				case SQL_API_SQLSETSTMTOPTION:
					*pfExists = TRUE;
					break;
				case SQL_API_SQLSPECIALCOLUMNS:
					*pfExists = TRUE;
					break;
				case SQL_API_SQLSTATISTICS:
					*pfExists = TRUE;
					break;
				case SQL_API_SQLTABLES:
					*pfExists = TRUE;
					break;

					/* ODBC level 2 functions */
				case SQL_API_SQLBROWSECONNECT:
					*pfExists = FALSE;
					break;
				case SQL_API_SQLCOLUMNPRIVILEGES:
					*pfExists = FALSE;
					break;
				case SQL_API_SQLDATASOURCES:
					*pfExists = FALSE;
					break;		/* only implemented by DM */
				case SQL_API_SQLDESCRIBEPARAM:
					*pfExists = FALSE;
					break;		/* not properly implemented */
				case SQL_API_SQLDRIVERS:
					*pfExists = FALSE;
					break;		/* only implemented by DM */
				case SQL_API_SQLEXTENDEDFETCH:
					*pfExists = TRUE;
					break;
				case SQL_API_SQLFOREIGNKEYS:
					*pfExists = TRUE;
					break;
				case SQL_API_SQLMORERESULTS:
					*pfExists = TRUE;
					break;
				case SQL_API_SQLNATIVESQL:
					*pfExists = TRUE;
					break;
				case SQL_API_SQLNUMPARAMS:
					*pfExists = TRUE;
					break;
				case SQL_API_SQLPARAMOPTIONS:
					*pfExists = FALSE;
					break;
				case SQL_API_SQLPRIMARYKEYS:
					*pfExists = TRUE;
					break;
				case SQL_API_SQLPROCEDURECOLUMNS:
					*pfExists = FALSE;
					break;
				case SQL_API_SQLPROCEDURES:
					if (PG_VERSION_LT(conn, 6.5))
						*pfExists = FALSE;
					else
						*pfExists = TRUE;
					break;
				case SQL_API_SQLSETPOS:
					*pfExists = TRUE;
					break;
				case SQL_API_SQLSETSCROLLOPTIONS:
					*pfExists = TRUE;
					break;		/* odbc 1.0 */
				case SQL_API_SQLTABLEPRIVILEGES:
					*pfExists = FALSE;
					break;
			}
		}
	}
	return SQL_SUCCESS;
}


RETCODE		SQL_API
PGAPI_Tables(
			 HSTMT hstmt,
			 UCHAR FAR * szTableQualifier,
			 SWORD cbTableQualifier,
			 UCHAR FAR * szTableOwner,
			 SWORD cbTableOwner,
			 UCHAR FAR * szTableName,
			 SWORD cbTableName,
			 UCHAR FAR * szTableType,
			 SWORD cbTableType)
{
	static char *func = "PGAPI_Tables";
	StatementClass *stmt = (StatementClass *) hstmt;
	StatementClass *tbl_stmt;
	TupleNode  *row;
	HSTMT		htbl_stmt;
	RETCODE		result;
	char	   *tableType;
	char		tables_query[INFO_INQUIRY_LEN];
	char		table_name[MAX_INFO_STRING],
				table_owner[MAX_INFO_STRING],
				relkind_or_hasrules[MAX_INFO_STRING];
	ConnectionClass *conn;
	ConnInfo   *ci;
	char	   *prefix[32],
				prefixes[MEDIUM_REGISTRY_LEN];
	char	   *table_type[32],
				table_types[MAX_INFO_STRING];
	char		show_system_tables,
				show_regular_tables,
				show_views;
	char		regular_table,
				view,
				systable;
	int			i;

	mylog("%s: entering...stmt=%u\n", func, stmt);

	if (!stmt)
	{
		SC_log_error(func, "", NULL);
		return SQL_INVALID_HANDLE;
	}

	stmt->manual_result = TRUE;
	stmt->errormsg_created = TRUE;

	conn = SC_get_conn(stmt);
	ci = &(conn->connInfo);

	result = PGAPI_AllocStmt(stmt->hdbc, &htbl_stmt);
	if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
	{
		stmt->errornumber = STMT_NO_MEMORY_ERROR;
		stmt->errormsg = "Couldn't allocate statement for PGAPI_Tables result.";
		SC_log_error(func, "", stmt);
		return SQL_ERROR;
	}
	tbl_stmt = (StatementClass *) htbl_stmt;

	/*
	 * Create the query to find out the tables
	 */
	if (PG_VERSION_GE(conn, 7.1))
	{
		/* view is represented by its relkind since 7.1 */
		strcpy(tables_query, "select relname, usename, relkind from pg_class, pg_user");
		strcat(tables_query, " where relkind in ('r', 'v')");
	}
	else
	{
		strcpy(tables_query, "select relname, usename, relhasrules from pg_class, pg_user");
		strcat(tables_query, " where relkind = 'r'");
	}

	my_strcat(tables_query, " and usename like '%.*s'", szTableOwner, cbTableOwner);
	my_strcat(tables_query, " and relname like '%.*s'", szTableName, cbTableName);

	/* Parse the extra systable prefix	*/
	strcpy(prefixes, ci->drivers.extra_systable_prefixes);
	i = 0;
	prefix[i] = strtok(prefixes, ";");
	while (prefix[i] && i < 32)
		prefix[++i] = strtok(NULL, ";");

	/* Parse the desired table types to return */
	show_system_tables = FALSE;
	show_regular_tables = FALSE;
	show_views = FALSE;

	/* make_string mallocs memory */
	tableType = make_string(szTableType, cbTableType, NULL);
	if (tableType)
	{
		strcpy(table_types, tableType);
		free(tableType);
		i = 0;
		table_type[i] = strtok(table_types, ",");
		while (table_type[i] && i < 32)
			table_type[++i] = strtok(NULL, ",");

		/* Check for desired table types to return */
		i = 0;
		while (table_type[i])
		{
			if (strstr(table_type[i], "SYSTEM TABLE"))
				show_system_tables = TRUE;
			else if (strstr(table_type[i], "TABLE"))
				show_regular_tables = TRUE;
			else if (strstr(table_type[i], "VIEW"))
				show_views = TRUE;
			i++;
		}
	}
	else
	{
		show_regular_tables = TRUE;
		show_views = TRUE;
	}

	/*
	 * If not interested in SYSTEM TABLES then filter them out to save
	 * some time on the query.	If treating system tables as regular
	 * tables, then dont filter either.
	 */
	if (!atoi(ci->show_system_tables) && !show_system_tables)
	{
		strcat(tables_query, " and relname !~ '^" POSTGRES_SYS_PREFIX);

		/* Also filter out user-defined system table types */
		i = 0;
		while (prefix[i])
		{
			strcat(tables_query, "|^");
			strcat(tables_query, prefix[i]);
			i++;
		}
		strcat(tables_query, "'");
	}

	/* match users */
	if (PG_VERSION_LT(conn, 7.1))
		/* filter out large objects in older versions */
		strcat(tables_query, " and relname !~ '^xinv[0-9]+'");

	strcat(tables_query, " and usesysid = relowner");
	strcat(tables_query, " order by relname");

	result = PGAPI_ExecDirect(htbl_stmt, tables_query, strlen(tables_query));
	if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
	{
		stmt->errormsg = SC_create_errormsg(htbl_stmt);
		stmt->errornumber = tbl_stmt->errornumber;
		SC_log_error(func, "", stmt);
		PGAPI_FreeStmt(htbl_stmt, SQL_DROP);
		return SQL_ERROR;
	}

	result = PGAPI_BindCol(htbl_stmt, 1, SQL_C_CHAR,
						   table_name, MAX_INFO_STRING, NULL);
	if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
	{
		stmt->errormsg = tbl_stmt->errormsg;
		stmt->errornumber = tbl_stmt->errornumber;
		SC_log_error(func, "", stmt);
		PGAPI_FreeStmt(htbl_stmt, SQL_DROP);
		return SQL_ERROR;
	}

	result = PGAPI_BindCol(htbl_stmt, 2, SQL_C_CHAR,
						   table_owner, MAX_INFO_STRING, NULL);
	if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
	{
		stmt->errormsg = tbl_stmt->errormsg;
		stmt->errornumber = tbl_stmt->errornumber;
		SC_log_error(func, "", stmt);
		PGAPI_FreeStmt(htbl_stmt, SQL_DROP);
		return SQL_ERROR;
	}
	result = PGAPI_BindCol(htbl_stmt, 3, SQL_C_CHAR,
						   relkind_or_hasrules, MAX_INFO_STRING, NULL);
	if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
	{
		stmt->errormsg = tbl_stmt->errormsg;
		stmt->errornumber = tbl_stmt->errornumber;
		SC_log_error(func, "", stmt);
		PGAPI_FreeStmt(htbl_stmt, SQL_DROP);
		return SQL_ERROR;
	}

	stmt->result = QR_Constructor();
	if (!stmt->result)
	{
		stmt->errormsg = "Couldn't allocate memory for PGAPI_Tables result.";
		stmt->errornumber = STMT_NO_MEMORY_ERROR;
		SC_log_error(func, "", stmt);
		PGAPI_FreeStmt(htbl_stmt, SQL_DROP);
		return SQL_ERROR;
	}

	/* the binding structure for a statement is not set up until */

	/*
	 * a statement is actually executed, so we'll have to do this
	 * ourselves.
	 */
	extend_bindings(stmt, 5);

	/* set the field names */
	QR_set_num_fields(stmt->result, 5);
	QR_set_field_info(stmt->result, 0, "TABLE_QUALIFIER", PG_TYPE_TEXT, MAX_INFO_STRING);
	QR_set_field_info(stmt->result, 1, "TABLE_OWNER", PG_TYPE_TEXT, MAX_INFO_STRING);
	QR_set_field_info(stmt->result, 2, "TABLE_NAME", PG_TYPE_TEXT, MAX_INFO_STRING);
	QR_set_field_info(stmt->result, 3, "TABLE_TYPE", PG_TYPE_TEXT, MAX_INFO_STRING);
	QR_set_field_info(stmt->result, 4, "REMARKS", PG_TYPE_TEXT, 254);

	/* add the tuples */
	result = PGAPI_Fetch(htbl_stmt);
	while ((result == SQL_SUCCESS) || (result == SQL_SUCCESS_WITH_INFO))
	{
		/*
		 * Determine if this table name is a system table. If treating
		 * system tables as regular tables, then no need to do this test.
		 */
		systable = FALSE;
		if (!atoi(ci->show_system_tables))
		{
			if (strncmp(table_name, POSTGRES_SYS_PREFIX, strlen(POSTGRES_SYS_PREFIX)) == 0)
				systable = TRUE;

			else
			{
				/* Check extra system table prefixes */
				i = 0;
				while (prefix[i])
				{
					mylog("table_name='%s', prefix[%d]='%s'\n", table_name, i, prefix[i]);
					if (strncmp(table_name, prefix[i], strlen(prefix[i])) == 0)
					{
						systable = TRUE;
						break;
					}
					i++;
				}
			}
		}

		/* Determine if the table name is a view */
		if (PG_VERSION_GE(conn, 7.1))
			/* view is represented by its relkind since 7.1 */
			view = (relkind_or_hasrules[0] == 'v');
		else
			view = (relkind_or_hasrules[0] == '1');

		/* It must be a regular table */
		regular_table = (!systable && !view);


		/* Include the row in the result set if meets all criteria */

		/*
		 * NOTE: Unsupported table types (i.e., LOCAL TEMPORARY, ALIAS,
		 * etc) will return nothing
		 */
		if ((systable && show_system_tables) ||
			(view && show_views) ||
			(regular_table && show_regular_tables))
		{
			row = (TupleNode *) malloc(sizeof(TupleNode) + (5 - 1) *sizeof(TupleField));

			set_tuplefield_string(&row->tuple[0], "");

			/*
			 * I have to hide the table owner from Access, otherwise it
			 * insists on referring to the table as 'owner.table'. (this
			 * is valid according to the ODBC SQL grammar, but Postgres
			 * won't support it.)
			 *
			 * set_tuplefield_string(&row->tuple[1], table_owner);
			 */

			mylog("%s: table_name = '%s'\n", func, table_name);

			set_tuplefield_string(&row->tuple[1], "");
			set_tuplefield_string(&row->tuple[2], table_name);
			set_tuplefield_string(&row->tuple[3], systable ? "SYSTEM TABLE" : (view ? "VIEW" : "TABLE"));
			set_tuplefield_string(&row->tuple[4], "");

			QR_add_tuple(stmt->result, row);
		}
		result = PGAPI_Fetch(htbl_stmt);
	}
	if (result != SQL_NO_DATA_FOUND)
	{
		stmt->errormsg = SC_create_errormsg(htbl_stmt);
		stmt->errornumber = tbl_stmt->errornumber;
		SC_log_error(func, "", stmt);
		PGAPI_FreeStmt(htbl_stmt, SQL_DROP);
		return SQL_ERROR;
	}

	/*
	 * also, things need to think that this statement is finished so the
	 * results can be retrieved.
	 */
	stmt->status = STMT_FINISHED;

	/* set up the current tuple pointer for SQLFetch */
	stmt->currTuple = -1;
	stmt->rowset_start = -1;
	stmt->current_col = -1;

	PGAPI_FreeStmt(htbl_stmt, SQL_DROP);
	mylog("%s: EXIT,  stmt=%u\n", func, stmt);
	return SQL_SUCCESS;
}


RETCODE		SQL_API
PGAPI_Columns(
			  HSTMT hstmt,
			  UCHAR FAR * szTableQualifier,
			  SWORD cbTableQualifier,
			  UCHAR FAR * szTableOwner,
			  SWORD cbTableOwner,
			  UCHAR FAR * szTableName,
			  SWORD cbTableName,
			  UCHAR FAR * szColumnName,
			  SWORD cbColumnName)
{
	static char *func = "PGAPI_Columns";
	StatementClass *stmt = (StatementClass *) hstmt;
	TupleNode  *row;
	HSTMT		hcol_stmt;
	StatementClass *col_stmt;
	char		columns_query[INFO_INQUIRY_LEN];
	RETCODE		result;
	char		table_owner[MAX_INFO_STRING],
				table_name[MAX_INFO_STRING],
				field_name[MAX_INFO_STRING],
				field_type_name[MAX_INFO_STRING];
	Int2		field_number,
				result_cols,
				scale;
	Int4		field_type,
				the_type,
				field_length,
				mod_length,
				precision;
	char		useStaticPrecision;
	char		not_null[MAX_INFO_STRING],
				relhasrules[MAX_INFO_STRING];
	ConnInfo   *ci;
	ConnectionClass *conn;


	mylog("%s: entering...stmt=%u\n", func, stmt);

	if (!stmt)
	{
		SC_log_error(func, "", NULL);
		return SQL_INVALID_HANDLE;
	}

	stmt->manual_result = TRUE;
	stmt->errormsg_created = TRUE;

	conn = SC_get_conn(stmt);
	ci = &(conn->connInfo);

	/*
	 * Create the query to find out the columns (Note: pre 6.3 did not
	 * have the atttypmod field)
	 */
	sprintf(columns_query, "select u.usename, c.relname, a.attname, a.atttypid"
	   ", t.typname, a.attnum, a.attlen, %s, a.attnotnull, c.relhasrules"
			" from pg_user u, pg_class c, pg_attribute a, pg_type t"
			" where u.usesysid = c.relowner"
	  " and c.oid= a.attrelid and a.atttypid = t.oid and (a.attnum > 0)",
			PG_VERSION_LE(conn, 6.2) ? "a.attlen" : "a.atttypmod");

	my_strcat(columns_query, " and c.relname like '%.*s'", szTableName, cbTableName);
	my_strcat(columns_query, " and u.usename like '%.*s'", szTableOwner, cbTableOwner);
	my_strcat(columns_query, " and a.attname like '%.*s'", szColumnName, cbColumnName);

	/*
	 * give the output in the order the columns were defined when the
	 * table was created
	 */
	strcat(columns_query, " order by attnum");

	result = PGAPI_AllocStmt(stmt->hdbc, &hcol_stmt);
	if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
	{
		stmt->errornumber = STMT_NO_MEMORY_ERROR;
		stmt->errormsg = "Couldn't allocate statement for PGAPI_Columns result.";
		SC_log_error(func, "", stmt);
		return SQL_ERROR;
	}
	col_stmt = (StatementClass *) hcol_stmt;

	mylog("%s: hcol_stmt = %u, col_stmt = %u\n", func, hcol_stmt, col_stmt);

	result = PGAPI_ExecDirect(hcol_stmt, columns_query,
							  strlen(columns_query));
	if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
	{
		stmt->errormsg = SC_create_errormsg(hcol_stmt);
		stmt->errornumber = col_stmt->errornumber;
		SC_log_error(func, "", stmt);
		PGAPI_FreeStmt(hcol_stmt, SQL_DROP);
		return SQL_ERROR;
	}

	result = PGAPI_BindCol(hcol_stmt, 1, SQL_C_CHAR,
						   table_owner, MAX_INFO_STRING, NULL);
	if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
	{
		stmt->errormsg = col_stmt->errormsg;
		stmt->errornumber = col_stmt->errornumber;
		SC_log_error(func, "", stmt);
		PGAPI_FreeStmt(hcol_stmt, SQL_DROP);
		return SQL_ERROR;
	}

	result = PGAPI_BindCol(hcol_stmt, 2, SQL_C_CHAR,
						   table_name, MAX_INFO_STRING, NULL);
	if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
	{
		stmt->errormsg = col_stmt->errormsg;
		stmt->errornumber = col_stmt->errornumber;
		SC_log_error(func, "", stmt);
		PGAPI_FreeStmt(hcol_stmt, SQL_DROP);
		return SQL_ERROR;
	}

	result = PGAPI_BindCol(hcol_stmt, 3, SQL_C_CHAR,
						   field_name, MAX_INFO_STRING, NULL);
	if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
	{
		stmt->errormsg = col_stmt->errormsg;
		stmt->errornumber = col_stmt->errornumber;
		SC_log_error(func, "", stmt);
		PGAPI_FreeStmt(hcol_stmt, SQL_DROP);
		return SQL_ERROR;
	}

	result = PGAPI_BindCol(hcol_stmt, 4, SQL_C_LONG,
						   &field_type, 4, NULL);
	if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
	{
		stmt->errormsg = col_stmt->errormsg;
		stmt->errornumber = col_stmt->errornumber;
		SC_log_error(func, "", stmt);
		PGAPI_FreeStmt(hcol_stmt, SQL_DROP);
		return SQL_ERROR;
	}

	result = PGAPI_BindCol(hcol_stmt, 5, SQL_C_CHAR,
						   field_type_name, MAX_INFO_STRING, NULL);
	if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
	{
		stmt->errormsg = col_stmt->errormsg;
		stmt->errornumber = col_stmt->errornumber;
		SC_log_error(func, "", stmt);
		PGAPI_FreeStmt(hcol_stmt, SQL_DROP);
		return SQL_ERROR;
	}

	result = PGAPI_BindCol(hcol_stmt, 6, SQL_C_SHORT,
						   &field_number, MAX_INFO_STRING, NULL);
	if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
	{
		stmt->errormsg = col_stmt->errormsg;
		stmt->errornumber = col_stmt->errornumber;
		SC_log_error(func, "", stmt);
		PGAPI_FreeStmt(hcol_stmt, SQL_DROP);
		return SQL_ERROR;
	}

	result = PGAPI_BindCol(hcol_stmt, 7, SQL_C_LONG,
						   &field_length, MAX_INFO_STRING, NULL);
	if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
	{
		stmt->errormsg = col_stmt->errormsg;
		stmt->errornumber = col_stmt->errornumber;
		SC_log_error(func, "", stmt);
		PGAPI_FreeStmt(hcol_stmt, SQL_DROP);
		return SQL_ERROR;
	}

	result = PGAPI_BindCol(hcol_stmt, 8, SQL_C_LONG,
						   &mod_length, MAX_INFO_STRING, NULL);
	if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
	{
		stmt->errormsg = col_stmt->errormsg;
		stmt->errornumber = col_stmt->errornumber;
		SC_log_error(func, "", stmt);
		PGAPI_FreeStmt(hcol_stmt, SQL_DROP);
		return SQL_ERROR;
	}

	result = PGAPI_BindCol(hcol_stmt, 9, SQL_C_CHAR,
						   not_null, MAX_INFO_STRING, NULL);
	if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
	{
		stmt->errormsg = col_stmt->errormsg;
		stmt->errornumber = col_stmt->errornumber;
		SC_log_error(func, "", stmt);
		PGAPI_FreeStmt(hcol_stmt, SQL_DROP);
		return SQL_ERROR;
	}

	result = PGAPI_BindCol(hcol_stmt, 10, SQL_C_CHAR,
						   relhasrules, MAX_INFO_STRING, NULL);
	if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
	{
		stmt->errormsg = col_stmt->errormsg;
		stmt->errornumber = col_stmt->errornumber;
		SC_log_error(func, "", stmt);
		PGAPI_FreeStmt(hcol_stmt, SQL_DROP);
		return SQL_ERROR;
	}

	stmt->result = QR_Constructor();
	if (!stmt->result)
	{
		stmt->errormsg = "Couldn't allocate memory for PGAPI_Columns result.";
		stmt->errornumber = STMT_NO_MEMORY_ERROR;
		SC_log_error(func, "", stmt);
		PGAPI_FreeStmt(hcol_stmt, SQL_DROP);
		return SQL_ERROR;
	}

	/* the binding structure for a statement is not set up until */

	/*
	 * a statement is actually executed, so we'll have to do this
	 * ourselves.
	 */
	result_cols = 14;
	extend_bindings(stmt, result_cols);

	/* set the field names */
	QR_set_num_fields(stmt->result, result_cols);
	QR_set_field_info(stmt->result, 0, "TABLE_QUALIFIER", PG_TYPE_TEXT, MAX_INFO_STRING);
	QR_set_field_info(stmt->result, 1, "TABLE_OWNER", PG_TYPE_TEXT, MAX_INFO_STRING);
	QR_set_field_info(stmt->result, 2, "TABLE_NAME", PG_TYPE_TEXT, MAX_INFO_STRING);
	QR_set_field_info(stmt->result, 3, "COLUMN_NAME", PG_TYPE_TEXT, MAX_INFO_STRING);
	QR_set_field_info(stmt->result, 4, "DATA_TYPE", PG_TYPE_INT2, 2);
	QR_set_field_info(stmt->result, 5, "TYPE_NAME", PG_TYPE_TEXT, MAX_INFO_STRING);
	QR_set_field_info(stmt->result, 6, "PRECISION", PG_TYPE_INT4, 4);
	QR_set_field_info(stmt->result, 7, "LENGTH", PG_TYPE_INT4, 4);
	QR_set_field_info(stmt->result, 8, "SCALE", PG_TYPE_INT2, 2);
	QR_set_field_info(stmt->result, 9, "RADIX", PG_TYPE_INT2, 2);
	QR_set_field_info(stmt->result, 10, "NULLABLE", PG_TYPE_INT2, 2);
	QR_set_field_info(stmt->result, 11, "REMARKS", PG_TYPE_TEXT, 254);

	/* User defined fields */
	QR_set_field_info(stmt->result, 12, "DISPLAY_SIZE", PG_TYPE_INT4, 4);
	QR_set_field_info(stmt->result, 13, "FIELD_TYPE", PG_TYPE_INT4, 4);

	result = PGAPI_Fetch(hcol_stmt);

	/*
	 * Only show oid if option AND there are other columns AND it's not
	 * being called by SQLStatistics . Always show OID if it's a system
	 * table
	 */

	if (result != SQL_ERROR && !stmt->internal)
	{
		if (relhasrules[0] != '1' &&
			(atoi(ci->show_oid_column) ||
			 strncmp(table_name, POSTGRES_SYS_PREFIX, strlen(POSTGRES_SYS_PREFIX)) == 0))
		{
			/* For OID fields */
			the_type = PG_TYPE_OID;
			row = (TupleNode *) malloc(sizeof(TupleNode) +
								  (result_cols - 1) *sizeof(TupleField));

			set_tuplefield_string(&row->tuple[0], "");
			/* see note in SQLTables() */
			/* set_tuplefield_string(&row->tuple[1], table_owner); */
			set_tuplefield_string(&row->tuple[1], "");
			set_tuplefield_string(&row->tuple[2], table_name);
			set_tuplefield_string(&row->tuple[3], "oid");
			set_tuplefield_int2(&row->tuple[4], pgtype_to_sqltype(stmt, the_type));
			set_tuplefield_string(&row->tuple[5], "OID");

			set_tuplefield_int4(&row->tuple[7], pgtype_length(stmt, the_type, PG_STATIC, PG_STATIC));
			set_tuplefield_int4(&row->tuple[6], pgtype_precision(stmt, the_type, PG_STATIC, PG_STATIC));

			set_nullfield_int2(&row->tuple[8], pgtype_scale(stmt, the_type, PG_STATIC));
			set_nullfield_int2(&row->tuple[9], pgtype_radix(stmt, the_type));
			set_tuplefield_int2(&row->tuple[10], SQL_NO_NULLS);
			set_tuplefield_string(&row->tuple[11], "");

			set_tuplefield_int4(&row->tuple[12], pgtype_display_size(stmt, the_type, PG_STATIC, PG_STATIC));
			set_tuplefield_int4(&row->tuple[13], the_type);

			QR_add_tuple(stmt->result, row);
		}
	}

	while ((result == SQL_SUCCESS) || (result == SQL_SUCCESS_WITH_INFO))
	{
		row = (TupleNode *) malloc(sizeof(TupleNode) +
								   (result_cols - 1) *sizeof(TupleField));


		set_tuplefield_string(&row->tuple[0], "");
		/* see note in SQLTables() */
		/* set_tuplefield_string(&row->tuple[1], table_owner); */
		set_tuplefield_string(&row->tuple[1], "");
		set_tuplefield_string(&row->tuple[2], table_name);
		set_tuplefield_string(&row->tuple[3], field_name);
		set_tuplefield_int2(&row->tuple[4], pgtype_to_sqltype(stmt, field_type));
		set_tuplefield_string(&row->tuple[5], field_type_name);


		/*----------
		 * Some Notes about Postgres Data Types:
		 *
		 * VARCHAR - the length is stored in the pg_attribute.atttypmod field
		 * BPCHAR  - the length is also stored as varchar is
		 *
		 * NUMERIC - the scale is stored in atttypmod as follows:
		 *
		 *	precision =((atttypmod - VARHDRSZ) >> 16) & 0xffff
		 *	scale	 = (atttypmod - VARHDRSZ) & 0xffff
		 *
		 *----------
		 */
		qlog("PGAPI_Columns: table='%s',field_name='%s',type=%d,sqltype=%d,name='%s'\n",
			 table_name, field_name, field_type, pgtype_to_sqltype, field_type_name);

		useStaticPrecision = TRUE;

		if (field_type == PG_TYPE_NUMERIC)
		{
			if (mod_length >= 4)
				mod_length -= 4;	/* the length is in atttypmod - 4 */

			if (mod_length >= 0)
			{
				useStaticPrecision = FALSE;

				precision = (mod_length >> 16) & 0xffff;
				scale = mod_length & 0xffff;

				mylog("%s: field type is NUMERIC: field_type = %d, mod_length=%d, precision=%d, scale=%d\n", func, field_type, mod_length, precision, scale);

				set_tuplefield_int4(&row->tuple[7], precision + 2);		/* sign+dec.point */
				set_tuplefield_int4(&row->tuple[6], precision);
				set_tuplefield_int4(&row->tuple[12], precision + 2);	/* sign+dec.point */
				set_nullfield_int2(&row->tuple[8], scale);
			}
		}

		if ((field_type == PG_TYPE_VARCHAR) ||
			(field_type == PG_TYPE_BPCHAR))
		{
			useStaticPrecision = FALSE;

			if (mod_length >= 4)
				mod_length -= 4;	/* the length is in atttypmod - 4 */

			if (mod_length > ci->drivers.max_varchar_size || mod_length <= 0)
				mod_length = ci->drivers.max_varchar_size;

			mylog("%s: field type is VARCHAR,BPCHAR: field_type = %d, mod_length = %d\n", func, field_type, mod_length);

			set_tuplefield_int4(&row->tuple[7], mod_length);
			set_tuplefield_int4(&row->tuple[6], mod_length);
			set_tuplefield_int4(&row->tuple[12], mod_length);
			set_nullfield_int2(&row->tuple[8], pgtype_scale(stmt, field_type, PG_STATIC));
		}

		if (useStaticPrecision)
		{
			mylog("%s: field type is OTHER: field_type = %d, pgtype_length = %d\n", func, field_type, pgtype_length(stmt, field_type, PG_STATIC, PG_STATIC));

			set_tuplefield_int4(&row->tuple[7], pgtype_length(stmt, field_type, PG_STATIC, PG_STATIC));
			set_tuplefield_int4(&row->tuple[6], pgtype_precision(stmt, field_type, PG_STATIC, PG_STATIC));
			set_tuplefield_int4(&row->tuple[12], pgtype_display_size(stmt, field_type, PG_STATIC, PG_STATIC));
			set_nullfield_int2(&row->tuple[8], pgtype_scale(stmt, field_type, PG_STATIC));
		}

		set_nullfield_int2(&row->tuple[9], pgtype_radix(stmt, field_type));
		set_tuplefield_int2(&row->tuple[10], (Int2) (not_null[0] == '1' ? SQL_NO_NULLS : pgtype_nullable(stmt, field_type)));
		set_tuplefield_string(&row->tuple[11], "");
		set_tuplefield_int4(&row->tuple[13], field_type);

		QR_add_tuple(stmt->result, row);


		result = PGAPI_Fetch(hcol_stmt);

	}
	if (result != SQL_NO_DATA_FOUND)
	{
		stmt->errormsg = SC_create_errormsg(hcol_stmt);
		stmt->errornumber = col_stmt->errornumber;
		SC_log_error(func, "", stmt);
		PGAPI_FreeStmt(hcol_stmt, SQL_DROP);
		return SQL_ERROR;
	}

	/*
	 * Put the row version column at the end so it might not be mistaken
	 * for a key field.
	 */
	if (relhasrules[0] != '1' && !stmt->internal && atoi(ci->row_versioning))
	{
		/* For Row Versioning fields */
		the_type = PG_TYPE_INT4;

		row = (TupleNode *) malloc(sizeof(TupleNode) +
								   (result_cols - 1) *sizeof(TupleField));

		set_tuplefield_string(&row->tuple[0], "");
		set_tuplefield_string(&row->tuple[1], "");
		set_tuplefield_string(&row->tuple[2], table_name);
		set_tuplefield_string(&row->tuple[3], "xmin");
		set_tuplefield_int2(&row->tuple[4], pgtype_to_sqltype(stmt, the_type));
		set_tuplefield_string(&row->tuple[5], pgtype_to_name(stmt, the_type));
		set_tuplefield_int4(&row->tuple[6], pgtype_precision(stmt, the_type, PG_STATIC, PG_STATIC));
		set_tuplefield_int4(&row->tuple[7], pgtype_length(stmt, the_type, PG_STATIC, PG_STATIC));
		set_nullfield_int2(&row->tuple[8], pgtype_scale(stmt, the_type, PG_STATIC));
		set_nullfield_int2(&row->tuple[9], pgtype_radix(stmt, the_type));
		set_tuplefield_int2(&row->tuple[10], SQL_NO_NULLS);
		set_tuplefield_string(&row->tuple[11], "");
		set_tuplefield_int4(&row->tuple[12], pgtype_display_size(stmt, the_type, PG_STATIC, PG_STATIC));
		set_tuplefield_int4(&row->tuple[13], the_type);

		QR_add_tuple(stmt->result, row);
	}

	/*
	 * also, things need to think that this statement is finished so the
	 * results can be retrieved.
	 */
	stmt->status = STMT_FINISHED;

	/* set up the current tuple pointer for SQLFetch */
	stmt->currTuple = -1;
	stmt->rowset_start = -1;
	stmt->current_col = -1;

	PGAPI_FreeStmt(hcol_stmt, SQL_DROP);
	mylog("%s: EXIT,  stmt=%u\n", func, stmt);
	return SQL_SUCCESS;
}


RETCODE		SQL_API
PGAPI_SpecialColumns(
					 HSTMT hstmt,
					 UWORD fColType,
					 UCHAR FAR * szTableQualifier,
					 SWORD cbTableQualifier,
					 UCHAR FAR * szTableOwner,
					 SWORD cbTableOwner,
					 UCHAR FAR * szTableName,
					 SWORD cbTableName,
					 UWORD fScope,
					 UWORD fNullable)
{
	static char *func = "PGAPI_SpecialColumns";
	TupleNode  *row;
	StatementClass *stmt = (StatementClass *) hstmt;
	ConnInfo   *ci;
	HSTMT		hcol_stmt;
	StatementClass *col_stmt;
	char		columns_query[INFO_INQUIRY_LEN];
	RETCODE		result;
	char		relhasrules[MAX_INFO_STRING];

	mylog("%s: entering...stmt=%u\n", func, stmt);

	if (!stmt)
	{
		SC_log_error(func, "", NULL);
		return SQL_INVALID_HANDLE;
	}
	ci = &(SC_get_conn(stmt)->connInfo);

	stmt->manual_result = TRUE;

	/*
	 * Create the query to find out if this is a view or not...
	 */
	sprintf(columns_query, "select c.relhasrules "
			"from pg_user u, pg_class c where "
			"u.usesysid = c.relowner");

	my_strcat(columns_query, " and c.relname like '%.*s'", szTableName, cbTableName);
	my_strcat(columns_query, " and u.usename like '%.*s'", szTableOwner, cbTableOwner);


	result = PGAPI_AllocStmt(stmt->hdbc, &hcol_stmt);
	if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
	{
		stmt->errornumber = STMT_NO_MEMORY_ERROR;
		stmt->errormsg = "Couldn't allocate statement for SQLSpecialColumns result.";
		SC_log_error(func, "", stmt);
		return SQL_ERROR;
	}
	col_stmt = (StatementClass *) hcol_stmt;

	mylog("%s: hcol_stmt = %u, col_stmt = %u\n", func, hcol_stmt, col_stmt);

	result = PGAPI_ExecDirect(hcol_stmt, columns_query,
							  strlen(columns_query));
	if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
	{
		stmt->errormsg = SC_create_errormsg(hcol_stmt);
		stmt->errornumber = col_stmt->errornumber;
		SC_log_error(func, "", stmt);
		PGAPI_FreeStmt(hcol_stmt, SQL_DROP);
		return SQL_ERROR;
	}

	result = PGAPI_BindCol(hcol_stmt, 1, SQL_C_CHAR,
						   relhasrules, MAX_INFO_STRING, NULL);
	if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
	{
		stmt->errormsg = col_stmt->errormsg;
		stmt->errornumber = col_stmt->errornumber;
		SC_log_error(func, "", stmt);
		PGAPI_FreeStmt(hcol_stmt, SQL_DROP);
		return SQL_ERROR;
	}

	result = PGAPI_Fetch(hcol_stmt);
	PGAPI_FreeStmt(hcol_stmt, SQL_DROP);

	stmt->result = QR_Constructor();
	extend_bindings(stmt, 8);

	QR_set_num_fields(stmt->result, 8);
	QR_set_field_info(stmt->result, 0, "SCOPE", PG_TYPE_INT2, 2);
	QR_set_field_info(stmt->result, 1, "COLUMN_NAME", PG_TYPE_TEXT, MAX_INFO_STRING);
	QR_set_field_info(stmt->result, 2, "DATA_TYPE", PG_TYPE_INT2, 2);
	QR_set_field_info(stmt->result, 3, "TYPE_NAME", PG_TYPE_TEXT, MAX_INFO_STRING);
	QR_set_field_info(stmt->result, 4, "PRECISION", PG_TYPE_INT4, 4);
	QR_set_field_info(stmt->result, 5, "LENGTH", PG_TYPE_INT4, 4);
	QR_set_field_info(stmt->result, 6, "SCALE", PG_TYPE_INT2, 2);
	QR_set_field_info(stmt->result, 7, "PSEUDO_COLUMN", PG_TYPE_INT2, 2);

	if (relhasrules[0] != '1')
	{
		/* use the oid value for the rowid */
		if (fColType == SQL_BEST_ROWID)
		{
			row = (TupleNode *) malloc(sizeof(TupleNode) + (8 - 1) *sizeof(TupleField));

			set_tuplefield_int2(&row->tuple[0], SQL_SCOPE_SESSION);
			set_tuplefield_string(&row->tuple[1], "oid");
			set_tuplefield_int2(&row->tuple[2], pgtype_to_sqltype(stmt, PG_TYPE_OID));
			set_tuplefield_string(&row->tuple[3], "OID");
			set_tuplefield_int4(&row->tuple[4], pgtype_precision(stmt, PG_TYPE_OID, PG_STATIC, PG_STATIC));
			set_tuplefield_int4(&row->tuple[5], pgtype_length(stmt, PG_TYPE_OID, PG_STATIC, PG_STATIC));
			set_tuplefield_int2(&row->tuple[6], pgtype_scale(stmt, PG_TYPE_OID, PG_STATIC));
			set_tuplefield_int2(&row->tuple[7], SQL_PC_PSEUDO);

			QR_add_tuple(stmt->result, row);

		}
		else if (fColType == SQL_ROWVER)
		{
			Int2		the_type = PG_TYPE_INT4;

			if (atoi(ci->row_versioning))
			{
				row = (TupleNode *) malloc(sizeof(TupleNode) + (8 - 1) *sizeof(TupleField));

				set_tuplefield_null(&row->tuple[0]);
				set_tuplefield_string(&row->tuple[1], "xmin");
				set_tuplefield_int2(&row->tuple[2], pgtype_to_sqltype(stmt, the_type));
				set_tuplefield_string(&row->tuple[3], pgtype_to_name(stmt, the_type));
				set_tuplefield_int4(&row->tuple[4], pgtype_precision(stmt, the_type, PG_STATIC, PG_STATIC));
				set_tuplefield_int4(&row->tuple[5], pgtype_length(stmt, the_type, PG_STATIC, PG_STATIC));
				set_tuplefield_int2(&row->tuple[6], pgtype_scale(stmt, the_type, PG_STATIC));
				set_tuplefield_int2(&row->tuple[7], SQL_PC_PSEUDO);

				QR_add_tuple(stmt->result, row);
			}
		}
	}

	stmt->status = STMT_FINISHED;
	stmt->currTuple = -1;
	stmt->rowset_start = -1;
	stmt->current_col = -1;

	mylog("%s: EXIT,  stmt=%u\n", func, stmt);
	return SQL_SUCCESS;
}


RETCODE		SQL_API
PGAPI_Statistics(
				 HSTMT hstmt,
				 UCHAR FAR * szTableQualifier,
				 SWORD cbTableQualifier,
				 UCHAR FAR * szTableOwner,
				 SWORD cbTableOwner,
				 UCHAR FAR * szTableName,
				 SWORD cbTableName,
				 UWORD fUnique,
				 UWORD fAccuracy)
{
	static char *func = "PGAPI_Statistics";
	StatementClass *stmt = (StatementClass *) hstmt;
	char		index_query[INFO_INQUIRY_LEN];
	HSTMT		hindx_stmt;
	RETCODE		result;
	char	   *table_name;
	char		index_name[MAX_INFO_STRING];
	short		fields_vector[16];
	char		isunique[10],
				isclustered[10],
				ishash[MAX_INFO_STRING];
	SDWORD		index_name_len,
				fields_vector_len;
	TupleNode  *row;
	int			i;
	HSTMT		hcol_stmt;
	StatementClass *col_stmt,
			   *indx_stmt;
	char		column_name[MAX_INFO_STRING],
				relhasrules[MAX_INFO_STRING];
	char	  **column_names = 0;
	SQLINTEGER	column_name_len;
	int			total_columns = 0;
	char		error = TRUE;
	ConnInfo   *ci;
	char		buf[256];

	mylog("%s: entering...stmt=%u\n", func, stmt);

	if (!stmt)
	{
		SC_log_error(func, "", NULL);
		return SQL_INVALID_HANDLE;
	}

	stmt->manual_result = TRUE;
	stmt->errormsg_created = TRUE;

	ci = &(SC_get_conn(stmt)->connInfo);

	stmt->result = QR_Constructor();
	if (!stmt->result)
	{
		stmt->errormsg = "Couldn't allocate memory for PGAPI_Statistics result.";
		stmt->errornumber = STMT_NO_MEMORY_ERROR;
		SC_log_error(func, "", stmt);
		return SQL_ERROR;
	}

	/* the binding structure for a statement is not set up until */

	/*
	 * a statement is actually executed, so we'll have to do this
	 * ourselves.
	 */
	extend_bindings(stmt, 13);

	/* set the field names */
	QR_set_num_fields(stmt->result, 13);
	QR_set_field_info(stmt->result, 0, "TABLE_QUALIFIER", PG_TYPE_TEXT, MAX_INFO_STRING);
	QR_set_field_info(stmt->result, 1, "TABLE_OWNER", PG_TYPE_TEXT, MAX_INFO_STRING);
	QR_set_field_info(stmt->result, 2, "TABLE_NAME", PG_TYPE_TEXT, MAX_INFO_STRING);
	QR_set_field_info(stmt->result, 3, "NON_UNIQUE", PG_TYPE_INT2, 2);
	QR_set_field_info(stmt->result, 4, "INDEX_QUALIFIER", PG_TYPE_TEXT, MAX_INFO_STRING);
	QR_set_field_info(stmt->result, 5, "INDEX_NAME", PG_TYPE_TEXT, MAX_INFO_STRING);
	QR_set_field_info(stmt->result, 6, "TYPE", PG_TYPE_INT2, 2);
	QR_set_field_info(stmt->result, 7, "SEQ_IN_INDEX", PG_TYPE_INT2, 2);
	QR_set_field_info(stmt->result, 8, "COLUMN_NAME", PG_TYPE_TEXT, MAX_INFO_STRING);
	QR_set_field_info(stmt->result, 9, "COLLATION", PG_TYPE_CHAR, 1);
	QR_set_field_info(stmt->result, 10, "CARDINALITY", PG_TYPE_INT4, 4);
	QR_set_field_info(stmt->result, 11, "PAGES", PG_TYPE_INT4, 4);
	QR_set_field_info(stmt->result, 12, "FILTER_CONDITION", PG_TYPE_TEXT, MAX_INFO_STRING);

	/*
	 * only use the table name... the owner should be redundant, and we
	 * never use qualifiers.
	 */
	table_name = make_string(szTableName, cbTableName, NULL);
	if (!table_name)
	{
		stmt->errormsg = "No table name passed to PGAPI_Statistics.";
		stmt->errornumber = STMT_INTERNAL_ERROR;
		SC_log_error(func, "", stmt);
		return SQL_ERROR;
	}

	/*
	 * we need to get a list of the field names first, so we can return
	 * them later.
	 */
	result = PGAPI_AllocStmt(stmt->hdbc, &hcol_stmt);
	if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
	{
		stmt->errormsg = "PGAPI_AllocStmt failed in PGAPI_Statistics for columns.";
		stmt->errornumber = STMT_NO_MEMORY_ERROR;
		goto SEEYA;
	}

	col_stmt = (StatementClass *) hcol_stmt;

	/*
	 * "internal" prevents SQLColumns from returning the oid if it is
	 * being shown. This would throw everything off.
	 */
	col_stmt->internal = TRUE;
	result = PGAPI_Columns(hcol_stmt, "", 0, "", 0,
						   table_name, (SWORD) strlen(table_name), "", 0);
	col_stmt->internal = FALSE;

	if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
	{
		stmt->errormsg = col_stmt->errormsg;	/* "SQLColumns failed in
												 * SQLStatistics."; */
		stmt->errornumber = col_stmt->errornumber;		/* STMT_EXEC_ERROR; */
		PGAPI_FreeStmt(hcol_stmt, SQL_DROP);
		goto SEEYA;
	}
	result = PGAPI_BindCol(hcol_stmt, 4, SQL_C_CHAR,
						 column_name, MAX_INFO_STRING, &column_name_len);
	if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
	{
		stmt->errormsg = col_stmt->errormsg;
		stmt->errornumber = col_stmt->errornumber;
		PGAPI_FreeStmt(hcol_stmt, SQL_DROP);
		goto SEEYA;

	}

	result = PGAPI_Fetch(hcol_stmt);
	while ((result == SQL_SUCCESS) || (result == SQL_SUCCESS_WITH_INFO))
	{
		total_columns++;

		column_names =
			(char **) realloc(column_names,
							  total_columns * sizeof(char *));
		column_names[total_columns - 1] =
			(char *) malloc(strlen(column_name) + 1);
		strcpy(column_names[total_columns - 1], column_name);

		mylog("%s: column_name = '%s'\n", func, column_name);

		result = PGAPI_Fetch(hcol_stmt);
	}

	if (result != SQL_NO_DATA_FOUND || total_columns == 0)
	{
		stmt->errormsg = SC_create_errormsg(hcol_stmt); /* "Couldn't get column
														 * names in
														 * SQLStatistics."; */
		stmt->errornumber = col_stmt->errornumber;
		PGAPI_FreeStmt(hcol_stmt, SQL_DROP);
		goto SEEYA;

	}

	PGAPI_FreeStmt(hcol_stmt, SQL_DROP);

	/* get a list of indexes on this table */
	result = PGAPI_AllocStmt(stmt->hdbc, &hindx_stmt);
	if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
	{
		stmt->errormsg = "PGAPI_AllocStmt failed in SQLStatistics for indices.";
		stmt->errornumber = STMT_NO_MEMORY_ERROR;
		goto SEEYA;

	}
	indx_stmt = (StatementClass *) hindx_stmt;

	sprintf(index_query, "select c.relname, i.indkey, i.indisunique"
			", i.indisclustered, a.amname, c.relhasrules"
			" from pg_index i, pg_class c, pg_class d, pg_am a"
			" where d.relname = '%s'"
			" and d.oid = i.indrelid"
			" and i.indexrelid = c.oid"
			" and c.relam = a.oid"
			,table_name);
	if (PG_VERSION_GT(SC_get_conn(stmt), 6.4))
		strcat(index_query, " order by i.indisprimary desc");

	result = PGAPI_ExecDirect(hindx_stmt, index_query, strlen(index_query));
	if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
	{
		/*
		 * "Couldn't execute index query (w/SQLExecDirect) in
		 * SQLStatistics.";
		 */
		stmt->errormsg = SC_create_errormsg(hindx_stmt);

		stmt->errornumber = indx_stmt->errornumber;
		PGAPI_FreeStmt(hindx_stmt, SQL_DROP);
		goto SEEYA;
	}

	/* bind the index name column */
	result = PGAPI_BindCol(hindx_stmt, 1, SQL_C_CHAR,
						   index_name, MAX_INFO_STRING, &index_name_len);
	if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
	{
		stmt->errormsg = indx_stmt->errormsg;	/* "Couldn't bind column
												 * in SQLStatistics."; */
		stmt->errornumber = indx_stmt->errornumber;
		PGAPI_FreeStmt(hindx_stmt, SQL_DROP);
		goto SEEYA;

	}
	/* bind the vector column */
	result = PGAPI_BindCol(hindx_stmt, 2, SQL_C_DEFAULT,
						   fields_vector, 32, &fields_vector_len);
	if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
	{
		stmt->errormsg = indx_stmt->errormsg;	/* "Couldn't bind column
												 * in SQLStatistics."; */
		stmt->errornumber = indx_stmt->errornumber;
		PGAPI_FreeStmt(hindx_stmt, SQL_DROP);
		goto SEEYA;

	}
	/* bind the "is unique" column */
	result = PGAPI_BindCol(hindx_stmt, 3, SQL_C_CHAR,
						   isunique, sizeof(isunique), NULL);
	if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
	{
		stmt->errormsg = indx_stmt->errormsg;	/* "Couldn't bind column
												 * in SQLStatistics."; */
		stmt->errornumber = indx_stmt->errornumber;
		PGAPI_FreeStmt(hindx_stmt, SQL_DROP);
		goto SEEYA;
	}

	/* bind the "is clustered" column */
	result = PGAPI_BindCol(hindx_stmt, 4, SQL_C_CHAR,
						   isclustered, sizeof(isclustered), NULL);
	if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
	{
		stmt->errormsg = indx_stmt->errormsg;	/* "Couldn't bind column
												 * in SQLStatistics."; */
		stmt->errornumber = indx_stmt->errornumber;
		PGAPI_FreeStmt(hindx_stmt, SQL_DROP);
		goto SEEYA;

	}

	/* bind the "is hash" column */
	result = PGAPI_BindCol(hindx_stmt, 5, SQL_C_CHAR,
						   ishash, sizeof(ishash), NULL);
	if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
	{
		stmt->errormsg = indx_stmt->errormsg;	/* "Couldn't bind column
												 * in SQLStatistics."; */
		stmt->errornumber = indx_stmt->errornumber;
		PGAPI_FreeStmt(hindx_stmt, SQL_DROP);
		goto SEEYA;

	}

	result = PGAPI_BindCol(hindx_stmt, 6, SQL_C_CHAR,
						   relhasrules, MAX_INFO_STRING, NULL);
	if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
	{
		stmt->errormsg = indx_stmt->errormsg;
		stmt->errornumber = indx_stmt->errornumber;
		PGAPI_FreeStmt(hindx_stmt, SQL_DROP);
		goto SEEYA;
	}

	/* fake index of OID */
	if (relhasrules[0] != '1' && atoi(ci->show_oid_column) && atoi(ci->fake_oid_index))
	{
		row = (TupleNode *) malloc(sizeof(TupleNode) +
								   (13 - 1) *sizeof(TupleField));

		/* no table qualifier */
		set_tuplefield_string(&row->tuple[0], "");
		/* don't set the table owner, else Access tries to use it */
		set_tuplefield_string(&row->tuple[1], "");
		set_tuplefield_string(&row->tuple[2], table_name);

		/* non-unique index? */
		set_tuplefield_int2(&row->tuple[3], (Int2) (ci->drivers.unique_index ? FALSE : TRUE));

		/* no index qualifier */
		set_tuplefield_string(&row->tuple[4], "");

		sprintf(buf, "%s_idx_fake_oid", table_name);
		set_tuplefield_string(&row->tuple[5], buf);

		/*
		 * Clustered/HASH index?
		 */
		set_tuplefield_int2(&row->tuple[6], (Int2) SQL_INDEX_OTHER);
		set_tuplefield_int2(&row->tuple[7], (Int2) 1);

		set_tuplefield_string(&row->tuple[8], "oid");
		set_tuplefield_string(&row->tuple[9], "A");
		set_tuplefield_null(&row->tuple[10]);
		set_tuplefield_null(&row->tuple[11]);
		set_tuplefield_null(&row->tuple[12]);

		QR_add_tuple(stmt->result, row);
	}

	result = PGAPI_Fetch(hindx_stmt);
	while ((result == SQL_SUCCESS) || (result == SQL_SUCCESS_WITH_INFO))
	{
		/* If only requesting unique indexs, then just return those. */
		if (fUnique == SQL_INDEX_ALL ||
			(fUnique == SQL_INDEX_UNIQUE && atoi(isunique)))
		{
			i = 0;
			/* add a row in this table for each field in the index */
			while (i < 16 && fields_vector[i] != 0)
			{
				row = (TupleNode *) malloc(sizeof(TupleNode) +
										   (13 - 1) *sizeof(TupleField));

				/* no table qualifier */
				set_tuplefield_string(&row->tuple[0], "");
				/* don't set the table owner, else Access tries to use it */
				set_tuplefield_string(&row->tuple[1], "");
				set_tuplefield_string(&row->tuple[2], table_name);

				/* non-unique index? */
				if (ci->drivers.unique_index)
					set_tuplefield_int2(&row->tuple[3], (Int2) (atoi(isunique) ? FALSE : TRUE));
				else
					set_tuplefield_int2(&row->tuple[3], TRUE);

				/* no index qualifier */
				set_tuplefield_string(&row->tuple[4], "");
				set_tuplefield_string(&row->tuple[5], index_name);

				/*
				 * Clustered/HASH index?
				 */
				set_tuplefield_int2(&row->tuple[6], (Int2)
							   (atoi(isclustered) ? SQL_INDEX_CLUSTERED :
								(!strncmp(ishash, "hash", 4)) ? SQL_INDEX_HASHED : SQL_INDEX_OTHER));
				set_tuplefield_int2(&row->tuple[7], (Int2) (i + 1));

				if (fields_vector[i] == OID_ATTNUM)
				{
					set_tuplefield_string(&row->tuple[8], "oid");
					mylog("%s: column name = oid\n", func);
				}
				else if (fields_vector[i] < 0 || fields_vector[i] > total_columns)
				{
					set_tuplefield_string(&row->tuple[8], "UNKNOWN");
					mylog("%s: column name = UNKNOWN\n", func);
				}
				else
				{
					set_tuplefield_string(&row->tuple[8], column_names[fields_vector[i] - 1]);
					mylog("%s: column name = '%s'\n", func, column_names[fields_vector[i] - 1]);
				}

				set_tuplefield_string(&row->tuple[9], "A");
				set_tuplefield_null(&row->tuple[10]);
				set_tuplefield_null(&row->tuple[11]);
				set_tuplefield_null(&row->tuple[12]);

				QR_add_tuple(stmt->result, row);
				i++;
			}
		}

		result = PGAPI_Fetch(hindx_stmt);
	}
	if (result != SQL_NO_DATA_FOUND)
	{
		/* "SQLFetch failed in SQLStatistics."; */
		stmt->errormsg = SC_create_errormsg(hindx_stmt);
		stmt->errornumber = indx_stmt->errornumber;
		PGAPI_FreeStmt(hindx_stmt, SQL_DROP);
		goto SEEYA;
	}

	PGAPI_FreeStmt(hindx_stmt, SQL_DROP);

	/*
	 * also, things need to think that this statement is finished so the
	 * results can be retrieved.
	 */
	stmt->status = STMT_FINISHED;

	/* set up the current tuple pointer for SQLFetch */
	stmt->currTuple = -1;
	stmt->rowset_start = -1;
	stmt->current_col = -1;

	error = FALSE;

SEEYA:
	/* These things should be freed on any error ALSO! */
	free(table_name);
	for (i = 0; i < total_columns; i++)
		free(column_names[i]);
	free(column_names);

	mylog("%s: EXIT, %s, stmt=%u\n", func, error ? "error" : "success", stmt);

	if (error)
	{
		SC_log_error(func, "", stmt);
		return SQL_ERROR;
	}
	else
		return SQL_SUCCESS;
}


RETCODE		SQL_API
PGAPI_ColumnPrivileges(
					   HSTMT hstmt,
					   UCHAR FAR * szTableQualifier,
					   SWORD cbTableQualifier,
					   UCHAR FAR * szTableOwner,
					   SWORD cbTableOwner,
					   UCHAR FAR * szTableName,
					   SWORD cbTableName,
					   UCHAR FAR * szColumnName,
					   SWORD cbColumnName)
{
	static char *func = "PGAPI_ColumnPrivileges";

	mylog("%s: entering...\n", func);

	/* Neither Access or Borland care about this. */

	SC_log_error(func, "Function not implemented", (StatementClass *) hstmt);
	return SQL_ERROR;
}


/*
 *	SQLPrimaryKeys()
 *
 *	Retrieve the primary key columns for the specified table.
 */
RETCODE		SQL_API
PGAPI_PrimaryKeys(
				  HSTMT hstmt,
				  UCHAR FAR * szTableQualifier,
				  SWORD cbTableQualifier,
				  UCHAR FAR * szTableOwner,
				  SWORD cbTableOwner,
				  UCHAR FAR * szTableName,
				  SWORD cbTableName)
{
	static char *func = "PGAPI_PrimaryKeys";
	StatementClass *stmt = (StatementClass *) hstmt;
	ConnectionClass *conn;
	TupleNode  *row;
	RETCODE		result;
	int			seq = 0;
	HSTMT		htbl_stmt;
	StatementClass *tbl_stmt;
	char		tables_query[INFO_INQUIRY_LEN];
	char		attname[MAX_INFO_STRING];
	SDWORD		attname_len;
	char		pktab[MAX_TABLE_LEN + 1];
	Int2		result_cols;
	int			qno,
				qstart,
				qend;

	mylog("%s: entering...stmt=%u\n", func, stmt);

	if (!stmt)
	{
		SC_log_error(func, "", NULL);
		return SQL_INVALID_HANDLE;
	}
	stmt->manual_result = TRUE;
	stmt->errormsg_created = TRUE;

	stmt->result = QR_Constructor();
	if (!stmt->result)
	{
		stmt->errormsg = "Couldn't allocate memory for PGAPI_PrimaryKeys result.";
		stmt->errornumber = STMT_NO_MEMORY_ERROR;
		SC_log_error(func, "", stmt);
		return SQL_ERROR;
	}

	/* the binding structure for a statement is not set up until */

	/*
	 * a statement is actually executed, so we'll have to do this
	 * ourselves.
	 */
	result_cols = 6;
	extend_bindings(stmt, result_cols);

	/* set the field names */
	QR_set_num_fields(stmt->result, result_cols);
	QR_set_field_info(stmt->result, 0, "TABLE_QUALIFIER", PG_TYPE_TEXT, MAX_INFO_STRING);
	QR_set_field_info(stmt->result, 1, "TABLE_OWNER", PG_TYPE_TEXT, MAX_INFO_STRING);
	QR_set_field_info(stmt->result, 2, "TABLE_NAME", PG_TYPE_TEXT, MAX_INFO_STRING);
	QR_set_field_info(stmt->result, 3, "COLUMN_NAME", PG_TYPE_TEXT, MAX_INFO_STRING);
	QR_set_field_info(stmt->result, 4, "KEY_SEQ", PG_TYPE_INT2, 2);
	QR_set_field_info(stmt->result, 5, "PK_NAME", PG_TYPE_TEXT, MAX_INFO_STRING);


	result = PGAPI_AllocStmt(stmt->hdbc, &htbl_stmt);
	if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
	{
		stmt->errornumber = STMT_NO_MEMORY_ERROR;
		stmt->errormsg = "Couldn't allocate statement for Primary Key result.";
		SC_log_error(func, "", stmt);
		return SQL_ERROR;
	}
	tbl_stmt = (StatementClass *) htbl_stmt;

	pktab[0] = '\0';
	make_string(szTableName, cbTableName, pktab);
	if (pktab[0] == '\0')
	{
		stmt->errormsg = "No Table specified to PGAPI_PrimaryKeys.";
		stmt->errornumber = STMT_INTERNAL_ERROR;
		SC_log_error(func, "", stmt);
		PGAPI_FreeStmt(htbl_stmt, SQL_DROP);
		return SQL_ERROR;
	}

	result = PGAPI_BindCol(htbl_stmt, 1, SQL_C_CHAR,
						   attname, MAX_INFO_STRING, &attname_len);
	if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
	{
		stmt->errormsg = tbl_stmt->errormsg;
		stmt->errornumber = tbl_stmt->errornumber;
		SC_log_error(func, "", stmt);
		PGAPI_FreeStmt(htbl_stmt, SQL_DROP);
		return SQL_ERROR;
	}

	conn = SC_get_conn(stmt);
	if (PG_VERSION_LE(conn, 6.4))
		qstart = 2;
	else
		qstart = 1;
	qend = 2;
	for (qno = qstart; qno <= qend; qno++)
	{
		switch (qno)
		{
			case 1:

				/*
				 * Simplified query to remove assumptions about number of
				 * possible index columns. Courtesy of Tom Lane - thomas
				 * 2000-03-21
				 */
				sprintf(tables_query, "select ta.attname, ia.attnum"
						" from pg_attribute ta, pg_attribute ia, pg_class c, pg_index i"
						" where c.relname = '%s'"
						" AND c.oid = i.indrelid"
						" AND i.indisprimary = 't'"
						" AND ia.attrelid = i.indexrelid"
						" AND ta.attrelid = i.indrelid"
						" AND ta.attnum = i.indkey[ia.attnum-1]"
						" order by ia.attnum", pktab);
				break;
			case 2:

				/*
				 * Simplified query to search old fashoned primary key
				 */
				sprintf(tables_query, "select ta.attname, ia.attnum"
						" from pg_attribute ta, pg_attribute ia, pg_class c, pg_index i"
						" where c.relname = '%s_pkey'"
						" AND c.oid = i.indexrelid"
						" AND ia.attrelid = i.indexrelid"
						" AND ta.attrelid = i.indrelid"
						" AND ta.attnum = i.indkey[ia.attnum-1]"
						" order by ia.attnum", pktab);
				break;
		}
		mylog("%s: tables_query='%s'\n", func, tables_query);

		result = PGAPI_ExecDirect(htbl_stmt, tables_query, strlen(tables_query));
		if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
		{
			stmt->errormsg = SC_create_errormsg(htbl_stmt);
			stmt->errornumber = tbl_stmt->errornumber;
			SC_log_error(func, "", stmt);
			PGAPI_FreeStmt(htbl_stmt, SQL_DROP);
			return SQL_ERROR;
		}

		result = PGAPI_Fetch(htbl_stmt);
		if (result != SQL_NO_DATA_FOUND)
			break;
	}

	while ((result == SQL_SUCCESS) || (result == SQL_SUCCESS_WITH_INFO))
	{
		row = (TupleNode *) malloc(sizeof(TupleNode) + (result_cols - 1) *sizeof(TupleField));

		set_tuplefield_null(&row->tuple[0]);

		/*
		 * I have to hide the table owner from Access, otherwise it
		 * insists on referring to the table as 'owner.table'. (this is
		 * valid according to the ODBC SQL grammar, but Postgres won't
		 * support it.)
		 */
		set_tuplefield_string(&row->tuple[1], "");
		set_tuplefield_string(&row->tuple[2], pktab);
		set_tuplefield_string(&row->tuple[3], attname);
		set_tuplefield_int2(&row->tuple[4], (Int2) (++seq));
		set_tuplefield_null(&row->tuple[5]);

		QR_add_tuple(stmt->result, row);

		mylog(">> primaryKeys: pktab = '%s', attname = '%s', seq = %d\n", pktab, attname, seq);

		result = PGAPI_Fetch(htbl_stmt);
	}

	if (result != SQL_NO_DATA_FOUND)
	{
		stmt->errormsg = SC_create_errormsg(htbl_stmt);
		stmt->errornumber = tbl_stmt->errornumber;
		SC_log_error(func, "", stmt);
		PGAPI_FreeStmt(htbl_stmt, SQL_DROP);
		return SQL_ERROR;
	}

	PGAPI_FreeStmt(htbl_stmt, SQL_DROP);


	/*
	 * also, things need to think that this statement is finished so the
	 * results can be retrieved.
	 */
	stmt->status = STMT_FINISHED;

	/* set up the current tuple pointer for SQLFetch */
	stmt->currTuple = -1;
	stmt->rowset_start = -1;
	stmt->current_col = -1;

	mylog("%s: EXIT, stmt=%u\n", func, stmt);
	return SQL_SUCCESS;
}


#ifdef	MULTIBYTE
/*
 *	Multibyte support stuff for SQLForeignKeys().
 *	There may be much more effective way in the
 *	future version. The way is very forcible currently.
 */
static BOOL
isMultibyte(const unsigned char *str)
{
	for (; *str; str++)
	{
		if (*str >= 0x80)
			return TRUE;
	}
	return FALSE;
}
static char *
getClientTableName(ConnectionClass *conn, char *serverTableName, BOOL *nameAlloced)
{
	char		query[1024],
				saveoid[24],
			   *ret = serverTableName;
	BOOL		continueExec = TRUE,
				bError = FALSE;
	QResultClass *res;

	*nameAlloced = FALSE;
	if (!conn->client_encoding || !isMultibyte(serverTableName))
		return ret;
	if (!conn->server_encoding)
	{
		if (res = CC_send_query(conn, "select getdatabaseencoding()", NULL), res)
		{
			if (QR_get_num_tuples(res) > 0)
				conn->server_encoding = strdup(QR_get_value_backend_row(res, 0, 0));
			QR_Destructor(res);
		}
	}
	if (!conn->server_encoding)
		return ret;
	sprintf(query, "SET CLIENT_ENCODING TO '%s'", conn->server_encoding);
	if (res = CC_send_query(conn, query, NULL), res)
	{
		bError = QR_get_aborted(res);
		QR_Destructor(res);
	}
	else
		bError = TRUE;
	if (!bError && continueExec)
	{
		sprintf(query, "select OID from pg_class where relname = '%s'", serverTableName);
		if (res = CC_send_query(conn, query, NULL), res)
		{
			if (QR_get_num_tuples(res) > 0)
				strcpy(saveoid, QR_get_value_backend_row(res, 0, 0));
			else
			{
				continueExec = FALSE;
				bError = QR_get_aborted(res);
			}
			QR_Destructor(res);
		}
		else
			bError = TRUE;
	}
	continueExec = (continueExec && !bError);
	if (bError && CC_is_in_trans(conn))
	{
		if (res = CC_send_query(conn, "abort", NULL), res)
			QR_Destructor(res);
		CC_set_no_trans(conn);
		bError = FALSE;
	}
	/* restore the client encoding */
	sprintf(query, "SET CLIENT_ENCODING TO '%s'", conn->client_encoding);
	if (res = CC_send_query(conn, query, NULL), res)
	{
		bError = QR_get_aborted(res);
		QR_Destructor(res);
	}
	else
		bError = TRUE;
	if (bError || !continueExec)
		return ret;
	sprintf(query, "select relname from pg_class where OID = %s", saveoid);
	if (res = CC_send_query(conn, query, NULL), res)
	{
		if (QR_get_num_tuples(res) > 0)
		{
			ret = strdup(QR_get_value_backend_row(res, 0, 0));
			*nameAlloced = TRUE;
		}
		QR_Destructor(res);
	}
	return ret;
}
static char *
getClientColumnName(ConnectionClass *conn, const char *serverTableName, char *serverColumnName, BOOL *nameAlloced)
{
	char		query[1024],
				saveattrelid[24],
				saveattnum[16],
			   *ret = serverColumnName;
	BOOL		continueExec = TRUE,
				bError = FALSE;
	QResultClass *res;

	*nameAlloced = FALSE;
	if (!conn->client_encoding || !isMultibyte(serverColumnName))
		return ret;
	if (!conn->server_encoding)
	{
		if (res = CC_send_query(conn, "select getdatabaseencoding()", NULL), res)
		{
			if (QR_get_num_tuples(res) > 0)
				conn->server_encoding = strdup(QR_get_value_backend_row(res, 0, 0));
			QR_Destructor(res);
		}
	}
	if (!conn->server_encoding)
		return ret;
	sprintf(query, "SET CLIENT_ENCODING TO '%s'", conn->server_encoding);
	if (res = CC_send_query(conn, query, NULL), res)
	{
		bError = QR_get_aborted(res);
		QR_Destructor(res);
	}
	else
		bError = TRUE;
	if (!bError && continueExec)
	{
		sprintf(query, "select attrelid, attnum from pg_class, pg_attribute "
				"where relname = '%s' and attrelid = pg_class.oid "
				"and attname = '%s'", serverTableName, serverColumnName);
		if (res = CC_send_query(conn, query, NULL), res)
		{
			if (QR_get_num_tuples(res) > 0)
			{
				strcpy(saveattrelid, QR_get_value_backend_row(res, 0, 0));
				strcpy(saveattnum, QR_get_value_backend_row(res, 0, 1));
			}
			else
			{
				continueExec = FALSE;
				bError = QR_get_aborted(res);
			}
			QR_Destructor(res);
		}
		else
			bError = TRUE;
	}
	continueExec = (continueExec && !bError);
	if (bError && CC_is_in_trans(conn))
	{
		if (res = CC_send_query(conn, "abort", NULL), res)
			QR_Destructor(res);
		CC_set_no_trans(conn);
		bError = FALSE;
	}
	/* restore the cleint encoding */
	sprintf(query, "SET CLIENT_ENCODING TO '%s'", conn->client_encoding);
	if (res = CC_send_query(conn, query, NULL), res)
	{
		bError = QR_get_aborted(res);
		QR_Destructor(res);
	}
	else
		bError = TRUE;
	if (bError || !continueExec)
		return ret;
	sprintf(query, "select attname from pg_attribute where attrelid = %s and attnum = %s", saveattrelid, saveattnum);
	if (res = CC_send_query(conn, query, NULL), res)
	{
		if (QR_get_num_tuples(res) > 0)
		{
			ret = strdup(QR_get_value_backend_row(res, 0, 0));
			*nameAlloced = TRUE;
		}
		QR_Destructor(res);
	}
	return ret;
}
#endif   /* MULTIBYTE */

RETCODE		SQL_API
PGAPI_ForeignKeys(
				  HSTMT hstmt,
				  UCHAR FAR * szPkTableQualifier,
				  SWORD cbPkTableQualifier,
				  UCHAR FAR * szPkTableOwner,
				  SWORD cbPkTableOwner,
				  UCHAR FAR * szPkTableName,
				  SWORD cbPkTableName,
				  UCHAR FAR * szFkTableQualifier,
				  SWORD cbFkTableQualifier,
				  UCHAR FAR * szFkTableOwner,
				  SWORD cbFkTableOwner,
				  UCHAR FAR * szFkTableName,
				  SWORD cbFkTableName)
{
	static char *func = "PGAPI_ForeignKeys";
	StatementClass *stmt = (StatementClass *) hstmt;
	TupleNode  *row;
	HSTMT		htbl_stmt,
				hpkey_stmt;
	StatementClass *tbl_stmt;
	RETCODE		result,
				keyresult;
	char		tables_query[INFO_INQUIRY_LEN];
	char		trig_deferrable[2];
	char		trig_initdeferred[2];
	char		trig_args[1024];
	char		upd_rule[MAX_TABLE_LEN],
				del_rule[MAX_TABLE_LEN];
	char		pk_table_needed[MAX_TABLE_LEN + 1];
	char		fk_table_needed[MAX_TABLE_LEN + 1];
	char	   *pkey_ptr,
			   *pkey_text,
			   *fkey_ptr,
			   *fkey_text,
			   *pk_table,
			   *pkt_text,
			   *fk_table,
			   *fkt_text;

#ifdef	MULTIBYTE
	BOOL		pkey_alloced,
				fkey_alloced,
				pkt_alloced,
				fkt_alloced;
	ConnectionClass *conn;
#endif   /* MULTIBYTE */
	int			i,
				j,
				k,
				num_keys;
	SWORD		trig_nargs,
				upd_rule_type = 0,
				del_rule_type = 0;

#if (ODBCVER >= 0x0300)
	SWORD		defer_type;
#endif
	char		pkey[MAX_INFO_STRING];
	Int2		result_cols;

	mylog("%s: entering...stmt=%u\n", func, stmt);

	if (!stmt)
	{
		SC_log_error(func, "", NULL);
		return SQL_INVALID_HANDLE;
	}

	stmt->manual_result = TRUE;
	stmt->errormsg_created = TRUE;

	stmt->result = QR_Constructor();
	if (!stmt->result)
	{
		stmt->errormsg = "Couldn't allocate memory for PGAPI_ForeignKeys result.";
		stmt->errornumber = STMT_NO_MEMORY_ERROR;
		SC_log_error(func, "", stmt);
		return SQL_ERROR;
	}

	/* the binding structure for a statement is not set up until */

	/*
	 * a statement is actually executed, so we'll have to do this
	 * ourselves.
	 */
	result_cols = 14;
	extend_bindings(stmt, result_cols);

	/* set the field names */
	QR_set_num_fields(stmt->result, result_cols);
	QR_set_field_info(stmt->result, 0, "PKTABLE_QUALIFIER", PG_TYPE_TEXT, MAX_INFO_STRING);
	QR_set_field_info(stmt->result, 1, "PKTABLE_OWNER", PG_TYPE_TEXT, MAX_INFO_STRING);
	QR_set_field_info(stmt->result, 2, "PKTABLE_NAME", PG_TYPE_TEXT, MAX_INFO_STRING);
	QR_set_field_info(stmt->result, 3, "PKCOLUMN_NAME", PG_TYPE_TEXT, MAX_INFO_STRING);
	QR_set_field_info(stmt->result, 4, "FKTABLE_QUALIFIER", PG_TYPE_TEXT, MAX_INFO_STRING);
	QR_set_field_info(stmt->result, 5, "FKTABLE_OWNER", PG_TYPE_TEXT, MAX_INFO_STRING);
	QR_set_field_info(stmt->result, 6, "FKTABLE_NAME", PG_TYPE_TEXT, MAX_INFO_STRING);
	QR_set_field_info(stmt->result, 7, "FKCOLUMN_NAME", PG_TYPE_TEXT, MAX_INFO_STRING);
	QR_set_field_info(stmt->result, 8, "KEY_SEQ", PG_TYPE_INT2, 2);
	QR_set_field_info(stmt->result, 9, "UPDATE_RULE", PG_TYPE_INT2, 2);
	QR_set_field_info(stmt->result, 10, "DELETE_RULE", PG_TYPE_INT2, 2);
	QR_set_field_info(stmt->result, 11, "FK_NAME", PG_TYPE_TEXT, MAX_INFO_STRING);
	QR_set_field_info(stmt->result, 12, "PK_NAME", PG_TYPE_TEXT, MAX_INFO_STRING);
	QR_set_field_info(stmt->result, 13, "TRIGGER_NAME", PG_TYPE_TEXT, MAX_INFO_STRING);
#if (ODBCVER >= 0x0300)
	QR_set_field_info(stmt->result, 14, "DEFERRABILITY", PG_TYPE_INT2, 2);
#endif   /* ODBCVER >= 0x0300 */

	/*
	 * also, things need to think that this statement is finished so the
	 * results can be retrieved.
	 */
	stmt->status = STMT_FINISHED;

	/* set up the current tuple pointer for SQLFetch */
	stmt->currTuple = -1;
	stmt->rowset_start = -1;
	stmt->current_col = -1;


	result = PGAPI_AllocStmt(stmt->hdbc, &htbl_stmt);
	if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
	{
		stmt->errornumber = STMT_NO_MEMORY_ERROR;
		stmt->errormsg = "Couldn't allocate statement for PGAPI_ForeignKeys result.";
		SC_log_error(func, "", stmt);
		return SQL_ERROR;
	}

	tbl_stmt = (StatementClass *) htbl_stmt;

	pk_table_needed[0] = '\0';
	fk_table_needed[0] = '\0';

	make_string(szPkTableName, cbPkTableName, pk_table_needed);
	make_string(szFkTableName, cbFkTableName, fk_table_needed);

#ifdef	MULTIBYTE
	pkey_text = fkey_text = pkt_text = fkt_text = NULL;
	pkey_alloced = fkey_alloced = pkt_alloced = fkt_alloced = FALSE;
	conn = SC_get_conn(stmt);
#endif   /* MULTIBYTE */

	/*
	 * Case #2 -- Get the foreign keys in the specified table (fktab) that
	 * refer to the primary keys of other table(s).
	 */
	if (fk_table_needed[0] != '\0')
	{
		mylog("%s: entering Foreign Key Case #2", func);
		sprintf(tables_query, "SELECT	pt.tgargs, "
				"		pt.tgnargs, "
				"		pt.tgdeferrable, "
				"		pt.tginitdeferred, "
				"		pg_proc.proname, "
				"		pg_proc_1.proname "
				"FROM	pg_class pc, "
				"		pg_proc pg_proc, "
				"		pg_proc pg_proc_1, "
				"		pg_trigger pg_trigger, "
				"		pg_trigger pg_trigger_1, "
				"		pg_proc pp, "
				"		pg_trigger pt "
				"WHERE	pt.tgrelid = pc.oid "
				"AND pp.oid = pt.tgfoid "
				"AND pg_trigger.tgconstrrelid = pc.oid "
				"AND pg_proc.oid = pg_trigger.tgfoid "
				"AND pg_trigger_1.tgfoid = pg_proc_1.oid "
				"AND pg_trigger_1.tgconstrrelid = pc.oid "
				"AND ((pc.relname='%s') "
				"AND (pp.proname LIKE '%%ins') "
				"AND (pg_proc.proname LIKE '%%upd') "
				"AND (pg_proc_1.proname LIKE '%%del') "
				"AND (pg_trigger.tgrelid=pt.tgconstrrelid) "
				"AND (pg_trigger.tgconstrname=pt.tgconstrname) "
				"AND (pg_trigger_1.tgrelid=pt.tgconstrrelid) "
				"AND (pg_trigger_1.tgconstrname=pt.tgconstrname))",
				fk_table_needed);

		result = PGAPI_ExecDirect(htbl_stmt, tables_query, strlen(tables_query));

		if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
		{
			stmt->errormsg = SC_create_errormsg(htbl_stmt);
			stmt->errornumber = tbl_stmt->errornumber;
			SC_log_error(func, "", stmt);
			PGAPI_FreeStmt(htbl_stmt, SQL_DROP);
			return SQL_ERROR;
		}

		result = PGAPI_BindCol(htbl_stmt, 1, SQL_C_BINARY,
							   trig_args, sizeof(trig_args), NULL);
		if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
		{
			stmt->errormsg = tbl_stmt->errormsg;
			stmt->errornumber = tbl_stmt->errornumber;
			SC_log_error(func, "", stmt);
			PGAPI_FreeStmt(htbl_stmt, SQL_DROP);
			return SQL_ERROR;
		}

		result = PGAPI_BindCol(htbl_stmt, 2, SQL_C_SHORT,
							   &trig_nargs, 0, NULL);
		if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
		{
			stmt->errormsg = tbl_stmt->errormsg;
			stmt->errornumber = tbl_stmt->errornumber;
			SC_log_error(func, "", stmt);
			PGAPI_FreeStmt(htbl_stmt, SQL_DROP);
			return SQL_ERROR;
		}

		result = PGAPI_BindCol(htbl_stmt, 3, SQL_C_CHAR,
						 trig_deferrable, sizeof(trig_deferrable), NULL);
		if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
		{
			stmt->errormsg = tbl_stmt->errormsg;
			stmt->errornumber = tbl_stmt->errornumber;
			SC_log_error(func, "", stmt);
			PGAPI_FreeStmt(htbl_stmt, SQL_DROP);
			return SQL_ERROR;
		}

		result = PGAPI_BindCol(htbl_stmt, 4, SQL_C_CHAR,
					 trig_initdeferred, sizeof(trig_initdeferred), NULL);
		if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
		{
			stmt->errormsg = tbl_stmt->errormsg;
			stmt->errornumber = tbl_stmt->errornumber;
			SC_log_error(func, "", stmt);
			PGAPI_FreeStmt(htbl_stmt, SQL_DROP);
			return SQL_ERROR;
		}

		result = PGAPI_BindCol(htbl_stmt, 5, SQL_C_CHAR,
							   upd_rule, sizeof(upd_rule), NULL);
		if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
		{
			stmt->errormsg = tbl_stmt->errormsg;
			stmt->errornumber = tbl_stmt->errornumber;
			SC_log_error(func, "", stmt);
			PGAPI_FreeStmt(htbl_stmt, SQL_DROP);
			return SQL_ERROR;
		}

		result = PGAPI_BindCol(htbl_stmt, 6, SQL_C_CHAR,
							   del_rule, sizeof(del_rule), NULL);
		if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
		{
			stmt->errormsg = tbl_stmt->errormsg;
			stmt->errornumber = tbl_stmt->errornumber;
			SC_log_error(func, "", stmt);
			PGAPI_FreeStmt(htbl_stmt, SQL_DROP);
			return SQL_ERROR;
		}

		result = PGAPI_Fetch(htbl_stmt);
		if (result == SQL_NO_DATA_FOUND)
			return SQL_SUCCESS;

		if (result != SQL_SUCCESS)
		{
			stmt->errormsg = SC_create_errormsg(htbl_stmt);
			stmt->errornumber = tbl_stmt->errornumber;
			SC_log_error(func, "", stmt);
			PGAPI_FreeStmt(htbl_stmt, SQL_DROP);
			return SQL_ERROR;
		}

		keyresult = PGAPI_AllocStmt(stmt->hdbc, &hpkey_stmt);
		if ((keyresult != SQL_SUCCESS) && (keyresult != SQL_SUCCESS_WITH_INFO))
		{
			stmt->errornumber = STMT_NO_MEMORY_ERROR;
			stmt->errormsg = "Couldn't allocate statement for PGAPI_ForeignKeys (pkeys) result.";
			SC_log_error(func, "", stmt);
			return SQL_ERROR;
		}

		keyresult = PGAPI_BindCol(hpkey_stmt, 4, SQL_C_CHAR,
								  pkey, sizeof(pkey), NULL);
		if (keyresult != SQL_SUCCESS)
		{
			stmt->errornumber = STMT_NO_MEMORY_ERROR;
			stmt->errormsg = "Couldn't bindcol for primary keys for PGAPI_ForeignKeys result.";
			SC_log_error(func, "", stmt);
			PGAPI_FreeStmt(hpkey_stmt, SQL_DROP);
			return SQL_ERROR;
		}

		while (result == SQL_SUCCESS)
		{
			/* Compute the number of keyparts. */
			num_keys = (trig_nargs - 4) / 2;

			mylog("Foreign Key Case#2: trig_nargs = %d, num_keys = %d\n", trig_nargs, num_keys);

			pk_table = trig_args;

			/* Get to the PK Table Name */
			for (k = 0; k < 2; k++)
				pk_table += strlen(pk_table) + 1;

#ifdef	MULTIBYTE
			fk_table = trig_args + strlen(trig_args) + 1;
			pkt_text = getClientTableName(conn, pk_table, &pkt_alloced);
#else
			pkt_text = pk_table;
#endif   /* MULTIBYTE */
			/* If there is a pk table specified, then check it. */
			if (pk_table_needed[0] != '\0')
			{
				/* If it doesn't match, then continue */
				if (strcmp(pkt_text, pk_table_needed))
				{
					result = PGAPI_Fetch(htbl_stmt);
					continue;
				}
			}

			keyresult = PGAPI_PrimaryKeys(hpkey_stmt, NULL, 0, NULL, 0, pkt_text, SQL_NTS);
			if (keyresult != SQL_SUCCESS)
			{
				stmt->errornumber = STMT_NO_MEMORY_ERROR;
				stmt->errormsg = "Couldn't get primary keys for PGAPI_ForeignKeys result.";
				SC_log_error(func, "", stmt);
				PGAPI_FreeStmt(hpkey_stmt, SQL_DROP);
				return SQL_ERROR;
			}


			/* Get to first primary key */
			pkey_ptr = trig_args;
			for (i = 0; i < 5; i++)
				pkey_ptr += strlen(pkey_ptr) + 1;

			for (k = 0; k < num_keys; k++)
			{
				/* Check that the key listed is the primary key */
				keyresult = PGAPI_Fetch(hpkey_stmt);
				if (keyresult != SQL_SUCCESS)
				{
					num_keys = 0;
					break;
				}
#ifdef	MULTIBYTE
				pkey_text = getClientColumnName(conn, pk_table, pkey_ptr, &pkey_alloced);
#else
				pkey_text = pkey_ptr;
#endif   /* MULTIBYTE */
				mylog("%s: pkey_ptr='%s', pkey='%s'\n", func, pkey_text, pkey);
				if (strcmp(pkey_text, pkey))
				{
					num_keys = 0;
					break;
				}
#ifdef	MULTIBYTE
				if (pkey_alloced)
					free(pkey_text);
#endif   /* MULTIBYTE */
				/* Get to next primary key */
				for (k = 0; k < 2; k++)
					pkey_ptr += strlen(pkey_ptr) + 1;

			}

			/* Set to first fk column */
			fkey_ptr = trig_args;
			for (k = 0; k < 4; k++)
				fkey_ptr += strlen(fkey_ptr) + 1;

			/* Set update and delete actions for foreign keys */
			if (!strcmp(upd_rule, "RI_FKey_cascade_upd"))
				upd_rule_type = SQL_CASCADE;
			else if (!strcmp(upd_rule, "RI_FKey_noaction_upd"))
				upd_rule_type = SQL_NO_ACTION;
			else if (!strcmp(upd_rule, "RI_FKey_restrict_upd"))
				upd_rule_type = SQL_NO_ACTION;
			else if (!strcmp(upd_rule, "RI_FKey_setdefault_upd"))
				upd_rule_type = SQL_SET_DEFAULT;
			else if (!strcmp(upd_rule, "RI_FKey_setnull_upd"))
				upd_rule_type = SQL_SET_NULL;

			if (!strcmp(upd_rule, "RI_FKey_cascade_del"))
				del_rule_type = SQL_CASCADE;
			else if (!strcmp(upd_rule, "RI_FKey_noaction_del"))
				del_rule_type = SQL_NO_ACTION;
			else if (!strcmp(upd_rule, "RI_FKey_restrict_del"))
				del_rule_type = SQL_NO_ACTION;
			else if (!strcmp(upd_rule, "RI_FKey_setdefault_del"))
				del_rule_type = SQL_SET_DEFAULT;
			else if (!strcmp(upd_rule, "RI_FKey_setnull_del"))
				del_rule_type = SQL_SET_NULL;

#if (ODBCVER >= 0x0300)
			/* Set deferrability type */
			if (!strcmp(trig_initdeferred, "y"))
				defer_type = SQL_INITIALLY_DEFERRED;
			else if (!strcmp(trig_deferrable, "y"))
				defer_type = SQL_INITIALLY_IMMEDIATE;
			else
				defer_type = SQL_NOT_DEFERRABLE;
#endif   /* ODBCVER >= 0x0300 */

			/* Get to first primary key */
			pkey_ptr = trig_args;
			for (i = 0; i < 5; i++)
				pkey_ptr += strlen(pkey_ptr) + 1;

			for (k = 0; k < num_keys; k++)
			{
				row = (TupleNode *) malloc(sizeof(TupleNode) + (result_cols - 1) *sizeof(TupleField));

#ifdef	MULTIBYTE
				pkey_text = getClientColumnName(conn, pk_table, pkey_ptr, &pkey_alloced);
				fkey_text = getClientColumnName(conn, fk_table, fkey_ptr, &fkey_alloced);
#else
				pkey_text = pkey_ptr;
				fkey_text = fkey_ptr;
#endif   /* MULTIBYTE */
				mylog("%s: pk_table = '%s', pkey_ptr = '%s'\n", func, pkt_text, pkey_text);
				set_tuplefield_null(&row->tuple[0]);
				set_tuplefield_string(&row->tuple[1], "");
				set_tuplefield_string(&row->tuple[2], pkt_text);
				set_tuplefield_string(&row->tuple[3], pkey_text);

				mylog("%s: fk_table_needed = '%s', fkey_ptr = '%s'\n", func, fk_table_needed, fkey_text);
				set_tuplefield_null(&row->tuple[4]);
				set_tuplefield_string(&row->tuple[5], "");
				set_tuplefield_string(&row->tuple[6], fk_table_needed);
				set_tuplefield_string(&row->tuple[7], fkey_text);

				mylog("%s: upd_rule_type = '%i', del_rule_type = '%i'\n, trig_name = '%s'", func, upd_rule_type, del_rule_type, trig_args);
				set_tuplefield_int2(&row->tuple[8], (Int2) (k + 1));
				set_tuplefield_int2(&row->tuple[9], (Int2) upd_rule_type);
				set_tuplefield_int2(&row->tuple[10], (Int2) del_rule_type);
				set_tuplefield_null(&row->tuple[11]);
				set_tuplefield_null(&row->tuple[12]);
				set_tuplefield_string(&row->tuple[13], trig_args);
#if (ODBCVER >= 0x0300)
				set_tuplefield_int2(&row->tuple[14], defer_type);
#endif   /* ODBCVER >= 0x0300 */

				QR_add_tuple(stmt->result, row);
#ifdef	MULTIBYTE
				if (fkey_alloced)
					free(fkey_text);
				fkey_alloced = FALSE;
				if (pkey_alloced)
					free(pkey_text);
				pkey_alloced = FALSE;
#endif   /* MULTIBYTE */
				/* next primary/foreign key */
				for (i = 0; i < 2; i++)
				{
					fkey_ptr += strlen(fkey_ptr) + 1;
					pkey_ptr += strlen(pkey_ptr) + 1;
				}
			}
#ifdef	MULTIBYTE
			if (pkt_alloced)
				free(pkt_text);
			pkt_alloced = FALSE;
#endif   /* MULTIBYTE */

			result = PGAPI_Fetch(htbl_stmt);
		}
		PGAPI_FreeStmt(hpkey_stmt, SQL_DROP);
	}

	/*
	 * Case #1 -- Get the foreign keys in other tables that refer to the
	 * primary key in the specified table (pktab).	i.e., Who points to
	 * me?
	 */
	else if (pk_table_needed[0] != '\0')
	{
		sprintf(tables_query, "SELECT	pg_trigger.tgargs, "
				"		pg_trigger.tgnargs, "
				"		pg_trigger.tgdeferrable, "
				"		pg_trigger.tginitdeferred, "
				"		pg_proc.proname, "
				"		pg_proc_1.proname "
				"FROM	pg_class pg_class, "
				"		pg_class pg_class_1, "
				"		pg_class pg_class_2, "
				"		pg_proc pg_proc, "
				"		pg_proc pg_proc_1, "
				"		pg_trigger pg_trigger, "
				"		pg_trigger pg_trigger_1, "
				"		pg_trigger pg_trigger_2 "
				"WHERE	pg_trigger.tgconstrrelid = pg_class.oid "
				"	AND pg_trigger.tgrelid = pg_class_1.oid "
				"	AND pg_trigger_1.tgfoid = pg_proc_1.oid "
				"	AND pg_trigger_1.tgconstrrelid = pg_class_1.oid "
				"	AND pg_trigger_2.tgconstrrelid = pg_class_2.oid "
				"	AND pg_trigger_2.tgfoid = pg_proc.oid "
				"	AND pg_class_2.oid = pg_trigger.tgrelid "
				"	AND ("
				"		 (pg_class.relname='%s') "
				"	AND  (pg_proc.proname Like '%%upd') "
				"	AND  (pg_proc_1.proname Like '%%del')"
				"	AND	 (pg_trigger_1.tgrelid = pg_trigger.tgconstrrelid) "
				"	AND	 (pg_trigger_2.tgrelid = pg_trigger.tgconstrrelid) "
				"		)",
				pk_table_needed);

		result = PGAPI_ExecDirect(htbl_stmt, tables_query, strlen(tables_query));
		if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
		{
			stmt->errormsg = SC_create_errormsg(htbl_stmt);
			stmt->errornumber = tbl_stmt->errornumber;
			SC_log_error(func, "", stmt);
			PGAPI_FreeStmt(htbl_stmt, SQL_DROP);
			return SQL_ERROR;
		}

		result = PGAPI_BindCol(htbl_stmt, 1, SQL_C_BINARY,
							   trig_args, sizeof(trig_args), NULL);
		if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
		{
			stmt->errormsg = tbl_stmt->errormsg;
			stmt->errornumber = tbl_stmt->errornumber;
			SC_log_error(func, "", stmt);
			PGAPI_FreeStmt(htbl_stmt, SQL_DROP);
			return SQL_ERROR;
		}

		result = PGAPI_BindCol(htbl_stmt, 2, SQL_C_SHORT,
							   &trig_nargs, 0, NULL);
		if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
		{
			stmt->errormsg = tbl_stmt->errormsg;
			stmt->errornumber = tbl_stmt->errornumber;
			SC_log_error(func, "", stmt);
			PGAPI_FreeStmt(htbl_stmt, SQL_DROP);
			return SQL_ERROR;
		}

		result = PGAPI_BindCol(htbl_stmt, 3, SQL_C_CHAR,
						 trig_deferrable, sizeof(trig_deferrable), NULL);
		if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
		{
			stmt->errormsg = tbl_stmt->errormsg;
			stmt->errornumber = tbl_stmt->errornumber;
			SC_log_error(func, "", stmt);
			PGAPI_FreeStmt(htbl_stmt, SQL_DROP);
			return SQL_ERROR;
		}

		result = PGAPI_BindCol(htbl_stmt, 4, SQL_C_CHAR,
					 trig_initdeferred, sizeof(trig_initdeferred), NULL);
		if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
		{
			stmt->errormsg = tbl_stmt->errormsg;
			stmt->errornumber = tbl_stmt->errornumber;
			SC_log_error(func, "", stmt);
			PGAPI_FreeStmt(htbl_stmt, SQL_DROP);
			return SQL_ERROR;
		}

		result = PGAPI_BindCol(htbl_stmt, 5, SQL_C_CHAR,
							   upd_rule, sizeof(upd_rule), NULL);
		if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
		{
			stmt->errormsg = tbl_stmt->errormsg;
			stmt->errornumber = tbl_stmt->errornumber;
			SC_log_error(func, "", stmt);
			PGAPI_FreeStmt(htbl_stmt, SQL_DROP);
			return SQL_ERROR;
		}

		result = PGAPI_BindCol(htbl_stmt, 6, SQL_C_CHAR,
							   del_rule, sizeof(del_rule), NULL);
		if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
		{
			stmt->errormsg = tbl_stmt->errormsg;
			stmt->errornumber = tbl_stmt->errornumber;
			SC_log_error(func, "", stmt);
			PGAPI_FreeStmt(htbl_stmt, SQL_DROP);
			return SQL_ERROR;
		}

		result = PGAPI_Fetch(htbl_stmt);
		if (result == SQL_NO_DATA_FOUND)
			return SQL_SUCCESS;

		if (result != SQL_SUCCESS)
		{
			stmt->errormsg = SC_create_errormsg(htbl_stmt);
			stmt->errornumber = tbl_stmt->errornumber;
			SC_log_error(func, "", stmt);
			PGAPI_FreeStmt(htbl_stmt, SQL_DROP);
			return SQL_ERROR;
		}

		while (result == SQL_SUCCESS)
		{
			/* Calculate the number of key parts */
			num_keys = (trig_nargs - 4) / 2;;

			/* Handle action (i.e., 'cascade', 'restrict', 'setnull') */
			if (!strcmp(upd_rule, "RI_FKey_cascade_upd"))
				upd_rule_type = SQL_CASCADE;
			else if (!strcmp(upd_rule, "RI_FKey_noaction_upd"))
				upd_rule_type = SQL_NO_ACTION;
			else if (!strcmp(upd_rule, "RI_FKey_restrict_upd"))
				upd_rule_type = SQL_NO_ACTION;
			else if (!strcmp(upd_rule, "RI_FKey_setdefault_upd"))
				upd_rule_type = SQL_SET_DEFAULT;
			else if (!strcmp(upd_rule, "RI_FKey_setnull_upd"))
				upd_rule_type = SQL_SET_NULL;

			if (!strcmp(upd_rule, "RI_FKey_cascade_del"))
				del_rule_type = SQL_CASCADE;
			else if (!strcmp(upd_rule, "RI_FKey_noaction_del"))
				del_rule_type = SQL_NO_ACTION;
			else if (!strcmp(upd_rule, "RI_FKey_restrict_del"))
				del_rule_type = SQL_NO_ACTION;
			else if (!strcmp(upd_rule, "RI_FKey_setdefault_del"))
				del_rule_type = SQL_SET_DEFAULT;
			else if (!strcmp(upd_rule, "RI_FKey_setnull_del"))
				del_rule_type = SQL_SET_NULL;

#if (ODBCVER >= 0x0300)
			/* Set deferrability type */
			if (!strcmp(trig_initdeferred, "y"))
				defer_type = SQL_INITIALLY_DEFERRED;
			else if (!strcmp(trig_deferrable, "y"))
				defer_type = SQL_INITIALLY_IMMEDIATE;
			else
				defer_type = SQL_NOT_DEFERRABLE;
#endif   /* ODBCVER >= 0x0300 */

			mylog("Foreign Key Case#1: trig_nargs = %d, num_keys = %d\n", trig_nargs, num_keys);

			/* Get to first primary key */
			pkey_ptr = trig_args;
			for (i = 0; i < 5; i++)
				pkey_ptr += strlen(pkey_ptr) + 1;

			/* Get to first foreign table */
			fk_table = trig_args;
			fk_table += strlen(fk_table) + 1;
#ifdef	MULTIBYTE
			pk_table = fk_table + strlen(fk_table) + 1;
			fkt_text = getClientTableName(conn, fk_table, &fkt_alloced);
#else
			fkt_text = fk_table;
#endif   /* MULTIBYTE */

			/* Get to first foreign key */
			fkey_ptr = trig_args;
			for (k = 0; k < 4; k++)
				fkey_ptr += strlen(fkey_ptr) + 1;

			for (k = 0; k < num_keys; k++)
			{
#ifdef	MULTIBYTE
				pkey_text = getClientColumnName(conn, pk_table, pkey_ptr, &pkey_alloced);
				fkey_text = getClientColumnName(conn, fk_table, fkey_ptr, &fkey_alloced);
#else
				pkey_text = pkey_ptr;
				fkey_text = fkey_ptr;
#endif   /* MULTIBYTE */
				mylog("pkey_ptr = '%s', fk_table = '%s', fkey_ptr = '%s'\n", pkey_text, fkt_text, fkey_text);

				row = (TupleNode *) malloc(sizeof(TupleNode) + (result_cols - 1) *sizeof(TupleField));

				mylog("pk_table_needed = '%s', pkey_ptr = '%s'\n", pk_table_needed, pkey_text);
				set_tuplefield_null(&row->tuple[0]);
				set_tuplefield_string(&row->tuple[1], "");
				set_tuplefield_string(&row->tuple[2], pk_table_needed);
				set_tuplefield_string(&row->tuple[3], pkey_text);

				mylog("fk_table = '%s', fkey_ptr = '%s'\n", fkt_text, fkey_text);
				set_tuplefield_null(&row->tuple[4]);
				set_tuplefield_string(&row->tuple[5], "");
				set_tuplefield_string(&row->tuple[6], fkt_text);
				set_tuplefield_string(&row->tuple[7], fkey_text);

				set_tuplefield_int2(&row->tuple[8], (Int2) (k + 1));

				mylog("upd_rule = %d, del_rule= %d", upd_rule_type, del_rule_type);
				set_nullfield_int2(&row->tuple[9], (Int2) upd_rule_type);
				set_nullfield_int2(&row->tuple[10], (Int2) del_rule_type);

				set_tuplefield_null(&row->tuple[11]);
				set_tuplefield_null(&row->tuple[12]);

				set_tuplefield_string(&row->tuple[13], trig_args);

#if (ODBCVER >= 0x0300)
				mylog("defer_type = '%s'", defer_type);
				set_tuplefield_int2(&row->tuple[14], defer_type);
#endif   /* ODBCVER >= 0x0300 */

				QR_add_tuple(stmt->result, row);
#ifdef	MULTIBYTE
				if (pkey_alloced)
					free(pkey_text);
				pkey_alloced = FALSE;
				if (fkey_alloced)
					free(fkey_text);
				fkey_alloced = FALSE;
#endif   /* MULTIBYTE */

				/* next primary/foreign key */
				for (j = 0; j < 2; j++)
				{
					pkey_ptr += strlen(pkey_ptr) + 1;
					fkey_ptr += strlen(fkey_ptr) + 1;
				}
			}
#ifdef	MULTIBYTE
			if (fkt_alloced)
				free(fkt_text);
			fkt_alloced = FALSE;
#endif   /* MULTIBYTE */
			result = PGAPI_Fetch(htbl_stmt);
		}
	}
	else
	{
		stmt->errormsg = "No tables specified to PGAPI_ForeignKeys.";
		stmt->errornumber = STMT_INTERNAL_ERROR;
		SC_log_error(func, "", stmt);
		PGAPI_FreeStmt(htbl_stmt, SQL_DROP);
		return SQL_ERROR;
	}
#ifdef	MULTIBYTE
	if (pkt_alloced)
		free(pkt_text);
	if (pkey_alloced)
		free(pkey_text);
	if (fkt_alloced)
		free(fkt_text);
	if (fkey_alloced)
		free(fkey_text);
#endif   /* MULTIBYTE */

	PGAPI_FreeStmt(htbl_stmt, SQL_DROP);

	mylog("PGAPI_ForeignKeys(): EXIT, stmt=%u\n", stmt);
	return SQL_SUCCESS;
}


RETCODE		SQL_API
PGAPI_ProcedureColumns(
					   HSTMT hstmt,
					   UCHAR FAR * szProcQualifier,
					   SWORD cbProcQualifier,
					   UCHAR FAR * szProcOwner,
					   SWORD cbProcOwner,
					   UCHAR FAR * szProcName,
					   SWORD cbProcName,
					   UCHAR FAR * szColumnName,
					   SWORD cbColumnName)
{
	static char *func = "PGAPI_ProcedureColumns";

	mylog("%s: entering...\n", func);

	SC_log_error(func, "Function not implemented", (StatementClass *) hstmt);
	return SQL_ERROR;
}


RETCODE		SQL_API
PGAPI_Procedures(
				 HSTMT hstmt,
				 UCHAR FAR * szProcQualifier,
				 SWORD cbProcQualifier,
				 UCHAR FAR * szProcOwner,
				 SWORD cbProcOwner,
				 UCHAR FAR * szProcName,
				 SWORD cbProcName)
{
	static char *func = "PGAPI_Procedures";
	StatementClass *stmt = (StatementClass *) hstmt;
	ConnectionClass *conn = SC_get_conn(stmt);
	char		proc_query[INFO_INQUIRY_LEN];
	QResultClass *res;

	mylog("%s: entering...\n", func);

	if (PG_VERSION_LT(conn, 6.5))
	{
		stmt->errornumber = STMT_NOT_IMPLEMENTED_ERROR;
		stmt->errormsg = "Version is too old";
		SC_log_error(func, "Function not implemented", (StatementClass *) hstmt);
		return SQL_ERROR;
	}
	if (!SC_recycle_statement(stmt))
		return SQL_ERROR;

	/*
	 * The following seems the simplest implementation
	 */
	strcpy(proc_query, "select '' as " "PROCEDURE_CAT" ", '' as " "PROCEDURE_SCHEM" ","
		" proname as " "PROCEDURE_NAME" ", '' as " "NUM_INPUT_PARAMS" ","
		   " '' as " "NUM_OUTPUT_PARAMS" ", '' as " "NUM_RESULT_SETS" ","
		   " '' as " "REMARKS" ","
		   " case when prorettype =0 then 1::int2 else 2::int2 end as " "PROCEDURE_TYPE" " from pg_proc");
	my_strcat(proc_query, " where proname like '%.*s'", szProcName, cbProcName);

	res = CC_send_query(conn, proc_query, NULL);
	if (!res || QR_aborted(res))
	{
		if (res)
			QR_Destructor(res);
		stmt->errornumber = STMT_EXEC_ERROR;
		stmt->errormsg = "PGAPI_Procedures query error";
		return SQL_ERROR;
	}
	stmt->result = res;

	/*
	 * also, things need to think that this statement is finished so the
	 * results can be retrieved.
	 */
	stmt->status = STMT_FINISHED;
	extend_bindings(stmt, 8);
	/* set up the current tuple pointer for SQLFetch */
	stmt->currTuple = -1;
	stmt->rowset_start = -1;
	stmt->current_col = -1;

	return SQL_SUCCESS;
}


RETCODE		SQL_API
PGAPI_TablePrivileges(
					  HSTMT hstmt,
					  UCHAR FAR * szTableQualifier,
					  SWORD cbTableQualifier,
					  UCHAR FAR * szTableOwner,
					  SWORD cbTableOwner,
					  UCHAR FAR * szTableName,
					  SWORD cbTableName)
{
	StatementClass *stmt = (StatementClass *) hstmt;
	static char *func = "PGAPI_TablePrivileges";
	Int2		result_cols;

	mylog("%s: entering...\n", func);

	/*
	 * a statement is actually executed, so we'll have to do this
	 * ourselves.
	 */
	result_cols = 7;
	extend_bindings(stmt, result_cols);

	/* set the field names */
	QR_set_num_fields(stmt->result, result_cols);
	QR_set_field_info(stmt->result, 0, "TABLE_CAT", PG_TYPE_TEXT, MAX_INFO_STRING);
	QR_set_field_info(stmt->result, 1, "TABLE_SCHEM", PG_TYPE_TEXT, MAX_INFO_STRING);
	QR_set_field_info(stmt->result, 2, "TABLE_NAME", PG_TYPE_TEXT, MAX_INFO_STRING);
	QR_set_field_info(stmt->result, 3, "GRANTOR", PG_TYPE_TEXT, MAX_INFO_STRING);
	QR_set_field_info(stmt->result, 4, "GRANTEE", PG_TYPE_TEXT, MAX_INFO_STRING);
	QR_set_field_info(stmt->result, 5, "PRIVILEGE", PG_TYPE_TEXT, MAX_INFO_STRING);
	QR_set_field_info(stmt->result, 6, "IS_GRANTABLE", PG_TYPE_TEXT, MAX_INFO_STRING);

	SC_log_error(func, "Function not implemented", (StatementClass *) hstmt);
	return SQL_ERROR;
}
