/* File:			descriptor.h
 *
 * Description:		This file contains defines and declarations that are related to
 *					the entire driver.
 *
 * Comments:		See "notice.txt" for copyright and license information.
 *
 * $Id: descriptor.h,v 1.2 2002/04/01 03:01:14 inoue Exp $
 *
 */

#ifndef __DESCRIPTOR_H__
#define __DESCRIPTOR_H__

#include "psqlodbc.h"

typedef struct
{
	COL_INFO	*col_info; /* cached SQLColumns info for this table */
	char		name[MAX_TABLE_LEN + 1];
	char		alias[MAX_TABLE_LEN + 1];
} TABLE_INFO;

typedef struct
{
	TABLE_INFO *ti;		/* resolve to explicit table names */
	int			column_size; /* precision in 2.x */
	int			decimal_digits; /* scale in 2.x */
	int			display_size;
	int			length;
	int			type;
	char		nullable;
	char		func;
	char		expr;
	char		quote;
	char		dquote;
	char		numeric;
	char		updatable;
	char		dot[MAX_TABLE_LEN + 1];
	char		name[MAX_COLUMN_LEN + 1];
	char		alias[MAX_COLUMN_LEN + 1];
} FIELD_INFO;

struct ARDFields_
{
	StatementClass	*stmt;
	int		rowset_size;
	int		bind_size;	/* size of each structure if using Row
							* Binding */
	UInt2		*row_operation_ptr;
	UInt4		*row_offset_ptr;
	BindInfoClass	*bookmark;
	BindInfoClass	*bindings;
	int		allocated;
};

struct APDFields_
{
	StatementClass	*stmt;
	int		paramset_size;
	int		param_bind_type; /* size of each structure if using Param
						* Binding */
	UInt2			*param_operation_ptr;
	UInt4			*param_offset_ptr;
	ParameterInfoClass	*parameters;
	int			allocated;
};

struct IRDFields_
{
	StatementClass	*stmt;
	UInt4		*rowsFetched;
	UInt2		*rowStatusArray;
	UInt4		nfields;
	FIELD_INFO	**fi;
};

struct IPDFields_
{
	StatementClass	*stmt;
	UInt4		*param_processed_ptr;
	UInt2		*param_status_ptr;
};

void	InitializeARDFields(ARDFields *self);
void	InitializeAPDFields(APDFields *self);
/* void	InitializeIRDFields(IRDFields *self);
void	InitializeIPDFiedls(IPDFields *self); */
void	ARDFields_free(ARDFields *self);
void	APDFields_free(APDFields *self);
void	IRDFields_free(IRDFields *self);
void	IPDFields_free(IPDFields *self);
void	ARD_unbind_cols(ARDFields *self, BOOL freeall);
void	APD_free_params(APDFields *self, char option);
#if (ODBCVER >= 0x0300)
void	Desc_set_error(SQLHDESC hdesc, int errornumber, const char * errormsg);
#endif /* ODBCVER */

#endif
