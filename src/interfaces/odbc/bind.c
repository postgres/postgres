
/* Module:			bind.c
 *
 * Description:		This module contains routines related to binding
 *					columns and parameters.
 *
 * Classes:			BindInfoClass, ParameterInfoClass
 *
 * API functions:	SQLBindParameter, SQLBindCol, SQLDescribeParam, SQLNumParams,
 *					SQLParamOptions(NI)
 *
 * Comments:		See "notice.txt" for copyright and license information.
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "bind.h"
#include "environ.h"
#include "statement.h"
#include "qresult.h"
#include "pgtypes.h"
#include <stdlib.h>
#include <string.h>

#ifndef WIN32
#include "iodbc.h"
#include "isql.h"
#include "isqlext.h"
#else
#include "sql.h"
#include "sqlext.h"
#endif

/*		Bind parameters on a statement handle */

RETCODE SQL_API
SQLBindParameter(
				 HSTMT hstmt,
				 UWORD ipar,
				 SWORD fParamType,
				 SWORD fCType,
				 SWORD fSqlType,
				 UDWORD cbColDef,
				 SWORD ibScale,
				 PTR rgbValue,
				 SDWORD cbValueMax,
				 SDWORD FAR *pcbValue)
{
	StatementClass *stmt = (StatementClass *) hstmt;
	static char *func = "SQLBindParameter";

	mylog("%s: entering...\n", func);

	if (!stmt)
	{
		SC_log_error(func, "", NULL);
		return SQL_INVALID_HANDLE;
	}

	if (stmt->parameters_allocated < ipar)
	{
		ParameterInfoClass *old_parameters;
		int			i,
					old_parameters_allocated;

		old_parameters = stmt->parameters;
		old_parameters_allocated = stmt->parameters_allocated;

		stmt->parameters = (ParameterInfoClass *) malloc(sizeof(ParameterInfoClass) * (ipar));
		if (!stmt->parameters)
		{
			stmt->errornumber = STMT_NO_MEMORY_ERROR;
			stmt->errormsg = "Could not allocate memory for statement parameters";
			SC_log_error(func, "", stmt);
			return SQL_ERROR;
		}

		stmt->parameters_allocated = ipar;

		/* copy the old parameters over */
		for (i = 0; i < old_parameters_allocated; i++)
		{
			/* a structure copy should work */
			stmt->parameters[i] = old_parameters[i];
		}

		/* get rid of the old parameters, if there were any */
		if (old_parameters)
			free(old_parameters);

		/*
		 * zero out the newly allocated parameters (in case they skipped
		 * some,
		 */
		/* so we don't accidentally try to use them later) */
		for (; i < stmt->parameters_allocated; i++)
		{
			stmt->parameters[i].buflen = 0;
			stmt->parameters[i].buffer = 0;
			stmt->parameters[i].used = 0;
			stmt->parameters[i].paramType = 0;
			stmt->parameters[i].CType = 0;
			stmt->parameters[i].SQLType = 0;
			stmt->parameters[i].precision = 0;
			stmt->parameters[i].scale = 0;
			stmt->parameters[i].data_at_exec = FALSE;
			stmt->parameters[i].lobj_oid = 0;
			stmt->parameters[i].EXEC_used = NULL;
			stmt->parameters[i].EXEC_buffer = NULL;
		}
	}

	ipar--;						/* use zero based column numbers for the
								 * below part */

	/* store the given info */
	stmt->parameters[ipar].buflen = cbValueMax;
	stmt->parameters[ipar].buffer = rgbValue;
	stmt->parameters[ipar].used = pcbValue;
	stmt->parameters[ipar].paramType = fParamType;
	stmt->parameters[ipar].CType = fCType;
	stmt->parameters[ipar].SQLType = fSqlType;
	stmt->parameters[ipar].precision = cbColDef;
	stmt->parameters[ipar].scale = ibScale;

	/*
	 * If rebinding a parameter that had data-at-exec stuff in it, then
	 * free that stuff
	 */
	if (stmt->parameters[ipar].EXEC_used)
	{
		free(stmt->parameters[ipar].EXEC_used);
		stmt->parameters[ipar].EXEC_used = NULL;
	}

	if (stmt->parameters[ipar].EXEC_buffer)
	{
		if (stmt->parameters[ipar].SQLType != SQL_LONGVARBINARY)
			free(stmt->parameters[ipar].EXEC_buffer);
		stmt->parameters[ipar].EXEC_buffer = NULL;
	}

	/* Data at exec macro only valid for C char/binary data */
	if ((fSqlType == SQL_LONGVARBINARY || fSqlType == SQL_LONGVARCHAR) && pcbValue && *pcbValue <= SQL_LEN_DATA_AT_EXEC_OFFSET)
		stmt->parameters[ipar].data_at_exec = TRUE;
	else
		stmt->parameters[ipar].data_at_exec = FALSE;

	mylog("SQLBindParamater: ipar=%d, paramType=%d, fCType=%d, fSqlType=%d, cbColDef=%d, ibScale=%d, rgbValue=%d, *pcbValue = %d, data_at_exec = %d\n", ipar, fParamType, fCType, fSqlType, cbColDef, ibScale, rgbValue, pcbValue ? *pcbValue : -777, stmt->parameters[ipar].data_at_exec);

	return SQL_SUCCESS;
}

