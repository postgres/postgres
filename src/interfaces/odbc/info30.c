/*-------
 * Module:			info30.c
 *
 * Description:		This module contains routines related to ODBC 3.0
 *			SQLGetInfo().
 *
 */

#ifndef ODBCVER
#define ODBCVER 0x0300
#endif

#include "connection.h"
#include "pgapifunc.h"

RETCODE		SQL_API
PGAPI_GetInfo30(HDBC hdbc, UWORD fInfoType, PTR rgbInfoValue,
				SWORD cbInfoValueMax, SWORD FAR * pcbInfoValue)
{
	static char *func = "PGAPI_GetInfo30";
	ConnectionClass *conn = (ConnectionClass *) hdbc;
	ConnInfo	*ci = &(conn->connInfo);
	char	   *p = NULL;
	int			len = 0,
				value = 0;
	RETCODE		result;

	switch (fInfoType)
	{
		case SQL_DYNAMIC_CURSOR_ATTRIBUTES1:
			len = 4;
			value = 0;
			break;
		case SQL_DYNAMIC_CURSOR_ATTRIBUTES2:
			len = 4;
			value = 0;
			break;

		case SQL_FORWARD_ONLY_CURSOR_ATTRIBUTES1:
			len = 4;
			value = SQL_CA1_NEXT | SQL_CA1_ABSOLUTE |
				SQL_CA1_RELATIVE | SQL_CA1_BOOKMARK;
			break;
		case SQL_FORWARD_ONLY_CURSOR_ATTRIBUTES2:
			len = 4;
			value = 0;
			break;
		case SQL_KEYSET_CURSOR_ATTRIBUTES1:
			len = 4;
			value = 0;
			if (ci->updatable_cursors || ci->drivers.lie)
				value |= (SQL_CA1_NEXT | SQL_CA1_ABSOLUTE
				| SQL_CA1_RELATIVE | SQL_CA1_BOOKMARK
				| SQL_CA1_LOCK_NO_CHANGE | SQL_CA1_POS_POSITION
				| SQL_CA1_POS_UPDATE | SQL_CA1_POS_DELETE
				| SQL_CA1_POS_REFRESH | SQL_CA1_BULK_ADD
				| SQL_CA1_BULK_UPDATE_BY_BOOKMARK
				| SQL_CA1_BULK_DELETE_BY_BOOKMARK
				| SQL_CA1_BULK_FETCH_BY_BOOKMARK
				);
			if (ci->drivers.lie)
				value |= (SQL_CA1_LOCK_EXCLUSIVE
				| SQL_CA1_LOCK_UNLOCK
				| SQL_CA1_POSITIONED_UPDATE
				| SQL_CA1_POSITIONED_DELETE
				| SQL_CA1_SELECT_FOR_UPDATE
				);
			break;
		case SQL_KEYSET_CURSOR_ATTRIBUTES2:
			len = 4;
			value = 0;
			if (ci->updatable_cursors || ci->drivers.lie)
				value |= (SQL_CA2_OPT_ROWVER_CONCURRENCY
				| SQL_CA2_SENSITIVITY_DELETIONS
				| SQL_CA2_SENSITIVITY_UPDATES
				/* | SQL_CA2_SENSITIVITY_ADDITIONS */
				);
			if (ci->drivers.lie)
				value |= (SQL_CA2_READ_ONLY_CONCURRENCY
				| SQL_CA2_LOCK_CONCURRENCY
				| SQL_CA2_OPT_VALUES_CONCURRENCY
				| SQL_CA2_MAX_ROWS_SELECT
				| SQL_CA2_MAX_ROWS_INSERT
				| SQL_CA2_MAX_ROWS_DELETE
				| SQL_CA2_MAX_ROWS_UPDATE
				| SQL_CA2_MAX_ROWS_CATALOG
				| SQL_CA2_MAX_ROWS_AFFECTS_ALL
				| SQL_CA2_CRC_EXACT
				| SQL_CA2_CRC_APPROXIMATE
				| SQL_CA2_SIMULATE_NON_UNIQUE
				| SQL_CA2_SIMULATE_TRY_UNIQUE
				| SQL_CA2_SIMULATE_UNIQUE
				);
			break;

		case SQL_STATIC_CURSOR_ATTRIBUTES1:
			len = 4;
			value = SQL_CA1_NEXT | SQL_CA1_ABSOLUTE
				| SQL_CA1_RELATIVE | SQL_CA1_BOOKMARK
				| SQL_CA1_LOCK_NO_CHANGE | SQL_CA1_POS_POSITION
				| SQL_CA1_POS_REFRESH;
			if (ci->updatable_cursors)
				value |= (SQL_CA1_POS_UPDATE | SQL_CA1_POS_DELETE
				);
			break;
		case SQL_STATIC_CURSOR_ATTRIBUTES2:
			len = 4;
			value = SQL_CA2_READ_ONLY_CONCURRENCY;
			if (ci->updatable_cursors)
				value |= (SQL_CA2_OPT_ROWVER_CONCURRENCY
				/* | SQL_CA2_SENSITIVITY_ADDITIONS
				| SQL_CA2_SENSITIVITY_DELETIONS
				| SQL_CA2_SENSITIVITY_UPDATES */
				);
			break;

		case SQL_ODBC_INTERFACE_CONFORMANCE:
			len = 4;
			value = SQL_OIC_CORE;
			break;
		case SQL_ACTIVE_ENVIRONMENTS:
			len = 2;
			value = 0;
			break;
		case SQL_AGGREGATE_FUNCTIONS:
			len = 4;
			value = SQL_AF_ALL;
			break;
		case SQL_ALTER_DOMAIN:
			len = 4;
			value = 0;
			break;
		case SQL_ASYNC_MODE:
			len = 4;
			value = SQL_AM_NONE;
			break;
		case SQL_BATCH_ROW_COUNT:
			len = 4;
			value = SQL_BRC_EXPLICIT;
			break;
		case SQL_BATCH_SUPPORT:
			len = 4;
			value = SQL_BS_SELECT_EXPLICIT | SQL_BS_ROW_COUNT_EXPLICIT;
			break;
		case SQL_CATALOG_NAME:
			len = 0;
			if (PG_VERSION_LE(conn, 7.2))
				p = "N";
			else
				p = "Y"; /* hopefully */
			break;
		case SQL_COLLATION_SEQ:
			len = 0;
			p = "";
			break;
		case SQL_CREATE_ASSERTION:
			len = 4;
			value = 0;
			break;
		case SQL_CREATE_CHARACTER_SET:
			len = 4;
			value = 0;
			break;
		case SQL_CREATE_COLLATION:
			len = 4;
			value = 0;
			break;
		case SQL_CREATE_DOMAIN:
			len = 4;
			value = 0;
			break;
		case SQL_CREATE_SCHEMA:
			len = 4;
			if (PG_VERSION_LE(conn, 7.2))
				value = 0;
			else
				value = SQL_CS_CREATE_SCHEMA | SQL_CS_AUTHORIZATION; /* hopefully */
			break;
		case SQL_CREATE_TABLE:
			len = 4;
			value = SQL_CT_CREATE_TABLE | SQL_CT_TABLE_CONSTRAINT
				| SQL_CT_CONSTRAINT_NAME_DEFINITION 
				| SQL_CT_LOCAL_TEMPORARY | SQL_CT_COLUMN_CONSTRAINT
				| SQL_CT_COLUMN_DEFAULT | SQL_CT_CONSTRAINT_INITIALLY_DEFERRED
				| SQL_CT_CONSTRAINT_INITIALLY_IMMEDIATE | SQL_CT_CONSTRAINT_DEFERRABLE;
			break;
		case SQL_CREATE_TRANSLATION:
			len = 4;
			value = 0;
			break;
		case SQL_CREATE_VIEW:
			len = 4;
			value = SQL_CV_CREATE_VIEW;
			break;
		case SQL_DDL_INDEX:
			len = 4;
			value = SQL_DI_CREATE_INDEX | SQL_DI_DROP_INDEX;
			break;
		case SQL_DESCRIBE_PARAMETER:
			len = 0;
			p = "N";
			break;
		case SQL_DROP_ASSERTION:
			len = 4;
			value = 0;
			break;
		case SQL_DROP_CHARACTER_SET:
			len = 4;
			value = 0;
			break;
		case SQL_DROP_COLLATION:
			len = 4;
			value = 0;
			break;
		case SQL_DROP_DOMAIN:
			len = 4;
			value = 0;
			break;
		case SQL_DROP_SCHEMA:
			len = 4;
			if (PG_VERSION_LE(conn, 7.2))
				value = 0;
			else
				value = SQL_DS_DROP_SCHEMA | SQL_DS_RESTRICT | SQL_DS_CASCADE; /* hopefully */
			break;
		case SQL_DROP_TABLE:
			len = 4;
			value = SQL_DT_DROP_TABLE;
			if (PG_VERSION_GT(conn, 7.2)) /* hopefully */
				value |= (SQL_DT_RESTRICT | SQL_DT_CASCADE);
			break;
		case SQL_DROP_TRANSLATION:
			len = 4;
			value = 0;
			break;
		case SQL_DROP_VIEW:
			len = 4;
			value = SQL_DV_DROP_VIEW;
			if (PG_VERSION_GT(conn, 7.2)) /* hopefully */
				value |= (SQL_DV_RESTRICT | SQL_DV_CASCADE);
			break;
		case SQL_INDEX_KEYWORDS:
			len = 4;
			value = SQL_IK_NONE;
		case SQL_INFO_SCHEMA_VIEWS:
			len = 4;
			value = 0;
			break;
		case SQL_INSERT_STATEMENT:
			len = 4;
			value = SQL_IS_INSERT_LITERALS | SQL_IS_INSERT_SEARCHED | SQL_IS_SELECT_INTO;
			break;
		case SQL_MAX_IDENTIFIER_LEN:
			len = 4;
			value = 32;
			break;
		case SQL_MAX_ROW_SIZE_INCLUDES_LONG:
			len = 0;
			p = "Y";
			break;
		case SQL_PARAM_ARRAY_ROW_COUNTS:
			len = 4;
			value = SQL_PARC_BATCH;
			break;
		case SQL_PARAM_ARRAY_SELECTS:
			len = 4;
			value = SQL_PAS_BATCH;
			break;
		case SQL_SQL_CONFORMANCE:
			len = 4;
			value = SQL_SC_SQL92_ENTRY;
			break;
		case SQL_SQL92_DATETIME_FUNCTIONS:
			len = 4;
			value = SQL_SDF_CURRENT_DATE | SQL_SDF_CURRENT_TIME | SQL_SDF_CURRENT_TIMESTAMP;
			break;
		case SQL_SQL92_FOREIGN_KEY_DELETE_RULE:
			len = 4;
			value = SQL_SFKD_CASCADE | SQL_SFKD_NO_ACTION | SQL_SFKD_SET_DEFAULT | SQL_SFKD_SET_NULL;
			break;
		case SQL_SQL92_FOREIGN_KEY_UPDATE_RULE:
			len = 4;
			value = SQL_SFKU_CASCADE | SQL_SFKU_NO_ACTION | SQL_SFKU_SET_DEFAULT | SQL_SFKU_SET_NULL;
			break;
		case SQL_SQL92_GRANT:
			len = 4;
			value = SQL_SG_DELETE_TABLE | SQL_SG_INSERT_TABLE | SQL_SG_REFERENCES_TABLE | SQL_SG_SELECT_TABLE | SQL_SG_UPDATE_TABLE;
			break;
		case SQL_SQL92_NUMERIC_VALUE_FUNCTIONS:
			len = 4;
			value = SQL_SNVF_BIT_LENGTH | SQL_SNVF_CHAR_LENGTH 
				| SQL_SNVF_CHARACTER_LENGTH | SQL_SNVF_EXTRACT
				| SQL_SNVF_OCTET_LENGTH | SQL_SNVF_POSITION;
			break;
		case SQL_SQL92_PREDICATES:
			len = 4;
			value = SQL_SP_BETWEEN | SQL_SP_COMPARISON
				| SQL_SP_EXISTS | SQL_SP_IN
				| SQL_SP_ISNOTNULL | SQL_SP_ISNULL
				| SQL_SP_LIKE | SQL_SP_OVERLAPS
				| SQL_SP_QUANTIFIED_COMPARISON;
			break;
		case SQL_SQL92_RELATIONAL_JOIN_OPERATORS:
			len = 4;
			if (PG_VERSION_GE(conn, 7.1))
				value = SQL_SRJO_CROSS_JOIN | SQL_SRJO_EXCEPT_JOIN
					| SQL_SRJO_FULL_OUTER_JOIN | SQL_SRJO_INNER_JOIN
					| SQL_SRJO_INTERSECT_JOIN | SQL_SRJO_LEFT_OUTER_JOIN
					| SQL_SRJO_NATURAL_JOIN | SQL_SRJO_RIGHT_OUTER_JOIN
					| SQL_SRJO_UNION_JOIN; 
			break;
		case SQL_SQL92_REVOKE:
			len = 4;
			value = SQL_SR_DELETE_TABLE | SQL_SR_INSERT_TABLE | SQL_SR_REFERENCES_TABLE | SQL_SR_SELECT_TABLE | SQL_SR_UPDATE_TABLE;
			break;
		case SQL_SQL92_ROW_VALUE_CONSTRUCTOR:
			len = 4;
			value = SQL_SRVC_VALUE_EXPRESSION | SQL_SRVC_NULL;
			break;
		case SQL_SQL92_STRING_FUNCTIONS:
			len = 4;
			value = SQL_SSF_CONVERT | SQL_SSF_LOWER
				| SQL_SSF_UPPER | SQL_SSF_SUBSTRING
				| SQL_SSF_TRANSLATE | SQL_SSF_TRIM_BOTH
				| SQL_SSF_TRIM_LEADING | SQL_SSF_TRIM_TRAILING;
			break;
		case SQL_SQL92_VALUE_EXPRESSIONS:
			len = 4;
			value = SQL_SVE_CASE | SQL_SVE_CAST | SQL_SVE_COALESCE | SQL_SVE_NULLIF;
			break;
		/* The followings aren't implemented yet */
		case SQL_DATETIME_LITERALS:
			len = 4;
		case SQL_DM_VER:
			len = 0;
		case SQL_DRIVER_HDESC:
			len = 4;
		case SQL_MAX_ASYNC_CONCURRENT_STATEMENTS:
			len = 4;
		case SQL_STANDARD_CLI_CONFORMANCE:
			len = 4;
		case SQL_XOPEN_CLI_YEAR:
			len = 0;
		default:
			/* unrecognized key */
			conn->errormsg = "Unrecognized key passed to SQLGetInfo30.";
			conn->errornumber = CONN_NOT_IMPLEMENTED_ERROR;
			CC_log_error(func, "", conn);
			return SQL_ERROR;
	}
	result = SQL_SUCCESS;
	mylog("%s: p='%s', len=%d, value=%d, cbMax=%d\n", func, p ? p : "<NULL>", len, value, cbInfoValueMax);
	if (p)
	{
		/* char/binary data */
		len = strlen(p);

		if (rgbInfoValue)
		{
#ifdef	UNICODE_SUPPORT
			if (conn->unicode)
			{
				len = utf8_to_ucs2(p, len, (SQLWCHAR *) rgbInfoValue, cbInfoValueMax / 2);
				len *= 2;
			}
			else
#endif /* UNICODE_SUPPORT */
			strncpy_null((char *) rgbInfoValue, p, (size_t) cbInfoValueMax);

			if (len >= cbInfoValueMax)
			{
				result = SQL_SUCCESS_WITH_INFO;
				conn->errornumber = CONN_TRUNCATED;
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
