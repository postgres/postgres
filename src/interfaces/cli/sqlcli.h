/* -*- C -*- */
/* sqlcli.h  Header File for SQL CLI.
 * The actual header file must contain at least the information
 * specified here, except that the comments may vary.
 *
 * This file is adapted for PostgreSQL
 * from the SQL98 August 1994 draft standard.
 * Thomas G. Lockhart 1999-06-16
 */

/* API declaration data types */
typedef unsigned char   SQLCHAR;
typedef long            SQLINTEGER;
typedef short           SQLSMALLINT;
typedef double          SQLDOUBLE;
typedef float           SQLREAL;
typedef void *          SQLPOINTER;
typedef unsigned char   SQLDATE;
typedef unsigned char   SQLTIME;
typedef unsigned char   SQLTIMESTAMP;
typedef unsigned char   SQLDECIMAL;
typedef unsigned char   SQLNUMERIC;

/* function return type */
typedef SQLSMALLINT     SQLRETURN;

/* generic data structures */
typedef SQLINTEGER      SQLHENV;    /* environment handle */
typedef SQLINTEGER      SQLHDBC;    /* connection handle */
typedef SQLINTEGER      SQLHSTMT;   /* statement handle */
typedef SQLINTEGER      SQLHDESC;   /* descriptor handle */

/* special length/indicator values */
#define SQL_NULL_DATA             -1
#define SQL_DATA_AT_EXEC          -2

/* return values from functions */
#define SQL_SUCCESS                0
#define SQL_SUCCESS_WITH_INFO      1
#define SQL_NEED_DATA             99
#define SQL_NO_DATA              100
#define SQL_ERROR                 -1
#define SQL_INVALID_HANDLE        -2

/* test for SQL_SUCCESS or SQL_SUCCESS_WITH_INFO */
#define SQL_SUCCEEDED(rc) (((rc)&(~1))==0)

/* flags for null-terminated string */
#define SQL_NTS                   -3
#define SQL_NTSL                  -3L

/* maximum message length */
#define SQL_MAX_MESSAGE_LENGTH   255

/* maximum identifier length */
#define SQL_MAX_ID_LENGTH         18

/* date/time length constants */
/* add p+1 for time and timestamp if precision is nonzero
#define SQL_DATE_LEN              10
#define SQL_TIME_LEN               8
#define SQL_TIMESTAMP_LEN         19

/* handle type identifiers */
#define SQL_HANDLE_ENV             1
#define SQL_HANDLE_DBC             2
#define SQL_HANDLE_STMT            3
#define SQL_HANDLE_DESC            4

/* environment attribute */
#define SQL_ATTR_OUTPUT_NTS    10001

/* connection attribute */
#define SQL_ATTR_AUTO_IPD      10001

/* statement attributes */
#define SQL_ATTR_APP_ROW_DESC    10010
#define SQL_ATTR_APP_PARAM_DESC  10011
#define SQL_ATTR_IMP_ROW_DESC    10012
#define SQL_ATTR_IMP_PARAM_DESC  10013

/* identifiers of fields in the SQL descriptor */
#define SQL_DESC_COUNT             1
#define SQL_DESC_TYPE              2
#define SQL_DESC_LENGTH            3
#define SQL_DESC_LENGTH_PTR        4
#define SQL_DESC_PRECISION         5
#define SQL_DESC_SCALE             6
#define SQL_DESC_DATETIME_INTERVAL_CODE 7
#define SQL_DESC_NULLABLE          8
#define SQL_DESC_INDICATOR_PTR     9
#define SQL_DESC_DATA_PTR         10
#define SQL_DESC_NAME             11
#define SQL_DESC_UNNAMED          12
#define SQL_DESC_ALLOC_TYPE       99

/* identifiers of fields in the diagnostics area */
#define SQL_DIAG_RETURNCODE        1
#define SQL_DIAG_NUMBER            2
#define SQL_DIAG_ROW_COUNT         3
#define SQL_DIAG_SQLSTATE          4
#define SQL_DIAG_NATIVE            5
#define SQL_DIAG_MESSAGE_TEXT      6
#define SQL_DIAG_DYNAMIC_FUNCTION  7
#define SQL_DIAG_CLASS_ORIGIN      8
#define SQL_DIAG_SUBCLASS_ORIGIN   9
#define SQL_DIAG_CONNECTION_NAME  10
#define SQL_DIAG_SERVER_NAME      11
#define SQL_DIAG_DYNAMIC_FUNCTION_CODE 12

