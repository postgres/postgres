/*--------
 * Module:			pgtypes.c
 *
 * Description:		This module contains routines for getting information
 *					about the supported Postgres data types.  Only the
 *					function pgtype_to_sqltype() returns an unknown condition.
 *					All other functions return a suitable default so that
 *					even data types that are not directly supported can be
 *					used (it is handled as char data).
 *
 * Classes:			n/a
 *
 * API functions:	none
 *
 * Comments:		See "notice.txt" for copyright and license information.
 *--------
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "psqlodbc.h"
#include "dlg_specific.h"
#include "pgtypes.h"
#include "statement.h"
#include "connection.h"
#include "qresult.h"

#ifndef WIN32
#include "iodbc.h"
#include "isql.h"
#include "isqlext.h"
#else
#include <windows.h>
#include <sql.h>
#include <sqlext.h>
#endif


extern GLOBAL_VALUES globals;

Int4		getCharPrecision(StatementClass *stmt, Int4 type, int col, int handle_unknown_size_as);

/*
 * these are the types we support.	all of the pgtype_ functions should
 * return values for each one of these.
 * Even types not directly supported are handled as character types
 * so all types should work (points, etc.)
 */

/*
 * ALL THESE TYPES ARE NO LONGER REPORTED in SQLGetTypeInfo.  Instead, all
 *  the SQL TYPES are reported and mapped to a corresponding Postgres Type
 */

/*
Int4 pgtypes_defined[]	= {
				PG_TYPE_CHAR,
				PG_TYPE_CHAR2,
				PG_TYPE_CHAR4,
				PG_TYPE_CHAR8,
				PG_TYPE_CHAR16,
				PG_TYPE_NAME,
				PG_TYPE_VARCHAR,
				PG_TYPE_BPCHAR,
				PG_TYPE_DATE,
				PG_TYPE_TIME,
				PG_TYPE_DATETIME,
				PG_TYPE_ABSTIME,
				PG_TYPE_TIMESTAMP,
				PG_TYPE_TEXT,
				PG_TYPE_INT2,
				PG_TYPE_INT4,
				PG_TYPE_FLOAT4,
				PG_TYPE_FLOAT8,
				PG_TYPE_OID,
				PG_TYPE_MONEY,
				PG_TYPE_BOOL,
				PG_TYPE_BYTEA,
				PG_TYPE_LO,
				0 };
*/


/*	These are NOW the SQL Types reported in SQLGetTypeInfo.  */
Int2		sqlTypes[] = {
	SQL_BIGINT,
	/* SQL_BINARY, -- Commented out because VarBinary is more correct. */
	SQL_BIT,
	SQL_CHAR,
	SQL_DATE,
	SQL_DECIMAL,
	SQL_DOUBLE,
	SQL_FLOAT,
	SQL_INTEGER,
	SQL_LONGVARBINARY,
	SQL_LONGVARCHAR,
	SQL_NUMERIC,
	SQL_REAL,
	SQL_SMALLINT,
	SQL_TIME,
	SQL_TIMESTAMP,
	SQL_TINYINT,
	SQL_VARBINARY,
	SQL_VARCHAR,
	0
};


