
/* File:			pgtypes.h
 *
 * Description:		See "pgtypes.c"
 *
 * Comments:		See "notice.txt" for copyright and license information.
 *
 */

#ifndef __PGTYPES_H__
#define __PGTYPES_H__

#include "psqlodbc.h"

/* the type numbers are defined by the OID's of the types' rows */
/* in table pg_type */


#if 0
#define PG_TYPE_LO			????/* waiting for permanent type */
#endif

#define PG_TYPE_BOOL		 16
#define PG_TYPE_BYTEA		 17
#define PG_TYPE_CHAR		 18
#define PG_TYPE_NAME		 19
#define PG_TYPE_INT8		 20
#define PG_TYPE_INT2		 21
#define PG_TYPE_INT2VECTOR	 22
#define PG_TYPE_INT4		 23
#define PG_TYPE_REGPROC		 24
#define PG_TYPE_TEXT		 25
#define PG_TYPE_OID			 26
#define PG_TYPE_TID			 27
#define PG_TYPE_XID			 28
#define PG_TYPE_CID			 29
#define PG_TYPE_OIDVECTOR	 30
#define PG_TYPE_SET			 32
#define PG_TYPE_CHAR2		409
#define PG_TYPE_CHAR4		410
#define PG_TYPE_CHAR8		411
#define PG_TYPE_POINT		600
#define PG_TYPE_LSEG		601
#define PG_TYPE_PATH		602
#define PG_TYPE_BOX			603
#define PG_TYPE_POLYGON		604
#define PG_TYPE_FILENAME	605
#define PG_TYPE_FLOAT4		700
#define PG_TYPE_FLOAT8		701
#define PG_TYPE_ABSTIME		702
#define PG_TYPE_RELTIME		703
#define PG_TYPE_TINTERVAL	704
#define PG_TYPE_UNKNOWN		705
#define PG_TYPE_MONEY		790
#define PG_TYPE_OIDINT2		810
#define PG_TYPE_OIDINT4		910
#define PG_TYPE_OIDNAME		911
#define PG_TYPE_BPCHAR	   1042
#define PG_TYPE_VARCHAR    1043
#define PG_TYPE_DATE	   1082
#define PG_TYPE_TIME	   1083
#define PG_TYPE_DATETIME   1184
#define PG_TYPE_TIMESTAMP  1296
#define PG_TYPE_NUMERIC    1700

/* extern Int4 pgtypes_defined[]; */
extern Int2 sqlTypes[];

/*	Defines for pgtype_precision */
#define PG_STATIC		-1

Int4		sqltype_to_pgtype(Int2 fSqlType);

Int2		pgtype_to_sqltype(StatementClass *stmt, Int4 type);
Int2		pgtype_to_ctype(StatementClass *stmt, Int4 type);
char	   *pgtype_to_name(StatementClass *stmt, Int4 type);

/*	These functions can use static numbers or result sets(col parameter) */
Int4		pgtype_precision(StatementClass *stmt, Int4 type, int col, int handle_unknown_size_as);
Int4		pgtype_display_size(StatementClass *stmt, Int4 type, int col, int handle_unknown_size_as);
Int4		pgtype_length(StatementClass *stmt, Int4 type, int col, int handle_unknown_size_as);

Int2		pgtype_scale(StatementClass *stmt, Int4 type, int col);
Int2		pgtype_radix(StatementClass *stmt, Int4 type);
Int2		pgtype_nullable(StatementClass *stmt, Int4 type);
Int2		pgtype_auto_increment(StatementClass *stmt, Int4 type);
Int2		pgtype_case_sensitive(StatementClass *stmt, Int4 type);
Int2		pgtype_money(StatementClass *stmt, Int4 type);
Int2		pgtype_searchable(StatementClass *stmt, Int4 type);
Int2		pgtype_unsigned(StatementClass *stmt, Int4 type);
char	   *pgtype_literal_prefix(StatementClass *stmt, Int4 type);
char	   *pgtype_literal_suffix(StatementClass *stmt, Int4 type);
char	   *pgtype_create_params(StatementClass *stmt, Int4 type);

Int2		sqltype_to_default_ctype(Int2 sqltype);

#endif