/* dynamic function codes returned in diagnostics area*/
#define SQL_DIAG_ALTER_DOMAIN                3
#define SQL_DIAG_ALTER_TABLE                 4
#define SQL_DIAG_CREATE_ASSERTION            6
#define SQL_DIAG_CREATE_CHARACTER_SET        8
#define SQL_DIAG_CREATE_COLLATION           10
#define SQL_DIAG_CREATE_DOMAIN              23
#define SQL_DIAG_CREATE_SCHEMA              64
#define SQL_DIAG_CREATE_TABLE               77
#define SQL_DIAG_CREATE_TRANSLATION         79
#define SQL_DIAG_CREATE_VIEW                84
#define SQL_DIAG_DELETE_WHERE               19
#define SQL_DIAG_DROP_ASSERTION             24
#define SQL_DIAG_DROP_CHARACTER_SET         25
#define SQL_DIAG_DROP_COLLATION             26
#define SQL_DIAG_DROP_DOMAIN                27
#define SQL_DIAG_DROP_SCHEMA                31
#define SQL_DIAG_DROP_TABLE                 32
#define SQL_DIAG_DROP_TRANSLATION           33
#define SQL_DIAG_DROP_VIEW                  36
#define SQL_DIAG_DYNAMIC_DELETE_CURSOR      54
#define SQL_DIAG_DYNAMIC_UPDATE_CURSOR      55
#define SQL_DIAG_GRANT                      48
#define SQL_DIAG_INSERT                     50
#define SQL_DIAG_REVOKE                     59
#define SQL_DIAG_SELECT                     41
#define SQL_DIAG_SELECT_CURSOR              85
#define SQL_DIAG_SET_CATALOG                66
#define SQL_DIAG_SET_CONSTRAINT             68
#define SQL_DIAG_SET_NAMES                  72
#define SQL_DIAG_SET_SCHEMA                 74
#define SQL_DIAG_SET_SESSION_AUTHORIZATION  76
#define SQL_DIAG_SET_TIME_ZONE              71
#define SQL_DIAG_SET_TRANSACTION            75
#define SQL_DIAG_UNKNOWN_STATEMENT           0
#define SQL_DIAG_UPDATE_WHERE               82

/* SQL data type codes */
#define SQL_CHAR         1
#define SQL_NUMERIC      2
#define SQL_DECIMAL      3
#define SQL_INTEGER      4
#define SQL_SMALLINT     5
#define SQL_FLOAT        6
#define SQL_REAL         7
#define SQL_DOUBLE       8
#define SQL_DATETIME     9
#define SQL_INTERVAL    10
#define SQL_VARCHAR     12
#define SQL_BIT         14
#define SQL_BIT_VARYING 15

/*      One-parameter shortcuts for datetime data types         */
#define         SQL_TYPE_DATE           91
#define         SQL_TYPE_TIME           92
#define         SQL_TYPE_TIMESTAMP      93

/*      GetTypeInfo request for all data types  */
#define         SQL_ALL_TYPES   0

/* BindCol()  and BindParam() default conversion code */
#define SQL_DEFAULT     99

/*      GetData code indicating that the application parameter */
/*      descriptor specifies the data type */
#define         SQL_ARD_TYPE            -99

/* date/time type subcodes */
#define SQL_CODE_DATE       1
#define SQL_CODE_TIME       2
#define SQL_CODE_TIMESTAMP  3
#define SQL_CODE_TIME_ZONE  4
#define SQL_CODE_TIMESTAMP_ZONE  5

/* interval qualifier codes */
#define SQL_DAY             1
#define SQL_DAY_TO_HOUR     2
#define SQL_DAY_TO_MINUTE   3
#define SQL_DAY_TO_SECOND   4
#define SQL_HOUR            5
#define SQL_HOUR_TO_MINUTE  6
#define SQL_HOUR_TO_SECOND  7
#define SQL_MINUTE          8
#define SQL_MINUTE_TO_SECOND 9
#define SQL_MONTH          10
#define SQL_SECOND         11
#define SQL_YEAR           12
#define SQL_YEAR_TO_MONTH  13

/* CLI option values */
#define SQL_FALSE        0
#define SQL_TRUE         1

/* values of NULLABLE field in descriptor */
#define SQL_NO_NULLS     0
#define SQL_NULLABLE     1

/*      Values returned by GetTypeInfo for the SEARCHABLE column        */
#define         SQL_PRED_NONE   0
#define         SQL_PRED_CHAR   1
#define         SQL_PRED_BASIC  2

/* values of UNNAMED field in descriptor */
#define SQL_NAMED        0
#define SQL_UNNAMED      1

/* values of ALLOC_TYPE field in descriptor */
#define SQL_DESC_ALLOC_AUTO  1
#define SQL_DESC_ALLOC_USER  2