Int4
sqltype_to_pgtype(SWORD fSqlType)
{
	Int4		pgType;

	switch (fSqlType)
	{
		case SQL_BINARY:
			pgType = PG_TYPE_BYTEA;
			break;

		case SQL_CHAR:
			pgType = PG_TYPE_BPCHAR;
			break;

		case SQL_BIT:
			pgType = globals.bools_as_char ? PG_TYPE_CHAR : PG_TYPE_BOOL;
			break;

		case SQL_DATE:
			pgType = PG_TYPE_DATE;
			break;

		case SQL_DOUBLE:
		case SQL_FLOAT:
			pgType = PG_TYPE_FLOAT8;
			break;

		case SQL_DECIMAL:
		case SQL_NUMERIC:
			pgType = PG_TYPE_NUMERIC;
			break;

		case SQL_BIGINT:
			pgType = PG_TYPE_INT8;
			break;

		case SQL_INTEGER:
			pgType = PG_TYPE_INT4;
			break;

		case SQL_LONGVARBINARY:
			pgType = PG_TYPE_LO;
			break;

		case SQL_LONGVARCHAR:
			pgType = globals.text_as_longvarchar ? PG_TYPE_TEXT : PG_TYPE_VARCHAR;
			break;

		case SQL_REAL:
			pgType = PG_TYPE_FLOAT4;
			break;

		case SQL_SMALLINT:
		case SQL_TINYINT:
			pgType = PG_TYPE_INT2;
			break;

		case SQL_TIME:
			pgType = PG_TYPE_TIME;
			break;

		case SQL_TIMESTAMP:
			pgType = PG_TYPE_DATETIME;
			break;

		case SQL_VARBINARY:
			pgType = PG_TYPE_BYTEA;
			break;

		case SQL_VARCHAR:
			pgType = PG_TYPE_VARCHAR;
			break;

		default:
			pgType = 0;			/* ??? */
			break;
	}

	return pgType;
}


/*
 *	There are two ways of calling this function:
 *
 *	1.	When going through the supported PG types (SQLGetTypeInfo)
 *
 *	2.	When taking any type id (SQLColumns, SQLGetData)
 *
 *	The first type will always work because all the types defined are returned here.
 *	The second type will return a default based on global parameter when it does not
 *	know.	This allows for supporting
 *	types that are unknown.  All other pg routines in here return a suitable default.
 */
Int2
pgtype_to_sqltype(StatementClass *stmt, Int4 type)
{
	switch (type)
	{
		case PG_TYPE_CHAR:
		case PG_TYPE_CHAR2:
		case PG_TYPE_CHAR4:
		case PG_TYPE_CHAR8:
		case PG_TYPE_NAME:
			return SQL_CHAR;

		case PG_TYPE_BPCHAR:
			return SQL_CHAR;

		case PG_TYPE_VARCHAR:
			return SQL_VARCHAR;

		case PG_TYPE_TEXT:
			return globals.text_as_longvarchar ? SQL_LONGVARCHAR : SQL_VARCHAR;

		case PG_TYPE_BYTEA:
			return SQL_VARBINARY;
		case PG_TYPE_LO:
			return SQL_LONGVARBINARY;

		case PG_TYPE_INT2:
			return SQL_SMALLINT;

		case PG_TYPE_OID:
		case PG_TYPE_XID:
		case PG_TYPE_INT4:
			return SQL_INTEGER;

		/* Change this to SQL_BIGINT for ODBC v3 bjm 2001-01-23 */
		case PG_TYPE_INT8:
			return SQL_CHAR;

		case PG_TYPE_NUMERIC:
			return SQL_NUMERIC;

		case PG_TYPE_FLOAT4:
			return SQL_REAL;
		case PG_TYPE_FLOAT8:
			return SQL_FLOAT;
		case PG_TYPE_DATE:
			return SQL_DATE;
		case PG_TYPE_TIME:
			return SQL_TIME;
		case PG_TYPE_ABSTIME:
		case PG_TYPE_DATETIME:
		case PG_TYPE_TIMESTAMP:
			return SQL_TIMESTAMP;
		case PG_TYPE_MONEY:
			return SQL_FLOAT;
		case PG_TYPE_BOOL:
			return globals.bools_as_char ? SQL_CHAR : SQL_BIT;

		default:
			/*
			 * first, check to see if 'type' is in list.  If not, look up
			 * with query. Add oid, name to list.  If it's already in
			 * list, just return.
			 */
			/* hack until permanent type is available */
			if (type == stmt->hdbc->lobj_type)	
				return SQL_LONGVARBINARY;

			return globals.unknowns_as_longvarchar ? SQL_LONGVARCHAR : SQL_VARCHAR;
	}
}


