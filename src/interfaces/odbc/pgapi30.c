/*-------
 * Module:			pgapi30.c
 *
 * Description:		This module contains routines related to ODBC 3.0
 *			most of their implementations are temporary
 *			and must be rewritten properly.
 *			2001/07/23	inoue
 *
 * Classes:			n/a
 *
 * API functions:	PGAPI_ColAttribute, PGAPI_GetDiagRec,
			PGAPI_GetConnectAttr, PGAPI_GetStmtAttr,
			PGAPI_SetConnectAttr, PGAPI_SetStmtAttr
 *-------
 */

#ifndef ODBCVER
#define ODBCVER 0x0300
#endif
#include "psqlodbc.h"
#include <stdio.h>
#include <string.h>

#include "environ.h"
#include "connection.h"
#include "statement.h"
#include "descriptor.h"
#include "pgapifunc.h"

static HSTMT statementHandleFromDescHandle(HSTMT, SQLINTEGER *descType); 
/*	SQLError -> SQLDiagRec */
RETCODE		SQL_API
PGAPI_GetDiagRec(SQLSMALLINT HandleType, SQLHANDLE Handle,
		SQLSMALLINT RecNumber, SQLCHAR *Sqlstate,
		SQLINTEGER *NativeError, SQLCHAR *MessageText,
		SQLSMALLINT BufferLength, SQLSMALLINT *TextLength)
{
	RETCODE		ret;
	static const char *func = "PGAPI_GetDiagRec";

	mylog("%s entering rec=%d", func, RecNumber);
	switch (HandleType)
	{
		case SQL_HANDLE_ENV:
			ret = PGAPI_EnvError(Handle, RecNumber, Sqlstate,
					NativeError, MessageText,
					BufferLength, TextLength, 0);
			break;
		case SQL_HANDLE_DBC:
			ret = PGAPI_ConnectError(Handle, RecNumber, Sqlstate,
					NativeError, MessageText, BufferLength,
					TextLength, 0);
			break;
		case SQL_HANDLE_STMT:
			ret = PGAPI_StmtError(Handle, RecNumber, Sqlstate,
					NativeError, MessageText, BufferLength,
					TextLength, 0);
			break;
		case SQL_HANDLE_DESC:
			ret = PGAPI_StmtError(statementHandleFromDescHandle(Handle, NULL),
					RecNumber, Sqlstate, NativeError,
					MessageText, BufferLength,
					TextLength, 0);
			break;
		default:
			ret = SQL_ERROR;
	}
	mylog("%s exiting %d\n", func, ret);
	return ret;
}

RETCODE		SQL_API
PGAPI_GetDiagField(SQLSMALLINT HandleType, SQLHANDLE Handle,
		SQLSMALLINT RecNumber, SQLSMALLINT DiagIdentifier,
		PTR DiagInfoPtr, SQLSMALLINT BufferLength,
		SQLSMALLINT *StringLengthPtr)
{
	RETCODE		ret = SQL_SUCCESS;
	static const char *func = "PGAPI_GetDiagField";

	mylog("%s entering rec=%d", func, RecNumber);
	switch (HandleType)
	{
		case SQL_HANDLE_ENV:
			switch (DiagIdentifier)
			{
				case SQL_DIAG_CLASS_ORIGIN:
				case SQL_DIAG_SUBCLASS_ORIGIN:
				case SQL_DIAG_CONNECTION_NAME:
				case SQL_DIAG_MESSAGE_TEXT:
				case SQL_DIAG_NATIVE:
				case SQL_DIAG_NUMBER:
				case SQL_DIAG_RETURNCODE:
				case SQL_DIAG_SERVER_NAME:
				case SQL_DIAG_SQLSTATE:
					break;
			}
			break;
		case SQL_HANDLE_DBC:
			switch (DiagIdentifier)
			{
				case SQL_DIAG_CLASS_ORIGIN:
				case SQL_DIAG_SUBCLASS_ORIGIN:
				case SQL_DIAG_CONNECTION_NAME:
				case SQL_DIAG_MESSAGE_TEXT:
				case SQL_DIAG_NATIVE:
				case SQL_DIAG_NUMBER:
				case SQL_DIAG_RETURNCODE:
				case SQL_DIAG_SERVER_NAME:
				case SQL_DIAG_SQLSTATE:
					break;
			}
			break;
		case SQL_HANDLE_STMT:
			switch (DiagIdentifier)
			{
				case SQL_DIAG_CLASS_ORIGIN:
				case SQL_DIAG_SUBCLASS_ORIGIN:
				case SQL_DIAG_CONNECTION_NAME:
				case SQL_DIAG_MESSAGE_TEXT:
				case SQL_DIAG_NATIVE:
				case SQL_DIAG_NUMBER:
				case SQL_DIAG_RETURNCODE:
				case SQL_DIAG_SERVER_NAME:
				case SQL_DIAG_SQLSTATE:
					break;
			}
			break;
		default:
			ret = SQL_ERROR;
	}
	mylog("%s exiting %d\n", func, ret);
	return ret;
}

/*	SQLGetConnectOption -> SQLGetconnectAttr */
RETCODE		SQL_API
PGAPI_GetConnectAttr(HDBC ConnectionHandle,
			SQLINTEGER Attribute, PTR Value,
			SQLINTEGER BufferLength, SQLINTEGER *StringLength)
{
	static const char *func = "PGAPI_GetConnectAttr";
	ConnectionClass *conn = (ConnectionClass *) ConnectionHandle;
	RETCODE	ret = SQL_SUCCESS;
	SQLINTEGER	len = 4;

	mylog("PGAPI_GetConnectAttr %d\n", Attribute);
	switch (Attribute)
	{
		case SQL_ATTR_ASYNC_ENABLE:
			*((SQLUINTEGER *) Value) = SQL_ASYNC_ENABLE_OFF;
			break;
		case SQL_ATTR_AUTO_IPD:
			*((SQLUINTEGER *) Value) = SQL_FALSE;
			break;
		case SQL_ATTR_CONNECTION_DEAD:
			*((SQLUINTEGER *) Value) = SQL_CD_FALSE;
			break;
		case SQL_ATTR_CONNECTION_TIMEOUT:
			*((SQLUINTEGER *) Value) = 0;
			break;
		case SQL_ATTR_METADATA_ID:
			conn->errornumber = STMT_INVALID_OPTION_IDENTIFIER;
			conn->errormsg = "Unsupported connect attribute (Get)";
			CC_log_error(func, "", conn);
			return SQL_ERROR;
		default:
			ret = PGAPI_GetConnectOption(ConnectionHandle, (UWORD) Attribute, Value);
	}
	if (StringLength)
		*StringLength = len;
	return ret;
}

