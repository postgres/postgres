/*-------------------------------------------------------------------------
 *
 * elog.h
 *	  POSTGRES error logging definitions.
 *
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: elog.h,v 1.56 2003/07/24 17:52:49 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef ELOG_H
#define ELOG_H

/* Error level codes */
#define DEBUG5		10			/* Debugging messages, in categories of
								 * decreasing detail. */
#define DEBUG4		11
#define DEBUG3		12
#define DEBUG2		13
#define DEBUG1		14			/* used by GUC debug_* variables */
#define LOG			15			/* Server operational messages; sent only
								 * to server log by default. */
#define COMMERROR	16			/* Client communication problems; same as
								 * LOG for server reporting, but never
								 * sent to client. */
#define INFO		17			/* Informative messages that are always
								 * sent to client;	is not affected by
								 * client_min_messages */
#define NOTICE		18			/* Helpful messages to users about query
								 * operation;  sent to client and server
								 * log by default. */
#define WARNING		19			/* Warnings */
#define ERROR		20			/* user error - abort transaction; return
								 * to known state */
/* Save ERROR value in PGERROR so it can be restored when Win32 includes
 * modify it.  We have to use a constant rather than ERROR because macros
 * are expanded only when referenced outside macros.
 */
#ifdef WIN32
#define PGERROR		20
#endif
#define FATAL		21			/* fatal error - abort process */
#define PANIC		22			/* take down the other backends with me */

 /*#define DEBUG DEBUG1*/	/* Backward compatibility with pre-7.3 */


/* macros for representing SQLSTATE strings compactly */
#define PGSIXBIT(ch)	(((ch) - '0') & 0x3F)
#define PGUNSIXBIT(val)	(((val) & 0x3F) + '0')

#define MAKE_SQLSTATE(ch1,ch2,ch3,ch4,ch5)	\
	(PGSIXBIT(ch1) + (PGSIXBIT(ch2) << 6) + (PGSIXBIT(ch3) << 12) + \
	 (PGSIXBIT(ch4) << 18) + (PGSIXBIT(ch5) << 24))


/*
 * SQLSTATE codes for errors.
 *
 * The SQL99 code set is rather impoverished, especially in the area of
 * syntactical and semantic errors.  We have borrowed codes from IBM's DB2
 * and invented our own codes to develop a useful code set.
 *
 * When adding a new code, make sure it is placed in the most appropriate
 * class (the first two characters of the code value identify the class).
 * The listing is organized by class to make this prominent.
 *
 * The generic '000' class code should be used for an error only when there
 * is not a more-specific code defined.
 */

/* Class 00 - Successful Completion */
#define ERRCODE_SUCCESSFUL_COMPLETION		MAKE_SQLSTATE('0','0', '0','0','0')

/* Class 01 - Warning */
/* (do not use this class for failure conditions!) */
#define ERRCODE_WARNING						MAKE_SQLSTATE('0','1', '0','0','0')
#define ERRCODE_WARNING_DYNAMIC_RESULT_SETS_RETURNED		MAKE_SQLSTATE('0','1', '0','0','C')
#define ERRCODE_WARNING_IMPLICIT_ZERO_BIT_PADDING	MAKE_SQLSTATE('0','1', '0','0','8')
#define ERRCODE_WARNING_NULL_VALUE_ELIMINATED_IN_SET_FUNCTION	MAKE_SQLSTATE('0','1', '0','0','3')
#define ERRCODE_WARNING_STRING_DATA_RIGHT_TRUNCATION	MAKE_SQLSTATE('0','1', '0','0','4')

/* Class 02 - No Data --- this is also a warning class per SQL99 */
/* (do not use this class for failure conditions!) */
#define ERRCODE_NO_DATA						MAKE_SQLSTATE('0','2', '0','0','0')
#define ERRCODE_NO_ADDITIONAL_DYNAMIC_RESULT_SETS_RETURNED	MAKE_SQLSTATE('0','2', '0','0','1')