Int2
pgtype_to_ctype(StatementClass *stmt, Int4 type)
{
	switch (type)
	{
		case PG_TYPE_INT8:
			return SQL_C_CHAR;
		case PG_TYPE_NUMERIC:
			return SQL_C_CHAR;
		case PG_TYPE_INT2:
			return SQL_C_SSHORT;
		case PG_TYPE_OID:
		case PG_TYPE_XID:
		case PG_TYPE_INT4:
			return SQL_C_SLONG;
		case PG_TYPE_FLOAT4:
			return SQL_C_FLOAT;
		case PG_TYPE_FLOAT8:
			return SQL_C_DOUBLE;
		case PG_TYPE_DATE:
			return SQL_C_DATE;
		case PG_TYPE_TIME:
			return SQL_C_TIME;
		case PG_TYPE_ABSTIME:
		case PG_TYPE_DATETIME:
		case PG_TYPE_TIMESTAMP:
			return SQL_C_TIMESTAMP;
		case PG_TYPE_MONEY:
			return SQL_C_FLOAT;
		case PG_TYPE_BOOL:
			return globals.bools_as_char ? SQL_C_CHAR : SQL_C_BIT;

		case PG_TYPE_BYTEA:
			return SQL_C_BINARY;
		case PG_TYPE_LO:
			return SQL_C_BINARY;

		default:
			/* hack until permanent type is available */
			if (type == stmt->hdbc->lobj_type)
				return SQL_C_BINARY;

			return SQL_C_CHAR;
	}
}


char *
pgtype_to_name(StatementClass *stmt, Int4 type)
{
	switch (type)
	{
		case PG_TYPE_CHAR:return "char";
		case PG_TYPE_CHAR2:
			return "char2";
		case PG_TYPE_CHAR4:
			return "char4";
		case PG_TYPE_CHAR8:
			return "char8";
		case PG_TYPE_INT8:
			return "int8";
		case PG_TYPE_NUMERIC:
			return "numeric";
		case PG_TYPE_VARCHAR:
			return "varchar";
		case PG_TYPE_BPCHAR:
			return "char";
		case PG_TYPE_TEXT:
			return "text";
		case PG_TYPE_NAME:
			return "name";
		case PG_TYPE_INT2:
			return "int2";
		case PG_TYPE_OID:
			return "oid";
		case PG_TYPE_INT4:
			return "int4";
		case PG_TYPE_FLOAT4:
			return "float4";
		case PG_TYPE_FLOAT8:
			return "float8";
		case PG_TYPE_DATE:
			return "date";
		case PG_TYPE_TIME:
			return "time";
		case PG_TYPE_ABSTIME:
			return "abstime";
		case PG_TYPE_DATETIME:
			return "datetime";
		case PG_TYPE_TIMESTAMP:
			return "timestamp";
		case PG_TYPE_MONEY:
			return "money";
		case PG_TYPE_BOOL:
			return "bool";
		case PG_TYPE_BYTEA:
			return "bytea";

		case PG_TYPE_LO:
			return PG_TYPE_LO_NAME;

		default:
				/* hack until permanent type is available */
			if (type == stmt->hdbc->lobj_type)	
				return PG_TYPE_LO_NAME;

			/*
			 * "unknown" can actually be used in alter table because it is
			 * a real PG type!
			 */
			return "unknown";
	}
}


static Int2
getNumericScale(StatementClass *stmt, Int4 type, int col)
{
	Int4		atttypmod;
	QResultClass *result;
	ColumnInfoClass *flds;

	mylog("getNumericScale: type=%d, col=%d, unknown = %d\n", type, col);

	if (col < 0)
		return PG_NUMERIC_MAX_SCALE;

	result = SC_get_Result(stmt);

	/*
	 * Manual Result Sets -- use assigned column width (i.e., from
	 * set_tuplefield_string)
	 */
	if (stmt->manual_result)
	{
		flds = result->fields;
		if (flds)
			return flds->adtsize[col];
		else
			return PG_NUMERIC_MAX_SCALE;
	}

	atttypmod = QR_get_atttypmod(result, col);
	if (atttypmod > -1)
		return (atttypmod & 0xffff);
	else
		return (QR_get_display_size(result, col) ?
				QR_get_display_size(result, col) :
				PG_NUMERIC_MAX_SCALE);
}


