
/* Module:          bind.c
 *
 * Description:     This module contains routines related to binding 
 *                  columns and parameters.
 *
 * Classes:         BindInfoClass, ParameterInfoClass
 *
 * API functions:   SQLBindParameter, SQLBindCol, SQLDescribeParam, SQLNumParams,
 *                  SQLParamOptions(NI)
 *
 * Comments:        See "notice.txt" for copyright and license information.
 *
 */
#include "bind.h"
#include "environ.h"
#include "statement.h"
#include "qresult.h"
#include "pgtypes.h"
#include <stdlib.h>
#include <malloc.h>
#include <sql.h>
#include <sqlext.h>

//      Bind parameters on a statement handle

RETCODE SQL_API SQLBindParameter(
        HSTMT      hstmt,
        UWORD      ipar,
        SWORD      fParamType,
        SWORD      fCType,
        SWORD      fSqlType,
        UDWORD     cbColDef,
        SWORD      ibScale,
        PTR        rgbValue,
        SDWORD     cbValueMax,
        SDWORD FAR *pcbValue)
{
StatementClass *stmt = (StatementClass *) hstmt;

	if( ! stmt)
		return SQL_INVALID_HANDLE;

	if(stmt->parameters_allocated < ipar) {
		ParameterInfoClass *old_parameters;
		int i, old_parameters_allocated;

		old_parameters = stmt->parameters;
		old_parameters_allocated = stmt->parameters_allocated;

		stmt->parameters = (ParameterInfoClass *) malloc(sizeof(ParameterInfoClass)*(ipar));
		if ( ! stmt->parameters) {
			stmt->errornumber = STMT_NO_MEMORY_ERROR;
			stmt->errormsg = "Could not allocate memory for statement parameters";
			return SQL_ERROR;
		}

		stmt->parameters_allocated = ipar;

		// copy the old parameters over
		for(i = 0; i < old_parameters_allocated; i++) {
		// a structure copy should work
			stmt->parameters[i] = old_parameters[i];
		}

		// get rid of the old parameters, if there were any
		if(old_parameters)
			free(old_parameters);

		// zero out the newly allocated parameters (in case they skipped some,
		// so we don't accidentally try to use them later)
		for(; i < stmt->parameters_allocated; i++) {
			stmt->parameters[i].buflen = 0;
			stmt->parameters[i].buffer = 0;
			stmt->parameters[i].used = 0;
			stmt->parameters[i].paramType = 0;
			stmt->parameters[i].CType = 0;
			stmt->parameters[i].SQLType = 0;
			stmt->parameters[i].precision = 0;
			stmt->parameters[i].scale = 0;
			stmt->parameters[i].data_at_exec = FALSE;
			stmt->parameters[i].EXEC_used = NULL;
			stmt->parameters[i].EXEC_buffer = NULL;
		}
	}

	ipar--;		/* use zero based column numbers for the below part */

	// store the given info
	stmt->parameters[ipar].buflen = cbValueMax;
	stmt->parameters[ipar].buffer = rgbValue;
	stmt->parameters[ipar].used = pcbValue;
	stmt->parameters[ipar].paramType = fParamType;
	stmt->parameters[ipar].CType = fCType;
	stmt->parameters[ipar].SQLType = fSqlType;
	stmt->parameters[ipar].precision = cbColDef;
	stmt->parameters[ipar].scale = ibScale;

	/*	If rebinding a parameter that had data-at-exec stuff in it,
		then free that stuff
	*/
	if (stmt->parameters[ipar].EXEC_used) {
		free(stmt->parameters[ipar].EXEC_used);
		stmt->parameters[ipar].EXEC_used = NULL;
	}

	if (stmt->parameters[ipar].EXEC_buffer) {
		free(stmt->parameters[ipar].EXEC_buffer);
		stmt->parameters[ipar].EXEC_buffer = NULL;
	}

	if (pcbValue && *pcbValue <= SQL_LEN_DATA_AT_EXEC_OFFSET)
		stmt->parameters[ipar].data_at_exec = TRUE;
	else
		stmt->parameters[ipar].data_at_exec = FALSE;


	return SQL_SUCCESS;
}

//      -       -       -       -       -       -       -       -       -