static HSTMT
descHandleFromStatementHandle(HSTMT StatementHandle, SQLINTEGER descType) 
{
	switch (descType)
	{
		case SQL_ATTR_APP_ROW_DESC:		/* 10010 */
			return StatementHandle;	/* this is bogus */
		case SQL_ATTR_APP_PARAM_DESC:	/* 10011 */
			return (HSTMT) ((SQLUINTEGER) StatementHandle + 1) ; /* this is bogus */
		case SQL_ATTR_IMP_ROW_DESC:		/* 10012 */
			return (HSTMT) ((SQLUINTEGER) StatementHandle + 2); /* this is bogus */
		case SQL_ATTR_IMP_PARAM_DESC:	/* 10013 */
			return (HSTMT) ((SQLUINTEGER) StatementHandle + 3); /* this is bogus */
	}
	return (HSTMT) 0;
}
static HSTMT
statementHandleFromDescHandle(HSTMT DescHandle, SQLINTEGER *descType) 
{
	SQLUINTEGER res = (SQLUINTEGER) DescHandle % 4;
	if (descType)
	{
		switch (res)
		{
			case 0: *descType = SQL_ATTR_APP_ROW_DESC; /* 10010 */
				break;
			case 1: *descType = SQL_ATTR_APP_PARAM_DESC; /* 10011 */
				break;
			case 2: *descType = SQL_ATTR_IMP_ROW_DESC; /* 10012 */
				break;
			case 3: *descType = SQL_ATTR_IMP_PARAM_DESC; /* 10013 */
				break;
		}
	}
	return (HSTMT) ((SQLUINTEGER) DescHandle - res);
}

static  void column_bindings_set(ARDFields *opts, int cols, BOOL maxset)
{
	int	i;

	if (cols == opts->allocated)
		return;
	if (cols > opts->allocated)
	{
		extend_column_bindings(opts, cols);
		return;
	}
	if (maxset)	return;

	for (i = opts->allocated; i > cols; i--)
		reset_a_column_binding(opts, i);
	opts->allocated = cols;
	if (0 == cols)
	{
		free(opts->bindings);
		opts->bindings = NULL;
	}
}

static RETCODE SQL_API
ARDSetField(StatementClass *stmt, SQLSMALLINT RecNumber,
		SQLSMALLINT FieldIdentifier, PTR Value, SQLINTEGER BufferLength)
{
	RETCODE		ret = SQL_SUCCESS;
	PTR		tptr;
	ARDFields	*opts = SC_get_ARD(stmt);
	switch (FieldIdentifier)
	{
		case SQL_DESC_ARRAY_SIZE:
			opts->rowset_size = (SQLUINTEGER) Value;
			break; 
		case SQL_DESC_ARRAY_STATUS_PTR:
			opts->row_operation_ptr = Value;
			break;
		case SQL_DESC_BIND_OFFSET_PTR:
			opts->row_offset_ptr = Value;
			break;
		case SQL_DESC_BIND_TYPE:
			opts->bind_size = (SQLUINTEGER) Value;
			break;

		case SQL_DESC_TYPE:
			column_bindings_set(opts, RecNumber, TRUE);
			reset_a_column_binding(opts, RecNumber);
			opts->bindings[RecNumber - 1].returntype = (Int4) Value;
			break;
		case SQL_DESC_DATETIME_INTERVAL_CODE:
			column_bindings_set(opts, RecNumber, TRUE);
			switch (opts->bindings[RecNumber - 1].returntype)
			{
				case SQL_DATETIME:
				case SQL_C_TYPE_DATE:
				case SQL_C_TYPE_TIME:
				case SQL_C_TYPE_TIMESTAMP:
				switch ((Int4) Value)
				{
					case SQL_CODE_DATE:
						opts->bindings[RecNumber - 1].returntype = SQL_C_TYPE_DATE;
						break;
					case SQL_CODE_TIME:
						opts->bindings[RecNumber - 1].returntype = SQL_C_TYPE_TIME;
						break;
					case SQL_CODE_TIMESTAMP:
						opts->bindings[RecNumber - 1].returntype = SQL_C_TYPE_TIMESTAMP;
						break;
				}
				break;
			}
			break;
		case SQL_DESC_CONCISE_TYPE:
			column_bindings_set(opts, RecNumber, TRUE);
			opts->bindings[RecNumber - 1].returntype = (Int4) Value;
			break;
		case SQL_DESC_DATA_PTR:
			if (!RecNumber)
				opts->bookmark->buffer = Value;
			else
			{
				column_bindings_set(opts, RecNumber, TRUE);
				opts->bindings[RecNumber - 1].buffer = Value;
			}
			break;
		case SQL_DESC_INDICATOR_PTR:
			if (!RecNumber)
				tptr = opts->bookmark->used;
			else
			{
				column_bindings_set(opts, RecNumber, TRUE);
				tptr = opts->bindings[RecNumber - 1].used;
			}
			if (Value != tptr)
			{
				ret = SQL_ERROR;
				stmt->errornumber = STMT_INVALID_DESCRIPTOR_IDENTIFIER; 
				stmt->errormsg = "INDICATOR != OCTET_LENGTH_PTR"; 
			}
			break;
		case SQL_DESC_OCTET_LENGTH_PTR:
			if (!RecNumber)
				opts->bookmark->used = Value;
			else
			{
				column_bindings_set(opts, RecNumber, TRUE);
				opts->bindings[RecNumber - 1].used = Value;
			}
			break;
		case SQL_DESC_COUNT:
			column_bindings_set(opts, (SQLUINTEGER) Value, FALSE);
			break;
		case SQL_DESC_OCTET_LENGTH:
			if (RecNumber)
			{
				column_bindings_set(opts, RecNumber, TRUE);
				opts->bindings[RecNumber - 1].buflen = (Int4) Value;
			}
			break;
		case SQL_DESC_ALLOC_TYPE: /* read-only */
		case SQL_DESC_DATETIME_INTERVAL_PRECISION:
		case SQL_DESC_LENGTH:
		case SQL_DESC_NUM_PREC_RADIX:
		case SQL_DESC_PRECISION:
		case SQL_DESC_SCALE:
		default:ret = SQL_ERROR;
			stmt->errornumber = STMT_INVALID_DESCRIPTOR_IDENTIFIER; 
	}
	return ret;
}

static  void parameter_bindings_set(APDFields *opts, int params, BOOL maxset)
{
	int	i;

	if (params == opts->allocated)
		return;
	if (params > opts->allocated)
	{
		extend_parameter_bindings(opts, params);
		return;
	}
	if (maxset)	return;

	for (i = opts->allocated; i > params; i--)
		reset_a_parameter_binding(opts, i);
	opts->allocated = params;
	if (0 == params)
	{
		free(opts->parameters);
		opts->parameters = NULL;
	}
}