static Int4
getNumericPrecision(StatementClass *stmt, Int4 type, int col)
{
	Int4		atttypmod;
	QResultClass *result;
	ColumnInfoClass *flds;

	mylog("getNumericPrecision: type=%d, col=%d, unknown = %d\n", type, col);

	if (col < 0)
		return PG_NUMERIC_MAX_PRECISION;

	result = SC_get_Result(stmt);

	/*
	 * Manual Result Sets -- use assigned column width (i.e., from
	 * set_tuplefield_string)
	 */
	if (stmt->manual_result)
	{
		flds = result->fields;
		if (flds)
			return flds->adtsize[col];
		else
			return PG_NUMERIC_MAX_PRECISION;
	}

	atttypmod = QR_get_atttypmod(result, col);
	if (atttypmod > -1)
		return (atttypmod >> 16) & 0xffff;
	else
		return (QR_get_display_size(result, col) >= 0 ?
				QR_get_display_size(result, col) :
				PG_NUMERIC_MAX_PRECISION);
}


Int4
getCharPrecision(StatementClass *stmt, Int4 type, int col, int handle_unknown_size_as)
{
	int			p = -1,
				maxsize;
	QResultClass *result;
	ColumnInfoClass *flds;

	mylog("getCharPrecision: type=%d, col=%d, unknown = %d\n", type, col, handle_unknown_size_as);

	/* Assign Maximum size based on parameters */
	switch (type)
	{
		case PG_TYPE_TEXT:
			if (globals.text_as_longvarchar)
				maxsize = globals.max_longvarchar_size;
			else
				maxsize = globals.max_varchar_size;
			break;

		case PG_TYPE_VARCHAR:
		case PG_TYPE_BPCHAR:
			maxsize = globals.max_varchar_size;
			break;

		default:
			if (globals.unknowns_as_longvarchar)
				maxsize = globals.max_longvarchar_size;
			else
				maxsize = globals.max_varchar_size;
			break;
	}

	/*
	 * Static Precision (i.e., the Maximum Precision of the datatype) This
	 * has nothing to do with a result set.
	 */
	if (col < 0)
		return maxsize;

	result = SC_get_Result(stmt);

	/*
	 * Manual Result Sets -- use assigned column width (i.e., from
	 * set_tuplefield_string)
	 */
	if (stmt->manual_result)
	{
		flds = result->fields;
		if (flds)
			return flds->adtsize[col];
		else
			return maxsize;
	}

	/* Size is unknown -- handle according to parameter */
	if (QR_get_atttypmod(result, col) > -1)
		return QR_get_atttypmod(result, col);

	if (type == PG_TYPE_BPCHAR || handle_unknown_size_as == UNKNOWNS_AS_LONGEST)
	{
		p = QR_get_display_size(result, col);
		mylog("getCharPrecision: LONGEST: p = %d\n", p);
	}

	if (p < 0 && handle_unknown_size_as == UNKNOWNS_AS_MAX)
		return maxsize;
	else
		return p;
}


/*
 *	For PG_TYPE_VARCHAR, PG_TYPE_BPCHAR, PG_TYPE_NUMERIC, SQLColumns will
 *	override this length with the atttypmod length from pg_attribute .
 *
 *	If col >= 0, then will attempt to get the info from the result set.
 *	This is used for functions SQLDescribeCol and SQLColAttributes.
 */
