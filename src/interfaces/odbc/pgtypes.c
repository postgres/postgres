
/* Module:          pgtypes.c
 *
 * Description:     This module contains routines for getting information
 *                  about the supported Postgres data types.  Only the function
 *                  pgtype_to_sqltype() returns an unknown condition.  All other
 *                  functions return a suitable default so that even data types that
 *                  are not directly supported can be used (it is handled as char data).
 *
 * Classes:         n/a
 *
 * API functions:   none
 *
 * Comments:        See "notice.txt" for copyright and license information.
 *
 */

#include "psqlodbc.h"
#include "pgtypes.h"
#include <windows.h>
#include <sql.h>
#include <sqlext.h>

/* these are the types we support.  all of the pgtype_ functions should */
/* return values for each one of these.                                 */

/* NOTE: Even types not directly supported are handled as character types
		so all types should work (points, etc.) */

Int4 pgtypes_defined[]  = { 
				PG_TYPE_CHAR,
				PG_TYPE_CHAR2,
				PG_TYPE_CHAR4,
			    PG_TYPE_CHAR8,
			    PG_TYPE_BPCHAR,
			    PG_TYPE_VARCHAR,
				PG_TYPE_DATE,
				PG_TYPE_TIME,
				PG_TYPE_ABSTIME,	/* a timestamp, sort of */
			    PG_TYPE_TEXT,
			    PG_TYPE_NAME,
			    PG_TYPE_INT2,
			    PG_TYPE_INT4,
			    PG_TYPE_FLOAT4,
			    PG_TYPE_FLOAT8,
			    PG_TYPE_OID,
				PG_TYPE_MONEY,
				PG_TYPE_BOOL,
				PG_TYPE_CHAR16,
				PG_TYPE_DATETIME,
				PG_TYPE_BYTEA,
			    0 };
			  

/*	There are two ways of calling this function:  
	1.	When going through the supported PG types (SQLGetTypeInfo)
	2.	When taking any type id (SQLColumns, SQLGetData)

	The first type will always work because all the types defined are returned here.
	The second type will return PG_UNKNOWN when it does not know.  The calling
	routine checks for this and changes it to a char type.  This allows for supporting
	types that are unknown.  All other pg routines in here return a suitable default.
*/
Int2 pgtype_to_sqltype(Int4 type)
{
    switch(type) {
    case PG_TYPE_CHAR:
	case PG_TYPE_CHAR2:
	case PG_TYPE_CHAR4:
    case PG_TYPE_CHAR8:
	case PG_TYPE_CHAR16:		return SQL_CHAR;

    case PG_TYPE_BPCHAR:
    case PG_TYPE_NAME:          
    case PG_TYPE_VARCHAR:		return SQL_VARCHAR;

    case PG_TYPE_TEXT:			return SQL_LONGVARCHAR;
	case PG_TYPE_BYTEA:			return SQL_LONGVARBINARY;

    case PG_TYPE_INT2:          return SQL_SMALLINT;
    case PG_TYPE_OID:
    case PG_TYPE_INT4:          return SQL_INTEGER;
    case PG_TYPE_FLOAT4:        return SQL_REAL;
    case PG_TYPE_FLOAT8:        return SQL_FLOAT;
	case PG_TYPE_DATE:			return SQL_DATE;
	case PG_TYPE_TIME:			return SQL_TIME;
	case PG_TYPE_ABSTIME:		
	case PG_TYPE_DATETIME:		return SQL_TIMESTAMP;
	case PG_TYPE_MONEY:			return SQL_FLOAT;
	case PG_TYPE_BOOL:			return SQL_CHAR;

    default:                    return PG_UNKNOWN;	/* check return for this */
    }
}

Int2 pgtype_to_ctype(Int4 type)
{
    switch(type) {
    case PG_TYPE_INT2:          return SQL_C_SSHORT;
    case PG_TYPE_OID:
    case PG_TYPE_INT4:          return SQL_C_SLONG;
    case PG_TYPE_FLOAT4:        return SQL_C_FLOAT;
    case PG_TYPE_FLOAT8:        return SQL_C_DOUBLE;
	case PG_TYPE_DATE:			return SQL_C_DATE;
	case PG_TYPE_TIME:			return SQL_C_TIME;
	case PG_TYPE_ABSTIME:		
	case PG_TYPE_DATETIME:		return SQL_C_TIMESTAMP;
	case PG_TYPE_MONEY:			return SQL_C_FLOAT;
	case PG_TYPE_BOOL:			return SQL_C_CHAR;

	case PG_TYPE_BYTEA:			return SQL_C_BINARY;

    default:                    return SQL_C_CHAR;
    }
}