static RETCODE SQL_API
APDSetField(StatementClass *stmt, SQLSMALLINT RecNumber,
		SQLSMALLINT FieldIdentifier, PTR Value, SQLINTEGER BufferLength)
{
	RETCODE		ret = SQL_SUCCESS;
	APDFields	*opts = SC_get_APD(stmt);
	switch (FieldIdentifier)
	{
		case SQL_DESC_ARRAY_SIZE:
			opts->paramset_size = (SQLUINTEGER) Value;
			break; 
		case SQL_DESC_ARRAY_STATUS_PTR:
			opts->param_operation_ptr = Value;
			break;
		case SQL_DESC_BIND_OFFSET_PTR:
			opts->param_offset_ptr = Value;
			break;
		case SQL_DESC_BIND_TYPE:
			opts->param_bind_type = (SQLUINTEGER) Value;
			break;

		case SQL_DESC_TYPE:
			parameter_bindings_set(opts, RecNumber, TRUE);
			reset_a_parameter_binding(opts, RecNumber);
			opts->parameters[RecNumber - 1].CType = (Int4) Value;
			break;
		case SQL_DESC_DATETIME_INTERVAL_CODE:
			parameter_bindings_set(opts, RecNumber, TRUE);
			switch (opts->parameters[RecNumber - 1].CType)
			{
				case SQL_DATETIME:
				case SQL_C_TYPE_DATE:
				case SQL_C_TYPE_TIME:
				case SQL_C_TYPE_TIMESTAMP:
				switch ((Int4) Value)
				{
					case SQL_CODE_DATE:
						opts->parameters[RecNumber - 1].CType = SQL_C_TYPE_DATE;
						break;
					case SQL_CODE_TIME:
						opts->parameters[RecNumber - 1].CType = SQL_C_TYPE_TIME;
						break;
					case SQL_CODE_TIMESTAMP:
						opts->parameters[RecNumber - 1].CType = SQL_C_TYPE_TIMESTAMP;
						break;
				}
				break;
			}
			break;
		case SQL_DESC_CONCISE_TYPE:
			parameter_bindings_set(opts, RecNumber, TRUE);
			opts->parameters[RecNumber - 1].CType = (Int4) Value;
			break;
		case SQL_DESC_DATA_PTR:
			parameter_bindings_set(opts, RecNumber, TRUE);
			opts->parameters[RecNumber - 1].buffer = Value;
			break;
		case SQL_DESC_INDICATOR_PTR:
			if (opts->allocated < RecNumber ||
			    Value != opts->parameters[RecNumber - 1].used)
			{
				ret = SQL_ERROR;
				stmt->errornumber = STMT_INVALID_DESCRIPTOR_IDENTIFIER; 
				stmt->errormsg = "INDICATOR != OCTET_LENGTH_PTR"; 
			}
			break;
		case SQL_DESC_OCTET_LENGTH:
			parameter_bindings_set(opts, RecNumber, TRUE);
			opts->parameters[RecNumber - 1].buflen = (Int4) Value;
			break;
		case SQL_DESC_OCTET_LENGTH_PTR:
			parameter_bindings_set(opts, RecNumber, TRUE);
			opts->parameters[RecNumber - 1].used = Value;
			break;
		case SQL_DESC_COUNT:
			parameter_bindings_set(opts, (SQLUINTEGER) Value, FALSE);
			break; 
		case SQL_DESC_ALLOC_TYPE: /* read-only */
		case SQL_DESC_DATETIME_INTERVAL_PRECISION:
		case SQL_DESC_LENGTH:
		case SQL_DESC_NUM_PREC_RADIX:
		case SQL_DESC_PRECISION:
		case SQL_DESC_SCALE:
		default:ret = SQL_ERROR;
			stmt->errornumber = STMT_INVALID_DESCRIPTOR_IDENTIFIER; 
	}
	return ret;
}

static RETCODE SQL_API
IRDSetField(StatementClass *stmt, SQLSMALLINT RecNumber,
		SQLSMALLINT FieldIdentifier, PTR Value, SQLINTEGER BufferLength)
{
	RETCODE		ret = SQL_SUCCESS;
	IRDFields	*opts = SC_get_IRD(stmt);
	switch (FieldIdentifier)
	{
		case SQL_DESC_ARRAY_STATUS_PTR:
			opts->rowStatusArray = (SQLUSMALLINT *) Value;
			break;
		case SQL_DESC_ROWS_PROCESSED_PTR:
			opts->rowsFetched = (SQLUINTEGER *) Value;
			break;
		case SQL_DESC_ALLOC_TYPE: /* read-only */
		case SQL_DESC_COUNT: /* read-only */
		case SQL_DESC_AUTO_UNIQUE_VALUE: /* read-only */
		case SQL_DESC_BASE_COLUMN_NAME: /* read-only */
		case SQL_DESC_BASE_TABLE_NAME: /* read-only */
		case SQL_DESC_CASE_SENSITIVE: /* read-only */
		case SQL_DESC_CATALOG_NAME: /* read-only */
		case SQL_DESC_CONCISE_TYPE: /* read-only */
		case SQL_DESC_DATETIME_INTERVAL_CODE: /* read-only */
		case SQL_DESC_DATETIME_INTERVAL_PRECISION: /* read-only */
		case SQL_DESC_DISPLAY_SIZE: /* read-only */
		case SQL_DESC_FIXED_PREC_SCALE: /* read-only */
		case SQL_DESC_LABEL: /* read-only */
		case SQL_DESC_LENGTH: /* read-only */
		case SQL_DESC_LITERAL_PREFIX: /* read-only */
		case SQL_DESC_LITERAL_SUFFIX: /* read-only */
		case SQL_DESC_LOCAL_TYPE_NAME: /* read-only */
		case SQL_DESC_NAME: /* read-only */
		case SQL_DESC_NULLABLE: /* read-only */
		case SQL_DESC_NUM_PREC_RADIX: /* read-only */
		case SQL_DESC_OCTET_LENGTH: /* read-only */
		case SQL_DESC_PRECISION: /* read-only */
#if (ODBCVER >= 0x0350)
		case SQL_DESC_ROWVER: /* read-only */
#endif /* ODBCVER */
		case SQL_DESC_SCALE: /* read-only */
		case SQL_DESC_SCHEMA_NAME: /* read-only */
		case SQL_DESC_SEARCHABLE: /* read-only */
		case SQL_DESC_TABLE_NAME: /* read-only */
		case SQL_DESC_TYPE: /* read-only */
		case SQL_DESC_TYPE_NAME: /* read-only */
		case SQL_DESC_UNNAMED: /* read-only */
		case SQL_DESC_UNSIGNED: /* read-only */
		case SQL_DESC_UPDATABLE: /* read-only */
		default:ret = SQL_ERROR;
			stmt->errornumber = STMT_INVALID_DESCRIPTOR_IDENTIFIER; 
	}
	return ret;
}