/* Class 03 - SQL Statement Not Yet Complete */
#define ERRCODE_SQL_STATEMENT_NOT_YET_COMPLETE		MAKE_SQLSTATE('0','3', '0','0','0')

/* Class 08 - Connection Exception */
#define ERRCODE_CONNECTION_EXCEPTION		MAKE_SQLSTATE('0','8', '0','0','0')
#define ERRCODE_CONNECTION_DOES_NOT_EXIST	MAKE_SQLSTATE('0','8', '0','0','3')
#define ERRCODE_CONNECTION_FAILURE			MAKE_SQLSTATE('0','8', '0','0','6')
#define ERRCODE_SQLCLIENT_UNABLE_TO_ESTABLISH_SQLCONNECTION		MAKE_SQLSTATE('0','8', '0','0','1')
#define ERRCODE_SQLSERVER_REJECTED_ESTABLISHMENT_OF_SQLCONNECTION	MAKE_SQLSTATE('0','8', '0','0','4')
#define ERRCODE_TRANSACTION_RESOLUTION_UNKNOWN		MAKE_SQLSTATE('0','8', '0','0','7')
#define ERRCODE_PROTOCOL_VIOLATION			MAKE_SQLSTATE('0','8', 'P','0','1')

/* Class 09 - Triggered Action Exception */
#define ERRCODE_TRIGGERED_ACTION_EXCEPTION	MAKE_SQLSTATE('0','9', '0','0','0')

/* Class 0A - Feature Not Supported */
#define ERRCODE_FEATURE_NOT_SUPPORTED		MAKE_SQLSTATE('0','A', '0','0','0')

/* Class 0B - Invalid Transaction Initiation */
#define ERRCODE_INVALID_TRANSACTION_INITIATION		MAKE_SQLSTATE('0','B', '0','0','0')

/* Class 0F - Locator Exception */
#define ERRCODE_LOCATOR_EXCEPTION			MAKE_SQLSTATE('0','F', '0','0','0')
#define ERRCODE_L_E_INVALID_SPECIFICATION	MAKE_SQLSTATE('0','F', '0','0','1')

/* Class 0L - Invalid Grantor */
#define ERRCODE_INVALID_GRANTOR				MAKE_SQLSTATE('0','L', '0','0','0')
#define ERRCODE_INVALID_GRANT_OPERATION		MAKE_SQLSTATE('0','L', 'P','0','1')

/* Class 0P - Invalid Role Specification */
#define ERRCODE_INVALID_ROLE_SPECIFICATION	MAKE_SQLSTATE('0','P', '0','0','0')

/* Class 21 - Cardinality Violation */
/* (this means something returned the wrong number of rows) */
#define ERRCODE_CARDINALITY_VIOLATION		MAKE_SQLSTATE('2','1', '0','0','0')

