
/* Module:          info.c
 *
 * Description:     This module contains routines related to
 *                  ODBC informational functions.
 *
 * Classes:         n/a
 *
 * API functions:   SQLGetInfo, SQLGetTypeInfo, SQLGetFunctions, 
 *                  SQLTables, SQLColumns, SQLStatistics, SQLSpecialColumns,
 *                  SQLPrimaryKeys, SQLForeignKeys, 
 *                  SQLProcedureColumns(NI), SQLProcedures(NI), 
 *                  SQLTablePrivileges(NI), SQLColumnPrivileges(NI)
 *
 * Comments:        See "notice.txt" for copyright and license information.
 *
 */

#include <string.h>
#include <stdio.h>
#include "psqlodbc.h"
#include <windows.h>
#include <sql.h> 
#include <sqlext.h>
#include "tuple.h"
#include "pgtypes.h"

#include "environ.h"
#include "connection.h"
#include "statement.h"
#include "qresult.h"
#include "bind.h"
#include "misc.h"
#include "pgtypes.h"


extern GLOBAL_VALUES globals;

//      -       -       -       -       -       -       -       -       -

RETCODE SQL_API SQLGetInfo(
        HDBC      hdbc,
        UWORD     fInfoType,
        PTR       rgbInfoValue,
        SWORD     cbInfoValueMax,
        SWORD FAR *pcbInfoValue)
{
char *func = "SQLGetInfo";
ConnectionClass *conn = (ConnectionClass *) hdbc;
char *p;

	if ( ! conn) {
		CC_log_error(func, "", NULL);
		return SQL_INVALID_HANDLE;
	}

    if (NULL == (char *)rgbInfoValue) {
		CC_log_error(func, "Bad rgbInfoValue", conn);
        return SQL_INVALID_HANDLE;
	}


    switch (fInfoType) {
    case SQL_ACCESSIBLE_PROCEDURES: /* ODBC 1.0 */
        // can the user call all functions returned by SQLProcedures?
        // I assume access permissions could prevent this in some cases(?)
        // anyway, SQLProcedures doesn't exist yet.
        if (pcbInfoValue) *pcbInfoValue = 1;
        strncpy_null((char *)rgbInfoValue, "N", (size_t)cbInfoValueMax);
        break;

    case SQL_ACCESSIBLE_TABLES: /* ODBC 1.0 */
        // is the user guaranteed "SELECT" on every table?
        if (pcbInfoValue) *pcbInfoValue = 1;
        strncpy_null((char *)rgbInfoValue, "N", (size_t)cbInfoValueMax);
        break;

    case SQL_ACTIVE_CONNECTIONS: /* ODBC 1.0 */
        // how many simultaneous connections do we support?
        *((WORD *)rgbInfoValue) = MAX_CONNECTIONS;
        if(pcbInfoValue) { *pcbInfoValue = 2; }
        break;

    case SQL_ACTIVE_STATEMENTS: /* ODBC 1.0 */
        // no limit on the number of active statements.
        *((WORD *)rgbInfoValue) = (WORD)0;
        if(pcbInfoValue) { *pcbInfoValue = 2; }
        break;

    case SQL_ALTER_TABLE: /* ODBC 2.0 */
        // what does 'alter table' support? (bitmask)
        // postgres doesn't seem to let you drop columns.
        *((DWORD *)rgbInfoValue) = SQL_AT_ADD_COLUMN;
        if(pcbInfoValue) { *pcbInfoValue = 4; }
        break;

    case SQL_BOOKMARK_PERSISTENCE: /* ODBC 2.0 */
        // through what operations do bookmarks persist? (bitmask)
        // bookmarks don't exist yet, so they're not very persistent.
        *((DWORD *)rgbInfoValue) = 0;
        if(pcbInfoValue) { *pcbInfoValue = 4; }
        break;

    case SQL_COLUMN_ALIAS: /* ODBC 2.0 */
        // do we support column aliases?  guess not.
        if (pcbInfoValue) *pcbInfoValue = 1;
        strncpy_null((char *)rgbInfoValue, "N", (size_t)cbInfoValueMax);
        break;

    case SQL_CONCAT_NULL_BEHAVIOR: /* ODBC 1.0 */
        // how does concatenation work with NULL columns?
        // not sure how you do concatentation, but this way seems
        // more reasonable
        *((WORD *)rgbInfoValue) = SQL_CB_NON_NULL;
        if(pcbInfoValue) { *pcbInfoValue = 2; }
        break;

        // which types of data-conversion do we support?
        // currently we don't support any, except converting a type
        // to itself.
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
    case SQL_CONVERT_VARCHAR: /* ODBC 1.0 */
        // only return the type we were called with (bitmask)
        *((DWORD *)rgbInfoValue) = fInfoType;
        if(pcbInfoValue) { *pcbInfoValue = 4; }
        break;

    case SQL_CONVERT_FUNCTIONS: /* ODBC 1.0 */
        // which conversion functions do we support? (bitmask)
        *((DWORD *)rgbInfoValue) = 0;
        if(pcbInfoValue) { *pcbInfoValue = 4; }
        break;

    case SQL_CORRELATION_NAME: /* ODBC 1.0 */
        // I don't know what a correlation name is, so I guess we don't
        // support them.

        // *((WORD *)rgbInfoValue) = (WORD)SQL_CN_NONE;

        // well, let's just say we do--otherwise Query won't work.
        *((WORD *)rgbInfoValue) = (WORD)SQL_CN_ANY;
        if(pcbInfoValue) { *pcbInfoValue = 2; }

        break;

    case SQL_CURSOR_COMMIT_BEHAVIOR: /* ODBC 1.0 */
        // postgres definitely closes cursors when a transaction ends,
        // but you shouldn't have to re-prepare a statement after
        // commiting a transaction (I don't think)
        *((WORD *)rgbInfoValue) = (WORD)SQL_CB_CLOSE;
        if(pcbInfoValue) { *pcbInfoValue = 2; }
        break;

    case SQL_CURSOR_ROLLBACK_BEHAVIOR: /* ODBC 1.0 */
        // see above
        *((WORD *)rgbInfoValue) = (WORD)SQL_CB_CLOSE;
        if(pcbInfoValue) { *pcbInfoValue = 2; }
        break;

    case SQL_DATA_SOURCE_NAME: /* ODBC 1.0 */
		p = CC_get_DSN(conn);
		if (pcbInfoValue) *pcbInfoValue = strlen(p);
		strncpy_null((char *)rgbInfoValue, p, (size_t)cbInfoValueMax);
        break;

    case SQL_DATA_SOURCE_READ_ONLY: /* ODBC 1.0 */
        if (pcbInfoValue) *pcbInfoValue = 1;
		sprintf((char *)rgbInfoValue, "%c", CC_is_readonly(conn) ? 'Y' : 'N');
        break;

    case SQL_DATABASE_NAME: /* Support for old ODBC 1.0 Apps */
        // case SQL_CURRENT_QUALIFIER:
        // this tag doesn't seem to be in ODBC 2.0, and it conflicts
        // with a valid tag (SQL_TIMEDATE_ADD_INTERVALS).

		/*	Returning the database name causes problems in MS Query.
			It generates query like: "SELECT DISTINCT a FROM byronncrap3 crap3"
		*/
		p = "";    // CC_get_database(conn);
		if (pcbInfoValue) *pcbInfoValue = strlen(p);
		strncpy_null((char *)rgbInfoValue, p, (size_t)cbInfoValueMax);
		break;

    case SQL_DBMS_NAME: /* ODBC 1.0 */
        if (pcbInfoValue) *pcbInfoValue = 10;
        strncpy_null((char *)rgbInfoValue, DBMS_NAME, (size_t)cbInfoValueMax);
        break;

    case SQL_DBMS_VER: /* ODBC 1.0 */
        if (pcbInfoValue) *pcbInfoValue = 25;
        strncpy_null((char *)rgbInfoValue, DBMS_VERSION, (size_t)cbInfoValueMax);
        break;

    case SQL_DEFAULT_TXN_ISOLATION: /* ODBC 1.0 */
        // are dirty reads, non-repeatable reads, and phantoms possible? (bitmask)
        // by direct experimentation they are not.  postgres forces
        // the newer transaction to wait before doing something that
        // would cause one of these problems.
        *((DWORD *)rgbInfoValue) = SQL_TXN_SERIALIZABLE;
        if(pcbInfoValue) { *pcbInfoValue = 4; }
        break;

    case SQL_DRIVER_NAME: /* ODBC 1.0 */
        // this should be the actual filename of the driver
        p = DRIVER_FILE_NAME;
        if (pcbInfoValue)  *pcbInfoValue = strlen(p);
        strncpy_null((char *)rgbInfoValue, p, (size_t)cbInfoValueMax);
        break;

    case SQL_DRIVER_ODBC_VER:
        if (pcbInfoValue) *pcbInfoValue = 5;
        strncpy_null((char *)rgbInfoValue, "02.00", (size_t)cbInfoValueMax);
        break;

    case SQL_DRIVER_VER: /* ODBC 1.0 */
        p = POSTGRESDRIVERVERSION;
        if (pcbInfoValue) *pcbInfoValue = strlen(p);
        strncpy_null((char *)rgbInfoValue, p, (size_t)cbInfoValueMax);
        break;

    case SQL_EXPRESSIONS_IN_ORDERBY: /* ODBC 1.0 */
        // can you have expressions in an 'order by' clause?
        // not sure about this.  say no for now.
        if (pcbInfoValue) *pcbInfoValue = 1;
        strncpy_null((char *)rgbInfoValue, "N", (size_t)cbInfoValueMax);
        break;

    case SQL_FETCH_DIRECTION: /* ODBC 1.0 */
        // which fetch directions are supported? (bitmask)
        *((DWORD *)rgbInfoValue) = globals.use_declarefetch ? 0 : (SQL_FD_FETCH_NEXT |
                                   SQL_FD_FETCH_FIRST |
                                   SQL_FD_FETCH_LAST |
                                   SQL_FD_FETCH_PRIOR |
                                   SQL_FD_FETCH_ABSOLUTE);
        if(pcbInfoValue) { *pcbInfoValue = 4; }
        break;

    case SQL_FILE_USAGE: /* ODBC 2.0 */
        // we are a two-tier driver, not a file-based one.
        *((WORD *)rgbInfoValue) = (WORD)SQL_FILE_NOT_SUPPORTED;
        if(pcbInfoValue) { *pcbInfoValue = 2; }
        break;

    case SQL_GETDATA_EXTENSIONS: /* ODBC 2.0 */
        // (bitmask)
        *((DWORD *)rgbInfoValue) = (SQL_GD_ANY_COLUMN | SQL_GD_ANY_ORDER | SQL_GD_BOUND);
        if(pcbInfoValue) { *pcbInfoValue = 4; }
        break;

    case SQL_GROUP_BY: /* ODBC 2.0 */
        // how do the columns selected affect the columns you can group by?
        *((WORD *)rgbInfoValue) = SQL_GB_GROUP_BY_EQUALS_SELECT;
        if(pcbInfoValue) { *pcbInfoValue = 2; }
        break;

    case SQL_IDENTIFIER_CASE: /* ODBC 1.0 */
        // are identifiers case-sensitive (yes)
        *((WORD *)rgbInfoValue) = SQL_IC_SENSITIVE;
        if(pcbInfoValue) { *pcbInfoValue = 2; }
        break;

    case SQL_IDENTIFIER_QUOTE_CHAR: /* ODBC 1.0 */
        // the character used to quote "identifiers" (what are they?)
        // the manual index lists 'owner names' and 'qualifiers' as
        // examples of identifiers.  it says return a blank for no
        // quote character, we'll try that...
        if (pcbInfoValue) *pcbInfoValue = 1;
        strncpy_null((char *)rgbInfoValue, " ", (size_t)cbInfoValueMax);
        break;

    case SQL_KEYWORDS: /* ODBC 2.0 */
        // do this later
        conn->errormsg = "SQL_KEYWORDS parameter to SQLGetInfo not implemented.";
        conn->errornumber = CONN_NOT_IMPLEMENTED_ERROR;
		CC_log_error(func, "", conn);
        return SQL_ERROR;
        break;

    case SQL_LIKE_ESCAPE_CLAUSE: /* ODBC 2.0 */
        // is there a character that escapes '%' and '_' in a LIKE clause?
        // not as far as I can tell
        if (pcbInfoValue) *pcbInfoValue = 1;
        strncpy_null((char *)rgbInfoValue, "N", (size_t)cbInfoValueMax);
        break;

    case SQL_LOCK_TYPES: /* ODBC 2.0 */
        // which lock types does SQLSetPos support? (bitmask)
        // SQLSetPos doesn't exist yet
        *((DWORD *)rgbInfoValue) = globals.lie ? (SQL_LCK_NO_CHANGE | SQL_LCK_EXCLUSIVE | SQL_LCK_UNLOCK) : 0;
        if(pcbInfoValue) { *pcbInfoValue = 4; }
        break;

    case SQL_MAX_BINARY_LITERAL_LEN: /* ODBC 2.0 */
        // the maximum length of a query is 2k, so maybe we should
        // set the maximum length of all these literals to that value?
        // for now just use zero for 'unknown or no limit'

        // maximum length of a binary literal
        *((DWORD *)rgbInfoValue) = 0;
        if(pcbInfoValue) { *pcbInfoValue = 4; }
        break;

    case SQL_MAX_CHAR_LITERAL_LEN: /* ODBC 2.0 */
        // maximum length of a character literal
        *((DWORD *)rgbInfoValue) = 0;
        if(pcbInfoValue) { *pcbInfoValue = 4; }
        break;

    case SQL_MAX_COLUMN_NAME_LEN: /* ODBC 1.0 */
        // maximum length of a column name
        *((WORD *)rgbInfoValue) = 32;
        if(pcbInfoValue) { *pcbInfoValue = 2; }
        break;

    case SQL_MAX_COLUMNS_IN_GROUP_BY: /* ODBC 2.0 */
        // maximum number of columns in a 'group by' clause
        *((WORD *)rgbInfoValue) = 0;
        if(pcbInfoValue) { *pcbInfoValue = 2; }
        break;

    case SQL_MAX_COLUMNS_IN_INDEX: /* ODBC 2.0 */
        // maximum number of columns in an index
        *((WORD *)rgbInfoValue) = 0;
        if(pcbInfoValue) { *pcbInfoValue = 2; }
        break;

    case SQL_MAX_COLUMNS_IN_ORDER_BY: /* ODBC 2.0 */
        // maximum number of columns in an ORDER BY statement
        *((WORD *)rgbInfoValue) = 0;
        if(pcbInfoValue) { *pcbInfoValue = 2; }
        break;

    case SQL_MAX_COLUMNS_IN_SELECT: /* ODBC 2.0 */
        *((WORD *)rgbInfoValue) = 0;
        if(pcbInfoValue) { *pcbInfoValue = 2; }
        break;

    case SQL_MAX_COLUMNS_IN_TABLE: /* ODBC 2.0 */
        *((WORD *)rgbInfoValue) = 0;
        if(pcbInfoValue) { *pcbInfoValue = 2; }
        break;

    case SQL_MAX_CURSOR_NAME_LEN: /* ODBC 1.0 */
        *((WORD *)rgbInfoValue) = 32;
        if(pcbInfoValue) { *pcbInfoValue = 2; }
        break;

    case SQL_MAX_INDEX_SIZE: /* ODBC 2.0 */
        *((DWORD *)rgbInfoValue) = 0;
        if(pcbInfoValue) { *pcbInfoValue = 4; }
        break;

    case SQL_MAX_OWNER_NAME_LEN: /* ODBC 1.0 */
        // the maximum length of a table owner's name.  (0 == none)
        // (maybe this should be 8)
        *((WORD *)rgbInfoValue) = (WORD)0;
        if(pcbInfoValue) { *pcbInfoValue = 2; }
        break;

    case SQL_MAX_PROCEDURE_NAME_LEN: /* ODBC 1.0 */
        *((WORD *)rgbInfoValue) = 0;
        if(pcbInfoValue) { *pcbInfoValue = 2; }
        break;

    case SQL_MAX_QUALIFIER_NAME_LEN: /* ODBC 1.0 */
        *((WORD *)rgbInfoValue) = 0;
        if(pcbInfoValue) { *pcbInfoValue = 2; }
        break;

    case SQL_MAX_ROW_SIZE: /* ODBC 2.0 */
        // the maximum size of one row
        // here I do know a definite value
        *((DWORD *)rgbInfoValue) = 8192;
        if(pcbInfoValue) { *pcbInfoValue = 4; }
        break;

    case SQL_MAX_ROW_SIZE_INCLUDES_LONG: /* ODBC 2.0 */
        // does the preceding value include LONGVARCHAR and LONGVARBINARY
        // fields?   Well, it does include longvarchar, but not longvarbinary.
        if (pcbInfoValue) *pcbInfoValue = 1;
        strncpy_null((char *)rgbInfoValue, "Y", (size_t)cbInfoValueMax);
        break;

    case SQL_MAX_STATEMENT_LEN: /* ODBC 2.0 */
        // there should be a definite value here (2k?)
        *((DWORD *)rgbInfoValue) = 0;
        if(pcbInfoValue) { *pcbInfoValue = 4; }
        break;

    case SQL_MAX_TABLE_NAME_LEN: /* ODBC 1.0 */
        *((WORD *)rgbInfoValue) = 32;
        if(pcbInfoValue) { *pcbInfoValue = 2; }
        break;

    case SQL_MAX_TABLES_IN_SELECT: /* ODBC 2.0 */
        *((WORD *)rgbInfoValue) = 0;
        if(pcbInfoValue) { *pcbInfoValue = 2; }
        break;

    case SQL_MAX_USER_NAME_LEN:
        *(SWORD FAR *)rgbInfoValue = 0;
        if(pcbInfoValue) { *pcbInfoValue = 2; }
        break;

    case SQL_MULT_RESULT_SETS: /* ODBC 1.0 */
        // do we support multiple result sets?  Not really, but say yes anyway?
        if (pcbInfoValue) *pcbInfoValue = 1;
        strncpy_null((char *)rgbInfoValue, "Y", (size_t)cbInfoValueMax);
        break;

    case SQL_MULTIPLE_ACTIVE_TXN: /* ODBC 1.0 */
        // do we support multiple simultaneous transactions?
        if (pcbInfoValue) *pcbInfoValue = 1;
        strncpy_null((char *)rgbInfoValue, "Y", (size_t)cbInfoValueMax);
        break;

    case SQL_NEED_LONG_DATA_LEN: /* ODBC 2.0 */
        if (pcbInfoValue) *pcbInfoValue = 1;
		/*	Dont need the length, SQLPutData can handle any size and multiple calls */
        strncpy_null((char *)rgbInfoValue, "N", (size_t)cbInfoValueMax);
        break;

    case SQL_NON_NULLABLE_COLUMNS: /* ODBC 1.0 */
        *((WORD *)rgbInfoValue) = (WORD)SQL_NNC_NON_NULL;
        if(pcbInfoValue) { *pcbInfoValue = 2; }
        break;

    case SQL_NULL_COLLATION: /* ODBC 2.0 */
        // where are nulls sorted?
        *((WORD *)rgbInfoValue) = (WORD)SQL_NC_END;
        if(pcbInfoValue) { *pcbInfoValue = 2; }
        break;

    case SQL_NUMERIC_FUNCTIONS: /* ODBC 1.0 */
        // what numeric functions are supported? (bitmask)
        // I'm not sure if any of these are actually supported
        *((DWORD *)rgbInfoValue) = 0;
        if(pcbInfoValue) { *pcbInfoValue = 4; }
        break;

    case SQL_ODBC_API_CONFORMANCE: /* ODBC 1.0 */
        *((WORD *)rgbInfoValue) = SQL_OAC_LEVEL1;
        if(pcbInfoValue) { *pcbInfoValue = 2; }
        break;

    case SQL_ODBC_SAG_CLI_CONFORMANCE: /* ODBC 1.0 */
        // can't find any reference to SAG in the ODBC reference manual
        // (although it's in the index, it doesn't actually appear on
        // the pages referenced)
        *((WORD *)rgbInfoValue) = SQL_OSCC_NOT_COMPLIANT;
        if(pcbInfoValue) { *pcbInfoValue = 2; }
        break;

    case SQL_ODBC_SQL_CONFORMANCE: /* ODBC 1.0 */
        *((WORD *)rgbInfoValue) = SQL_OSC_CORE;
        if(pcbInfoValue) { *pcbInfoValue = 2; }
        break;

    case SQL_ODBC_SQL_OPT_IEF: /* ODBC 1.0 */
        // do we support the "Integrity Enhancement Facility" (?)
        // (something to do with referential integrity?)
        if (pcbInfoValue) *pcbInfoValue = 1;
        strncpy_null((char *)rgbInfoValue, "N", (size_t)cbInfoValueMax);
        break;

    case SQL_ORDER_BY_COLUMNS_IN_SELECT: /* ODBC 2.0 */
        // do the columns sorted by have to be in the list of
        // columns selected?
        if (pcbInfoValue) *pcbInfoValue = 1;
        strncpy_null((char *)rgbInfoValue, "Y", (size_t)cbInfoValueMax);
        break;

    case SQL_OUTER_JOINS: /* ODBC 1.0 */
        // do we support outer joins?
        if (pcbInfoValue) *pcbInfoValue = 1;
        strncpy_null((char *)rgbInfoValue, "N", (size_t)cbInfoValueMax);
        break;

    case SQL_OWNER_TERM: /* ODBC 1.0 */
        // what we call an owner
        if (pcbInfoValue) *pcbInfoValue = 5;
        strncpy_null((char *)rgbInfoValue, "owner", (size_t)cbInfoValueMax);
        break;

    case SQL_OWNER_USAGE: /* ODBC 2.0 */
        // in which statements can "owners be used"?  (what does that mean?
        // specifying 'owner.table' instead of just 'table' or something?)
        // (bitmask)
        *((DWORD *)rgbInfoValue) = 0;
        if(pcbInfoValue) { *pcbInfoValue = 4; }
        break;

    case SQL_POS_OPERATIONS: /* ODBC 2.0 */
        // what functions does SQLSetPos support? (bitmask)
        // SQLSetPos does not exist yet
        *((DWORD *)rgbInfoValue) = globals.lie ? (SQL_POS_POSITION | SQL_POS_REFRESH | SQL_POS_UPDATE | SQL_POS_DELETE | SQL_POS_ADD) : 0;
        if(pcbInfoValue) { *pcbInfoValue = 4; }
        break;

    case SQL_POSITIONED_STATEMENTS: /* ODBC 2.0 */
        // what 'positioned' functions are supported? (bitmask)
        *((DWORD *)rgbInfoValue) = globals.lie ? (SQL_PS_POSITIONED_DELETE | 
											SQL_PS_POSITIONED_UPDATE | 
											SQL_PS_SELECT_FOR_UPDATE) : 0;
        if(pcbInfoValue) { *pcbInfoValue = 4; }
        break;

    case SQL_PROCEDURE_TERM: /* ODBC 1.0 */
        // what do we call a procedure?
        if (pcbInfoValue) *pcbInfoValue = 9;
        strncpy_null((char *)rgbInfoValue, "procedure", (size_t)cbInfoValueMax);
        break;

    case SQL_PROCEDURES: /* ODBC 1.0 */
        // do we support procedures?
        if (pcbInfoValue) *pcbInfoValue = 1;
        strncpy_null((char *)rgbInfoValue, "Y", (size_t)cbInfoValueMax);
        break;

    case SQL_QUALIFIER_LOCATION: /* ODBC 2.0 */
        // where does the qualifier go (before or after the table name?)
        // we don't really use qualifiers, so...
        *((WORD *)rgbInfoValue) = SQL_QL_START;
        if(pcbInfoValue) { *pcbInfoValue = 2; }
        break;

    case SQL_QUALIFIER_NAME_SEPARATOR: /* ODBC 1.0 */
        // not really too sure what a qualifier is supposed to do either
        // (specify the name of a database in certain cases?), so nix
        // on that, too.
        if (pcbInfoValue) *pcbInfoValue = 0;
        strncpy_null((char *)rgbInfoValue, "", (size_t)cbInfoValueMax);
        break;

    case SQL_QUALIFIER_TERM: /* ODBC 1.0 */
        // what we call a qualifier
        if (pcbInfoValue) *pcbInfoValue = 0;
        strncpy_null((char *)rgbInfoValue, "", (size_t)cbInfoValueMax);
        break;

    case SQL_QUALIFIER_USAGE: /* ODBC 2.0 */
        // where can qualifiers be used? (bitmask)
        // nowhere
        *((DWORD *)rgbInfoValue) = 0;
        if(pcbInfoValue) { *pcbInfoValue = 4; }
        break;

    case SQL_QUOTED_IDENTIFIER_CASE: /* ODBC 2.0 */
        // are "quoted" identifiers case-sensitive?
        // well, we don't really let you quote identifiers, so...
        *((WORD *)rgbInfoValue) = SQL_IC_SENSITIVE;
        if(pcbInfoValue) { *pcbInfoValue = 2; }
        break;

    case SQL_ROW_UPDATES: /* ODBC 1.0 */
        //  Driver doesn't support keyset-driven or mixed cursors, so
		//	not much point in saying row updates are supported
        if (pcbInfoValue) *pcbInfoValue = 1;
        strncpy_null((char *)rgbInfoValue, globals.lie ? "Y" : "N", (size_t)cbInfoValueMax);
        break;

    case SQL_SCROLL_CONCURRENCY: /* ODBC 1.0 */
        // what concurrency options are supported BY THE CURSOR? (bitmask)
        *((DWORD *)rgbInfoValue) = globals.lie ? (SQL_SCCO_READ_ONLY | 
									SQL_SCCO_LOCK | 
									SQL_SCCO_OPT_ROWVER | 
									SQL_SCCO_OPT_VALUES) : (SQL_SCCO_READ_ONLY);
        if(pcbInfoValue) { *pcbInfoValue = 4; }
        break;

    case SQL_SCROLL_OPTIONS: /* ODBC 1.0 */
        // what options are supported for scrollable cursors? (bitmask)
		// for declare/fetch, only FORWARD scrolling is allowed
		// otherwise, the result set is STATIC (to SQLExtendedFetch for example)
        *((DWORD *)rgbInfoValue) = globals.lie ? (SQL_SO_FORWARD_ONLY | 
									SQL_SO_STATIC | 
									SQL_SO_KEYSET_DRIVEN | 
									SQL_SO_DYNAMIC | 
									SQL_SO_MIXED) : (globals.use_declarefetch ? SQL_SO_FORWARD_ONLY : (SQL_SO_FORWARD_ONLY | SQL_SO_STATIC));
        if(pcbInfoValue) { *pcbInfoValue = 4; }
        break;

    case SQL_SEARCH_PATTERN_ESCAPE: /* ODBC 1.0 */
        // this is supposed to be the character that escapes '_' or '%'
        // in LIKE clauses.  as far as I can tell postgres doesn't have one
        // (backslash generates an error).  returning an empty string means
        // no escape character is supported.
        if (pcbInfoValue) *pcbInfoValue = 0;
        strncpy_null((char *)rgbInfoValue, "", (size_t)cbInfoValueMax);
        break;

    case SQL_SERVER_NAME: /* ODBC 1.0 */
		p = CC_get_server(conn);
		if (pcbInfoValue)  *pcbInfoValue = strlen(p);
		strncpy_null((char *)rgbInfoValue, p, (size_t)cbInfoValueMax);
        break;

    case SQL_SPECIAL_CHARACTERS: /* ODBC 2.0 */
        // what special characters can be used in table and column names, etc.?
        // probably more than just this...
        if (pcbInfoValue) *pcbInfoValue = 1;
        strncpy_null((char *)rgbInfoValue, "_", (size_t)cbInfoValueMax);
        break;

    case SQL_STATIC_SENSITIVITY: /* ODBC 2.0 */
        // can changes made inside a cursor be detected? (or something like that)
        // (bitmask)
        // only applies to SQLSetPos, which doesn't exist yet.
        *((DWORD *)rgbInfoValue) = globals.lie ? (SQL_SS_ADDITIONS | SQL_SS_DELETIONS | SQL_SS_UPDATES) : 0;
        if(pcbInfoValue) { *pcbInfoValue = 4; }
        break;

    case SQL_STRING_FUNCTIONS: /* ODBC 1.0 */
        // what string functions exist? (bitmask)
        *((DWORD *)rgbInfoValue) = (SQL_FN_STR_CONCAT |
			                        SQL_FN_STR_LCASE | 
									SQL_FN_STR_LENGTH | 
									SQL_FN_STR_LOCATE | 
									SQL_FN_STR_LTRIM | 
									SQL_FN_STR_RTRIM |
									SQL_FN_STR_SUBSTRING |
									SQL_FN_STR_UCASE);
        if(pcbInfoValue) { *pcbInfoValue = 4; }
        break;

    case SQL_SUBQUERIES: /* ODBC 2.0 */
		/* postgres 6.3 supports subqueries */
        *((DWORD *)rgbInfoValue) = (SQL_SQ_QUANTIFIED |
			                        SQL_SQ_IN |
                                    SQL_SQ_EXISTS |
								    SQL_SQ_COMPARISON);
        if(pcbInfoValue) { *pcbInfoValue = 4; }
        break;

    case SQL_SYSTEM_FUNCTIONS: /* ODBC 1.0 */
        // what system functions are supported? (bitmask)
        // none of these seem to be supported, either
        *((DWORD *)rgbInfoValue) = 0;
        if(pcbInfoValue) { *pcbInfoValue = 4; }
        break;

    case SQL_TABLE_TERM: /* ODBC 1.0 */
        // what we call a table
        if (pcbInfoValue) *pcbInfoValue = 5;
        strncpy_null((char *)rgbInfoValue, "table", (size_t)cbInfoValueMax);
        break;

    case SQL_TIMEDATE_ADD_INTERVALS: /* ODBC 2.0 */
        // what resolutions are supported by the "TIMESTAMPADD scalar
        // function" (whatever that is)? (bitmask)
        *((DWORD *)rgbInfoValue) = 0;
        if(pcbInfoValue) { *pcbInfoValue = 4; }
        break;

    case SQL_TIMEDATE_DIFF_INTERVALS: /* ODBC 2.0 */
        // what resolutions are supported by the "TIMESTAMPDIFF scalar
        // function" (whatever that is)? (bitmask)
        *((DWORD *)rgbInfoValue) = 0;
        if(pcbInfoValue) { *pcbInfoValue = 4; }
        break;

    case SQL_TIMEDATE_FUNCTIONS: /* ODBC 1.0 */
        // what time and date functions are supported? (bitmask)
        *((DWORD *)rgbInfoValue) = (SQL_FN_TD_NOW);
        if(pcbInfoValue) { *pcbInfoValue = 4; }
        break;

    case SQL_TXN_CAPABLE: /* ODBC 1.0 */
        *((WORD *)rgbInfoValue) = (WORD)SQL_TC_ALL;
        // Postgres can deal with create or drop table statements in a transaction
        if(pcbInfoValue) { *pcbInfoValue = 2; }
        break;

    case SQL_TXN_ISOLATION_OPTION: /* ODBC 1.0 */
        // what transaction isolation options are available? (bitmask)
        // only the default--serializable transactions.
        *((DWORD *)rgbInfoValue) = SQL_TXN_SERIALIZABLE;
        if(pcbInfoValue) { *pcbInfoValue = 4; }
        break;

    case SQL_UNION: /* ODBC 2.0 */
		/*  unions with all supported in postgres 6.3 */
        *((DWORD *)rgbInfoValue) = (SQL_U_UNION | SQL_U_UNION_ALL);
        if(pcbInfoValue) { *pcbInfoValue = 4; }
        break;

    case SQL_USER_NAME: /* ODBC 1.0 */
		p = CC_get_username(conn);
        if (pcbInfoValue) *pcbInfoValue = strlen(p);
        strncpy_null((char *)rgbInfoValue, p, (size_t)cbInfoValueMax);
        break;

    default:
        /* unrecognized key */
        conn->errormsg = "Unrecognized key passed to SQLGetInfo.";
        conn->errornumber = CONN_NOT_IMPLEMENTED_ERROR;
		CC_log_error(func, "", conn);
        return SQL_ERROR;
    }

    return SQL_SUCCESS;
}