/* EndTran()  options */
#define SQL_COMMIT       0
#define SQL_ROLLBACK     1

/* FreeStmt()  options */
#define SQL_CLOSE        0
#define SQL_DROP         1
#define SQL_UNBIND       2
#define SQL_RESET_PARAMS 3

/* null handles returned by AllocHandle()  */
#define SQL_NULL_HENV    0
#define SQL_NULL_HDBC    0
#define SQL_NULL_HSTMT   0
#define SQL_NULL_HDESC   0

/*      GetFunctions values to identify CLI routines    */
#define         SQL_API_SQLALLOCCONNECT         1
#define         SQL_API_SQLALLOCENV             2
#define         SQL_API_SQLALLOCHANDLE          1001
#define         SQL_API_SQLALLOCSTMT            3
#define         SQL_API_SQLBINDCOL              4
#define         SQL_API_SQLBINDPARAM            1002
#define         SQL_API_SQLCANCEL               5
#define         SQL_API_SQLCLOSECURSOR          1003
#define         SQL_API_SQLCONNECT              7
#define         SQL_API_SQLCOPYDESC             1004
#define         SQL_API_SQLCOLATTRIBUTE         6
#define         SQL_API_SQLDESCRIBECOL          8
#define         SQL_API_SQLDISCONNECT           9
#define         SQL_API_SQLENDTRAN              1005
#define         SQL_API_SQLERROR                10
#define         SQL_API_SQLEXECDIRECT           11
#define         SQL_API_SQLEXECUTE              12
#define         SQL_API_SQLFETCH                13
#define         SQL_API_SQLFREECONNECT          14
#define         SQL_API_SQLFREEENV              15
#define         SQL_API_SQLFREEHANDLE           1006
#define         SQL_API_SQLFREESTMT             16
#define         SQL_API_SQLFUNCTIONS            44
#define         SQL_API_SQLGETCONNECTATTR       1007
#define         SQL_API_SQLGETCURSORNAME        17
#define         SQL_API_SQLGETDATA              43
#define         SQL_API_SQLGETDESCFIELD         1008
#define         SQL_API_SQLGETDESCREC           1009
#define         SQL_API_SQLGETDIAGFIELD         1010
#define         SQL_API_SQLGETDIAGREC           1011
#define         SQL_API_SQLGETENVATTR           1012
#define         SQL_API_SQLGETINFO              45
#define         SQL_API_SQLGETSTMTATTR          1014
#define         SQL_API_SQLGETTYPEINFO          47
#define         SQL_API_SQLNUMRESULTCOLS        18
#define         SQL_API_SQLPARAMDATA            48
#define         SQL_API_SQLPREPARE              19
#define         SQL_API_SQLPUTDATA              49
#define         SQL_API_SQLRELEASEENV           1015
#define         SQL_API_SQLROWCOUNT             20
#define         SQL_API_SQLSCROLLFETCH          1021
#define         SQL_API_SQLSETCONNECTATTR       1016
#define         SQL_API_SQLSETCURSORNAME        21
#define         SQL_API_SQLSETDESCFIELD         1017
#define         SQL_API_SQLSETDESCREC           1018
#define         SQL_API_SQLSETENVATTR           1019
#define         SQL_API_SQLSETSTMTATTR          1020

/*      information requested by GetInfo        */
#define         SQL_MAX_DRIVER_CONNECTIONS      0
#define         SQL_MAX_RESULT_SETS             1
#define         SQL_DBMS_NAME                   17
#define         SQL_DBMS_VER                    18
#define         SQL_MAX_COLUMN_NAME_LEN         30
#define         SQL_MAX_CURSOR_NAME_LEN         31
#define         SQL_MAX_TABLE_NAME_LEN          35

/* null handle used when allocating HENV */
#define SQL_NULL_HANDLE  0L

SQLRETURN  SQLAllocConnect(SQLHENV EnvironmentHandle,
			   SQLHDBC *ConnectionHandle);

SQLRETURN  SQLAllocEnv(SQLHENV *EnvironmentHandle);

SQLRETURN  SQLAllocHandle(SQLSMALLINT HandleType,
			  SQLINTEGER InputHandle,
			  SQLINTEGER *OutputHandle);


SQLRETURN  SQLAllocStmt(SQLHDBC ConnectionHandle,
                         SQLSTMT *StatementHandle);

SQLRETURN  SQLBindCol(SQLHSTMT StatementHandle,
		      SQLSMALLINT ColumnNumber,
		      SQLSMALLINT BufferType,
		      SQLPOINTER Data,
		      SQLINTEGER BufferLength,
		      SQLINTEGER *DataLength);