static RETCODE SQL_API
IPDSetField(StatementClass *stmt, SQLSMALLINT RecNumber,
		SQLSMALLINT FieldIdentifier, PTR Value, SQLINTEGER BufferLength)
{
	RETCODE		ret = SQL_SUCCESS;
	IPDFields	*ipdopts = SC_get_IPD(stmt);
	APDFields	*apdopts = SC_get_APD(stmt);

	switch (FieldIdentifier)
	{
		case SQL_DESC_ARRAY_STATUS_PTR:
			ipdopts->param_status_ptr = (SQLUSMALLINT *) Value;
			break;
		case SQL_DESC_ROWS_PROCESSED_PTR:
			ipdopts->param_processed_ptr = (SQLUINTEGER *) Value;
			break;
		case SQL_DESC_UNNAMED: /* only SQL_UNNAMED is allowed */ 
			if (SQL_UNNAMED !=  (SQLUINTEGER) Value)
			{
				ret = SQL_ERROR;
				stmt->errornumber = STMT_INVALID_DESCRIPTOR_IDENTIFIER;
			}
			break;
		case SQL_DESC_TYPE:
			parameter_bindings_set(apdopts, RecNumber, TRUE);
			apdopts->parameters[RecNumber - 1].SQLType = (Int4) Value;
			break;
		case SQL_DESC_DATETIME_INTERVAL_CODE:
			parameter_bindings_set(apdopts, RecNumber, TRUE);
			switch (apdopts->parameters[RecNumber - 1].SQLType)
			{
				case SQL_DATETIME:
				case SQL_TYPE_DATE:
				case SQL_TYPE_TIME:
				case SQL_TYPE_TIMESTAMP:
				switch ((Int4) Value)
				{
					case SQL_CODE_DATE:
						apdopts->parameters[RecNumber - 1].SQLType = SQL_TYPE_DATE;
						break;
					case SQL_CODE_TIME:
						apdopts->parameters[RecNumber - 1].SQLType = SQL_TYPE_TIME;
						break;
					case SQL_CODE_TIMESTAMP:
						apdopts->parameters[RecNumber - 1].SQLType = SQL_TYPE_TIMESTAMP;
						break;
				}
				break;
			}
			break;
		case SQL_DESC_CONCISE_TYPE:
			parameter_bindings_set(apdopts, RecNumber, TRUE);
			apdopts->parameters[RecNumber - 1].SQLType = (Int4) Value;
			break;
		case SQL_DESC_COUNT:
			parameter_bindings_set(apdopts, (SQLUINTEGER) Value, FALSE);
			break; 
		case SQL_DESC_PARAMETER_TYPE:
			apdopts->parameters[RecNumber - 1].paramType = (Int2) Value;
			break;
		case SQL_DESC_SCALE:
			apdopts->parameters[RecNumber - 1].scale = (Int2) Value;
			break;
		case SQL_DESC_ALLOC_TYPE: /* read-only */ 
		case SQL_DESC_CASE_SENSITIVE: /* read-only */
		case SQL_DESC_DATETIME_INTERVAL_PRECISION:
		case SQL_DESC_FIXED_PREC_SCALE: /* read-only */
		case SQL_DESC_LENGTH:
		case SQL_DESC_LOCAL_TYPE_NAME: /* read-only */
		case SQL_DESC_NAME:
		case SQL_DESC_NULLABLE: /* read-only */
		case SQL_DESC_NUM_PREC_RADIX:
		case SQL_DESC_OCTET_LENGTH:
		case SQL_DESC_PRECISION:
#if (ODBCVER >= 0x0350)
		case SQL_DESC_ROWVER: /* read-only */
#endif /* ODBCVER */
		case SQL_DESC_TYPE_NAME: /* read-only */
		case SQL_DESC_UNSIGNED: /* read-only */
		default:ret = SQL_ERROR;
			stmt->errornumber = STMT_INVALID_DESCRIPTOR_IDENTIFIER; 
	}
	return ret;
}


static RETCODE SQL_API
ARDGetField(StatementClass *stmt, SQLSMALLINT RecNumber,
		SQLSMALLINT FieldIdentifier, PTR Value, SQLINTEGER BufferLength,
		SQLINTEGER *StringLength)
{
	RETCODE		ret = SQL_SUCCESS;
	SQLINTEGER	len, ival;
	PTR		ptr = NULL;
	const ARDFields	*opts = SC_get_ARD(stmt);

	len = 4;
	switch (FieldIdentifier)
	{
		case SQL_DESC_ARRAY_SIZE:
			ival = opts->rowset_size;
			break; 
		case SQL_DESC_ARRAY_STATUS_PTR:
			ptr = opts->row_operation_ptr;
			break;
		case SQL_DESC_BIND_OFFSET_PTR:
			ptr = opts->row_offset_ptr;
			break;
		case SQL_DESC_BIND_TYPE:
			ival = opts->bind_size;
			break;
		case SQL_DESC_TYPE:
			switch (opts->bindings[RecNumber - 1].returntype)
			{
				case SQL_C_TYPE_DATE:
				case SQL_C_TYPE_TIME:
				case SQL_C_TYPE_TIMESTAMP:
					ival = SQL_DATETIME;
					break;
				default:
					ival = opts->bindings[RecNumber - 1].returntype;
			}
			break;
		case SQL_DESC_DATETIME_INTERVAL_CODE:
			switch (opts->bindings[RecNumber - 1].returntype)
			{
				case SQL_C_TYPE_DATE:
					ival = SQL_CODE_DATE;
					break;
				case SQL_C_TYPE_TIME:
					ival = SQL_CODE_TIME;
					break;
				case SQL_C_TYPE_TIMESTAMP:
					ival = SQL_CODE_TIMESTAMP;
					break;
				default:
					ival = 0;
					break;
			}
			break;
		case SQL_DESC_CONCISE_TYPE:
			ival = opts->bindings[RecNumber - 1].returntype;
			break;
		case SQL_DESC_DATA_PTR:
			if (!RecNumber)
				ptr = opts->bookmark->buffer;
			else
			{
				ptr = opts->bindings[RecNumber - 1].buffer;
			}
			break;
		case SQL_DESC_INDICATOR_PTR:
			if (!RecNumber)
				ptr = opts->bookmark->used;
			else
			{
				ptr = opts->bindings[RecNumber - 1].used;
			}
			break;
		case SQL_DESC_OCTET_LENGTH_PTR:
			if (!RecNumber)
				ptr = opts->bookmark->used;
			else
			{
				ptr = opts->bindings[RecNumber - 1].used;
			}
			break;
		case SQL_DESC_COUNT:
			ival = opts->allocated;
			break;
		case SQL_DESC_OCTET_LENGTH:
			if (RecNumber)
			{
				ival = opts->bindings[RecNumber - 1].buflen;
			}
			break;
		case SQL_DESC_ALLOC_TYPE: /* read-only */
			ival = SQL_DESC_ALLOC_AUTO;
			break;
		case SQL_DESC_DATETIME_INTERVAL_PRECISION:
		case SQL_DESC_LENGTH:
		case SQL_DESC_NUM_PREC_RADIX:
		case SQL_DESC_PRECISION:
		case SQL_DESC_SCALE:
		default:ret = SQL_ERROR;
			stmt->errornumber = STMT_INVALID_DESCRIPTOR_IDENTIFIER; 
	}
	switch (BufferLength)
	{
		case 0:
		case SQL_IS_INTEGER:
			len = 4;
			*((SQLINTEGER *) Value) = ival;
			break;
		case SQL_IS_UINTEGER:
			len = 4;
			*((UInt4 *) Value) = ival;
			break;
		case SQL_IS_SMALLINT:
			len = 2;
			*((SQLSMALLINT *) Value) = (SQLSMALLINT) ival;
			break;
		case SQL_IS_USMALLINT:
			len = 2;
			*((SQLUSMALLINT *) Value) = (SQLUSMALLINT) ival;
			break;
		case SQL_IS_POINTER:
			len = 4;
			*((void **) Value) = ptr;
			break;
	}
			
	if (StringLength)
		*StringLength = len;
	return ret;
}