/* Class 22 - Data Exception */
#define ERRCODE_DATA_EXCEPTION				MAKE_SQLSTATE('2','2', '0','0','0')
#define ERRCODE_ARRAY_ELEMENT_ERROR			MAKE_SQLSTATE('2','2', '0','2','E')
/* SQL99's actual definition of "array element error" is subscript error */
#define ERRCODE_ARRAY_SUBSCRIPT_ERROR		ERRCODE_ARRAY_ELEMENT_ERROR
#define ERRCODE_CHARACTER_NOT_IN_REPERTOIRE	MAKE_SQLSTATE('2','2', '0','2','1')
#define ERRCODE_DATETIME_FIELD_OVERFLOW		MAKE_SQLSTATE('2','2', '0','0','8')
#define ERRCODE_DIVISION_BY_ZERO			MAKE_SQLSTATE('2','2', '0','1','2')
#define ERRCODE_ERROR_IN_ASSIGNMENT			MAKE_SQLSTATE('2','2', '0','0','5')
#define ERRCODE_ESCAPE_CHARACTER_CONFLICT	MAKE_SQLSTATE('2','2', '0','0','B')
#define ERRCODE_INDICATOR_OVERFLOW			MAKE_SQLSTATE('2','2', '0','2','2')
#define ERRCODE_INTERVAL_FIELD_OVERFLOW		MAKE_SQLSTATE('2','2', '0','1','5')
#define ERRCODE_INVALID_CHARACTER_VALUE_FOR_CAST		MAKE_SQLSTATE('2','2', '0','1','8')
#define ERRCODE_INVALID_DATETIME_FORMAT		MAKE_SQLSTATE('2','2', '0','0','7')
#define ERRCODE_INVALID_ESCAPE_CHARACTER	MAKE_SQLSTATE('2','2', '0','1','9')
#define ERRCODE_INVALID_ESCAPE_OCTET		MAKE_SQLSTATE('2','2', '0','0','D')
#define ERRCODE_INVALID_ESCAPE_SEQUENCE		MAKE_SQLSTATE('2','2', '0','2','5')
#define ERRCODE_INVALID_INDICATOR_PARAMETER_VALUE		MAKE_SQLSTATE('2','2', '0','1','0')
#define ERRCODE_INVALID_LIMIT_VALUE			MAKE_SQLSTATE('2','2', '0','2','0')
#define ERRCODE_INVALID_PARAMETER_VALUE		MAKE_SQLSTATE('2','2', '0','2','3')
#define ERRCODE_INVALID_REGULAR_EXPRESSION	MAKE_SQLSTATE('2','2', '0','1','B')
#define ERRCODE_INVALID_TIME_ZONE_DISPLACEMENT_VALUE	MAKE_SQLSTATE('2','2', '0','0','9')
#define ERRCODE_INVALID_USE_OF_ESCAPE_CHARACTER		MAKE_SQLSTATE('2','2', '0','0','C')
#define ERRCODE_MOST_SPECIFIC_TYPE_MISMATCH	MAKE_SQLSTATE('2','2', '0','0','2')
#define ERRCODE_NULL_VALUE_NOT_ALLOWED		MAKE_SQLSTATE('2','2', '0','0','4')
#define ERRCODE_NULL_VALUE_NO_INDICATOR_PARAMETER	MAKE_SQLSTATE('2','2', '0','0','G')
#define ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE	MAKE_SQLSTATE('2','2', '0','0','3')
#define ERRCODE_STRING_DATA_LENGTH_MISMATCH	MAKE_SQLSTATE('2','2', '0','2','6')
#define ERRCODE_STRING_DATA_RIGHT_TRUNCATION		MAKE_SQLSTATE('2','2', '0','0','1')
#define ERRCODE_SUBSTRING_ERROR				MAKE_SQLSTATE('2','2', '0','1','1')
#define ERRCODE_TRIM_ERROR					MAKE_SQLSTATE('2','2', '0','2','7')
#define ERRCODE_UNTERMINATED_C_STRING		MAKE_SQLSTATE('2','2', '0','2','4')
#define ERRCODE_ZERO_LENGTH_CHARACTER_STRING		MAKE_SQLSTATE('2','2', '0','0','F')
#define ERRCODE_BAD_COPY_FILE_FORMAT		MAKE_SQLSTATE('2','2', 'P','0','1')
#define ERRCODE_INVALID_BINARY_REPRESENTATION	MAKE_SQLSTATE('2','2', 'P','0','2')
#define ERRCODE_FLOATING_POINT_EXCEPTION	MAKE_SQLSTATE('2','2', 'P','0','3')

/* Class 23 - Integrity Constraint Violation */
#define ERRCODE_INTEGRITY_CONSTRAINT_VIOLATION		MAKE_SQLSTATE('2','3', '0','0','0')
#define ERRCODE_RESTRICT_VIOLATION			MAKE_SQLSTATE('2','3', '0','0','1')
#define ERRCODE_NOT_NULL_VIOLATION			MAKE_SQLSTATE('2','3', '5','0','2')
#define ERRCODE_FOREIGN_KEY_VIOLATION		MAKE_SQLSTATE('2','3', '5','0','3')
#define ERRCODE_UNIQUE_VIOLATION			MAKE_SQLSTATE('2','3', '5','0','5')
#define ERRCODE_CHECK_VIOLATION				MAKE_SQLSTATE('2','3', '5','1','4')