Int4
pgtype_precision(StatementClass *stmt, Int4 type, int col, int handle_unknown_size_as)
{
	switch (type)
	{
		case PG_TYPE_CHAR:
			return 1;
		case PG_TYPE_CHAR2:
			return 2;
		case PG_TYPE_CHAR4:
			return 4;
		case PG_TYPE_CHAR8:
			return 8;

		case PG_TYPE_NAME:
			return NAME_FIELD_SIZE;

		case PG_TYPE_INT2:
			return 5;

		case PG_TYPE_OID:
		case PG_TYPE_XID:
		case PG_TYPE_INT4:
			return 10;

		case PG_TYPE_INT8:
			return 19;			/* signed */

		case PG_TYPE_NUMERIC:
			return getNumericPrecision(stmt, type, col);

		case PG_TYPE_FLOAT4:
		case PG_TYPE_MONEY:
			return 7;

		case PG_TYPE_FLOAT8:
			return 15;

		case PG_TYPE_DATE:
			return 10;
		case PG_TYPE_TIME:
			return 8;

		case PG_TYPE_ABSTIME:
		case PG_TYPE_DATETIME:
		case PG_TYPE_TIMESTAMP:
			return 19;

		case PG_TYPE_BOOL:
			return 1;

		case PG_TYPE_LO:
			return SQL_NO_TOTAL;

		default:

			if (type == stmt->hdbc->lobj_type)	/* hack until permanent
												 * type is available */
				return SQL_NO_TOTAL;

			/* Handle Character types and unknown types */
			return getCharPrecision(stmt, type, col, handle_unknown_size_as);
	}
}


Int4
pgtype_display_size(StatementClass *stmt, Int4 type, int col, int handle_unknown_size_as)
{
	switch (type)
	{
		case PG_TYPE_INT2:
			return 6;

		case PG_TYPE_OID:
		case PG_TYPE_XID:
			return 10;

		case PG_TYPE_INT4:
			return 11;

		case PG_TYPE_INT8:
			return 20;			/* signed: 19 digits + sign */

		case PG_TYPE_NUMERIC:
			return getNumericPrecision(stmt, type, col) + 2;

		case PG_TYPE_MONEY:
			return 15;			/* ($9,999,999.99) */

		case PG_TYPE_FLOAT4:
			return 13;

		case PG_TYPE_FLOAT8:
			return 22;

		/* Character types use regular precision */
		default:
			return pgtype_precision(stmt, type, col, handle_unknown_size_as);
	}
}


/*
 *	For PG_TYPE_VARCHAR, PG_TYPE_BPCHAR, SQLColumns will
 *	override this length with the atttypmod length from pg_attribute
 */
Int4
pgtype_length(StatementClass *stmt, Int4 type, int col, int handle_unknown_size_as)
{
	switch (type)
	{
		case PG_TYPE_INT2:
			return 2;

		case PG_TYPE_OID:
		case PG_TYPE_XID:
		case PG_TYPE_INT4:
			return 4;

		case PG_TYPE_INT8:
			return 20;			/* signed: 19 digits + sign */

		case PG_TYPE_NUMERIC:
			return getNumericPrecision(stmt, type, col) + 2;

		case PG_TYPE_FLOAT4:
		case PG_TYPE_MONEY:
			return 4;

		case PG_TYPE_FLOAT8:
			return 8;

		case PG_TYPE_DATE:
		case PG_TYPE_TIME:
			return 6;

		case PG_TYPE_ABSTIME:
		case PG_TYPE_DATETIME:
		case PG_TYPE_TIMESTAMP:
			return 16;

		/* Character types (and NUMERIC) use the default precision */
		default:
			return pgtype_precision(stmt, type, col, handle_unknown_size_as);
	}
}