/*		-		-		-		-		-		-		-		-		- */

/*		Associate a user-supplied buffer with a database column. */
RETCODE SQL_API
SQLBindCol(
		   HSTMT hstmt,
		   UWORD icol,
		   SWORD fCType,
		   PTR rgbValue,
		   SDWORD cbValueMax,
		   SDWORD FAR *pcbValue)
{
	StatementClass *stmt = (StatementClass *) hstmt;
	static char *func = "SQLBindCol";

	mylog("%s: entering...\n", func);

	mylog("**** SQLBindCol: stmt = %u, icol = %d\n", stmt, icol);

	if (!stmt)
	{
		SC_log_error(func, "", NULL);
		return SQL_INVALID_HANDLE;
	}


	SC_clear_error(stmt);

	if (stmt->status == STMT_EXECUTING)
	{
		stmt->errormsg = "Can't bind columns while statement is still executing.";
		stmt->errornumber = STMT_SEQUENCE_ERROR;
		SC_log_error(func, "", stmt);
		return SQL_ERROR;
	}

	/* If the bookmark column is being bound, then just save it */
	if (icol == 0)
	{

		if (rgbValue == NULL)
		{
			stmt->bookmark.buffer = NULL;
			stmt->bookmark.used = NULL;
		}
		else
		{
			/* Make sure it is the bookmark data type */
			if (fCType != SQL_C_BOOKMARK)
			{
				stmt->errormsg = "Column 0 is not of type SQL_C_BOOKMARK";
				stmt->errornumber = STMT_PROGRAM_TYPE_OUT_OF_RANGE;
				SC_log_error(func, "", stmt);
				return SQL_ERROR;
			}

			stmt->bookmark.buffer = rgbValue;
			stmt->bookmark.used = pcbValue;
		}
		return SQL_SUCCESS;
	}

	/* allocate enough bindings if not already done */
	/* Most likely, execution of a statement would have setup the  */
	/* necessary bindings. But some apps call BindCol before any */
	/* statement is executed. */
	if (icol > stmt->bindings_allocated)
		extend_bindings(stmt, icol);

	/* check to see if the bindings were allocated */
	if (!stmt->bindings)
	{
		stmt->errormsg = "Could not allocate memory for bindings.";
		stmt->errornumber = STMT_NO_MEMORY_ERROR;
		SC_log_error(func, "", stmt);
		return SQL_ERROR;
	}

	icol--;						/* use zero based col numbers from here
								 * out */

	/* Reset for SQLGetData */
	stmt->bindings[icol].data_left = -1;

	if (rgbValue == NULL)
	{
		/* we have to unbind the column */
		stmt->bindings[icol].buflen = 0;
		stmt->bindings[icol].buffer = NULL;
		stmt->bindings[icol].used = NULL;
		stmt->bindings[icol].returntype = SQL_C_CHAR;
	}
	else
	{
		/* ok, bind that column */
		stmt->bindings[icol].buflen = cbValueMax;
		stmt->bindings[icol].buffer = rgbValue;
		stmt->bindings[icol].used = pcbValue;
		stmt->bindings[icol].returntype = fCType;

		mylog("       bound buffer[%d] = %u\n", icol, stmt->bindings[icol].buffer);
	}

	return SQL_SUCCESS;
}

/*		-		-		-		-		-		-		-		-		- */

/*	Returns the description of a parameter marker. */
/*	This function is listed as not being supported by SQLGetFunctions() because it is  */
/*	used to describe "parameter markers" (not bound parameters), in which case,  */
/*	the dbms should return info on the markers.  Since Postgres doesn't support that, */
/*	it is best to say this function is not supported and let the application assume a  */
/*	data type (most likely varchar). */

RETCODE SQL_API
SQLDescribeParam(
				 HSTMT hstmt,
				 UWORD ipar,
				 SWORD FAR *pfSqlType,
				 UDWORD FAR *pcbColDef,
				 SWORD FAR *pibScale,
				 SWORD FAR *pfNullable)
{
	StatementClass *stmt = (StatementClass *) hstmt;
	static char *func = "SQLDescribeParam";

	mylog("%s: entering...\n", func);

	if (!stmt)
	{
		SC_log_error(func, "", NULL);
		return SQL_INVALID_HANDLE;
	}

	if ((ipar < 1) || (ipar > stmt->parameters_allocated))
	{
		stmt->errormsg = "Invalid parameter number for SQLDescribeParam.";
		stmt->errornumber = STMT_BAD_PARAMETER_NUMBER_ERROR;
		SC_log_error(func, "", stmt);
		return SQL_ERROR;
	}

	ipar--;

	/*
	 * This implementation is not very good, since it is supposed to
	 * describe
	 */
	/* parameter markers, not bound parameters.  */
	if (pfSqlType)
		*pfSqlType = stmt->parameters[ipar].SQLType;

	if (pcbColDef)
		*pcbColDef = stmt->parameters[ipar].precision;

	if (pibScale)
		*pibScale = stmt->parameters[ipar].scale;

	if (pfNullable)
		*pfNullable = pgtype_nullable(stmt, stmt->parameters[ipar].paramType);

	return SQL_SUCCESS;
}