/* Class 24 - Invalid Cursor State */
#define ERRCODE_INVALID_CURSOR_STATE		MAKE_SQLSTATE('2','4', '0','0','0')

/* Class 25 - Invalid Transaction State */
#define ERRCODE_INVALID_TRANSACTION_STATE	MAKE_SQLSTATE('2','5', '0','0','0')
#define ERRCODE_ACTIVE_SQL_TRANSACTION		MAKE_SQLSTATE('2','5', '0','0','1')
#define ERRCODE_BRANCH_TRANSACTION_ALREADY_ACTIVE	MAKE_SQLSTATE('2','5', '0','0','2')
#define ERRCODE_HELD_CURSOR_REQUIRES_SAME_ISOLATION_LEVEL	MAKE_SQLSTATE('2','5', '0','0','8')
#define ERRCODE_INAPPROPRIATE_ACCESS_MODE_FOR_BRANCH_TRANSACTION	MAKE_SQLSTATE('2','5', '0','0','3')
#define ERRCODE_INAPPROPRIATE_ISOLATION_LEVEL_FOR_BRANCH_TRANSACTION	MAKE_SQLSTATE('2','5', '0','0','4')
#define ERRCODE_NO_ACTIVE_SQL_TRANSACTION_FOR_BRANCH_TRANSACTION	MAKE_SQLSTATE('2','5', '0','0','5')
#define ERRCODE_READ_ONLY_SQL_TRANSACTION	MAKE_SQLSTATE('2','5', '0','0','6')
#define ERRCODE_SCHEMA_AND_DATA_STATEMENT_MIXING_NOT_SUPPORTED	MAKE_SQLSTATE('2','5', '0','0','7')
#define ERRCODE_NO_ACTIVE_SQL_TRANSACTION	MAKE_SQLSTATE('2','5', 'P','0','1')
#define ERRCODE_IN_FAILED_SQL_TRANSACTION	MAKE_SQLSTATE('2','5', 'P','0','2')

/* Class 26 - Invalid SQL Statement Name */
/* (we take this to mean prepared statements) */
#define ERRCODE_INVALID_SQL_STATEMENT_NAME	MAKE_SQLSTATE('2','6', '0','0','0')

/* Class 27 - Triggered Data Change Violation */
#define ERRCODE_TRIGGERED_DATA_CHANGE_VIOLATION		MAKE_SQLSTATE('2','7', '0','0','0')

/* Class 28 - Invalid Authorization Specification */
#define ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION	MAKE_SQLSTATE('2','8', '0','0','0')

/* Class 2B - Dependent Privilege Descriptors Still Exist */
#define ERRCODE_DEPENDENT_PRIVILEGE_DESCRIPTORS_STILL_EXIST		MAKE_SQLSTATE('2','B', '0','0','0')
#define ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST		MAKE_SQLSTATE('2','B', 'P','0','1')

/* Class 2D - Invalid Transaction Termination */
#define ERRCODE_INVALID_TRANSACTION_TERMINATION		MAKE_SQLSTATE('2','D', '0','0','0')

/* Class 2F - SQL Routine Exception */
#define ERRCODE_SQL_ROUTINE_EXCEPTION		MAKE_SQLSTATE('2','F', '0','0','0')
#define ERRCODE_S_R_E_FUNCTION_EXECUTED_NO_RETURN_STATEMENT	MAKE_SQLSTATE('2','F', '0','0','5')
#define ERRCODE_S_R_E_MODIFYING_SQL_DATA_NOT_PERMITTED		MAKE_SQLSTATE('2','F', '0','0','2')
#define ERRCODE_S_R_E_PROHIBITED_SQL_STATEMENT_ATTEMPTED	MAKE_SQLSTATE('2','F', '0','0','3')
#define ERRCODE_S_R_E_READING_SQL_DATA_NOT_PERMITTED		MAKE_SQLSTATE('2','F', '0','0','4')