char *pgtype_to_name(Int4 type)
{
    switch(type) {
    case PG_TYPE_CHAR:          return "char";
	case PG_TYPE_CHAR2:			return "char2";
	case PG_TYPE_CHAR4:			return "char4";
    case PG_TYPE_CHAR8:         return "char8";
	case PG_TYPE_CHAR16:		return "char16";
    case PG_TYPE_VARCHAR:       return "varchar";
    case PG_TYPE_BPCHAR:        return "bpchar";
    case PG_TYPE_TEXT:          return "text";
    case PG_TYPE_NAME:          return "name";
    case PG_TYPE_INT2:          return "int2";
    case PG_TYPE_OID:           return "oid";
    case PG_TYPE_INT4:          return "int4";
    case PG_TYPE_FLOAT4:        return "float4";
    case PG_TYPE_FLOAT8:        return "float8";
	case PG_TYPE_DATE:			return "date";
	case PG_TYPE_TIME:			return "time";
	case PG_TYPE_ABSTIME:		return "abstime";
	case PG_TYPE_DATETIME:		return "datetime";
	case PG_TYPE_MONEY:			return "money";
	case PG_TYPE_BOOL:			return "bool";
	case PG_TYPE_BYTEA:			return "bytea";

	/* "unknown" can actually be used in alter table because it is a real PG type! */
    default:                    return "unknown";	
    }    
}

/*	For PG_TYPE_VARCHAR, PG_TYPE_BPCHAR, SQLColumns will 
	override this length with the atttypmod length from pg_attribute 
*/
Int4 pgtype_precision(Int4 type)
{
	switch(type) {

	case PG_TYPE_CHAR:			return 1;
	case PG_TYPE_CHAR2:			return 2;
	case PG_TYPE_CHAR4:			return 4;
	case PG_TYPE_CHAR8:       	return 8;
	case PG_TYPE_CHAR16:       	return 16;

	case PG_TYPE_NAME:			return 32;

	case PG_TYPE_VARCHAR:
	case PG_TYPE_BPCHAR:		return MAX_VARCHAR_SIZE;

	case PG_TYPE_INT2:          return 5;

	case PG_TYPE_OID:
	case PG_TYPE_INT4:          return 10;

	case PG_TYPE_FLOAT4:        
	case PG_TYPE_MONEY:			return 7;

	case PG_TYPE_FLOAT8:        return 15;

	case PG_TYPE_DATE:			return 10;
	case PG_TYPE_TIME:			return 8;

	case PG_TYPE_ABSTIME:		
	case PG_TYPE_DATETIME:		return 19;

	case PG_TYPE_BOOL:			return 1;

	default:
		return TEXT_FIELD_SIZE;		/* text field types and unknown types */
    }
}

/*	For PG_TYPE_VARCHAR, PG_TYPE_BPCHAR, SQLColumns will 
	override this length with the atttypmod length from pg_attribute 
*/
Int4 pgtype_length(Int4 type)
{
	switch(type) {

	case PG_TYPE_CHAR:			return 1;
	case PG_TYPE_CHAR2:			return 2;
	case PG_TYPE_CHAR4:			return 4;
	case PG_TYPE_CHAR8:         return 8;
	case PG_TYPE_CHAR16:        return 16;

	case PG_TYPE_NAME:			return 32;

	case PG_TYPE_VARCHAR:
	case PG_TYPE_BPCHAR:		return MAX_VARCHAR_SIZE;

	case PG_TYPE_INT2:          return 2;

	case PG_TYPE_OID:
	case PG_TYPE_INT4:          return 4;

	case PG_TYPE_FLOAT4:
	case PG_TYPE_MONEY:			return 4;

	case PG_TYPE_FLOAT8:        return 8;

	case PG_TYPE_DATE:
	case PG_TYPE_TIME:			return 6;

	case PG_TYPE_ABSTIME:		
	case PG_TYPE_DATETIME:		return 16;

	case PG_TYPE_BOOL:			return 1;

	default:
		return TEXT_FIELD_SIZE;		/* text field types and unknown types */
    }
}