//      Associate a user-supplied buffer with a database column.
RETCODE SQL_API SQLBindCol(
        HSTMT      hstmt,
        UWORD      icol,
        SWORD      fCType,
        PTR        rgbValue,
        SDWORD     cbValueMax,
        SDWORD FAR *pcbValue)
{
StatementClass *stmt = (StatementClass *) hstmt;
Int2 numcols;
    
mylog("**** SQLBindCol: stmt = %u, icol = %d\n", stmt, icol);

	if ( ! stmt)
		return SQL_INVALID_HANDLE;

	if (icol < 1) {
		/* currently we do not support bookmarks */
		stmt->errormsg = "Bookmarks are not currently supported.";
		stmt->errornumber = STMT_NOT_IMPLEMENTED_ERROR;
		return SQL_ERROR;
	}

	icol--;		/* use zero based col numbers */

	SC_clear_error(stmt);
    
	if( ! stmt->result) {
		stmt->errormsg = "Can't bind columns with a NULL query result structure.";
		stmt->errornumber = STMT_SEQUENCE_ERROR;
		return SQL_ERROR;
	}

	if( stmt->status == STMT_EXECUTING) {
		stmt->errormsg = "Can't bind columns while statement is still executing.";
		stmt->errornumber = STMT_SEQUENCE_ERROR;
		return SQL_ERROR;
	}

	numcols = QR_NumResultCols(stmt->result);

	mylog("SQLBindCol: numcols = %d\n", numcols);

	if (icol >= numcols) {
		stmt->errornumber = STMT_COLNUM_ERROR;
		stmt->errormsg = "Column number too big";
		return SQL_ERROR;
	}

	if ( ! stmt->bindings) {
		stmt->errormsg = "Bindings were not allocated properly.";
		stmt->errornumber = STMT_SEQUENCE_ERROR;
		return SQL_ERROR;
	}

	if ((cbValueMax == 0) || (rgbValue == NULL)) {
		/* we have to unbind the column */
		stmt->bindings[icol].buflen = 0;
		stmt->bindings[icol].buffer = NULL;
		stmt->bindings[icol].used =   NULL;
		stmt->bindings[icol].returntype = SQL_C_CHAR;
	} else {
		/* ok, bind that column */
		stmt->bindings[icol].buflen     = cbValueMax;
		stmt->bindings[icol].buffer     = rgbValue;
		stmt->bindings[icol].used       = pcbValue;
		stmt->bindings[icol].returntype = fCType;
	}

	return SQL_SUCCESS;
}

//      -       -       -       -       -       -       -       -       -

//      Returns the description of a parameter marker.

RETCODE SQL_API SQLDescribeParam(
        HSTMT      hstmt,
        UWORD      ipar,
        SWORD  FAR *pfSqlType,
        UDWORD FAR *pcbColDef,
        SWORD  FAR *pibScale,
        SWORD  FAR *pfNullable)
{
StatementClass *stmt = (StatementClass *) hstmt;

	if( ! stmt)
		return SQL_INVALID_HANDLE;

	if( (ipar < 1) || (ipar > stmt->parameters_allocated) ) {
		stmt->errormsg = "Invalid parameter number for SQLDescribeParam.";
		stmt->errornumber = STMT_BAD_PARAMETER_NUMBER_ERROR;
		return SQL_ERROR;
	}

	ipar--;

	if(pfSqlType)
		*pfSqlType = stmt->parameters[ipar].SQLType;

	if(pcbColDef)
		*pcbColDef = stmt->parameters[ipar].precision;

	if(pibScale)
		*pibScale = stmt->parameters[ipar].scale;

	if(pfNullable)
		*pfNullable = pgtype_nullable(stmt->parameters[ipar].paramType);

	return SQL_SUCCESS;
}

//      -       -       -       -       -       -       -       -       -

//      Sets multiple values (arrays) for the set of parameter markers.

RETCODE SQL_API SQLParamOptions(
        HSTMT      hstmt,
        UDWORD     crow,
        UDWORD FAR *pirow)
{
	return SQL_ERROR;
}

//      -       -       -       -       -       -       -       -       -

//      Returns the number of parameter markers.

RETCODE SQL_API SQLNumParams(
        HSTMT      hstmt,
        SWORD  FAR *pcpar)
{
StatementClass *stmt = (StatementClass *) hstmt;
unsigned int i;

	// I guess this is the number of actual parameter markers
	// in the statement, not the number of parameters that are bound.
	// why does this have to be driver-specific?

	if(!stmt)
		return SQL_INVALID_HANDLE;

	if(!stmt->statement) {
		// no statement has been allocated
		*pcpar = 0;
		stmt->errormsg = "SQLNumParams called with no statement ready.";
		stmt->errornumber = STMT_SEQUENCE_ERROR;
		return SQL_ERROR;
	} else {
		*pcpar = 0;
		for(i=0; i < strlen(stmt->statement); i++) {
			if(stmt->statement[i] == '?')
				(*pcpar)++;
		}

		return SQL_SUCCESS;
	}
}

/********************************************************************
 *   Bindings Implementation
 */
BindInfoClass *
create_empty_bindings(int num_columns)
{
BindInfoClass *new_bindings;
int i;

	new_bindings = (BindInfoClass *)malloc(num_columns * sizeof(BindInfoClass));
	if(!new_bindings) {
		return 0;
	}

	for(i=0; i < num_columns; i++) {
		new_bindings[i].buflen = 0;
		new_bindings[i].buffer = NULL;
		new_bindings[i].used = NULL;
	}

	return new_bindings;
}

void
extend_bindings(StatementClass *stmt, int num_columns)
{
BindInfoClass *new_bindings;
int i;

	mylog("in extend_bindings\n");

	/* if we have too few, allocate room for more, and copy the old */
	/* entries into the new structure */
	if(stmt->bindings_allocated < num_columns) {

		new_bindings = create_empty_bindings(num_columns);

		if(stmt->bindings) {
			for(i=0; i<stmt->bindings_allocated; i++)
				new_bindings[i] = stmt->bindings[i];

			free(stmt->bindings);
		}

		stmt->bindings = new_bindings;		// null indicates error

    } else {
	/* if we have too many, make sure the extra ones are emptied out */
	/* so we don't accidentally try to use them for anything */
		for(i = num_columns; i < stmt->bindings_allocated; i++) {
			stmt->bindings[i].buflen = 0;
			stmt->bindings[i].buffer = NULL;
			stmt->bindings[i].used = NULL;
		}
	}

	mylog("exit extend_bindings\n");
}