static RETCODE SQL_API
APDGetField(StatementClass *stmt, SQLSMALLINT RecNumber,
		SQLSMALLINT FieldIdentifier, PTR Value, SQLINTEGER BufferLength,
		SQLINTEGER *StringLength)
{
	RETCODE		ret = SQL_SUCCESS;
	SQLINTEGER	ival = 0, len;
	PTR		ptr = NULL;
	const APDFields	*opts = SC_get_APD(stmt);

	len = 4;
	switch (FieldIdentifier)
	{
		case SQL_DESC_ARRAY_SIZE:
			ival = opts->paramset_size;
			break; 
		case SQL_DESC_ARRAY_STATUS_PTR:
			ptr = opts->param_operation_ptr;
			break;
		case SQL_DESC_BIND_OFFSET_PTR:
			ptr = opts->param_offset_ptr;
			break;
		case SQL_DESC_BIND_TYPE:
			ival = opts->param_bind_type;
			break;

		case SQL_DESC_TYPE:
			switch (opts->parameters[RecNumber - 1].CType)
			{
				case SQL_C_TYPE_DATE:
				case SQL_C_TYPE_TIME:
				case SQL_C_TYPE_TIMESTAMP:
					ival = SQL_DATETIME;
					break;
				default:
					ival = opts->parameters[RecNumber - 1].CType;
			}
			break;
		case SQL_DESC_DATETIME_INTERVAL_CODE:
			switch (opts->parameters[RecNumber - 1].CType)
			{
				case SQL_C_TYPE_DATE:
					ival = SQL_CODE_DATE;
					break;
				case SQL_C_TYPE_TIME:
					ival = SQL_CODE_TIME;
					break;
				case SQL_C_TYPE_TIMESTAMP:
					ival = SQL_CODE_TIMESTAMP;
					break;
				default:
					ival = 0;
					break;
			}
			break;
		case SQL_DESC_CONCISE_TYPE:
			ival = opts->parameters[RecNumber - 1].CType;
			break;
		case SQL_DESC_DATA_PTR:
			ptr = opts->parameters[RecNumber - 1].buffer;
			break;
		case SQL_DESC_INDICATOR_PTR:
			ptr = opts->parameters[RecNumber - 1].used;
			break;
		case SQL_DESC_OCTET_LENGTH:
			ival = opts->parameters[RecNumber - 1].buflen;
			break;
		case SQL_DESC_OCTET_LENGTH_PTR:
			ptr = opts->parameters[RecNumber - 1].used;
			break;
		case SQL_DESC_COUNT:
			ival = opts->allocated;
			break; 
		case SQL_DESC_ALLOC_TYPE: /* read-only */
			ival = SQL_DESC_ALLOC_AUTO;
			break;
		case SQL_DESC_DATETIME_INTERVAL_PRECISION:
		case SQL_DESC_LENGTH:
		case SQL_DESC_NUM_PREC_RADIX:
		case SQL_DESC_PRECISION:
		case SQL_DESC_SCALE:
		default:ret = SQL_ERROR;
			stmt->errornumber = STMT_INVALID_DESCRIPTOR_IDENTIFIER; 
	}
	switch (BufferLength)
	{
		case 0:
		case SQL_IS_INTEGER:
			len = 4;
			*((Int4 *) Value) = ival;
			break;
		case SQL_IS_UINTEGER:
			len = 4;
			*((UInt4 *) Value) = ival;
			break;
		case SQL_IS_SMALLINT:
			len = 2;
			*((SQLSMALLINT *) Value) = (SQLSMALLINT) ival;
			break;
		case SQL_IS_USMALLINT:
			len = 2;
			*((SQLUSMALLINT *) Value) = (SQLUSMALLINT) ival;
			break;
		case SQL_IS_POINTER:
			len = 4;
			*((void **) Value) = ptr;
			break;
	}
			
	if (StringLength)
		*StringLength = len;
	return ret;
}