Int2 pgtype_scale(Int4 type)
{
	switch(type) {

	case PG_TYPE_INT2:
	case PG_TYPE_OID:
	case PG_TYPE_INT4:
	case PG_TYPE_FLOAT4:
	case PG_TYPE_FLOAT8:
	case PG_TYPE_MONEY:
	case PG_TYPE_BOOL:

	/*	Number of digits to the right of the decimal point in "yyyy-mm=dd hh:mm:ss[.f...]" */
	case PG_TYPE_ABSTIME:		
	case PG_TYPE_DATETIME:		return 0;

	default:					return -1;
	}
}


Int2 pgtype_radix(Int4 type)
{
    switch(type) {
    case PG_TYPE_INT2:
    case PG_TYPE_OID:
    case PG_TYPE_INT4:
    case PG_TYPE_FLOAT4:
	case PG_TYPE_MONEY:
    case PG_TYPE_FLOAT8:        return 10;

    default:                    return -1;
    }
}

Int2 pgtype_nullable(Int4 type)
{
	return SQL_NULLABLE;	/* everything should be nullable */
}

Int2 pgtype_auto_increment(Int4 type)
{
	switch(type) {

	case PG_TYPE_INT2:         
	case PG_TYPE_OID:
	case PG_TYPE_INT4:         
	case PG_TYPE_FLOAT4:       
	case PG_TYPE_MONEY:
	case PG_TYPE_BOOL:
	case PG_TYPE_FLOAT8:

	case PG_TYPE_DATE:
	case PG_TYPE_TIME:			
	case PG_TYPE_ABSTIME:		
	case PG_TYPE_DATETIME:		return FALSE;

	default:					return -1;
	}    
}

Int2 pgtype_case_sensitive(Int4 type)
{
    switch(type) {
    case PG_TYPE_CHAR:          

	case PG_TYPE_CHAR2:
	case PG_TYPE_CHAR4:
    case PG_TYPE_CHAR8:         
	case PG_TYPE_CHAR16:		

    case PG_TYPE_VARCHAR:       
    case PG_TYPE_BPCHAR:
    case PG_TYPE_TEXT:
    case PG_TYPE_NAME:          return TRUE;

    default:                    return FALSE;
    }
}

Int2 pgtype_money(Int4 type)
{
	switch(type) {
	case PG_TYPE_MONEY:			return TRUE;
	default:					return FALSE;
	}    
}

Int2 pgtype_searchable(Int4 type)
{
	switch(type) {
	case PG_TYPE_CHAR:          
	case PG_TYPE_CHAR2:
	case PG_TYPE_CHAR4:			
	case PG_TYPE_CHAR8:
	case PG_TYPE_CHAR16:		

	case PG_TYPE_VARCHAR:       
	case PG_TYPE_BPCHAR:
	case PG_TYPE_TEXT:
	case PG_TYPE_NAME:          return SQL_SEARCHABLE;

	default:					return SQL_ALL_EXCEPT_LIKE;

	}    
}

Int2 pgtype_unsigned(Int4 type)
{
	switch(type) {
	case PG_TYPE_OID:			return TRUE;

	case PG_TYPE_INT2:
	case PG_TYPE_INT4:
	case PG_TYPE_FLOAT4:
	case PG_TYPE_FLOAT8:
	case PG_TYPE_MONEY:			return FALSE;

	default:					return -1;
	}
}

char *pgtype_literal_prefix(Int4 type)
{
	switch(type) {

	case PG_TYPE_INT2:
	case PG_TYPE_OID:
	case PG_TYPE_INT4:
	case PG_TYPE_FLOAT4:
	case PG_TYPE_FLOAT8:        
	case PG_TYPE_MONEY:			return NULL;

	default:					return "'";
	}
}

char *pgtype_literal_suffix(Int4 type)
{
	switch(type) {

	case PG_TYPE_INT2:
	case PG_TYPE_OID:
	case PG_TYPE_INT4:
	case PG_TYPE_FLOAT4:
	case PG_TYPE_FLOAT8:        
	case PG_TYPE_MONEY:			return NULL;

	default:					return "'";
	}
}

char *pgtype_create_params(Int4 type)
{
	switch(type) {
	case PG_TYPE_CHAR:
	case PG_TYPE_VARCHAR:		return "max. length";
	default:					return NULL;
	}
}


Int2 sqltype_to_default_ctype(Int2 sqltype)
{
    // from the table on page 623 of ODBC 2.0 Programmer's Reference
    // (Appendix D)
    switch(sqltype) {
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

    default:			/* should never happen */
		return SQL_C_CHAR;	
    }
}

