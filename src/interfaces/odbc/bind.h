/* File:			bind.h
 *
 * Description:		See "bind.c"
 *
 * Comments:		See "notice.txt" for copyright and license information.
 *
 */

#ifndef __BIND_H__
#define __BIND_H__

#include "psqlodbc.h"

/*
 * BindInfoClass -- stores information about a bound column
 */
struct BindInfoClass_
{
	Int4		buflen;			/* size of buffer */
	Int4		data_left;		/* amount of data left to read
								 * (SQLGetData) */
	char	   *buffer;			/* pointer to the buffer */
	Int4	   *used;			/* used space in the buffer (for strings
								 * not counting the '\0') */
	char	   *ttlbuf;			/* to save the large result */
	Int4		ttlbuflen;		/* the buffer length */
	Int2		returntype;		/* kind of conversion to be applied when
								 * returning (SQL_C_DEFAULT,
								 * SQL_C_CHAR...) */
	Int2	precision;		/* the precision for numeric or timestamp type */
	Int2	scale;			/* the scale for numeric type */
};

/*
 * ParameterInfoClass -- stores information about a bound parameter
 */
struct ParameterInfoClass_
{
	Int4		buflen;
	char	   *buffer;
	Int4	   *used;
	Int2		paramType;
	Int2		CType;
	Int2		SQLType;
	Int2		decimal_digits;
	UInt4		column_size;
	Oid			lobj_oid;
	Int4	   *EXEC_used;		/* amount of data OR the oid of the large
								 * object */
	char	   *EXEC_buffer;	/* the data or the FD of the large object */
	Int2		precision;	/* the precision for numeric or timestamp type */
	Int2		scale;		/* the scale for numeric type */
	char		data_at_exec;
};

BindInfoClass *create_empty_bindings(int num_columns);
void	extend_column_bindings(ARDFields *opts, int num_columns);
void	reset_a_column_binding(ARDFields *opts, int icol);
void	extend_parameter_bindings(APDFields *opts, int num_columns);
void	reset_a_parameter_binding(APDFields *opts, int ipar);

#endif