/* Class 34 - Invalid Cursor Name */
#define ERRCODE_INVALID_CURSOR_NAME			MAKE_SQLSTATE('3','4', '0','0','0')

/* Class 38 - External Routine Exception */
#define ERRCODE_EXTERNAL_ROUTINE_EXCEPTION	MAKE_SQLSTATE('3','8', '0','0','0')
#define ERRCODE_E_R_E_CONTAINING_SQL_NOT_PERMITTED	MAKE_SQLSTATE('3','8', '0','0','1')
#define ERRCODE_E_R_E_MODIFYING_SQL_DATA_NOT_PERMITTED	MAKE_SQLSTATE('3','8', '0','0','2')
#define ERRCODE_E_R_E_PROHIBITED_SQL_STATEMENT_ATTEMPTED	MAKE_SQLSTATE('3','8', '0','0','3')
#define ERRCODE_E_R_E_READING_SQL_DATA_NOT_PERMITTED	MAKE_SQLSTATE('3','8', '0','0','4')

/* Class 39 - External Routine Invocation Exception */
#define ERRCODE_EXTERNAL_ROUTINE_INVOCATION_EXCEPTION	MAKE_SQLSTATE('3','9', '0','0','0')
#define ERRCODE_E_R_I_E_INVALID_SQLSTATE_RETURNED	MAKE_SQLSTATE('3','9', '0','0','1')
#define ERRCODE_E_R_I_E_NULL_VALUE_NOT_ALLOWED	MAKE_SQLSTATE('3','9', '0','0','4')
#define ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED	MAKE_SQLSTATE('3','9', 'P','0','1')
#define ERRCODE_E_R_I_E_SRF_PROTOCOL_VIOLATED	MAKE_SQLSTATE('3','9', 'P','0','2')

/* Class 3D - Invalid Catalog Name */
#define ERRCODE_INVALID_CATALOG_NAME		MAKE_SQLSTATE('3','D', '0','0','0')

/* Class 3F - Invalid Schema Name */
#define ERRCODE_INVALID_SCHEMA_NAME			MAKE_SQLSTATE('3','F', '0','0','0')

/* Class 40 - Transaction Rollback */
#define ERRCODE_TRANSACTION_ROLLBACK		MAKE_SQLSTATE('4','0', '0','0','0')
#define ERRCODE_T_R_INTEGRITY_CONSTRAINT_VIOLATION	MAKE_SQLSTATE('4','0', '0','0','2')
#define ERRCODE_T_R_SERIALIZATION_FAILURE	MAKE_SQLSTATE('4','0', '0','0','1')
#define ERRCODE_T_R_STATEMENT_COMPLETION_UNKNOWN	MAKE_SQLSTATE('4','0', '0','0','3')

/* Class 42 - Syntax Error or Access Rule Violation */
#define ERRCODE_SYNTAX_ERROR_OR_ACCESS_RULE_VIOLATION		MAKE_SQLSTATE('4','2', '0','0','0')
/* never use the above; use one of these two if no specific code exists: */
#define ERRCODE_SYNTAX_ERROR				MAKE_SQLSTATE('4','2', '6','0','1')
#define ERRCODE_INSUFFICIENT_PRIVILEGE		MAKE_SQLSTATE('4','2', '5','0','1')
#define ERRCODE_CANNOT_COERCE				MAKE_SQLSTATE('4','2', '8','4','6')
#define ERRCODE_GROUPING_ERROR				MAKE_SQLSTATE('4','2', '8','0','3')
#define ERRCODE_INVALID_FOREIGN_KEY			MAKE_SQLSTATE('4','2', '8','3','0')
#define ERRCODE_INVALID_NAME				MAKE_SQLSTATE('4','2', '6','0','2')
#define ERRCODE_NAME_TOO_LONG				MAKE_SQLSTATE('4','2', '6','2','2')
#define ERRCODE_RESERVED_NAME				MAKE_SQLSTATE('4','2', '9','3','9')
#define ERRCODE_DATATYPE_MISMATCH			MAKE_SQLSTATE('4','2', '8','0','4')
#define ERRCODE_INDETERMINATE_DATATYPE		MAKE_SQLSTATE('4','2', 'P','1','8')
#define ERRCODE_WRONG_OBJECT_TYPE			MAKE_SQLSTATE('4','2', '8','0','9')
/*
 * Note: for ERRCODE purposes, we divide namable objects into these categories:
 * databases, schemas, prepared statements, cursors, tables, columns,
 * functions (including operators), and all else (lumped as "objects").
 * (The first four categories are mandated by the existence of separate
 * SQLSTATE classes for them in the spec; in this file, however, we group
 * the ERRCODE names with all the rest under class 42.)  Parameters are
 * sort-of-named objects and get their own ERRCODE.
 *
 * The same breakdown is used for "duplicate" and "ambiguous" complaints,
 * as well as complaints associated with incorrect declarations.
 */
