/*
 * $PostgreSQL: pgsql/contrib/btree_gist/btree_gist.h,v 1.9 2009/06/11 14:48:50 momjian Exp $
 */
#ifndef __BTREE_GIST_H__
#define __BTREE_GIST_H__

#include "postgres.h"
#include "access/gist.h"
#include "access/itup.h"
#include "access/nbtree.h"

/* indexed types */

enum gbtree_type
{
	gbt_t_var,
	gbt_t_int2,
	gbt_t_int4,
	gbt_t_int8,
	gbt_t_float4,
	gbt_t_float8,
	gbt_t_numeric,
	gbt_t_ts,
	gbt_t_cash,
	gbt_t_oid,
	gbt_t_time,
	gbt_t_date,
	gbt_t_intv,
	gbt_t_macad,
	gbt_t_text,
	gbt_t_bpchar,
	gbt_t_bytea,
	gbt_t_bit,
	gbt_t_inet
};



/*
 * Generic btree functions
 */

Datum		gbtreekey_in(PG_FUNCTION_ARGS);

Datum		gbtreekey_out(PG_FUNCTION_ARGS);

#endif