static RETCODE SQL_API
IRDGetField(StatementClass *stmt, SQLSMALLINT RecNumber,
		SQLSMALLINT FieldIdentifier, PTR Value, SQLINTEGER BufferLength,
		SQLINTEGER *StringLength)
{
	RETCODE		ret = SQL_SUCCESS;
	SQLINTEGER	ival = 0, len;
	PTR		ptr = NULL;
	const IRDFields	*opts = SC_get_IRD(stmt);

	switch (FieldIdentifier)
	{
		case SQL_DESC_ARRAY_STATUS_PTR:
			ptr = opts->rowStatusArray;
			break;
		case SQL_DESC_ROWS_PROCESSED_PTR:
			ptr = opts->rowsFetched;
			break;
		case SQL_DESC_ALLOC_TYPE: /* read-only */
		case SQL_DESC_COUNT: /* read-only */
		case SQL_DESC_AUTO_UNIQUE_VALUE: /* read-only */
		case SQL_DESC_BASE_COLUMN_NAME: /* read-only */
		case SQL_DESC_BASE_TABLE_NAME: /* read-only */
		case SQL_DESC_CASE_SENSITIVE: /* read-only */
		case SQL_DESC_CATALOG_NAME: /* read-only */
		case SQL_DESC_CONCISE_TYPE: /* read-only */
		case SQL_DESC_DATETIME_INTERVAL_CODE: /* read-only */
		case SQL_DESC_DATETIME_INTERVAL_PRECISION: /* read-only */
		case SQL_DESC_DISPLAY_SIZE: /* read-only */
		case SQL_DESC_FIXED_PREC_SCALE: /* read-only */
		case SQL_DESC_LABEL: /* read-only */
		case SQL_DESC_LENGTH: /* read-only */
		case SQL_DESC_LITERAL_PREFIX: /* read-only */
		case SQL_DESC_LITERAL_SUFFIX: /* read-only */
		case SQL_DESC_LOCAL_TYPE_NAME: /* read-only */
		case SQL_DESC_NAME: /* read-only */
		case SQL_DESC_NULLABLE: /* read-only */
		case SQL_DESC_NUM_PREC_RADIX: /* read-only */
		case SQL_DESC_OCTET_LENGTH: /* read-only */
		case SQL_DESC_PRECISION: /* read-only */
#if (ODBCVER >= 0x0350)
		case SQL_DESC_ROWVER: /* read-only */
#endif /* ODBCVER */
		case SQL_DESC_SCALE: /* read-only */
		case SQL_DESC_SCHEMA_NAME: /* read-only */
		case SQL_DESC_SEARCHABLE: /* read-only */
		case SQL_DESC_TABLE_NAME: /* read-only */
		case SQL_DESC_TYPE: /* read-only */
		case SQL_DESC_TYPE_NAME: /* read-only */
		case SQL_DESC_UNNAMED: /* read-only */
		case SQL_DESC_UNSIGNED: /* read-only */
		case SQL_DESC_UPDATABLE: /* read-only */
		default:ret = SQL_ERROR;
			stmt->errornumber = STMT_INVALID_DESCRIPTOR_IDENTIFIER; 
	}
	switch (BufferLength)
	{
		case 0:
		case SQL_IS_INTEGER:
			len = 4;
			*((Int4 *) Value) = ival;
			break;
		case SQL_IS_UINTEGER:
			len = 4;
			*((UInt4 *) Value) = ival;
			break;
		case SQL_IS_SMALLINT:
			len = 2;
			*((SQLSMALLINT *) Value) = (SQLSMALLINT) ival;
			break;
		case SQL_IS_USMALLINT:
			len = 2;
			*((SQLUSMALLINT *) Value) = (SQLUSMALLINT) ival;
			break;
		case SQL_IS_POINTER:
			len = 4;
			*((void **) Value) = ptr;
			break;
	}
			
	if (StringLength)
		*StringLength = len;
	return ret;
}

static RETCODE SQL_API
IPDGetField(StatementClass *stmt, SQLSMALLINT RecNumber,
		SQLSMALLINT FieldIdentifier, PTR Value, SQLINTEGER BufferLength,
		SQLINTEGER *StringLength)
{
	RETCODE		ret = SQL_SUCCESS;
	SQLINTEGER	ival = 0, len;
	PTR		ptr = NULL;
	const IPDFields	*ipdopts = SC_get_IPD(stmt);
	const APDFields	*apdopts = SC_get_APD(stmt);

	switch (FieldIdentifier)
	{
		case SQL_DESC_ARRAY_STATUS_PTR:
			ptr = ipdopts->param_status_ptr;
			break;
		case SQL_DESC_ROWS_PROCESSED_PTR:
			ptr = ipdopts->param_processed_ptr;
			break;
		case SQL_DESC_UNNAMED: /* only SQL_UNNAMED is allowed */
			ival = SQL_UNNAMED;
			break;
		case SQL_DESC_TYPE:
			switch (apdopts->parameters[RecNumber - 1].SQLType)
			{
				case SQL_TYPE_DATE:
				case SQL_TYPE_TIME:
				case SQL_TYPE_TIMESTAMP:
					ival = SQL_DATETIME;
					break;
				default:
					ival = apdopts->parameters[RecNumber - 1].SQLType;
			}
			break;
		case SQL_DESC_DATETIME_INTERVAL_CODE:
			switch (apdopts->parameters[RecNumber - 1].SQLType)
			{
				case SQL_TYPE_DATE:
					ival = SQL_CODE_DATE;
				case SQL_TYPE_TIME:
					ival = SQL_CODE_TIME;
					break;
				case SQL_TYPE_TIMESTAMP:
					ival = SQL_CODE_TIMESTAMP;
					break;
				default:
					ival = 0;
			}
			break;
		case SQL_DESC_CONCISE_TYPE:
			ival = apdopts->parameters[RecNumber - 1].SQLType;
			break;
		case SQL_DESC_COUNT:
			ival = apdopts->allocated;
			break; 
		case SQL_DESC_PARAMETER_TYPE:
			ival = apdopts->parameters[RecNumber - 1].paramType;
			break;
		case SQL_DESC_SCALE:
			ival = apdopts->parameters[RecNumber - 1].scale ;
			break;
		case SQL_DESC_ALLOC_TYPE: /* read-only */
			ival = SQL_DESC_ALLOC_AUTO;
			break; 
		case SQL_DESC_CASE_SENSITIVE: /* read-only */
		case SQL_DESC_DATETIME_INTERVAL_PRECISION:
		case SQL_DESC_FIXED_PREC_SCALE: /* read-only */
		case SQL_DESC_LENGTH:
		case SQL_DESC_LOCAL_TYPE_NAME: /* read-only */
		case SQL_DESC_NAME:
		case SQL_DESC_NULLABLE: /* read-only */
		case SQL_DESC_NUM_PREC_RADIX:
		case SQL_DESC_OCTET_LENGTH:
		case SQL_DESC_PRECISION:
#if (ODBCVER >= 0x0350)
		case SQL_DESC_ROWVER: /* read-only */
#endif /* ODBCVER */
		case SQL_DESC_TYPE_NAME: /* read-only */
		case SQL_DESC_UNSIGNED: /* read-only */
		default:ret = SQL_ERROR;
			stmt->errornumber = STMT_INVALID_DESCRIPTOR_IDENTIFIER; 
	}
	switch (BufferLength)
	{
		case 0:
		case SQL_IS_INTEGER:
			len = 4;
			*((Int4 *) Value) = ival;
			break;
		case SQL_IS_UINTEGER:
			len = 4;
			*((UInt4 *) Value) = ival;
			break;
		case SQL_IS_SMALLINT:
			len = 2;
			*((SQLSMALLINT *) Value) = (SQLSMALLINT) ival;
			break;
		case SQL_IS_USMALLINT:
			len = 2;
			*((SQLUSMALLINT *) Value) = (SQLUSMALLINT) ival;
			break;
		case SQL_IS_POINTER:
			len = 4;
			*((void **)Value) = ptr;
			break;
	}
			
	if (StringLength)
		*StringLength = len;
	return ret;
}