#define ERRCODE_UNDEFINED_COLUMN			MAKE_SQLSTATE('4','2', '7','0','3')
#define ERRCODE_UNDEFINED_CURSOR			ERRCODE_INVALID_CURSOR_NAME
#define ERRCODE_UNDEFINED_DATABASE			ERRCODE_INVALID_CATALOG_NAME
#define ERRCODE_UNDEFINED_FUNCTION			MAKE_SQLSTATE('4','2', '8','8','3')
#define ERRCODE_UNDEFINED_PSTATEMENT		ERRCODE_INVALID_SQL_STATEMENT_NAME
#define ERRCODE_UNDEFINED_SCHEMA			ERRCODE_INVALID_SCHEMA_NAME
#define ERRCODE_UNDEFINED_TABLE				MAKE_SQLSTATE('4','2', 'P','0','1')
#define ERRCODE_UNDEFINED_PARAMETER			MAKE_SQLSTATE('4','2', 'P','0','2')
#define ERRCODE_UNDEFINED_OBJECT			MAKE_SQLSTATE('4','2', '7','0','4')
#define ERRCODE_DUPLICATE_COLUMN			MAKE_SQLSTATE('4','2', '7','0','1')
#define ERRCODE_DUPLICATE_CURSOR			MAKE_SQLSTATE('4','2', 'P','0','3')
#define ERRCODE_DUPLICATE_DATABASE			MAKE_SQLSTATE('4','2', 'P','0','4')
#define ERRCODE_DUPLICATE_FUNCTION			MAKE_SQLSTATE('4','2', '7','2','3')
#define ERRCODE_DUPLICATE_PSTATEMENT		MAKE_SQLSTATE('4','2', 'P','0','5')
#define ERRCODE_DUPLICATE_SCHEMA			MAKE_SQLSTATE('4','2', 'P','0','6')
#define ERRCODE_DUPLICATE_TABLE				MAKE_SQLSTATE('4','2', 'P','0','7')
#define ERRCODE_DUPLICATE_ALIAS				MAKE_SQLSTATE('4','2', '7','1','2')
#define ERRCODE_DUPLICATE_OBJECT			MAKE_SQLSTATE('4','2', '7','1','0')
#define ERRCODE_AMBIGUOUS_COLUMN			MAKE_SQLSTATE('4','2', '7','0','2')
#define ERRCODE_AMBIGUOUS_FUNCTION			MAKE_SQLSTATE('4','2', '7','2','5')
#define ERRCODE_AMBIGUOUS_PARAMETER			MAKE_SQLSTATE('4','2', 'P','0','8')
#define ERRCODE_AMBIGUOUS_ALIAS				MAKE_SQLSTATE('4','2', 'P','0','9')
#define ERRCODE_INVALID_COLUMN_REFERENCE	MAKE_SQLSTATE('4','2', 'P','1','0')
#define ERRCODE_INVALID_COLUMN_DEFINITION	MAKE_SQLSTATE('4','2', '6','1','1')
#define ERRCODE_INVALID_CURSOR_DEFINITION	MAKE_SQLSTATE('4','2', 'P','1','1')
#define ERRCODE_INVALID_DATABASE_DEFINITION	MAKE_SQLSTATE('4','2', 'P','1','2')
#define ERRCODE_INVALID_FUNCTION_DEFINITION	MAKE_SQLSTATE('4','2', 'P','1','3')
#define ERRCODE_INVALID_PSTATEMENT_DEFINITION	MAKE_SQLSTATE('4','2', 'P','1','4')
#define ERRCODE_INVALID_SCHEMA_DEFINITION	MAKE_SQLSTATE('4','2', 'P','1','5')
#define ERRCODE_INVALID_TABLE_DEFINITION	MAKE_SQLSTATE('4','2', 'P','1','6')
#define ERRCODE_INVALID_OBJECT_DEFINITION	MAKE_SQLSTATE('4','2', 'P','1','7')