SQLRETURN  SQLBindParam(SQLHSTMT StatementHandle,
			SQLSMALLINT ParamNumber,
			SQLSMALLINT BufferType,
			SQLSMALLINT ParamType,
			SQLINTEGER ParamLength,
			SQLSMALLINT Scale,
			SQLPOINTER Data,
			SQLINTEGER *DataLength);

SQLRETURN  SQLCancel(SQLHSTMT StatementHandle);

SQLRETURN  SQLCloseCursor(SQLHSTMT StatementHandle);

SQLRETURN  SQLColAttribute(SQLHENV StatementHandle,
			   SQLSMALLINT ColumnNumber,
			   SQLSMALLINT FieldIdentifier,
			   SQLCHAR *CharacterAttribute,
			   SQLINTEGER BufferLength,
			   SQLINTEGER *AttributetLength,
			   SQLINTEGER *NumericAttribute);


SQLRETURN  SQLConnect(SQLHDBC ConnectionHandle,
		      SQLCHAR *ServerName,
		      SQLSMALLINT NameLength1,
		      SQLCHAR *UserName,
		      SQLSMALLINT NameLength2,
		      SQLCHAR *Authentication,
		      SQLSMALLINT NameLength3);


SQLRETURN  SQLCopyDesc(SQLHDESC SourceDescHandle,
		       SQLHDESC TargetDescHandle);

SQLRETURN  SQLDescribeCol(SQLHSTMT StatementHandle,
			  SQLSMALLINT ColumnNumber,
			  SQLCHAR *ColumnName,
			  SQLSMALLINT BufferLength,
			  SQLSMALLINT *ColumnNameLength,
			  SQLSMALLINT *ColumnType,
			  SQLINTEGER *ColumnLength,
			  SQLSMALLINT *ColumnScale,
			  SQLSMALLINT *Nullable);

SQLRETURN  SQLDisconnect(SQLHDBC ConnectionHandle);

SQLRETURN  SQLEndTran(SQLSMALLINT HandleType,
		      SQLHENV Handle,
		      SQLSMALLINT CompletionType);

SQLRETURN  SQLError(SQLHENV EnvironmentHandle,
		    SQLHDBC ConnectionHandle,
		    SQLSTMT StatementHandle,
		    SQLCHAR *Sqlstate,
		    SQLINTEGER *NativeError,
		    SQLCHAR *MessageText,
		    SQLINTEGER BufferLength,
		    SQLINTEGER *TextLength);

SQLRETURN  SQLExecDirect(SQLHSTMT StatementHandle,
                         SQLCHAR *StatementText,
			 SQLSMALLINT StringLength);


SQLRETURN  SQLExecute(SQLHSTMT StatementHandle);

SQLRETURN  SQLFetch(SQLHSTMT StatementHandle);

SQLRETURN  SQLFreeConnect(SQLHDBC ConnectionHandle);

SQLRETURN  SQLFreeEnv(SQLHENV EnvironmentHandle);

SQLRETURN  SQLFreeHandle(SQLSMALLINT HandleType,
                         SQLINTEGER Handle);

SQLRETURN  SQLFreeStmt(SQLHSTMT StatementHandle);

SQLRETURN  SQLGetConnectAttr(SQLHDBC ConnectionHandle,
			     SQLINTEGER Attribute,
			     SQLPOINTER Value,
			     SQLINTEGER BufferLength,
			     SQLINTEGER *StringLength);


SQLRETURN  SQLGetCursorName(SQLHSTMT StatementHandle,
			    SQLCHAR *CursorName,
			    SQLSMALLINT BufferLength,
			    SQLSMALLINT *NameLength);

SQLRETURN  SQLGetData(SQLHSTMT StatementHandle,
		      SQLSMALLINT ColumnNumber,
		      SQLSMALLINT TargetType,
		      SQLPOINTER TargetValue,
		      SQLINTEGER BufferLength,
		      SQLINTEGER *IndicatorValue);

SQLRETURN  SQLGetFunctions(SQLHDBC ConnectionHandle,
			   SQLSMALLINT FunctionId,
			   SQLSMALLINT *Supported);

SQLRETURN  SQLGetInfo(SQLHDBC ConnectionHandle,
		      SQLSMALLINT InfoType,
		      SQLPOINTER InfoValue,
		      SQLSMALLINT BufferLength,
		      SQLSMALLINT *StringLength);

SQLRETURN  SQLGetTypeInfo(SQLHSTMT StatementHandle,
			  SQLSMALLINT DataType);