//      -       -       -       -       -       -       -       -       -


RETCODE SQL_API SQLGetTypeInfo(
        HSTMT   hstmt,
        SWORD   fSqlType)
{
char *func = "SQLGetTypeInfo";
StatementClass *stmt = (StatementClass *) hstmt;
TupleNode *row;
int i;
Int4 type;

	mylog("**** in SQLGetTypeInfo: fSqlType = %d\n", fSqlType);

	if( ! stmt) {
		SC_log_error(func, "", NULL);
		return SQL_INVALID_HANDLE;
	}

	stmt->manual_result = TRUE;
	stmt->result = QR_Constructor();
	if( ! stmt->result) {
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

    // cycle through the types
    for(i=0, type = pgtypes_defined[0]; type; type = pgtypes_defined[++i]) {

		if(fSqlType == SQL_ALL_TYPES || fSqlType == pgtype_to_sqltype(stmt, type)) {

			row = (TupleNode *)malloc(sizeof(TupleNode) + (15 - 1)*sizeof(TupleField));

			/*	These values can't be NULL */
			set_tuplefield_string(&row->tuple[0], pgtype_to_name(stmt, type));
			set_tuplefield_int2(&row->tuple[1], pgtype_to_sqltype(stmt, type));
			set_tuplefield_int2(&row->tuple[6], pgtype_nullable(stmt, type));
			set_tuplefield_int2(&row->tuple[7], pgtype_case_sensitive(stmt, type));
			set_tuplefield_int2(&row->tuple[8], pgtype_searchable(stmt, type));
			set_tuplefield_int2(&row->tuple[10], pgtype_money(stmt, type));

			/*	Localized data-source dependent data type name (always NULL) */
			set_tuplefield_null(&row->tuple[12]);	

			/*	These values can be NULL */
			set_nullfield_int4(&row->tuple[2], pgtype_precision(stmt, type, PG_STATIC, PG_STATIC));
			set_nullfield_string(&row->tuple[3], pgtype_literal_prefix(stmt, type));
			set_nullfield_string(&row->tuple[4], pgtype_literal_suffix(stmt, type));
			set_nullfield_string(&row->tuple[5], pgtype_create_params(stmt, type));
			set_nullfield_int2(&row->tuple[9], pgtype_unsigned(stmt, type));
			set_nullfield_int2(&row->tuple[11], pgtype_auto_increment(stmt, type));
			set_nullfield_int2(&row->tuple[13], pgtype_scale(stmt, type));
			set_nullfield_int2(&row->tuple[14], pgtype_scale(stmt, type));

			QR_add_tuple(stmt->result, row);
		}
    }

    stmt->status = STMT_FINISHED;
    stmt->currTuple = -1;
	stmt->current_col = -1;

    return SQL_SUCCESS;
}

//      -       -       -       -       -       -       -       -       -

RETCODE SQL_API SQLGetFunctions(
        HDBC      hdbc,
        UWORD     fFunction,
        UWORD FAR *pfExists)
{
    if (fFunction == SQL_API_ALL_FUNCTIONS) {

		if (globals.lie) {
			int i;
			memset(pfExists, 0, sizeof(UWORD)*100);

			pfExists[SQL_API_SQLALLOCENV] = TRUE;
			pfExists[SQL_API_SQLFREEENV] = TRUE;
			for (i = SQL_API_SQLALLOCCONNECT; i <= SQL_NUM_FUNCTIONS; i++)
				pfExists[i] = TRUE;
			for (i = SQL_EXT_API_START; i <= SQL_EXT_API_LAST; i++)
				pfExists[i] = TRUE;
		}
		else {
			memset(pfExists, 0, sizeof(UWORD)*100);

			// ODBC core functions
			pfExists[SQL_API_SQLALLOCCONNECT]     = TRUE;
			pfExists[SQL_API_SQLALLOCENV]         = TRUE;
			pfExists[SQL_API_SQLALLOCSTMT]        = TRUE;
			pfExists[SQL_API_SQLBINDCOL]          = TRUE;  
			pfExists[SQL_API_SQLCANCEL]           = TRUE;
			pfExists[SQL_API_SQLCOLATTRIBUTES]    = TRUE;
			pfExists[SQL_API_SQLCONNECT]          = TRUE;
			pfExists[SQL_API_SQLDESCRIBECOL]      = TRUE;  // partial
			pfExists[SQL_API_SQLDISCONNECT]       = TRUE;
			pfExists[SQL_API_SQLERROR]            = TRUE;
			pfExists[SQL_API_SQLEXECDIRECT]       = TRUE;
			pfExists[SQL_API_SQLEXECUTE]          = TRUE;
			pfExists[SQL_API_SQLFETCH]            = TRUE;
			pfExists[SQL_API_SQLFREECONNECT]      = TRUE;
			pfExists[SQL_API_SQLFREEENV]          = TRUE;
			pfExists[SQL_API_SQLFREESTMT]         = TRUE;
			pfExists[SQL_API_SQLGETCURSORNAME]    = TRUE;
			pfExists[SQL_API_SQLNUMRESULTCOLS]    = TRUE;
			pfExists[SQL_API_SQLPREPARE]          = TRUE;  // complete?
			pfExists[SQL_API_SQLROWCOUNT]         = TRUE;
			pfExists[SQL_API_SQLSETCURSORNAME]    = TRUE;
			pfExists[SQL_API_SQLSETPARAM]         = FALSE; // odbc 1.0
			pfExists[SQL_API_SQLTRANSACT]         = TRUE;

			// ODBC level 1 functions
			pfExists[SQL_API_SQLBINDPARAMETER]    = TRUE;
			pfExists[SQL_API_SQLCOLUMNS]          = TRUE;
			pfExists[SQL_API_SQLDRIVERCONNECT]    = TRUE;
			pfExists[SQL_API_SQLGETCONNECTOPTION] = TRUE;  // partial
			pfExists[SQL_API_SQLGETDATA]          = TRUE;
			pfExists[SQL_API_SQLGETFUNCTIONS]     = TRUE;                                                       
			pfExists[SQL_API_SQLGETINFO]          = TRUE;
			pfExists[SQL_API_SQLGETSTMTOPTION]    = TRUE;  // partial
			pfExists[SQL_API_SQLGETTYPEINFO]      = TRUE;
			pfExists[SQL_API_SQLPARAMDATA]        = TRUE;
			pfExists[SQL_API_SQLPUTDATA]          = TRUE;
			pfExists[SQL_API_SQLSETCONNECTOPTION] = TRUE;  // partial
			pfExists[SQL_API_SQLSETSTMTOPTION]    = TRUE;
			pfExists[SQL_API_SQLSPECIALCOLUMNS]   = TRUE;
			pfExists[SQL_API_SQLSTATISTICS]       = TRUE;
			pfExists[SQL_API_SQLTABLES]           = TRUE;

			// ODBC level 2 functions
			pfExists[SQL_API_SQLBROWSECONNECT]    = FALSE;
			pfExists[SQL_API_SQLCOLUMNPRIVILEGES] = FALSE;
			pfExists[SQL_API_SQLDATASOURCES]      = FALSE;  // only implemented by DM
			pfExists[SQL_API_SQLDESCRIBEPARAM]    = FALSE;	// not properly implemented
			pfExists[SQL_API_SQLDRIVERS]          = FALSE;  // only implemented by DM
			pfExists[SQL_API_SQLEXTENDEDFETCH]    = globals.use_declarefetch ? FALSE : TRUE;
			pfExists[SQL_API_SQLFOREIGNKEYS]      = TRUE;
			pfExists[SQL_API_SQLMORERESULTS]      = TRUE;
			pfExists[SQL_API_SQLNATIVESQL]        = TRUE;
			pfExists[SQL_API_SQLNUMPARAMS]        = TRUE;
			pfExists[SQL_API_SQLPARAMOPTIONS]     = FALSE;
			pfExists[SQL_API_SQLPRIMARYKEYS]      = TRUE;
			pfExists[SQL_API_SQLPROCEDURECOLUMNS] = FALSE;
			pfExists[SQL_API_SQLPROCEDURES]       = FALSE;
			pfExists[SQL_API_SQLSETPOS]           = FALSE;
			pfExists[SQL_API_SQLSETSCROLLOPTIONS] = FALSE;	// odbc 1.0
			pfExists[SQL_API_SQLTABLEPRIVILEGES]  = FALSE;
		}
    } else {

		if (globals.lie)
			*pfExists = TRUE;

		else {

			switch(fFunction) {
			case SQL_API_SQLALLOCCONNECT:     *pfExists = TRUE; break;
			case SQL_API_SQLALLOCENV:         *pfExists = TRUE; break;
			case SQL_API_SQLALLOCSTMT:        *pfExists = TRUE; break;
			case SQL_API_SQLBINDCOL:          *pfExists = TRUE; break;
			case SQL_API_SQLCANCEL:           *pfExists = TRUE; break;
			case SQL_API_SQLCOLATTRIBUTES:    *pfExists = TRUE; break;
			case SQL_API_SQLCONNECT:          *pfExists = TRUE; break;
			case SQL_API_SQLDESCRIBECOL:      *pfExists = TRUE; break;  // partial
			case SQL_API_SQLDISCONNECT:       *pfExists = TRUE; break;
			case SQL_API_SQLERROR:            *pfExists = TRUE; break;
			case SQL_API_SQLEXECDIRECT:       *pfExists = TRUE; break;
			case SQL_API_SQLEXECUTE:          *pfExists = TRUE; break;
			case SQL_API_SQLFETCH:            *pfExists = TRUE; break;
			case SQL_API_SQLFREECONNECT:      *pfExists = TRUE; break;
			case SQL_API_SQLFREEENV:          *pfExists = TRUE; break;
			case SQL_API_SQLFREESTMT:         *pfExists = TRUE; break;
			case SQL_API_SQLGETCURSORNAME:    *pfExists = TRUE; break;
			case SQL_API_SQLNUMRESULTCOLS:    *pfExists = TRUE; break;
			case SQL_API_SQLPREPARE:          *pfExists = TRUE; break;
			case SQL_API_SQLROWCOUNT:         *pfExists = TRUE; break;
			case SQL_API_SQLSETCURSORNAME:    *pfExists = TRUE; break;
			case SQL_API_SQLSETPARAM:         *pfExists = FALSE; break; // odbc 1.0
			case SQL_API_SQLTRANSACT:         *pfExists = TRUE; break;

				// ODBC level 1 functions
			case SQL_API_SQLBINDPARAMETER:    *pfExists = TRUE; break;
			case SQL_API_SQLCOLUMNS:          *pfExists = TRUE; break;
			case SQL_API_SQLDRIVERCONNECT:    *pfExists = TRUE; break;
			case SQL_API_SQLGETCONNECTOPTION: *pfExists = TRUE; break;  // partial
			case SQL_API_SQLGETDATA:          *pfExists = TRUE; break;
			case SQL_API_SQLGETFUNCTIONS:     *pfExists = TRUE; break;
			case SQL_API_SQLGETINFO:          *pfExists = TRUE; break;
			case SQL_API_SQLGETSTMTOPTION:    *pfExists = TRUE; break;  // partial
			case SQL_API_SQLGETTYPEINFO:      *pfExists = TRUE; break;
			case SQL_API_SQLPARAMDATA:        *pfExists = TRUE; break;
			case SQL_API_SQLPUTDATA:          *pfExists = TRUE; break;
			case SQL_API_SQLSETCONNECTOPTION: *pfExists = TRUE; break;  // partial
			case SQL_API_SQLSETSTMTOPTION:    *pfExists = TRUE; break;
			case SQL_API_SQLSPECIALCOLUMNS:   *pfExists = TRUE; break;
			case SQL_API_SQLSTATISTICS:       *pfExists = TRUE; break;
			case SQL_API_SQLTABLES:           *pfExists = TRUE; break;

				// ODBC level 2 functions
			case SQL_API_SQLBROWSECONNECT:    *pfExists = FALSE; break;
			case SQL_API_SQLCOLUMNPRIVILEGES: *pfExists = FALSE; break;
			case SQL_API_SQLDATASOURCES:      *pfExists = FALSE; break;  // only implemented by DM
			case SQL_API_SQLDESCRIBEPARAM:    *pfExists = FALSE; break;  // not properly implemented
			case SQL_API_SQLDRIVERS:          *pfExists = FALSE; break;  // only implemented by DM
			case SQL_API_SQLEXTENDEDFETCH:    *pfExists = globals.use_declarefetch ? FALSE : TRUE; break;
			case SQL_API_SQLFOREIGNKEYS:      *pfExists = TRUE; break;
			case SQL_API_SQLMORERESULTS:      *pfExists = TRUE; break;
			case SQL_API_SQLNATIVESQL:        *pfExists = TRUE; break;
			case SQL_API_SQLNUMPARAMS:        *pfExists = TRUE; break;
			case SQL_API_SQLPARAMOPTIONS:     *pfExists = FALSE; break;
			case SQL_API_SQLPRIMARYKEYS:      *pfExists = TRUE; break;
			case SQL_API_SQLPROCEDURECOLUMNS: *pfExists = FALSE; break;
			case SQL_API_SQLPROCEDURES:       *pfExists = FALSE; break;
			case SQL_API_SQLSETPOS:           *pfExists = FALSE; break;
			case SQL_API_SQLSETSCROLLOPTIONS: *pfExists = FALSE; break;	// odbc 1.0
			case SQL_API_SQLTABLEPRIVILEGES:  *pfExists = FALSE; break;
			}
		}
    }

    return SQL_SUCCESS;
}



RETCODE SQL_API SQLTables(
                          HSTMT       hstmt,
                          UCHAR FAR * szTableQualifier,
                          SWORD       cbTableQualifier,
                          UCHAR FAR * szTableOwner,
                          SWORD       cbTableOwner,
                          UCHAR FAR * szTableName,
                          SWORD       cbTableName,
                          UCHAR FAR * szTableType,
                          SWORD       cbTableType)
{
char *func = "SQLTables";
StatementClass *stmt = (StatementClass *) hstmt;
StatementClass *tbl_stmt;
TupleNode *row;
HSTMT htbl_stmt;
RETCODE result;
char *tableType;
char tables_query[MAX_STATEMENT_LEN];
char table_name[MAX_INFO_STRING], table_owner[MAX_INFO_STRING], relhasrules[MAX_INFO_STRING];
ConnInfo *ci;
char *prefix[32], prefixes[MEDIUM_REGISTRY_LEN];
char *table_type[32], table_types[MAX_INFO_STRING];
char show_system_tables, show_regular_tables, show_views;
char regular_table, view, systable;
int i;

mylog("**** SQLTables(): ENTER, stmt=%u\n", stmt);

	if( ! stmt) {
		SC_log_error(func, "", NULL);
		return SQL_INVALID_HANDLE;
	}

	stmt->manual_result = TRUE;
	stmt->errormsg_created = TRUE;

	ci = &stmt->hdbc->connInfo;

	result = SQLAllocStmt( stmt->hdbc, &htbl_stmt);
	if((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO)) {
		stmt->errornumber = STMT_NO_MEMORY_ERROR;
		stmt->errormsg = "Couldn't allocate statement for SQLTables result.";
		SC_log_error(func, "", stmt);
		return SQL_ERROR;
	}
	tbl_stmt = (StatementClass *) htbl_stmt;

	// **********************************************************************
	//	Create the query to find out the tables
	// **********************************************************************

	strcpy(tables_query, "select relname, usename, relhasrules from pg_class, pg_user where relkind = 'r' ");

	my_strcat(tables_query, " and usename like '%.*s'", szTableOwner, cbTableOwner);
	my_strcat(tables_query, " and relname like '%.*s'", szTableName, cbTableName);


	//	Parse the extra systable prefix 
	strcpy(prefixes, globals.extra_systable_prefixes);
	i = 0;
	prefix[i] = strtok(prefixes, ";");
	while (prefix[i] && i<32) {
		prefix[++i] = strtok(NULL, ";");
	}

	/*	Parse the desired table types to return */
	show_system_tables = FALSE;
	show_regular_tables = FALSE;
	show_views = FALSE;

	//	make_string mallocs memory
	tableType = make_string(szTableType, cbTableType, NULL);
	if (tableType) {
		strcpy(table_types, tableType);
		free(tableType);
		i = 0;
		table_type[i] = strtok(table_types, ",");
		while (table_type[i] && i<32) {
			table_type[++i] = strtok(NULL, ",");
		}

		/*	Check for desired table types to return */
		i = 0;
		while (table_type[i]) {
			if ( strstr(table_type[i], "SYSTEM TABLE"))
				show_system_tables = TRUE;
			else if ( strstr(table_type[i], "TABLE"))
				show_regular_tables = TRUE;
			else if ( strstr(table_type[i], "VIEW"))
				show_views = TRUE;

			i++;
		}

	}

	/*  If not interested in SYSTEM TABLES then filter them out
		to save some time on the query.  If treating system tables
		as regular tables, then dont filter either.
	*/
	if ( ! atoi(ci->show_system_tables) && ! show_system_tables) {
		strcat(tables_query, " and relname !~ '^" POSTGRES_SYS_PREFIX);

		/*	Also filter out user-defined system table types */
		i = 0;
		while(prefix[i]) {
			strcat(tables_query, "|^");
			strcat(tables_query, prefix[i]);
			i++;
		}

		strcat(tables_query, "'");
	}


	/*	filter out large objects unconditionally (they are not system tables) and match users */
	strcat(tables_query, " and relname !~ '^xinv[0-9]+' and int4out(usesysid) = int4out(relowner) order by relname");

	// **********************************************************************

	result = SQLExecDirect(htbl_stmt, tables_query, strlen(tables_query));
	if((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO)) {
		stmt->errormsg = SC_create_errormsg(htbl_stmt);
		stmt->errornumber = tbl_stmt->errornumber;
		SC_log_error(func, "", stmt);
		SQLFreeStmt(htbl_stmt, SQL_DROP);
		return SQL_ERROR;
	}

    result = SQLBindCol(htbl_stmt, 1, SQL_C_CHAR,
                        table_name, MAX_INFO_STRING, NULL);
    if((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO)) {
		stmt->errormsg = tbl_stmt->errormsg;
		stmt->errornumber = tbl_stmt->errornumber;
		SC_log_error(func, "", stmt);
		SQLFreeStmt(htbl_stmt, SQL_DROP);
        return SQL_ERROR;
    }

    result = SQLBindCol(htbl_stmt, 2, SQL_C_CHAR,
                        table_owner, MAX_INFO_STRING, NULL);
    if((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO)) {
		stmt->errormsg = tbl_stmt->errormsg;
		stmt->errornumber = tbl_stmt->errornumber;
		SC_log_error(func, "", stmt);
		SQLFreeStmt(htbl_stmt, SQL_DROP);
        return SQL_ERROR;
    }
    result = SQLBindCol(htbl_stmt, 3, SQL_C_CHAR,
                        relhasrules, MAX_INFO_STRING, NULL);
    if((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO)) {
		stmt->errormsg = tbl_stmt->errormsg;
		stmt->errornumber = tbl_stmt->errornumber;
		SC_log_error(func, "", stmt);
		SQLFreeStmt(htbl_stmt, SQL_DROP);
        return SQL_ERROR;
    }

	stmt->result = QR_Constructor();
	if(!stmt->result) {
		stmt->errormsg = "Couldn't allocate memory for SQLTables result.";
		stmt->errornumber = STMT_NO_MEMORY_ERROR;
		SC_log_error(func, "", stmt);
		SQLFreeStmt(htbl_stmt, SQL_DROP);
		return SQL_ERROR;
	}

    // the binding structure for a statement is not set up until
    // a statement is actually executed, so we'll have to do this ourselves.
	extend_bindings(stmt, 5);
	
    // set the field names
	QR_set_num_fields(stmt->result, 5);
	QR_set_field_info(stmt->result, 0, "TABLE_QUALIFIER", PG_TYPE_TEXT, MAX_INFO_STRING);
	QR_set_field_info(stmt->result, 1, "TABLE_OWNER", PG_TYPE_TEXT, MAX_INFO_STRING);
	QR_set_field_info(stmt->result, 2, "TABLE_NAME", PG_TYPE_TEXT, MAX_INFO_STRING);
	QR_set_field_info(stmt->result, 3, "TABLE_TYPE", PG_TYPE_TEXT, MAX_INFO_STRING);
	QR_set_field_info(stmt->result, 4, "REMARKS", PG_TYPE_TEXT, 254);

    // add the tuples
	result = SQLFetch(htbl_stmt);
	while((result == SQL_SUCCESS) || (result == SQL_SUCCESS_WITH_INFO)) {

		/*	Determine if this table name is a system table.
			If treating system tables as regular tables, then 
			no need to do this test.
		*/		
		systable = FALSE;
		if( ! atoi(ci->show_system_tables)) {

			if ( strncmp(table_name, POSTGRES_SYS_PREFIX, strlen(POSTGRES_SYS_PREFIX)) == 0)
				systable = TRUE;

			else {			/* Check extra system table prefixes */
				i = 0;
				while (prefix[i]) {
					mylog("table_name='%s', prefix[%d]='%s'\n", table_name, i, prefix[i]);
					if (strncmp(table_name, prefix[i], strlen(prefix[i])) == 0) {
						systable = TRUE;
						break;
					}
					i++;
				}
			}
		}

		/*	Determine if the table name is a view */
		view = (relhasrules[0] == '1');

		/*	It must be a regular table */
		regular_table = ( ! systable && ! view);


		/*	Include the row in the result set if meets all criteria */
		/*	NOTE: Unsupported table types (i.e., LOCAL TEMPORARY, ALIAS, etc)
					will return nothing */
		if ( (systable && show_system_tables) ||
			 (view && show_views) || 
			 (regular_table && show_regular_tables)) {

			row = (TupleNode *)malloc(sizeof(TupleNode) + (5 - 1) * sizeof(TupleField));

			set_tuplefield_string(&row->tuple[0], "");

			// I have to hide the table owner from Access, otherwise it
			// insists on referring to the table as 'owner.table'.
			// (this is valid according to the ODBC SQL grammar, but
			// Postgres won't support it.)
			// set_tuplefield_string(&row->tuple[1], table_owner);

			mylog("SQLTables: table_name = '%s'\n", table_name);

			set_tuplefield_string(&row->tuple[1], "");
			set_tuplefield_string(&row->tuple[2], table_name);
			set_tuplefield_string(&row->tuple[3], systable ? "SYSTEM TABLE" : (view ? "VIEW" : "TABLE"));
			set_tuplefield_string(&row->tuple[4], "");

			QR_add_tuple(stmt->result, row);
		}
		result = SQLFetch(htbl_stmt);
    }
	if(result != SQL_NO_DATA_FOUND) {
		stmt->errormsg = SC_create_errormsg(htbl_stmt);
		stmt->errornumber = tbl_stmt->errornumber;
		SC_log_error(func, "", stmt);
		SQLFreeStmt(htbl_stmt, SQL_DROP);
		return SQL_ERROR;
	}

    // also, things need to think that this statement is finished so
    // the results can be retrieved.
	stmt->status = STMT_FINISHED;

    // set up the current tuple pointer for SQLFetch
	stmt->currTuple = -1;
	stmt->current_col = -1;

	SQLFreeStmt(htbl_stmt, SQL_DROP);
	mylog("SQLTables(): EXIT,  stmt=%u\n", stmt);
	return SQL_SUCCESS;
}

RETCODE SQL_API SQLColumns(
                           HSTMT        hstmt,
                           UCHAR FAR *  szTableQualifier,
                           SWORD        cbTableQualifier,
                           UCHAR FAR *  szTableOwner,
                           SWORD        cbTableOwner,
                           UCHAR FAR *  szTableName,
                           SWORD        cbTableName,
                           UCHAR FAR *  szColumnName,
                           SWORD        cbColumnName)
{
char *func = "SQLColumns";
StatementClass *stmt = (StatementClass *) hstmt;
TupleNode *row;
HSTMT hcol_stmt;
StatementClass *col_stmt;
char columns_query[MAX_STATEMENT_LEN];
RETCODE result;
char table_owner[MAX_INFO_STRING], table_name[MAX_INFO_STRING], field_name[MAX_INFO_STRING], field_type_name[MAX_INFO_STRING];
Int2 field_number, field_length, mod_length;
Int4 field_type;
Int2 the_type;
char not_null[MAX_INFO_STRING];
ConnInfo *ci;



mylog("**** SQLColumns(): ENTER, stmt=%u\n", stmt);

	if( ! stmt) {
		SC_log_error(func, "", NULL);
		return SQL_INVALID_HANDLE;
	}

	stmt->manual_result = TRUE;
	stmt->errormsg_created = TRUE;

	ci = &stmt->hdbc->connInfo;

	// **********************************************************************
	//	Create the query to find out the columns (Note: pre 6.3 did not have the atttypmod field)
	// **********************************************************************
	sprintf(columns_query, "select u.usename, c.relname, a.attname, a.atttypid,t.typname, a.attnum, a.attlen, %s, a.attnotnull from pg_user u, pg_class c, pg_attribute a, pg_type t where "
		"int4out(u.usesysid) = int4out(c.relowner) and c.oid= a.attrelid and a.atttypid = t.oid and (a.attnum > 0)",
		PROTOCOL_62(ci) ? "a.attlen" : "a.atttypmod");

	my_strcat(columns_query, " and c.relname like '%.*s'", szTableName, cbTableName);
	my_strcat(columns_query, " and u.usename like '%.*s'", szTableOwner, cbTableOwner);
	my_strcat(columns_query, " and a.attname like '%.*s'", szColumnName, cbColumnName);

    // give the output in the order the columns were defined
    // when the table was created
    strcat(columns_query, " order by attnum");
	// **********************************************************************

    result = SQLAllocStmt( stmt->hdbc, &hcol_stmt);
    if((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO)) {
		stmt->errornumber = STMT_NO_MEMORY_ERROR;
		stmt->errormsg = "Couldn't allocate statement for SQLColumns result.";
		SC_log_error(func, "", stmt);
        return SQL_ERROR;
    }
	col_stmt = (StatementClass *) hcol_stmt;

    result = SQLExecDirect(hcol_stmt, columns_query,
                           strlen(columns_query));
    if((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO)) {
		stmt->errormsg = SC_create_errormsg(hcol_stmt);
		stmt->errornumber = col_stmt->errornumber;
		SC_log_error(func, "", stmt);
		SQLFreeStmt(hcol_stmt, SQL_DROP);
        return SQL_ERROR;
    }

    result = SQLBindCol(hcol_stmt, 1, SQL_C_CHAR,
                        table_owner, MAX_INFO_STRING, NULL);
    if((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO)) {
		stmt->errormsg = col_stmt->errormsg;
		stmt->errornumber = col_stmt->errornumber;
		SC_log_error(func, "", stmt);
		SQLFreeStmt(hcol_stmt, SQL_DROP);
        return SQL_ERROR;
    }

    result = SQLBindCol(hcol_stmt, 2, SQL_C_CHAR,
                        table_name, MAX_INFO_STRING, NULL);
    if((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO)) {
		stmt->errormsg = col_stmt->errormsg;
		stmt->errornumber = col_stmt->errornumber;
		SC_log_error(func, "", stmt);
		SQLFreeStmt(hcol_stmt, SQL_DROP);
        return SQL_ERROR;
    }

    result = SQLBindCol(hcol_stmt, 3, SQL_C_CHAR,
                        field_name, MAX_INFO_STRING, NULL);
    if((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO)) {
		stmt->errormsg = col_stmt->errormsg;
		stmt->errornumber = col_stmt->errornumber;
		SC_log_error(func, "", stmt);
		SQLFreeStmt(hcol_stmt, SQL_DROP);
        return SQL_ERROR;
    }

    result = SQLBindCol(hcol_stmt, 4, SQL_C_DEFAULT,
                        &field_type, 4, NULL);
    if((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO)) {
		stmt->errormsg = col_stmt->errormsg;
		stmt->errornumber = col_stmt->errornumber;
		SC_log_error(func, "", stmt);
		SQLFreeStmt(hcol_stmt, SQL_DROP);
        return SQL_ERROR;
    }

    result = SQLBindCol(hcol_stmt, 5, SQL_C_CHAR,
                        field_type_name, MAX_INFO_STRING, NULL);
    if((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO)) {
		stmt->errormsg = col_stmt->errormsg;
		stmt->errornumber = col_stmt->errornumber;
		SC_log_error(func, "", stmt);
		SQLFreeStmt(hcol_stmt, SQL_DROP);
        return SQL_ERROR;
    }

    result = SQLBindCol(hcol_stmt, 6, SQL_C_DEFAULT,
                        &field_number, MAX_INFO_STRING, NULL);
    if((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO)) {
		stmt->errormsg = col_stmt->errormsg;
		stmt->errornumber = col_stmt->errornumber;
		SC_log_error(func, "", stmt);
		SQLFreeStmt(hcol_stmt, SQL_DROP);
        return SQL_ERROR;
    }

    result = SQLBindCol(hcol_stmt, 7, SQL_C_DEFAULT,
                        &field_length, MAX_INFO_STRING, NULL);
    if((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO)) {
		stmt->errormsg = col_stmt->errormsg;
		stmt->errornumber = col_stmt->errornumber;
		SC_log_error(func, "", stmt);
		SQLFreeStmt(hcol_stmt, SQL_DROP);
        return SQL_ERROR;
    }

    result = SQLBindCol(hcol_stmt, 8, SQL_C_DEFAULT,
                        &mod_length, MAX_INFO_STRING, NULL);
    if((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO)) {
		stmt->errormsg = col_stmt->errormsg;
		stmt->errornumber = col_stmt->errornumber;
		SC_log_error(func, "", stmt);
		SQLFreeStmt(hcol_stmt, SQL_DROP);
        return SQL_ERROR;
    }

    result = SQLBindCol(hcol_stmt, 9, SQL_C_CHAR,
                        not_null, MAX_INFO_STRING, NULL);
    if((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO)) {
		stmt->errormsg = col_stmt->errormsg;
		stmt->errornumber = col_stmt->errornumber;
		SC_log_error(func, "", stmt);
		SQLFreeStmt(hcol_stmt, SQL_DROP);
        return SQL_ERROR;
    }

    stmt->result = QR_Constructor();
    if(!stmt->result) {
		stmt->errormsg = "Couldn't allocate memory for SQLColumns result.";
        stmt->errornumber = STMT_NO_MEMORY_ERROR;
		SC_log_error(func, "", stmt);
		SQLFreeStmt(hcol_stmt, SQL_DROP);
        return SQL_ERROR;
    }

    // the binding structure for a statement is not set up until
    // a statement is actually executed, so we'll have to do this ourselves.
    extend_bindings(stmt, 12);

    // set the field names
    QR_set_num_fields(stmt->result, 12);
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

	
	result = SQLFetch(hcol_stmt);

	/*	Only show oid if option AND there are other columns AND 
		its not being called by SQLStatistics .
		Always show OID if its a system table
	*/

	if (result != SQL_ERROR && ! stmt->internal) {

		if (atoi(ci->show_oid_column) || strncmp(table_name, POSTGRES_SYS_PREFIX, strlen(POSTGRES_SYS_PREFIX)) == 0) {

			/*	For OID fields */
			the_type = PG_TYPE_OID;
			row = (TupleNode *)malloc(sizeof(TupleNode) +
									  (12 - 1) * sizeof(TupleField));

			set_tuplefield_string(&row->tuple[0], "");
			// see note in SQLTables()
			//      set_tuplefield_string(&row->tuple[1], table_owner);
			set_tuplefield_string(&row->tuple[1], "");
			set_tuplefield_string(&row->tuple[2], table_name);
			set_tuplefield_string(&row->tuple[3], "oid");
			set_tuplefield_int2(&row->tuple[4], pgtype_to_sqltype(stmt, the_type));
			set_tuplefield_string(&row->tuple[5], "OID");

			set_tuplefield_int4(&row->tuple[7], pgtype_length(stmt, the_type, PG_STATIC,
			PG_STATIC));
			set_tuplefield_int4(&row->tuple[6], pgtype_precision(stmt, the_type, PG_STATIC,
			PG_STATIC));

			set_nullfield_int2(&row->tuple[8], pgtype_scale(stmt, the_type));
			set_nullfield_int2(&row->tuple[9], pgtype_radix(stmt, the_type));
			set_tuplefield_int2(&row->tuple[10], SQL_NO_NULLS);
			set_tuplefield_string(&row->tuple[11], "");

			QR_add_tuple(stmt->result, row);
		}

	}

    while((result == SQL_SUCCESS) || (result == SQL_SUCCESS_WITH_INFO)) {
        row = (TupleNode *)malloc(sizeof(TupleNode) +
                                  (12 - 1) * sizeof(TupleField));

        set_tuplefield_string(&row->tuple[0], "");
        // see note in SQLTables()
        //      set_tuplefield_string(&row->tuple[1], table_owner);
        set_tuplefield_string(&row->tuple[1], "");
        set_tuplefield_string(&row->tuple[2], table_name);
        set_tuplefield_string(&row->tuple[3], field_name);
        set_tuplefield_int2(&row->tuple[4], pgtype_to_sqltype(stmt, field_type));
        set_tuplefield_string(&row->tuple[5], field_type_name);


		/*	Some Notes about Postgres Data Types:

			VARCHAR - the length is stored in the pg_attribute.atttypmod field
			BPCHAR  - the length is also stored as varchar is

		*/
        if((field_type == PG_TYPE_VARCHAR) ||
		   (field_type == PG_TYPE_BPCHAR)) {

			if (mod_length >= 4)
				mod_length -= 4;			// the length is in atttypmod - 4

			if (mod_length > globals.max_varchar_size || mod_length <= 0)
				mod_length = globals.max_varchar_size;

			mylog("SQLColumns: field type is VARCHAR,BPCHAR: field_type = %d, mod_length = %d\n", field_type, mod_length);

            set_tuplefield_int4(&row->tuple[7], mod_length);
			set_tuplefield_int4(&row->tuple[6], mod_length);
        } else {
			mylog("SQLColumns: field type is OTHER: field_type = %d, pgtype_length = %d\n", field_type, pgtype_length(stmt, field_type, PG_STATIC, PG_STATIC));

            set_tuplefield_int4(&row->tuple[7], pgtype_length(stmt, field_type, PG_STATIC, PG_STATIC));
			set_tuplefield_int4(&row->tuple[6], pgtype_precision(stmt, field_type, PG_STATIC, PG_STATIC));

        }

		set_nullfield_int2(&row->tuple[8], pgtype_scale(stmt, field_type));
		set_nullfield_int2(&row->tuple[9], pgtype_radix(stmt, field_type));
		set_tuplefield_int2(&row->tuple[10], (Int2) (not_null[0] == '1' ? SQL_NO_NULLS : pgtype_nullable(stmt, field_type)));
		set_tuplefield_string(&row->tuple[11], "");

        QR_add_tuple(stmt->result, row);

        result = SQLFetch(hcol_stmt);
    }
    if(result != SQL_NO_DATA_FOUND) {
		stmt->errormsg = SC_create_errormsg(hcol_stmt);
		stmt->errornumber = col_stmt->errornumber;
		SC_log_error(func, "", stmt);
		SQLFreeStmt(hcol_stmt, SQL_DROP);
        return SQL_ERROR;
    }

	//	Put the row version column at the end so it might not be
	//	mistaken for a key field.
	if ( ! stmt->internal && atoi(ci->row_versioning)) {
		/*	For Row Versioning fields */
		the_type = PG_TYPE_INT4;

		row = (TupleNode *)malloc(sizeof(TupleNode) +
								  (12 - 1) * sizeof(TupleField));

		set_tuplefield_string(&row->tuple[0], "");
		set_tuplefield_string(&row->tuple[1], "");
		set_tuplefield_string(&row->tuple[2], table_name);
		set_tuplefield_string(&row->tuple[3], "xmin");
		set_tuplefield_int2(&row->tuple[4], pgtype_to_sqltype(stmt, the_type));
		set_tuplefield_string(&row->tuple[5], pgtype_to_name(stmt, the_type));
		set_tuplefield_int4(&row->tuple[6], pgtype_precision(stmt, the_type, PG_STATIC, PG_STATIC));
		set_tuplefield_int4(&row->tuple[7], pgtype_length(stmt, the_type, PG_STATIC, PG_STATIC));
		set_nullfield_int2(&row->tuple[8], pgtype_scale(stmt, the_type));
		set_nullfield_int2(&row->tuple[9], pgtype_radix(stmt, the_type));
		set_tuplefield_int2(&row->tuple[10], SQL_NO_NULLS);
		set_tuplefield_string(&row->tuple[11], "");

		QR_add_tuple(stmt->result, row);
	}

    // also, things need to think that this statement is finished so
    // the results can be retrieved.
    stmt->status = STMT_FINISHED;

    // set up the current tuple pointer for SQLFetch
    stmt->currTuple = -1;
	stmt->current_col = -1;

	SQLFreeStmt(hcol_stmt, SQL_DROP);
	mylog("SQLColumns(): EXIT,  stmt=%u\n", stmt);
    return SQL_SUCCESS;
}

RETCODE SQL_API SQLSpecialColumns(
                                  HSTMT        hstmt,
                                  UWORD        fColType,
                                  UCHAR FAR *  szTableQualifier,
                                  SWORD        cbTableQualifier,
                                  UCHAR FAR *  szTableOwner,
                                  SWORD        cbTableOwner,
                                  UCHAR FAR *  szTableName,
                                  SWORD        cbTableName,
                                  UWORD        fScope,
                                  UWORD        fNullable)
{
char *func = "SQLSpecialColumns";
TupleNode *row;
StatementClass *stmt = (StatementClass *) hstmt;
ConnInfo *ci;


mylog("**** SQLSpecialColumns(): ENTER,  stmt=%u\n", stmt);

    if( ! stmt) {
		SC_log_error(func, "", NULL);
        return SQL_INVALID_HANDLE;
    }
	ci = &stmt->hdbc->connInfo;

	stmt->manual_result = TRUE;
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

    /* use the oid value for the rowid */
    if(fColType == SQL_BEST_ROWID) {
        row = (TupleNode *)malloc(sizeof(TupleNode) + (8 - 1) * sizeof(TupleField));

        set_tuplefield_int2(&row->tuple[0], SQL_SCOPE_SESSION);
        set_tuplefield_string(&row->tuple[1], "oid");
        set_tuplefield_int2(&row->tuple[2], pgtype_to_sqltype(stmt, PG_TYPE_OID));
        set_tuplefield_string(&row->tuple[3], "OID");
        set_tuplefield_int4(&row->tuple[4], pgtype_precision(stmt, PG_TYPE_OID, PG_STATIC, PG_STATIC));
        set_tuplefield_int4(&row->tuple[5], pgtype_length(stmt, PG_TYPE_OID, PG_STATIC, PG_STATIC));
        set_tuplefield_int2(&row->tuple[6], pgtype_scale(stmt, PG_TYPE_OID));
        set_tuplefield_int2(&row->tuple[7], SQL_PC_PSEUDO);

        QR_add_tuple(stmt->result, row);

    } else if(fColType == SQL_ROWVER) {

		Int2 the_type = PG_TYPE_INT4;

		if (atoi(ci->row_versioning)) {
			row = (TupleNode *)malloc(sizeof(TupleNode) + (8 - 1) * sizeof(TupleField));

			set_tuplefield_null(&row->tuple[0]);
			set_tuplefield_string(&row->tuple[1], "xmin");
			set_tuplefield_int2(&row->tuple[2], pgtype_to_sqltype(stmt, the_type));
			set_tuplefield_string(&row->tuple[3], pgtype_to_name(stmt, the_type));
			set_tuplefield_int4(&row->tuple[4], pgtype_precision(stmt, the_type, PG_STATIC, PG_STATIC));
			set_tuplefield_int4(&row->tuple[5], pgtype_length(stmt, the_type, PG_STATIC, PG_STATIC));
			set_tuplefield_int2(&row->tuple[6], pgtype_scale(stmt, the_type));
			set_tuplefield_int2(&row->tuple[7], SQL_PC_PSEUDO);

			QR_add_tuple(stmt->result, row);
		}
	}
    stmt->status = STMT_FINISHED;
    stmt->currTuple = -1;
	stmt->current_col = -1;

	mylog("SQLSpecialColumns(): EXIT,  stmt=%u\n", stmt);
    return SQL_SUCCESS;
}

RETCODE SQL_API SQLStatistics(
                              HSTMT         hstmt,
                              UCHAR FAR *   szTableQualifier,
                              SWORD         cbTableQualifier,
                              UCHAR FAR *   szTableOwner,
                              SWORD         cbTableOwner,
                              UCHAR FAR *   szTableName,
                              SWORD         cbTableName,
                              UWORD         fUnique,
                              UWORD         fAccuracy)
{
char *func="SQLStatistics";
StatementClass *stmt = (StatementClass *) hstmt;
char index_query[MAX_STATEMENT_LEN];
HSTMT hindx_stmt;
RETCODE result;
char *table_name;
char index_name[MAX_INFO_STRING];
short fields_vector[8];
char isunique[10], isclustered[10];
SDWORD index_name_len, fields_vector_len;
TupleNode *row;
int i;
HSTMT hcol_stmt;
StatementClass *col_stmt, *indx_stmt;
char column_name[MAX_INFO_STRING];
char **column_names = 0;
Int4 column_name_len;
int total_columns = 0;
char error = TRUE;
ConnInfo *ci;
char buf[256];

mylog("**** SQLStatistics(): ENTER,  stmt=%u\n", stmt);

    if( ! stmt) {
		SC_log_error(func, "", NULL);
        return SQL_INVALID_HANDLE;
    }

	stmt->manual_result = TRUE;
	stmt->errormsg_created = TRUE;

	ci = &stmt->hdbc->connInfo;

    stmt->result = QR_Constructor();
    if(!stmt->result) {
        stmt->errormsg = "Couldn't allocate memory for SQLStatistics result.";
        stmt->errornumber = STMT_NO_MEMORY_ERROR;
		SC_log_error(func, "", stmt);
        return SQL_ERROR;
    }

    // the binding structure for a statement is not set up until
    // a statement is actually executed, so we'll have to do this ourselves.
    extend_bindings(stmt, 13);

    // set the field names
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


    // only use the table name... the owner should be redundant, and
    // we never use qualifiers.
	table_name = make_string(szTableName, cbTableName, NULL);
	if ( ! table_name) {
        stmt->errormsg = "No table name passed to SQLStatistics.";
        stmt->errornumber = STMT_INTERNAL_ERROR;
		SC_log_error(func, "", stmt);
        return SQL_ERROR;
    }

	// we need to get a list of the field names first,
	// so we can return them later.
	result = SQLAllocStmt( stmt->hdbc, &hcol_stmt);
	if((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO)) {
		stmt->errormsg = "SQLAllocStmt failed in SQLStatistics for columns.";
		stmt->errornumber = STMT_NO_MEMORY_ERROR;
		goto SEEYA;
	}

	col_stmt = (StatementClass *) hcol_stmt;

	/*	"internal" prevents SQLColumns from returning the oid if it is being shown.
		This would throw everything off.
	*/
	col_stmt->internal = TRUE;
	result = SQLColumns(hcol_stmt, "", 0, "", 0, 
				table_name, (SWORD) strlen(table_name), "", 0);
	col_stmt->internal = FALSE;

	if((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO)) {
			stmt->errormsg = col_stmt->errormsg;        // "SQLColumns failed in SQLStatistics.";
			stmt->errornumber = col_stmt->errornumber;  // STMT_EXEC_ERROR;
			SQLFreeStmt(hcol_stmt, SQL_DROP);
			goto SEEYA;
	}
	result = SQLBindCol(hcol_stmt, 4, SQL_C_CHAR,
				column_name, MAX_INFO_STRING, &column_name_len);
	if((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO)) {
		stmt->errormsg = col_stmt->errormsg;
		stmt->errornumber = col_stmt->errornumber;
		SQLFreeStmt(hcol_stmt, SQL_DROP);
		goto SEEYA;

	}

	result = SQLFetch(hcol_stmt);
	while((result == SQL_SUCCESS) || (result == SQL_SUCCESS_WITH_INFO)) {
		total_columns++;

		column_names = 
		(char **)realloc(column_names, 
				 total_columns * sizeof(char *));
		column_names[total_columns-1] = 
		(char *)malloc(strlen(column_name)+1);
		strcpy(column_names[total_columns-1], column_name);

		mylog("SQLStatistics: column_name = '%s'\n", column_name);

		result = SQLFetch(hcol_stmt);
	}
	if(result != SQL_NO_DATA_FOUND || total_columns == 0) {
			stmt->errormsg = SC_create_errormsg(hcol_stmt); // "Couldn't get column names in SQLStatistics.";
			stmt->errornumber = col_stmt->errornumber;
			SQLFreeStmt(hcol_stmt, SQL_DROP);
   			goto SEEYA;

	}
	
	SQLFreeStmt(hcol_stmt, SQL_DROP);

    // get a list of indexes on this table
    result = SQLAllocStmt( stmt->hdbc, &hindx_stmt);
    if((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO)) {
		stmt->errormsg = "SQLAllocStmt failed in SQLStatistics for indices.";
		stmt->errornumber = STMT_NO_MEMORY_ERROR;
		goto SEEYA;

    }
	indx_stmt = (StatementClass *) hindx_stmt;

	sprintf(index_query, "select c.relname, i.indkey, i.indisunique, i.indisclustered from pg_index i, pg_class c, pg_class d where c.oid = i.indexrelid and d.relname = '%s' and d.oid = i.indrelid", 
		table_name);

    result = SQLExecDirect(hindx_stmt, index_query, strlen(index_query));
    if((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO)) {
		stmt->errormsg = SC_create_errormsg(hindx_stmt); // "Couldn't execute index query (w/SQLExecDirect) in SQLStatistics.";
		stmt->errornumber = indx_stmt->errornumber;
		SQLFreeStmt(hindx_stmt, SQL_DROP);
  		goto SEEYA;

    }

    // bind the index name column
    result = SQLBindCol(hindx_stmt, 1, SQL_C_CHAR,
                        index_name, MAX_INFO_STRING, &index_name_len);
    if((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO)) {
		stmt->errormsg = indx_stmt->errormsg; // "Couldn't bind column in SQLStatistics.";
		stmt->errornumber = indx_stmt->errornumber;
		SQLFreeStmt(hindx_stmt, SQL_DROP);
   		goto SEEYA;

    }
    // bind the vector column
    result = SQLBindCol(hindx_stmt, 2, SQL_C_DEFAULT,
                        fields_vector, 16, &fields_vector_len);
    if((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO)) {
		stmt->errormsg = indx_stmt->errormsg;  // "Couldn't bind column in SQLStatistics.";
		stmt->errornumber = indx_stmt->errornumber;
		SQLFreeStmt(hindx_stmt, SQL_DROP);
		goto SEEYA;

    }
    // bind the "is unique" column
    result = SQLBindCol(hindx_stmt, 3, SQL_C_CHAR,
                        isunique, sizeof(isunique), NULL);
    if((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO)) {
		stmt->errormsg = indx_stmt->errormsg;  // "Couldn't bind column in SQLStatistics.";
		stmt->errornumber = indx_stmt->errornumber;
		SQLFreeStmt(hindx_stmt, SQL_DROP);
		goto SEEYA;
    }

    // bind the "is clustered" column
    result = SQLBindCol(hindx_stmt, 4, SQL_C_CHAR,
                        isclustered, sizeof(isclustered), NULL);
    if((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO)) {
		stmt->errormsg = indx_stmt->errormsg;  // "Couldn't bind column in SQLStatistics.";
		stmt->errornumber = indx_stmt->errornumber;
		SQLFreeStmt(hindx_stmt, SQL_DROP);
		goto SEEYA;

    }

	/*	fake index of OID */
	if (atoi(ci->show_oid_column) && atoi(ci->fake_oid_index)) {
		row = (TupleNode *)malloc(sizeof(TupleNode) + 
					  (13 - 1) * sizeof(TupleField));

		// no table qualifier
		set_tuplefield_string(&row->tuple[0], "");
		// don't set the table owner, else Access tries to use it
		set_tuplefield_string(&row->tuple[1], "");
		set_tuplefield_string(&row->tuple[2], table_name);

		// non-unique index?
		set_tuplefield_int2(&row->tuple[3], (Int2) (globals.unique_index ? FALSE : TRUE));
		
		// no index qualifier
		set_tuplefield_string(&row->tuple[4], "");

		sprintf(buf, "%s_idx_fake_oid", table_name);
		set_tuplefield_string(&row->tuple[5], buf);

		// Clustered index?  I think non-clustered should be type OTHER not HASHED
		set_tuplefield_int2(&row->tuple[6], (Int2) SQL_INDEX_OTHER);
		set_tuplefield_int2(&row->tuple[7], (Int2) 1);

		set_tuplefield_string(&row->tuple[8], "oid");
		set_tuplefield_string(&row->tuple[9], "A");
		set_tuplefield_null(&row->tuple[10]);
		set_tuplefield_null(&row->tuple[11]);
		set_tuplefield_null(&row->tuple[12]);

		QR_add_tuple(stmt->result, row);
	}

    result = SQLFetch(hindx_stmt);
    while((result == SQL_SUCCESS) || (result == SQL_SUCCESS_WITH_INFO)) {

		//	If only requesting unique indexs, then just return those.
		if (fUnique == SQL_INDEX_ALL || 
			(fUnique == SQL_INDEX_UNIQUE && atoi(isunique))) {
			i = 0;
			// add a row in this table for each field in the index
			while(i < 8 && fields_vector[i] != 0) {

				row = (TupleNode *)malloc(sizeof(TupleNode) + 
							  (13 - 1) * sizeof(TupleField));

				// no table qualifier
				set_tuplefield_string(&row->tuple[0], "");
				// don't set the table owner, else Access tries to use it
				set_tuplefield_string(&row->tuple[1], "");
				set_tuplefield_string(&row->tuple[2], table_name);

				// non-unique index?
				if (globals.unique_index)
					set_tuplefield_int2(&row->tuple[3], (Int2) (atoi(isunique) ? FALSE : TRUE));
				else
					set_tuplefield_int2(&row->tuple[3], TRUE);
				
				// no index qualifier
				set_tuplefield_string(&row->tuple[4], "");
				set_tuplefield_string(&row->tuple[5], index_name);

				// Clustered index?  I think non-clustered should be type OTHER not HASHED
				set_tuplefield_int2(&row->tuple[6], (Int2) (atoi(isclustered) ? SQL_INDEX_CLUSTERED : SQL_INDEX_OTHER));
				set_tuplefield_int2(&row->tuple[7], (Int2) (i+1));

				if(fields_vector[i] == OID_ATTNUM) {
					set_tuplefield_string(&row->tuple[8], "oid");
					mylog("SQLStatistics: column name = oid\n");
				}
				else if(fields_vector[i] < 0 || fields_vector[i] > total_columns) {
					set_tuplefield_string(&row->tuple[8], "UNKNOWN");
					mylog("SQLStatistics: column name = UNKNOWN\n");
				}
				else {
					set_tuplefield_string(&row->tuple[8], column_names[fields_vector[i]-1]);
					mylog("SQLStatistics: column name = '%s'\n", column_names[fields_vector[i]-1]);
				}

				set_tuplefield_string(&row->tuple[9], "A");
				set_tuplefield_null(&row->tuple[10]);
				set_tuplefield_null(&row->tuple[11]);
				set_tuplefield_null(&row->tuple[12]);

				QR_add_tuple(stmt->result, row);
				i++;
			}
		}

        result = SQLFetch(hindx_stmt);
    }
    if(result != SQL_NO_DATA_FOUND) {
		stmt->errormsg = SC_create_errormsg(hindx_stmt); // "SQLFetch failed in SQLStatistics.";
		stmt->errornumber = indx_stmt->errornumber;
		SQLFreeStmt(hindx_stmt, SQL_DROP);
		goto SEEYA;
    }

	SQLFreeStmt(hindx_stmt, SQL_DROP);

    // also, things need to think that this statement is finished so
    // the results can be retrieved.
    stmt->status = STMT_FINISHED;

    // set up the current tuple pointer for SQLFetch
    stmt->currTuple = -1;
	stmt->current_col = -1;

	error = FALSE;

SEEYA:
	/* These things should be freed on any error ALSO! */
	free(table_name);
    for(i = 0; i < total_columns; i++) {
		free(column_names[i]);
    }
    free(column_names);

	mylog("SQLStatistics(): EXIT, %s, stmt=%u\n", error ? "error" : "success", stmt);

	if (error) {
		SC_log_error(func, "", stmt);
		return SQL_ERROR;
	}
	else
		return SQL_SUCCESS;
}

RETCODE SQL_API SQLColumnPrivileges(
                                    HSTMT        hstmt,
                                    UCHAR FAR *  szTableQualifier,
                                    SWORD        cbTableQualifier,
                                    UCHAR FAR *  szTableOwner,
                                    SWORD        cbTableOwner,
                                    UCHAR FAR *  szTableName,
                                    SWORD        cbTableName,
                                    UCHAR FAR *  szColumnName,
                                    SWORD        cbColumnName)
{
char *func="SQLColumnPrivileges";
/*	Neither Access or Borland care about this. */

	SC_log_error(func, "Function not implemented", (StatementClass *) hstmt);
    return SQL_ERROR;
}

RETCODE
getPrimaryKeyString(StatementClass *stmt, char *szTableName, SWORD cbTableName, char *svKey, int *nKey)
{
char *func = "getPrimaryKeyString";
HSTMT htbl_stmt;
StatementClass *tbl_stmt;
RETCODE result;
char tables_query[MAX_STATEMENT_LEN];
char attname[MAX_INFO_STRING];
SDWORD attname_len;
int nk = 0;

	if (nKey != NULL)
		*nKey = 0;

	svKey[0] = '\0';

	stmt->errormsg_created = TRUE;

    result = SQLAllocStmt( stmt->hdbc, &htbl_stmt);
    if((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO)) {
		stmt->errornumber = STMT_NO_MEMORY_ERROR;
		stmt->errormsg = "Couldn't allocate statement for Primary Key result.";
		SC_log_error(func, "", stmt);
        return SQL_ERROR;
    }
	tbl_stmt = (StatementClass *) htbl_stmt;

	tables_query[0] = '\0';
	if ( ! my_strcat(tables_query, "select distinct on attnum a2.attname, a2.attnum from pg_attribute a1, pg_attribute a2, pg_class c, pg_index i where c.relname = '%.*s_pkey' AND c.oid = i.indexrelid AND a1.attrelid = c.oid AND a2.attrelid = c.oid AND (i.indkey[0] = a1.attnum OR i.indkey[1] = a1.attnum OR i.indkey[2] = a1.attnum OR i.indkey[3] = a1.attnum OR i.indkey[4] = a1.attnum OR i.indkey[5] = a1.attnum OR i.indkey[6] = a1.attnum OR i.indkey[7] = a1.attnum) order by a2.attnum",
			szTableName, cbTableName)) {

		stmt->errormsg = "No Table specified to getPrimaryKeyString.";
	    stmt->errornumber = STMT_INTERNAL_ERROR;
		SC_log_error(func, "", stmt);
		SQLFreeStmt(htbl_stmt, SQL_DROP);
		return SQL_ERROR;
	}

	mylog("getPrimaryKeyString: tables_query='%s'\n", tables_query);

    result = SQLExecDirect(htbl_stmt, tables_query, strlen(tables_query));
    if((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO)) {
		stmt->errormsg = SC_create_errormsg(htbl_stmt);
		stmt->errornumber = tbl_stmt->errornumber;
		SC_log_error(func, "", stmt);
		SQLFreeStmt(htbl_stmt, SQL_DROP);
        return SQL_ERROR;
    }

    result = SQLBindCol(htbl_stmt, 1, SQL_C_CHAR,
                        attname, MAX_INFO_STRING, &attname_len);
    if((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO)) {
		stmt->errormsg = tbl_stmt->errormsg;
		stmt->errornumber = tbl_stmt->errornumber;
		SC_log_error(func, "", stmt);
		SQLFreeStmt(htbl_stmt, SQL_DROP);
        return SQL_ERROR;
    }

    result = SQLFetch(htbl_stmt);
    while((result == SQL_SUCCESS) || (result == SQL_SUCCESS_WITH_INFO)) {

		if (strlen(svKey) > 0)
			strcat(svKey, "+");
		strcat(svKey, attname);

        result = SQLFetch(htbl_stmt);
		nk++;
    }

    if(result != SQL_NO_DATA_FOUND) {
		stmt->errormsg = SC_create_errormsg(htbl_stmt);
		stmt->errornumber = tbl_stmt->errornumber;
		SC_log_error(func, "", stmt);
		SQLFreeStmt(htbl_stmt, SQL_DROP);
        return SQL_ERROR;
    }

	SQLFreeStmt(htbl_stmt, SQL_DROP);

	if (nKey != NULL)
		*nKey = nk;

	mylog(">> getPrimaryKeyString: returning nKey=%d, svKey='%s'\n", nk, svKey);
	return result;
}

RETCODE
getPrimaryKeyArray(StatementClass *stmt, char *szTableName, SWORD cbTableName, char keyArray[][MAX_INFO_STRING], int *nKey)
{
RETCODE result;
char svKey[MAX_KEYLEN], *svKeyPtr;
int i = 0;

	result = getPrimaryKeyString(stmt, szTableName, cbTableName, svKey, nKey);
	if (result != SQL_SUCCESS && result != SQL_NO_DATA_FOUND)
		//  error passed from above
		return result;

	//	If no keys, return NO_DATA_FOUND
	if (svKey[0] == '\0') {
		mylog("!!!!!! getPrimaryKeyArray: svKey was null\n");
		return SQL_NO_DATA_FOUND;
	}

	// mylog(">> primarykeyArray: nKey=%d, svKey='%s'\n",  *nKey, svKey);

	svKeyPtr = strtok(svKey, "+");
	while (svKeyPtr != NULL && i < MAX_KEYPARTS) {
		strcpy(keyArray[i++], svKeyPtr);
		svKeyPtr = strtok(NULL, "+");
	}

	/*
	for (i = 0; i < *nKey; i++)
		mylog(">> keyArray[%d] = '%s'\n", i, keyArray[i]);
	*/

	return result;
}


RETCODE SQL_API SQLPrimaryKeys(
                               HSTMT         hstmt,
                               UCHAR FAR *   szTableQualifier,
                               SWORD         cbTableQualifier,
                               UCHAR FAR *   szTableOwner,
                               SWORD         cbTableOwner,
                               UCHAR FAR *   szTableName,
                               SWORD         cbTableName)
{
char *func = "SQLPrimaryKeys";
StatementClass *stmt = (StatementClass *) hstmt;
TupleNode *row;
RETCODE result;
char svKey[MAX_KEYLEN], *ptr;
int seq = 1, nkeys = 0;

mylog("**** SQLPrimaryKeys(): ENTER, stmt=%u\n", stmt);

    if( ! stmt) {
		SC_log_error(func, "", NULL);
        return SQL_INVALID_HANDLE;
    }
	stmt->manual_result = TRUE;

	result = getPrimaryKeyString(stmt, szTableName, cbTableName, svKey, &nkeys);

	mylog(">> PrimaryKeys: getPrimaryKeyString() returned %d, nkeys=%d, svKey = '%s'\n", result, nkeys, svKey);

	if (result != SQL_SUCCESS && result != SQL_NO_DATA_FOUND) {
		//	error msg passed from above
		return result;
	}

	//	I'm not sure if this is correct to return when there are no keys or
	//	if an empty result set would be better.
	if (nkeys == 0) {
		stmt->errornumber = STMT_INFO_ONLY;
		stmt->errormsg = "No primary keys for this table.";
		return SQL_SUCCESS_WITH_INFO;
	}

    stmt->result = QR_Constructor();
    if(!stmt->result) {
        stmt->errormsg = "Couldn't allocate memory for SQLPrimaryKeys result.";
        stmt->errornumber = STMT_NO_MEMORY_ERROR;
		SC_log_error(func, "", stmt);
        return SQL_ERROR;
    }


    // the binding structure for a statement is not set up until
    // a statement is actually executed, so we'll have to do this ourselves.
    extend_bindings(stmt, 6);
	
    // set the field names
    QR_set_num_fields(stmt->result, 6);
    QR_set_field_info(stmt->result, 0, "TABLE_QUALIFIER", PG_TYPE_TEXT, MAX_INFO_STRING);
    QR_set_field_info(stmt->result, 1, "TABLE_OWNER", PG_TYPE_TEXT, MAX_INFO_STRING);
    QR_set_field_info(stmt->result, 2, "TABLE_NAME", PG_TYPE_TEXT, MAX_INFO_STRING);
    QR_set_field_info(stmt->result, 3, "COLUMN_NAME", PG_TYPE_TEXT, MAX_INFO_STRING);
    QR_set_field_info(stmt->result, 4, "KEY_SEQ", PG_TYPE_INT2, 2);
    QR_set_field_info(stmt->result, 5, "PK_NAME", PG_TYPE_TEXT, MAX_INFO_STRING);

    // add the tuples
	ptr = strtok(svKey, "+");
    while( ptr != NULL) {
        row = (TupleNode *)malloc(sizeof(TupleNode) + (6 - 1) * sizeof(TupleField));

        set_tuplefield_string(&row->tuple[0], "");

        // I have to hide the table owner from Access, otherwise it
        // insists on referring to the table as 'owner.table'.
        // (this is valid according to the ODBC SQL grammar, but
        // Postgres won't support it.)

		mylog(">> primaryKeys: ptab = '%s', seq = %d\n", ptr, seq);

        set_tuplefield_string(&row->tuple[1], "");
        set_tuplefield_string(&row->tuple[2], szTableName);
        set_tuplefield_string(&row->tuple[3], ptr);
		set_tuplefield_int2(&row->tuple[4], (Int2) (seq++));
		set_tuplefield_null(&row->tuple[5]);

        QR_add_tuple(stmt->result, row);

		ptr = strtok(NULL, "+");
	}

    // also, things need to think that this statement is finished so
    // the results can be retrieved.
    stmt->status = STMT_FINISHED;

    // set up the current tuple pointer for SQLFetch
    stmt->currTuple = -1;
	stmt->current_col = -1;

	mylog("SQLPrimaryKeys(): EXIT, stmt=%u\n", stmt);
    return SQL_SUCCESS;
}

RETCODE SQL_API SQLForeignKeys(
                               HSTMT         hstmt,
                               UCHAR FAR *   szPkTableQualifier,
                               SWORD         cbPkTableQualifier,
                               UCHAR FAR *   szPkTableOwner,
                               SWORD         cbPkTableOwner,
                               UCHAR FAR *   szPkTableName,
                               SWORD         cbPkTableName,
                               UCHAR FAR *   szFkTableQualifier,
                               SWORD         cbFkTableQualifier,
                               UCHAR FAR *   szFkTableOwner,
                               SWORD         cbFkTableOwner,
                               UCHAR FAR *   szFkTableName,
                               SWORD         cbFkTableName)
{
char *func = "SQLForeignKeys";
StatementClass *stmt = (StatementClass *) hstmt;
TupleNode *row;
HSTMT htbl_stmt;
StatementClass *tbl_stmt;
RETCODE result;
char tables_query[MAX_STATEMENT_LEN];
char relname[MAX_INFO_STRING], attnames[MAX_INFO_STRING], frelname[MAX_INFO_STRING];
SDWORD relname_len, attnames_len, frelname_len;
char *pktab, *fktab;
char fkey = FALSE;
char primaryKey[MAX_KEYPARTS][MAX_INFO_STRING];
char *attnamePtr;
int pkeys, seq;

mylog("**** SQLForeignKeys(): ENTER, stmt=%u\n", stmt);

	memset(primaryKey, 0, sizeof(primaryKey));

    if( ! stmt) {
		SC_log_error(func, "", NULL);
        return SQL_INVALID_HANDLE;
    }
	stmt->manual_result = TRUE;
	stmt->errormsg_created = TRUE;

    result = SQLAllocStmt( stmt->hdbc, &htbl_stmt);
    if((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO)) {
		stmt->errornumber = STMT_NO_MEMORY_ERROR;
		stmt->errormsg = "Couldn't allocate statement for SQLForeignKeys result.";
		SC_log_error(func, "", stmt);
        return SQL_ERROR;
    }

	tbl_stmt = (StatementClass *) htbl_stmt;

	pktab = make_string(szPkTableName, cbPkTableName, NULL);
	fktab = make_string(szFkTableName, cbFkTableName, NULL);

	if (pktab && fktab) {
        //	Get the primary key of the table listed in szPkTable
		result = getPrimaryKeyArray(stmt, pktab, (SWORD) strlen(pktab), primaryKey, &pkeys);
		if (result != SQL_SUCCESS && result != SQL_NO_DATA_FOUND) {
			//	error msg passed from above
			SQLFreeStmt(htbl_stmt, SQL_DROP);
			free(pktab); free(fktab);
			return result;
		}
		if (pkeys == 0) {
			stmt->errornumber = STMT_INFO_ONLY;
			stmt->errormsg = "No primary keys for this table.";
			SQLFreeStmt(htbl_stmt, SQL_DROP);
			free(pktab); free(fktab);
			return SQL_SUCCESS_WITH_INFO;
		}

	    sprintf(tables_query, "select relname, attnames, frelname from %s where relname='%s' AND frelname='%s'", KEYS_TABLE, fktab, pktab);
		free(pktab); free(fktab);
	}
    else if (pktab) {
        //	Get the primary key of the table listed in szPkTable
		result = getPrimaryKeyArray(stmt, pktab, (SWORD) strlen(pktab), primaryKey, &pkeys);
		if (result != SQL_SUCCESS && result != SQL_NO_DATA_FOUND) {
			//	error msg passed from above
			SQLFreeStmt(htbl_stmt, SQL_DROP);
			free(pktab);
			return result;
		}
		if (pkeys == 0) {
			stmt->errornumber = STMT_INFO_ONLY;
			stmt->errormsg = "No primary keys for this table.";
			SQLFreeStmt(htbl_stmt, SQL_DROP);
			free(pktab);
			return SQL_SUCCESS_WITH_INFO;
		}

	    sprintf(tables_query, "select relname, attnames, frelname from %s where frelname='%s'", KEYS_TABLE, pktab);
		free(pktab);
    }
    else if (fktab) {
		//	This query could involve multiple calls to getPrimaryKey()
		//	so put that off till we know what pktables we need.
		fkey = TRUE;

	    sprintf(tables_query, "select relname, attnames, frelname from %s where relname='%s'", KEYS_TABLE, fktab);
		free(fktab);
    }
	else {
		stmt->errormsg = "No tables specified to SQLForeignKeys.";
		stmt->errornumber = STMT_INTERNAL_ERROR;
		SC_log_error(func, "", stmt);
		SQLFreeStmt(htbl_stmt, SQL_DROP);
		return SQL_ERROR;
	}

    result = SQLExecDirect(htbl_stmt, tables_query, strlen(tables_query));
    if((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO)) {
		stmt->errormsg = SC_create_errormsg(htbl_stmt);
		stmt->errornumber = tbl_stmt->errornumber;
		SC_log_error(func, "", stmt);
    	SQLFreeStmt(htbl_stmt, SQL_DROP);
	    return SQL_ERROR;
    }

    result = SQLBindCol(htbl_stmt, 1, SQL_C_CHAR,
                        relname, MAX_INFO_STRING, &relname_len);
    if((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO)) {
		stmt->errormsg = tbl_stmt->errormsg;
		stmt->errornumber = tbl_stmt->errornumber;
		SC_log_error(func, "", stmt);
		SQLFreeStmt(htbl_stmt, SQL_DROP);
        return SQL_ERROR;
    }
    result = SQLBindCol(htbl_stmt, 2, SQL_C_CHAR,
                        attnames, MAX_INFO_STRING, &attnames_len);
    if((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO)) {
		stmt->errormsg = tbl_stmt->errormsg;
		stmt->errornumber = tbl_stmt->errornumber;
		SC_log_error(func, "", stmt);
		SQLFreeStmt(htbl_stmt, SQL_DROP);
        return SQL_ERROR;
    }

    result = SQLBindCol(htbl_stmt, 3, SQL_C_CHAR,
                        frelname, MAX_INFO_STRING, &frelname_len);
    if((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO)) {
		stmt->errormsg = tbl_stmt->errormsg;
		stmt->errornumber = tbl_stmt->errornumber;
		SC_log_error(func, "", stmt);
		SQLFreeStmt(htbl_stmt, SQL_DROP);
        return SQL_ERROR;
    }

    stmt->result = QR_Constructor();
    if(!stmt->result) {
		stmt->errormsg = "Couldn't allocate memory for SQLForeignKeys result.";
        stmt->errornumber = STMT_NO_MEMORY_ERROR;
		SC_log_error(func, "", stmt);
		SQLFreeStmt(htbl_stmt, SQL_DROP);
        return SQL_ERROR;
    }

    // the binding structure for a statement is not set up until
    // a statement is actually executed, so we'll have to do this ourselves.
    extend_bindings(stmt, 13);

    // set the field names
    QR_set_num_fields(stmt->result, 13);
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

    // add the tuples
    result = SQLFetch(htbl_stmt);

    while((result == SQL_SUCCESS) || (result == SQL_SUCCESS_WITH_INFO)) {

		if (fkey == TRUE) {
			result = getPrimaryKeyArray(stmt, frelname, (SWORD) strlen(frelname), primaryKey, &pkeys);

			//  mylog(">> getPrimaryKeyArray: frelname = '%s', pkeys = %d, result = %d\n", frelname, pkeys, result);

			//	If an error occurs or for some reason there is no primary key for a
			//	table that is a foreign key, then skip that one.
			if ((result != SQL_SUCCESS && result != SQL_NO_DATA_FOUND) || pkeys == 0) {
		        result = SQLFetch(htbl_stmt);
				continue;
			}

			/*
			for (i = 0; i< pkeys; i++)
				mylog(">> fkey: pkeys=%d, primaryKey[%d] = '%s'\n", pkeys, i, primaryKey[i]);
			mylog(">> !!!!!!!!! pkeys = %d\n", pkeys);
			*/
		}

		// mylog(">> attnames='%s'\n", attnames);

		attnamePtr = strtok(attnames, "+");
		seq = 0;

		while (attnamePtr != NULL && seq < pkeys) {

	        row = (TupleNode *)malloc(sizeof(TupleNode) + (13 - 1) * sizeof(TupleField));

			set_tuplefield_null(&row->tuple[0]);

			// I have to hide the table owner from Access, otherwise it
			// insists on referring to the table as 'owner.table'.
			// (this is valid according to the ODBC SQL grammar, but
			// Postgres won't support it.)

			mylog(">> foreign keys: pktab='%s' patt='%s' fktab='%s' fatt='%s' seq=%d\n", 
				frelname, primaryKey[seq], relname, attnamePtr, (seq+1));

			set_tuplefield_string(&row->tuple[1], "");
			set_tuplefield_string(&row->tuple[2], frelname);
			set_tuplefield_string(&row->tuple[3], primaryKey[seq]);
			set_tuplefield_null(&row->tuple[4]);
			set_tuplefield_string(&row->tuple[5], "");
			set_tuplefield_string(&row->tuple[6], relname);
			set_tuplefield_string(&row->tuple[7], attnamePtr);
			set_tuplefield_int2(&row->tuple[8], (Int2) (++seq));
			set_tuplefield_null(&row->tuple[9]);
			set_tuplefield_null(&row->tuple[10]);
			set_tuplefield_null(&row->tuple[11]);
			set_tuplefield_null(&row->tuple[12]);

			QR_add_tuple(stmt->result, row);

			attnamePtr = strtok(NULL, "+");
		}
        result = SQLFetch(htbl_stmt);
    }

    if(result != SQL_NO_DATA_FOUND) {
		stmt->errormsg = SC_create_errormsg(htbl_stmt);
		stmt->errornumber = tbl_stmt->errornumber;
		SC_log_error(func, "", stmt);
		SQLFreeStmt(htbl_stmt, SQL_DROP);
        return SQL_ERROR;
    }

	SQLFreeStmt(htbl_stmt, SQL_DROP);

    // also, things need to think that this statement is finished so
    // the results can be retrieved.
    stmt->status = STMT_FINISHED;

    // set up the current tuple pointer for SQLFetch
    stmt->currTuple = -1;
	stmt->current_col = -1;

	mylog("SQLForeignKeys(): EXIT, stmt=%u\n", stmt);
    return SQL_SUCCESS;
}



RETCODE SQL_API SQLProcedureColumns(
                                    HSTMT         hstmt,
                                    UCHAR FAR *   szProcQualifier,
                                    SWORD         cbProcQualifier,
                                    UCHAR FAR *   szProcOwner,
                                    SWORD         cbProcOwner,
                                    UCHAR FAR *   szProcName,
                                    SWORD         cbProcName,
                                    UCHAR FAR *   szColumnName,
                                    SWORD         cbColumnName)
{
char *func="SQLProcedureColumns";

	SC_log_error(func, "Function not implemented", (StatementClass *) hstmt);
    return SQL_ERROR;
}

RETCODE SQL_API SQLProcedures(
                              HSTMT          hstmt,
                              UCHAR FAR *    szProcQualifier,
                              SWORD          cbProcQualifier,
                              UCHAR FAR *    szProcOwner,
                              SWORD          cbProcOwner,
                              UCHAR FAR *    szProcName,
                              SWORD          cbProcName)
{
char *func="SQLProcedures";

	SC_log_error(func, "Function not implemented", (StatementClass *) hstmt);
    return SQL_ERROR;
}

RETCODE SQL_API SQLTablePrivileges(
                                   HSTMT           hstmt,
                                   UCHAR FAR *     szTableQualifier,
                                   SWORD           cbTableQualifier,
                                   UCHAR FAR *     szTableOwner,
                                   SWORD           cbTableOwner,
                                   UCHAR FAR *     szTableName,
                                   SWORD           cbTableName)
{
char *func="SQLTablePrivileges";

	SC_log_error(func, "Function not implemented", (StatementClass *) hstmt);
    return SQL_ERROR;
}