/* Class 44 - WITH CHECK OPTION Violation */
#define ERRCODE_WITH_CHECK_OPTION_VIOLATION	MAKE_SQLSTATE('4','4', '0','0','0')

/* Class 53 - Insufficient Resources (PostgreSQL-specific error class) */
#define ERRCODE_INSUFFICIENT_RESOURCES		MAKE_SQLSTATE('5','3', '0','0','0')
#define ERRCODE_DISK_FULL					MAKE_SQLSTATE('5','3', '1','0','0')
#define ERRCODE_OUT_OF_MEMORY				MAKE_SQLSTATE('5','3', '2','0','0')
#define ERRCODE_TOO_MANY_CONNECTIONS		MAKE_SQLSTATE('5','3', '3','0','0')

/* Class 54 - Program Limit Exceeded (class borrowed from DB2) */
/* (this is for wired-in limits, not resource exhaustion problems) */
#define ERRCODE_PROGRAM_LIMIT_EXCEEDED		MAKE_SQLSTATE('5','4', '0','0','0')
#define ERRCODE_STATEMENT_TOO_COMPLEX		MAKE_SQLSTATE('5','4', '0','0','1')
#define ERRCODE_TOO_MANY_COLUMNS			MAKE_SQLSTATE('5','4', '0','1','1')
#define ERRCODE_TOO_MANY_ARGUMENTS			MAKE_SQLSTATE('5','4', '0','2','3')

/* Class 55 - Object Not In Prerequisite State (class borrowed from DB2) */
#define ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE	MAKE_SQLSTATE('5','5', '0','0','0')
#define ERRCODE_OBJECT_IN_USE				MAKE_SQLSTATE('5','5', '0','0','6')
#define ERRCODE_INDEXES_DEACTIVATED			MAKE_SQLSTATE('5','5', 'P','0','1')
#define ERRCODE_INDEX_CORRUPTED				MAKE_SQLSTATE('5','5', 'P','0','2')

/* Class 57 - Operator Intervention (class borrowed from DB2) */
#define ERRCODE_OPERATOR_INTERVENTION		MAKE_SQLSTATE('5','7', '0','0','0')
#define ERRCODE_QUERY_CANCELED				MAKE_SQLSTATE('5','7', '0','1','4')
#define ERRCODE_ADMIN_SHUTDOWN				MAKE_SQLSTATE('5','7', 'P','0','1')
#define ERRCODE_CRASH_SHUTDOWN				MAKE_SQLSTATE('5','7', 'P','0','2')
#define ERRCODE_CANNOT_CONNECT_NOW			MAKE_SQLSTATE('5','7', 'P','0','3')

/* Class 58 - System Error (class borrowed from DB2) */
/* (we define this as errors external to PostgreSQL itself) */
#define ERRCODE_IO_ERROR					MAKE_SQLSTATE('5','8', '0','3','0')

/* Class F0 - Configuration File Error (PostgreSQL-specific error class) */
#define ERRCODE_CONFIG_FILE_ERROR			MAKE_SQLSTATE('F','0', '0','0','0')