Int2
pgtype_scale(StatementClass *stmt, Int4 type, int col)
{
	switch (type)
	{
		case PG_TYPE_INT2:
		case PG_TYPE_OID:
		case PG_TYPE_XID:
		case PG_TYPE_INT4:
		case PG_TYPE_INT8:
		case PG_TYPE_FLOAT4:
		case PG_TYPE_FLOAT8:
		case PG_TYPE_MONEY:
		case PG_TYPE_BOOL:

		/*
		 * Number of digits to the right of the decimal point in
		 * "yyyy-mm=dd hh:mm:ss[.f...]"
		 */
		case PG_TYPE_ABSTIME:
		case PG_TYPE_DATETIME:
		case PG_TYPE_TIMESTAMP:
			return 0;

		case PG_TYPE_NUMERIC:
			return getNumericScale(stmt, type, col);

		default:
			return -1;
	}
}


Int2
pgtype_radix(StatementClass *stmt, Int4 type)
{
	switch (type)
	{
		case PG_TYPE_INT2:
		case PG_TYPE_OID:
		case PG_TYPE_INT4:
		case PG_TYPE_INT8:
		case PG_TYPE_NUMERIC:
		case PG_TYPE_FLOAT4:
		case PG_TYPE_MONEY:
		case PG_TYPE_FLOAT8:
			return 10;
		default:
			return -1;
	}
}


Int2
pgtype_nullable(StatementClass *stmt, Int4 type)
{
	return SQL_NULLABLE;		/* everything should be nullable */
}


Int2
pgtype_auto_increment(StatementClass *stmt, Int4 type)
{
	switch (type)
	{
		case PG_TYPE_INT2:
		case PG_TYPE_OID:
		case PG_TYPE_XID:
		case PG_TYPE_INT4:
		case PG_TYPE_FLOAT4:
		case PG_TYPE_MONEY:
		case PG_TYPE_BOOL:
		case PG_TYPE_FLOAT8:
		case PG_TYPE_INT8:
		case PG_TYPE_NUMERIC:

		case PG_TYPE_DATE:
		case PG_TYPE_TIME:
		case PG_TYPE_ABSTIME:
		case PG_TYPE_DATETIME:
		case PG_TYPE_TIMESTAMP:
			return FALSE;

		default:
			return -1;
	}
}


Int2
pgtype_case_sensitive(StatementClass *stmt, Int4 type)
{
	switch (type)
	{
		case PG_TYPE_CHAR:

		case PG_TYPE_CHAR2:
		case PG_TYPE_CHAR4:
		case PG_TYPE_CHAR8:

		case PG_TYPE_VARCHAR:
		case PG_TYPE_BPCHAR:
		case PG_TYPE_TEXT:
		case PG_TYPE_NAME:
			return TRUE;

		default:
			return FALSE;
	}
}


Int2
pgtype_money(StatementClass *stmt, Int4 type)
{
	switch (type)
	{
		case PG_TYPE_MONEY:
			return TRUE;
		default:
			return FALSE;
	}
}


Int2
pgtype_searchable(StatementClass *stmt, Int4 type)
{
	switch (type)
	{
		case PG_TYPE_CHAR:
		case PG_TYPE_CHAR2:
		case PG_TYPE_CHAR4:
		case PG_TYPE_CHAR8:

		case PG_TYPE_VARCHAR:
		case PG_TYPE_BPCHAR:
		case PG_TYPE_TEXT:
		case PG_TYPE_NAME:
			return SQL_SEARCHABLE;

		default:
			return SQL_ALL_EXCEPT_LIKE;
	}
}


Int2
pgtype_unsigned(StatementClass *stmt, Int4 type)
{
	switch (type)
	{
		case PG_TYPE_OID:
		case PG_TYPE_XID:
			return TRUE;

		case PG_TYPE_INT2:
		case PG_TYPE_INT4:
		case PG_TYPE_INT8:
		case PG_TYPE_NUMERIC:
		case PG_TYPE_FLOAT4:
		case PG_TYPE_FLOAT8:
		case PG_TYPE_MONEY:
			return FALSE;

		default:
			return -1;
	}
}