SQLRETURN  SQLGetDescField(SQLHDESC DescriptorHandle,
			   SQLSMALLINT RecordNumber,
			   SQLSMALLINT FieldIdentifier,
			   SQLPOINTER Value,
			   SQLINTEGER BufferLength,
			   SQLINTEGER *StringLength);

SQLRETURN  SQLGetDescRec(SQLHDESC DescriptorHandle,
                         SQLSMALLINT RecordNumber,
			 SQLCHAR *Name,
                         SQLSMALLINT BufferLength,
			 SQLSMALLINT *StringLength,
                         SQLSMALLINT *Type,
			 SQLSMALLINT *SubType,
                         SQLINTEGER *Length,
			 SQLSMALLINT *Precision,
                         SQLSMALLINT *Scale,
			 SQLSMALLINT *Nullable);

SQLRETURN  SQLGetDiagField(SQLSMALLINT HandleType,
			   SQLINTEGER Handle,
			   SQLSMALLINT RecordNumber,
			   SQLSMALLINT DiagIdentifier,
			   SQLPOINTER DiagInfo,
			   SQLSMALLINT BufferLength,
			   SQLSMALLINT *StringLength);


SQLRETURN  SQLGetDiagRec(SQLSMALLINT HandleType,
			 SQLINTEGER Handle,
                         SQLSMALLINT RecordNumber,
			 SQLCHAR *Sqlstate,
                         SQLINTEGER *NativeError,
			 SQLCHAR *MessageText,
                         SQLSMALLINT BufferLength,
			 SQLSMALLINT *StringLength);

SQLRETURN  SQLGetEnvAttr(SQLHENV EnvironmentHandle,
                         SQLINTEGER Attribute,
			 SQLPOINTER Value,
                         SQLINTEGER BufferLength,
			 SQLINTEGER *StringLength);

SQLRETURN  SQLGetStmtAttr(SQLHSTMT StatementHandle,
			  SQLINTEGER Attribute,
			  SQLPOINTER Value,
			  SQLINTEGER BufferLength,
			  SQLINTEGER *StringLength);

SQLRETURN  SQLLanguages(SQLHSTMT StatementHandle);

SQLRETURN  SQLNumResultCols(SQLHSTMT StatementHandle,
			    SQLINTEGER *ColumnCount);

SQLRETURN  SQLParamData(SQLHSTMT StatementHandle,
			SQLPOINTER *Value);

SQLRETURN  SQLPrepare(SQLHSTMT StatementHandle,
		      SQLCHAR *StatementText,
		      SQLSMALLINT StringLength);


SQLRETURN  SQLPutData(SQLHSTMT StatementHandle,
		      SQLPOINTER Data,
		      SQLINTEGER StringLength);

SQLRETURN  SQLReleaseEnv(SQLHENV EnvironmentHandle);

SQLRETURN  SQLRowCount(SQLHSTMT StatementHandle,
		       SQLINTEGER *RowCount);

SQLRETURN  SQLScrollFetch(SQLHSTMT StatementHandle,
			  SQLINTEGER FetchOrientation,
			  SQLINTEGER FetchOffset);

SQLRETURN  SQLSetConnectAttr(SQLHDBC ConnectionHandle,
			     SQLINTEGER AttributeCursorName,
			     SQLPOINTER Value,
			     SQLINTEGER StringLength);

SQLRETURN  SQLSetCursorName(SQLHSTMT StatementHandle,
			    SQLCHAR *CursorName,
			    SQLSMALLINT NameLength);

SQLRETURN  SQLSetDescField(SQLHDESC DescriptorHandle,
			   SQLSMALLINT RecordNumber,
			   SQLSMALLINT FieldIdentifier,
			   SQLPOINTER Value, SQLINTEGER BufferLength);

SQLRETURN  SQLSetDescRec(SQLHDESC DescriptorHandle,
                         SQLSMALLINT RecordNumber,
			 SQLSMALLINT Type,
                         SQLSMALLINT SubType,
			 SQLINTEGER Length,
                         SQLSMALLINT Precision,
			 SQLSMALLINT Scale,
                         SQLPOINTER Data,
			 SQLINTEGER *StringLength,
                         SQLSMALLINT *Indicator);

SQLRETURN  SQLSetEnvAttr(SQLHENV EnvironmentHandle,
                         SQLINTEGER Attribute,
			 SQLPOINTER Value,
                         SQLINTEGER StringLength);

SQLRETURN  SQLSetStmtAttr(SQLHSTMT StatementHandle,
			  SQLINTEGER Attribute,
			  SQLPOINTER Value,
			  SQLINTEGER StringLength);