/*		-		-		-		-		-		-		-		-		- */

/*		Sets multiple values (arrays) for the set of parameter markers. */

RETCODE SQL_API
SQLParamOptions(
				HSTMT hstmt,
				UDWORD crow,
				UDWORD FAR *pirow)
{
	static char *func = "SQLParamOptions";

	mylog("%s: entering...\n", func);

	SC_log_error(func, "Function not implemented", (StatementClass *) hstmt);
	return SQL_ERROR;
}

/*		-		-		-		-		-		-		-		-		- */

/*	This function should really talk to the dbms to determine the number of  */
/*	"parameter markers" (not bound parameters) in the statement.  But, since */
/*	Postgres doesn't support that, the driver should just count the number of markers */
/*	and return that.  The reason the driver just can't say this function is unsupported */
/*	like it does for SQLDescribeParam is that some applications don't care and try  */
/*	to call it anyway. */
/*	If the statement does not have parameters, it should just return 0. */
RETCODE SQL_API
SQLNumParams(
			 HSTMT hstmt,
			 SWORD FAR *pcpar)
{
	StatementClass *stmt = (StatementClass *) hstmt;
	char		in_quote = FALSE;
	unsigned int i;
	static char *func = "SQLNumParams";

	mylog("%s: entering...\n", func);

	if (!stmt)
	{
		SC_log_error(func, "", NULL);
		return SQL_INVALID_HANDLE;
	}

	if (pcpar)
		*pcpar = 0;
	else
	{
		SC_log_error(func, "pcpar was null", stmt);
		return SQL_ERROR;
	}


	if (!stmt->statement)
	{
		/* no statement has been allocated */
		stmt->errormsg = "SQLNumParams called with no statement ready.";
		stmt->errornumber = STMT_SEQUENCE_ERROR;
		SC_log_error(func, "", stmt);
		return SQL_ERROR;
	}
	else
	{

		for (i = 0; i < strlen(stmt->statement); i++)
		{

			if (stmt->statement[i] == '?' && !in_quote)
				(*pcpar)++;
			else
			{
				if (stmt->statement[i] == '\'')
					in_quote = (in_quote ? FALSE : TRUE);
			}
		}

		return SQL_SUCCESS;
	}
}

/********************************************************************
 *	 Bindings Implementation
 */
BindInfoClass *
create_empty_bindings(int num_columns)
{
	BindInfoClass *new_bindings;
	int			i;

	new_bindings = (BindInfoClass *) malloc(num_columns * sizeof(BindInfoClass));
	if (!new_bindings)
		return 0;

	for (i = 0; i < num_columns; i++)
	{
		new_bindings[i].buflen = 0;
		new_bindings[i].buffer = NULL;
		new_bindings[i].used = NULL;
		new_bindings[i].data_left = -1;
	}

	return new_bindings;
}

void
extend_bindings(StatementClass *stmt, int num_columns)
{
	static char *func = "extend_bindings";
	BindInfoClass *new_bindings;
	int			i;

	mylog("%s: entering ... stmt=%u, bindings_allocated=%d, num_columns=%d\n", func, stmt, stmt->bindings_allocated, num_columns);

	/* if we have too few, allocate room for more, and copy the old */
	/* entries into the new structure */
	if (stmt->bindings_allocated < num_columns)
	{

		new_bindings = create_empty_bindings(num_columns);
		if (!new_bindings)
		{
			mylog("%s: unable to create %d new bindings from %d old bindings\n", func, num_columns, stmt->bindings_allocated);

			if (stmt->bindings)
			{
				free(stmt->bindings);
				stmt->bindings = NULL;
			}
			stmt->bindings_allocated = 0;
			return;
		}

		if (stmt->bindings)
		{
			for (i = 0; i < stmt->bindings_allocated; i++)
				new_bindings[i] = stmt->bindings[i];

			free(stmt->bindings);
		}

		stmt->bindings = new_bindings;
		stmt->bindings_allocated = num_columns;

	}
	/* There is no reason to zero out extra bindings if there are */
	/* more than needed.  If an app has allocated extra bindings,  */
	/* let it worry about it by unbinding those columns. */

	/* SQLBindCol(1..) ... SQLBindCol(10...)   # got 10 bindings */
	/* SQLExecDirect(...)  # returns 5 cols */
	/* SQLExecDirect(...)  # returns 10 cols  (now OK) */

	mylog("exit extend_bindings\n");
}