/*	SQLGetStmtOption -> SQLGetStmtAttr */
RETCODE		SQL_API
PGAPI_GetStmtAttr(HSTMT StatementHandle,
		SQLINTEGER Attribute, PTR Value,
		SQLINTEGER BufferLength, SQLINTEGER *StringLength)
{
	static char *func = "PGAPI_GetStmtAttr";
	StatementClass *stmt = (StatementClass *) StatementHandle;
	RETCODE		ret = SQL_SUCCESS;
	int			len = 0;

	mylog("%s Handle=%u %d\n", func, StatementHandle, Attribute);
	switch (Attribute)
	{
		case SQL_ATTR_FETCH_BOOKMARK_PTR:		/* 16 */
			*((void **) Value) = stmt->options.bookmark_ptr;
			len = 4;
			break;
		case SQL_ATTR_PARAM_BIND_OFFSET_PTR:	/* 17 */
			*((SQLUINTEGER **) Value) = SC_get_APD(stmt)->param_offset_ptr;
			len = 4;
			break;
		case SQL_ATTR_PARAM_BIND_TYPE:	/* 18 */
			*((SQLUINTEGER *) Value) = SC_get_APD(stmt)->param_bind_type;
			len = 4;
			break;
		case SQL_ATTR_PARAM_OPERATION_PTR:		/* 19 */
			*((SQLUSMALLINT **) Value) = SC_get_APD(stmt)->param_operation_ptr;
			len = 4;
			break;
		case SQL_ATTR_PARAM_STATUS_PTR: /* 20 */
			*((SQLUSMALLINT **) Value) = SC_get_IPD(stmt)->param_status_ptr;
			len = 4;
			break;
		case SQL_ATTR_PARAMS_PROCESSED_PTR:		/* 21 */
			*((SQLUINTEGER **) Value) = SC_get_IPD(stmt)->param_processed_ptr;
			len = 4;
			break;
		case SQL_ATTR_PARAMSET_SIZE:	/* 22 */
			*((SQLUINTEGER *) Value) = SC_get_APD(stmt)->paramset_size;
			len = 4;
			break;
		case SQL_ATTR_ROW_BIND_OFFSET_PTR:		/* 23 */
			*((SQLUINTEGER **) Value) = SC_get_ARD(stmt)->row_offset_ptr;
			len = 4;
			break;
		case SQL_ATTR_ROW_OPERATION_PTR:		/* 24 */
			*((SQLUSMALLINT **) Value) = SC_get_ARD(stmt)->row_operation_ptr;
			len = 4;
			break;
		case SQL_ATTR_ROW_STATUS_PTR:	/* 25 */
			*((SQLUSMALLINT **) Value) = SC_get_IRD(stmt)->rowStatusArray;
			len = 4;
			break;
		case SQL_ATTR_ROWS_FETCHED_PTR: /* 26 */
			*((SQLUINTEGER **) Value) = SC_get_IRD(stmt)->rowsFetched;
			len = 4;
			break;
		case SQL_ATTR_ROW_ARRAY_SIZE:	/* 27 */
			*((SQLUINTEGER *) Value) = SC_get_ARD(stmt)->rowset_size;
			len = 4;
			break;
		case SQL_ATTR_APP_ROW_DESC:		/* 10010 */
		case SQL_ATTR_APP_PARAM_DESC:	/* 10011 */
		case SQL_ATTR_IMP_ROW_DESC:		/* 10012 */
		case SQL_ATTR_IMP_PARAM_DESC:	/* 10013 */
			len = 4;
			*((HSTMT *) Value) = descHandleFromStatementHandle(StatementHandle, Attribute); 
			break;
		case SQL_ATTR_AUTO_IPD:	/* 10001 */
			/* case SQL_ATTR_ROW_BIND_TYPE: ** == SQL_BIND_TYPE(ODBC2.0) */

		case SQL_ATTR_CURSOR_SCROLLABLE:		/* -1 */
		case SQL_ATTR_CURSOR_SENSITIVITY:		/* -2 */
		case SQL_ATTR_ENABLE_AUTO_IPD:	/* 15 */
		case SQL_ATTR_METADATA_ID:		/* 10014 */

			/*
			 * case SQL_ATTR_PREDICATE_PTR: case
			 * SQL_ATTR_PREDICATE_OCTET_LENGTH_PTR:
			 */
			stmt->errornumber = STMT_INVALID_OPTION_IDENTIFIER;
			stmt->errormsg = "Unsupported statement option (Get)";
			SC_log_error(func, "", stmt);
			return SQL_ERROR;
		default:
			len = 4;
			ret = PGAPI_GetStmtOption(StatementHandle, (UWORD) Attribute, Value);
	}
	if (ret == SQL_SUCCESS && StringLength)
		*StringLength = len;
	return ret;
}

/*	SQLSetConnectOption -> SQLSetConnectAttr */
RETCODE		SQL_API
PGAPI_SetConnectAttr(HDBC ConnectionHandle,
			SQLINTEGER Attribute, PTR Value,
			SQLINTEGER StringLength)
{
	ConnectionClass *conn = (ConnectionClass *) ConnectionHandle;
	RETCODE	ret = SQL_SUCCESS;

	mylog("PGAPI_SetConnectAttr %d\n", Attribute);
	switch (Attribute)
	{
		case SQL_ATTR_ASYNC_ENABLE:
		case SQL_ATTR_AUTO_IPD:
		case SQL_ATTR_CONNECTION_DEAD:
		case SQL_ATTR_CONNECTION_TIMEOUT:
		case SQL_ATTR_METADATA_ID:
			conn->errornumber = STMT_INVALID_OPTION_IDENTIFIER;
			conn->errormsg = "Unsupported connect attribute (Set)";
			return SQL_ERROR;
		default:
			ret = PGAPI_SetConnectOption(ConnectionHandle, (UWORD) Attribute, (UDWORD) Value);
	}
	return ret;
}

/*	new function */
RETCODE		SQL_API
PGAPI_GetDescField(SQLHDESC DescriptorHandle,
			SQLSMALLINT RecNumber, SQLSMALLINT FieldIdentifier,
			PTR Value, SQLINTEGER BufferLength,
			SQLINTEGER *StringLength)
{
	RETCODE		ret = SQL_SUCCESS;
	HSTMT		hstmt;
	SQLUINTEGER	descType;
	StatementClass *stmt;
	static const char *func = "PGAPI_GetDescField";

	mylog("%s h=%u rec=%d field=%d blen=%d\n", func, DescriptorHandle, RecNumber, FieldIdentifier, BufferLength);
	hstmt = statementHandleFromDescHandle(DescriptorHandle, &descType);
	mylog("stmt=%x type=%d\n", hstmt, descType);
	stmt = (StatementClass *) hstmt;
	switch (descType)
	{
		case SQL_ATTR_APP_ROW_DESC:
			ret = ARDGetField(stmt, RecNumber, FieldIdentifier, Value, BufferLength, StringLength);
			break;
		case SQL_ATTR_APP_PARAM_DESC:
			ret = APDGetField(stmt, RecNumber, FieldIdentifier, Value, BufferLength, StringLength);
			break;
		case SQL_ATTR_IMP_ROW_DESC:
			ret = IRDGetField(stmt, RecNumber, FieldIdentifier, Value, BufferLength, StringLength);
			break;
		case SQL_ATTR_IMP_PARAM_DESC:
			ret = IPDGetField(stmt, RecNumber, FieldIdentifier, Value, BufferLength, StringLength);
			break;
		default:ret = SQL_ERROR;
			stmt->errornumber = STMT_INTERNAL_ERROR; 
			stmt->errormsg = "Error not implemented";
	}
	if (ret == SQL_ERROR)
	{
		if (!stmt->errormsg && stmt->errornumber == STMT_INVALID_DESCRIPTOR_IDENTIFIER) 
			stmt->errormsg = "can't SQLGetDescField for this descriptor identifier"; 
		SC_log_error(func, "", stmt);
	}
	return ret;
}