/* Class XX - Internal Error (PostgreSQL-specific error class) */
/* (this is for "can't-happen" conditions and software bugs) */
#define ERRCODE_INTERNAL_ERROR				MAKE_SQLSTATE('X','X', '0','0','0')


/* Which __func__ symbol do we have, if any? */
#ifdef HAVE_FUNCNAME__FUNC
#define PG_FUNCNAME_MACRO	__func__
#else
#ifdef HAVE_FUNCNAME__FUNCTION
#define PG_FUNCNAME_MACRO	__FUNCTION__
#else
#define PG_FUNCNAME_MACRO	NULL
#endif
#endif


/*----------
 * New-style error reporting API: to be used in this way:
 *		ereport(ERROR,
 *				(errcode(ERRCODE_UNDEFINED_CURSOR),
 *				 errmsg("portal \"%s\" not found", stmt->portalname),
 *				 ... other errxxx() fields as needed ...));
 *
 * The error level is required, and so is a primary error message (errmsg
 * or errmsg_internal).  All else is optional.  errcode() defaults to
 * ERRCODE_INTERNAL_ERROR if elevel is ERROR or more, ERRCODE_WARNING
 * if elevel is WARNING, or ERRCODE_SUCCESSFUL_COMPLETION if elevel is
 * NOTICE or below.
 *----------
 */
#define ereport(elevel, rest)  \
	(errstart(elevel, __FILE__, __LINE__, PG_FUNCNAME_MACRO) ? \
	 (errfinish rest) : (void) 0)

extern bool errstart(int elevel, const char *filename, int lineno,
					 const char *funcname);
extern void errfinish(int dummy, ...);

extern int errcode(int sqlerrcode);

extern int errcode_for_file_access(void);
extern int errcode_for_socket_access(void);

extern int errmsg(const char *fmt, ...)
/* This extension allows gcc to check the format string for consistency with
   the supplied arguments. */
__attribute__((format(printf, 1, 2)));

extern int errmsg_internal(const char *fmt, ...)
/* This extension allows gcc to check the format string for consistency with
   the supplied arguments. */
__attribute__((format(printf, 1, 2)));

extern int errdetail(const char *fmt, ...)
/* This extension allows gcc to check the format string for consistency with
   the supplied arguments. */
__attribute__((format(printf, 1, 2)));

extern int errhint(const char *fmt, ...)
/* This extension allows gcc to check the format string for consistency with
   the supplied arguments. */
__attribute__((format(printf, 1, 2)));

extern int errcontext(const char *fmt, ...)
/* This extension allows gcc to check the format string for consistency with
   the supplied arguments. */
__attribute__((format(printf, 1, 2)));

extern int errfunction(const char *funcname);
extern int errposition(int cursorpos);


/*----------
 * Old-style error reporting API: to be used in this way:
 *		elog(ERROR, "portal \"%s\" not found", stmt->portalname);
 *----------
 */
#define elog    errstart(ERROR, __FILE__, __LINE__, PG_FUNCNAME_MACRO), elog_finish

extern void
elog_finish(int elevel, const char *fmt, ...)
/* This extension allows gcc to check the format string for consistency with
   the supplied arguments. */
__attribute__((format(printf, 2, 3)));


/* Support for attaching context information to error reports */

typedef struct ErrorContextCallback
{
	struct ErrorContextCallback *previous;
	void (*callback) (void *arg);
	void *arg;
} ErrorContextCallback;

extern DLLIMPORT ErrorContextCallback *error_context_stack;


/* GUC-configurable parameters */

typedef enum
{
	PGERROR_TERSE,				/* single-line error messages */
	PGERROR_DEFAULT,			/* recommended style */
	PGERROR_VERBOSE				/* all the facts, ma'am */
} PGErrorVerbosity;

extern PGErrorVerbosity Log_error_verbosity;
extern bool Log_timestamp;
extern bool Log_pid;
#ifdef HAVE_SYSLOG
extern int	Use_syslog;
#endif


/* Other exported functions */
extern void DebugFileOpen(void);

#endif   /* ELOG_H */
