/*-------------------------------------------------------------------------
 *
 * elog.h
 *	  POSTGRES error logging definitions.
 *
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: elog.h,v 1.42 2003/05/15 16:35:29 momjian Exp $
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
#define DEBUG1		14
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
#define ERROR		20			/* user error - abort transaction; return
								 * to known state */
/* Save ERROR value in PGERROR so it can bve restored when Win32 includes
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


/* SQLSTATE codes defined by SQL99 */
#define ERRCODE_AMBIGUOUS_CURSOR_NAME		MAKE_SQLSTATE('3','C', '0','0','0')
#define ERRCODE_CARDINALITY_VIOLATION		MAKE_SQLSTATE('2','1', '0','0','0')
#define ERRCODE_CLI_SPECIFIC_CONDITION		MAKE_SQLSTATE('H','Y', '0','0','0')
#define ERRCODE_CONNECTION_EXCEPTION		MAKE_SQLSTATE('0','8', '0','0','0')
#define ERRCODE_CONNECTION_DOES_NOT_EXIST	MAKE_SQLSTATE('0','8', '0','0','3')
#define ERRCODE_CONNECTION_FAILURE			MAKE_SQLSTATE('0','8', '0','0','6')
#define ERRCODE_CONNECTION_NAME_IN_USE		MAKE_SQLSTATE('0','8', '0','0','2')
#define ERRCODE_SQLCLIENT_UNABLE_TO_ESTABLISH_SQLCONNECTION		MAKE_SQLSTATE('0','8', '0','0','1')
#define ERRCODE_SQLSERVER_REJECTED_ESTABLISHMENT_OF_SQLCONNECTION	MAKE_SQLSTATE('0','8', '0','0','4')
#define ERRCODE_TRANSACTION_RESOLUTION_UNKNOWN		MAKE_SQLSTATE('0','8', '0','0','7')
#define ERRCODE_DATA_EXCEPTION				MAKE_SQLSTATE('2','2', '0','0','0')
#define ERRCODE_ARRAY_ELEMENT_ERROR			MAKE_SQLSTATE('2','2', '0','2','E')
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
#define ERRCODE_NULL_VALUE_NO_INDICATOR_PARAMETER	MAKE_SQLSTATE('2','2', '0','0','G')
#define ERRCODE_MOST_SPECIFIC_TYPE_MISMATCH	MAKE_SQLSTATE('2','2', '0','0','2')
#define ERRCODE_NULL_VALUE_NOT_ALLOWED		MAKE_SQLSTATE('2','2', '0','0','4')
#define ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE	MAKE_SQLSTATE('2','2', '0','0','3')
#define ERRCODE_STRING_DATA_LENGTH_MISMATCH	MAKE_SQLSTATE('2','2', '0','2','6')
#define ERRCODE_STRING_DATA_RIGHT_TRUNCATION		MAKE_SQLSTATE('2','2', '0','0','1')
#define ERRCODE_SUBSTRING_ERROR				MAKE_SQLSTATE('2','2', '0','1','1')
#define ERRCODE_TRIM_ERROR					MAKE_SQLSTATE('2','2', '0','2','7')
#define ERRCODE_UNTERMINATED_C_STRING		MAKE_SQLSTATE('2','2', '0','2','4')
#define ERRCODE_ZERO_LENGTH_CHARACTER_STRING		MAKE_SQLSTATE('2','2', '0','0','F')
#define ERRCODE_DEPENDENT_PRIVILEGE_DESCRIPTORS_STILL_EXIST		MAKE_SQLSTATE('2','B', '0','0','0')
#define ERRCODE_EXTERNAL_ROUTINE_EXCEPTION	MAKE_SQLSTATE('3','8', '0','0','0')
#define ERRCODE_EXTERNAL_ROUTINE_EXCEPTION_CONTAINING_SQL_NOT_PERMITTED	MAKE_SQLSTATE('3','8', '0','0','1')
#define ERRCODE_EXTERNAL_ROUTINE_EXCEPTION_MODIFYING_SQL_DATA_NOT_PERMITTED	MAKE_SQLSTATE('3','8', '0','0','2')
#define ERRCODE_EXTERNAL_ROUTINE_EXCEPTION_PROHIBITED_SQL_STATEMENT_ATTEMPTED	MAKE_SQLSTATE('3','8', '0','0','3')
#define ERRCODE_EXTERNAL_ROUTINE_EXCEPTION_READING_SQL_DATA_NOT_PERMITTED	MAKE_SQLSTATE('3','8', '0','0','4')
#define ERRCODE_EXTERNAL_ROUTINE_INVOCATION_EXCEPTION	MAKE_SQLSTATE('3','9', '0','0','0')
#define ERRCODE_EXTERNAL_ROUTINE_INVOCATION_EXCEPTION_INVALID_SQLSTATE_RETURNED	MAKE_SQLSTATE('3','9', '0','0','1')
#define ERRCODE_EXTERNAL_ROUTINE_INVOCATION_EXCEPTION_NULL_VALUE_NOT_ALLOWED	MAKE_SQLSTATE('3','9', '0','0','4')
#define ERRCODE_FEATURE_NOT_SUPPORTED		MAKE_SQLSTATE('0','A', '0','0','0')
#define ERRCODE_MULTIPLE_ENVIRONMENT_TRANSACTIONS	MAKE_SQLSTATE('0','A', '0','0','1')
#define ERRCODE_INTEGRITY_CONSTRAINT_VIOLATION		MAKE_SQLSTATE('2','3', '0','0','0')
#define ERRCODE_RESTRICT_VIOLATION			MAKE_SQLSTATE('2','3', '0','0','1')
#define ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION	MAKE_SQLSTATE('2','8', '0','0','0')
#define ERRCODE_INVALID_CATALOG_NAME		MAKE_SQLSTATE('3','D', '0','0','0')
#define ERRCODE_INVALID_CONDITION_NUMBER	MAKE_SQLSTATE('3','5', '0','0','0')
#define ERRCODE_INVALID_CONNECTION_NAME		MAKE_SQLSTATE('2','E', '0','0','0')
#define ERRCODE_INVALID_CURSOR_NAME			MAKE_SQLSTATE('3','4', '0','0','0')
#define ERRCODE_INVALID_CURSOR_STATE		MAKE_SQLSTATE('2','4', '0','0','0')
#define ERRCODE_INVALID_GRANTOR_STATE		MAKE_SQLSTATE('0','L', '0','0','0')
#define ERRCODE_INVALID_ROLE_SPECIFICATION	MAKE_SQLSTATE('0','P', '0','0','0')
#define ERRCODE_INVALID_SCHEMA_NAME			MAKE_SQLSTATE('3','F', '0','0','0')
#define ERRCODE_INVALID_SQL_DESCRIPTOR_NAME	MAKE_SQLSTATE('3','3', '0','0','0')
#define ERRCODE_INVALID_SQL_STATEMENT		MAKE_SQLSTATE('3','0', '0','0','0')
#define ERRCODE_INVALID_SQL_STATEMENT_NAME	MAKE_SQLSTATE('2','6', '0','0','0')
#define ERRCODE_INVALID_TARGET_SPECIFICATION_VALUE	MAKE_SQLSTATE('3','1', '0','0','0')
#define ERRCODE_INVALID_TRANSACTION_STATE	MAKE_SQLSTATE('2','5', '0','0','0')
#define ERRCODE_ACTIVE_SQL_TRANSACTION		MAKE_SQLSTATE('2','5', '0','0','1')
#define ERRCODE_BRANCH_TRANSACTION_ALREADY_ACTIVE	MAKE_SQLSTATE('2','5', '0','0','2')
#define ERRCODE_HELD_CURSOR_REQUIRES_SAME_ISOLATION_LEVEL	MAKE_SQLSTATE('2','5', '0','0','8')
#define ERRCODE_INAPPROPRIATE_ACCESS_MODE_FOR_BRANCH_TRANSACTION	MAKE_SQLSTATE('2','5', '0','0','3')
#define ERRCODE_INAPPROPRIATE_ISOLATION_LEVEL_FOR_BRANCH_TRANSACTION	MAKE_SQLSTATE('2','5', '0','0','4')
#define ERRCODE_NO_ACTIVE_SQL_TRANSACTION_FOR_BRANCH_TRANSACTION	MAKE_SQLSTATE('2','5', '0','0','5')
#define ERRCODE_READ_ONLY_SQL_TRANSACTION	MAKE_SQLSTATE('2','5', '0','0','6')
#define ERRCODE_SCHEMA_AND_DATA_STATEMENT_MIXING_NOT_SUPPORTED	MAKE_SQLSTATE('2','5', '0','0','7')
#define ERRCODE_INVALID_TRANSACTION_INITIATION		MAKE_SQLSTATE('0','B', '0','0','0')
#define ERRCODE_INVALID_TRANSACTION_TERMINATION		MAKE_SQLSTATE('2','D', '0','0','0')
#define ERRCODE_LOCATOR_EXCEPTION			MAKE_SQLSTATE('0','F', '0','0','0')
#define ERRCODE_LOCATOR_EXCEPTION_INVALID_SPECIFICATION		MAKE_SQLSTATE('0','F', '0','0','1')
#define ERRCODE_NO_DATA						MAKE_SQLSTATE('0','2', '0','0','0')
#define ERRCODE_NO_ADDITIONAL_DYNAMIC_RESULT_SETS_RETURNED	MAKE_SQLSTATE('0','2', '0','0','1')
#define ERRCODE_REMOTE_DATABASE_ACCESS		MAKE_SQLSTATE('H','Z', '0','0','0')
#define ERRCODE_SAVEPOINT_EXCEPTION			MAKE_SQLSTATE('3','B', '0','0','0')
#define ERRCODE_SAVEPOINT_EXCEPTION_INVALID_SPECIFICATION	MAKE_SQLSTATE('3','B', '0','0','1')
#define ERRCODE_SAVEPOINT_EXCEPTION_TOO_MANY		MAKE_SQLSTATE('3','B', '0','0','2')
#define ERRCODE_SQL_ROUTINE_EXCEPTION		MAKE_SQLSTATE('2','F', '0','0','0')
#define ERRCODE_FUNCTION_EXECUTED_NO_RETURN_STATEMENT		MAKE_SQLSTATE('2','F', '0','0','5')
#define ERRCODE_MODIFYING_SQL_DATA_NOT_PERMITTED	MAKE_SQLSTATE('2','F', '0','0','2')
#define ERRCODE_PROHIBITED_SQL_STATEMENT_ATTEMPTED	MAKE_SQLSTATE('2','F', '0','0','3')
#define ERRCODE_READING_SQL_DATA_NOT_PERMITTED		MAKE_SQLSTATE('2','F', '0','0','4')
#define ERRCODE_SQL_STATEMENT_NOT_YET_COMPLETE		MAKE_SQLSTATE('0','3', '0','0','0')
#define ERRCODE_SUCCESSFUL_COMPLETION		MAKE_SQLSTATE('0','0', '0','0','0')
#define ERRCODE_SYNTAX_ERROR_OR_ACCESS_RULE_VIOLATION		MAKE_SQLSTATE('4','2', '0','0','0')
#define ERRCODE_SYNTAX_ERROR_OR_ACCESS_RULE_VIOLATION_IN_DIRECT_STATEMENT	MAKE_SQLSTATE('2','A', '0','0','0')
#define ERRCODE_SYNTAX_ERROR_OR_ACCESS_RULE_VIOLATION_IN_DYNAMIC_STATEMENT	MAKE_SQLSTATE('3','7', '0','0','0')
#define ERRCODE_TRANSACTION_ROLLBACK		MAKE_SQLSTATE('4','0', '0','0','0')
#define ERRCODE_TRANSACTION_ROLLBACK_INTEGRITY_CONSTRAINT_VIOLATION	MAKE_SQLSTATE('4','0', '0','0','2')
#define ERRCODE_TRANSACTION_ROLLBACK_SERIALIZATION_FAILURE	MAKE_SQLSTATE('4','0', '0','0','1')
#define ERRCODE_TRANSACTION_ROLLBACK_STATEMENT_COMPLETION_UNKNOWN	MAKE_SQLSTATE('4','0', '0','0','3')
#define ERRCODE_TRIGGERED_DATA_CHANGE_VIOLATION		MAKE_SQLSTATE('2','7', '0','0','0')
#define ERRCODE_WARNING						MAKE_SQLSTATE('0','1', '0','0','0')
#define ERRCODE_CURSOR_OPERATION_CONFLICT	MAKE_SQLSTATE('0','1', '0','0','1')
#define ERRCODE_DISCONNECT_ERROR			MAKE_SQLSTATE('0','1', '0','0','2')
#define ERRCODE_DYNAMIC_RESULT_SETS_RETURNED		MAKE_SQLSTATE('0','1', '0','0','C')
#define ERRCODE_IMPLICIT_ZERO_BIT_PADDING	MAKE_SQLSTATE('0','1', '0','0','8')
#define ERRCODE_NULL_VALUE_ELIMINATED_IN_SET_FUNCTION	MAKE_SQLSTATE('0','1', '0','0','3')
#define ERRCODE_PRIVILEGE_NOT_GRANTED		MAKE_SQLSTATE('0','1', '0','0','7')
#define ERRCODE_PRIVILEGE_NOT_REVOKED		MAKE_SQLSTATE('0','1', '0','0','6')
#define ERRCODE_QUERY_EXPRESSION_TOO_LONG_FOR_INFORMATION_SCHEMA	MAKE_SQLSTATE('0','1', '0','0','A')
#define ERRCODE_SEARCH_CONDITION_TOO_LONG_FOR_INFORMATION_SCHEMA	MAKE_SQLSTATE('0','1', '0','0','9')
#define ERRCODE_STATEMENT_TOO_LONG_FOR_INFORMATION_SCHEMA	MAKE_SQLSTATE('0','1', '0','0','5')
#define ERRCODE_STRING_DATA_RIGHT_TRUNCATION_WARNING	MAKE_SQLSTATE('0','1', '0','0','4')
#define ERRCODE_WITH_CHECK_OPTION_VIOLATION	MAKE_SQLSTATE('4','4', '0','0','0')

/* Implementation-defined error codes for PostgreSQL */
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
 *				(errcode(ERRCODE_INVALID_CURSOR_NAME),
 *				 errmsg("portal \"%s\" not found", stmt->portalname),
 *				 ... other errxxx() fields as needed ...));
 *
 * The error level is required, and so is a primary error message (errmsg
 * or errmsg_internal).  All else is optional.  errcode() defaults to
 * ERRCODE_INTERNAL_ERROR.
 *----------
 */
#define ereport(elevel, rest)  \
	(errstart(elevel, __FILE__, __LINE__, PG_FUNCNAME_MACRO) ? \
	 (errfinish rest) : (void) 0)

extern bool errstart(int elevel, const char *filename, int lineno,
					 const char *funcname);
extern void errfinish(int dummy, ...);

extern int errcode(int sqlerrcode);

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

extern ErrorContextCallback *error_context_stack;


/* GUC-configurable parameters */
extern bool Log_timestamp;
extern bool Log_pid;
#ifdef HAVE_SYSLOG
extern int	Use_syslog;
#endif


/* Other exported functions */
extern void DebugFileOpen(void);

#endif   /* ELOG_H */