char *
pgtype_literal_prefix(StatementClass *stmt, Int4 type)
{
	switch (type)
	{
			case PG_TYPE_INT2:
			case PG_TYPE_OID:
			case PG_TYPE_XID:
			case PG_TYPE_INT4:
			case PG_TYPE_INT8:
			case PG_TYPE_NUMERIC:
			case PG_TYPE_FLOAT4:
			case PG_TYPE_FLOAT8:
			case PG_TYPE_MONEY:return NULL;

		default:
			return "'";
	}
}


char *
pgtype_literal_suffix(StatementClass *stmt, Int4 type)
{
	switch (type)
	{
			case PG_TYPE_INT2:
			case PG_TYPE_OID:
			case PG_TYPE_XID:
			case PG_TYPE_INT4:
			case PG_TYPE_INT8:
			case PG_TYPE_NUMERIC:
			case PG_TYPE_FLOAT4:
			case PG_TYPE_FLOAT8:
			case PG_TYPE_MONEY:return NULL;

		default:
			return "'";
	}
}


char *
pgtype_create_params(StatementClass *stmt, Int4 type)
{
	switch (type)
	{
		case PG_TYPE_CHAR:
		case PG_TYPE_VARCHAR:return "max. length";
		default:
			return NULL;
	}
}


Int2
sqltype_to_default_ctype(Int2 sqltype)
{
	/*
	 *	from the table on page 623 of ODBC 2.0 Programmer's Reference
	 * 	(Appendix D)
	 */
	switch (sqltype)
	{
		case SQL_CHAR:
		case SQL_VARCHAR:
		case SQL_LONGVARCHAR:
		case SQL_DECIMAL:
		case SQL_NUMERIC:
		case SQL_BIGINT:
			return SQL_C_CHAR;

		case SQL_BIT:
			return SQL_C_BIT;

		case SQL_TINYINT:
			return SQL_C_STINYINT;

		case SQL_SMALLINT:
			return SQL_C_SSHORT;

		case SQL_INTEGER:
			return SQL_C_SLONG;

		case SQL_REAL:
			return SQL_C_FLOAT;

		case SQL_FLOAT:
		case SQL_DOUBLE:
			return SQL_C_DOUBLE;

		case SQL_BINARY:
		case SQL_VARBINARY:
		case SQL_LONGVARBINARY:
			return SQL_C_BINARY;

		case SQL_DATE:
			return SQL_C_DATE;

		case SQL_TIME:
			return SQL_C_TIME;

		case SQL_TIMESTAMP:
			return SQL_C_TIMESTAMP;

		default:
			/* should never happen */
			return SQL_C_CHAR;
	}
}

Int4
ctype_length(Int2 ctype)
{
	switch (ctype)
	{
		case SQL_C_SSHORT:
		case SQL_C_SHORT:
			return sizeof(SWORD);

		case SQL_C_USHORT:
			return sizeof(UWORD);

		case SQL_C_SLONG:
		case SQL_C_LONG:
			return sizeof(SDWORD);

		case SQL_C_ULONG:
			return sizeof(UDWORD);

		case SQL_C_FLOAT:
			return sizeof(SFLOAT);

		case SQL_C_DOUBLE:
			return sizeof(SDOUBLE);

		case SQL_C_BIT:
			return sizeof(UCHAR);

		case SQL_C_STINYINT:
		case SQL_C_TINYINT:
			return sizeof(SCHAR);

		case SQL_C_UTINYINT:
			return sizeof(UCHAR);

		case SQL_C_DATE:
			return sizeof(DATE_STRUCT);

		case SQL_C_TIME:
			return sizeof(TIME_STRUCT);

		case SQL_C_TIMESTAMP:
			return sizeof(TIMESTAMP_STRUCT);

		case SQL_C_BINARY:
		case SQL_C_CHAR:
			return 0;

		default:				/* should never happen */
			return 0;
	}
}