/*	new function */
RETCODE		SQL_API
PGAPI_SetDescField(SQLHDESC DescriptorHandle,
			SQLSMALLINT RecNumber, SQLSMALLINT FieldIdentifier,
			PTR Value, SQLINTEGER BufferLength)
{
	RETCODE		ret = SQL_SUCCESS;
	HSTMT		hstmt;
	SQLUINTEGER	descType;
	StatementClass *stmt;
	static const char *func = "PGAPI_SetDescField";

	mylog("%s h=%u rec=%d field=%d val=%x,%d\n", func, DescriptorHandle, RecNumber, FieldIdentifier, Value, BufferLength);
	hstmt = statementHandleFromDescHandle(DescriptorHandle, &descType);
	mylog("stmt=%x type=%d\n", hstmt, descType);
	stmt = (StatementClass *) hstmt;
	switch (descType)
	{
		case SQL_ATTR_APP_ROW_DESC:
			ret = ARDSetField(stmt, RecNumber, FieldIdentifier, Value, BufferLength);
			break;
		case SQL_ATTR_APP_PARAM_DESC:
			ret = APDSetField(stmt, RecNumber, FieldIdentifier, Value, BufferLength);
			break;
		case SQL_ATTR_IMP_ROW_DESC:
			ret = IRDSetField(stmt, RecNumber, FieldIdentifier, Value, BufferLength);
			break;
		case SQL_ATTR_IMP_PARAM_DESC:
			ret = IPDSetField(stmt, RecNumber, FieldIdentifier, Value, BufferLength);
			break;
		default:ret = SQL_ERROR;
			stmt->errornumber = STMT_INTERNAL_ERROR; 
			stmt->errormsg = "Error not implemented";
	}
	if (ret == SQL_ERROR)
	{
		if (!stmt->errormsg && stmt->errornumber == STMT_INVALID_DESCRIPTOR_IDENTIFIER) 
			stmt->errormsg = "can't SQLSetDescField for this descriptor identifier"; 
		SC_log_error(func, "", stmt);
	}
	return ret;
}

/*	SQLSet(Param/Scroll/Stmt)Option -> SQLSetStmtAttr */
RETCODE		SQL_API
PGAPI_SetStmtAttr(HSTMT StatementHandle,
		SQLINTEGER Attribute, PTR Value,
		SQLINTEGER StringLength)
{
	static char *func = "PGAPI_SetStmtAttr";
	StatementClass *stmt = (StatementClass *) StatementHandle;

	mylog("%s Handle=%u %d,%u\n", func, StatementHandle, Attribute, Value);
	switch (Attribute)
	{
		case SQL_ATTR_CURSOR_SCROLLABLE:		/* -1 */
		case SQL_ATTR_CURSOR_SENSITIVITY:		/* -2 */

		case SQL_ATTR_ENABLE_AUTO_IPD:	/* 15 */

		case SQL_ATTR_APP_ROW_DESC:		/* 10010 */
		case SQL_ATTR_APP_PARAM_DESC:	/* 10011 */
		case SQL_ATTR_AUTO_IPD:	/* 10001 */
		/* case SQL_ATTR_ROW_BIND_TYPE: ** == SQL_BIND_TYPE(ODBC2.0) */
		case SQL_ATTR_IMP_ROW_DESC:	/* 10012 (read-only) */
		case SQL_ATTR_IMP_PARAM_DESC:	/* 10013 (read-only) */
		case SQL_ATTR_METADATA_ID:		/* 10014 */

			/*
			 * case SQL_ATTR_PREDICATE_PTR: case
			 * SQL_ATTR_PREDICATE_OCTET_LENGTH_PTR:
			 */
			stmt->errornumber = STMT_INVALID_OPTION_IDENTIFIER;
			stmt->errormsg = "Unsupported statement option (Set)";
			SC_log_error(func, "", stmt);
			return SQL_ERROR;

		case SQL_ATTR_FETCH_BOOKMARK_PTR:		/* 16 */
			stmt->options.bookmark_ptr = Value;
			break;
		case SQL_ATTR_PARAM_BIND_OFFSET_PTR:	/* 17 */
			SC_get_APD(stmt)->param_offset_ptr = (SQLUINTEGER *) Value;
			break;
		case SQL_ATTR_PARAM_BIND_TYPE:	/* 18 */
			SC_get_APD(stmt)->param_bind_type = (SQLUINTEGER) Value;
			break;
		case SQL_ATTR_PARAM_OPERATION_PTR:		/* 19 */
			SC_get_APD(stmt)->param_operation_ptr = Value;
			break;
		case SQL_ATTR_PARAM_STATUS_PTR:			/* 20 */
			SC_get_IPD(stmt)->param_status_ptr = (SQLUSMALLINT *) Value;
			break;
		case SQL_ATTR_PARAMS_PROCESSED_PTR:		/* 21 */
			SC_get_IPD(stmt)->param_processed_ptr = (SQLUINTEGER *) Value;
			break;
		case SQL_ATTR_PARAMSET_SIZE:	/* 22 */
			SC_get_APD(stmt)->paramset_size = (SQLUINTEGER) Value;
			break;
		case SQL_ATTR_ROW_BIND_OFFSET_PTR:		/* 23 */
			SC_get_ARD(stmt)->row_offset_ptr = (SQLUINTEGER *) Value;
			break;
		case SQL_ATTR_ROW_OPERATION_PTR:		/* 24 */
			SC_get_ARD(stmt)->row_operation_ptr = Value;
			break;
		case SQL_ATTR_ROW_STATUS_PTR:	/* 25 */
			SC_get_IRD(stmt)->rowStatusArray = (SQLUSMALLINT *) Value;
			break;
		case SQL_ATTR_ROWS_FETCHED_PTR: /* 26 */
			SC_get_IRD(stmt)->rowsFetched = (SQLUINTEGER *) Value;
			break;
		case SQL_ATTR_ROW_ARRAY_SIZE:	/* 27 */
			SC_get_ARD(stmt)->rowset_size = (SQLUINTEGER) Value;
			break;
		default:
			return PGAPI_SetStmtOption(StatementHandle, (UWORD) Attribute, (UDWORD) Value);
	}
	return SQL_SUCCESS;
}
