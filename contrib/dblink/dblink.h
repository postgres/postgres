/*
 * dblink.h
 *
 * Functions returning results from a remote database
 *
 * Copyright (c) Joseph Conway <mail@joeconway.com>, 2001, 2002,
 * ALL RIGHTS RESERVED;
 *
 * Permission to use, copy, modify, and distribute this software and its
 * documentation for any purpose, without fee, and without a written agreement
 * is hereby granted, provided that the above copyright notice and this
 * paragraph and the following two paragraphs appear in all copies.
 *
 * IN NO EVENT SHALL THE AUTHOR OR DISTRIBUTORS BE LIABLE TO ANY PARTY FOR
 * DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES, INCLUDING
 * LOST PROFITS, ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS
 * DOCUMENTATION, EVEN IF THE AUTHOR OR DISTRIBUTORS HAVE BEEN ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * THE AUTHOR AND DISTRIBUTORS SPECIFICALLY DISCLAIMS ANY WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS FOR A PARTICULAR PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS
 * ON AN "AS IS" BASIS, AND THE AUTHOR AND DISTRIBUTORS HAS NO OBLIGATIONS TO
 * PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.
 *
 */

#ifndef DBLINK_H
#define DBLINK_H

#include <string.h>
#include "postgres.h"
#include "libpq-fe.h"
#include "libpq-int.h"
#include "fmgr.h"
#include "access/tupdesc.h"
#include "access/heapam.h"
#include "catalog/catname.h"
#include "catalog/pg_index.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "executor/spi.h"
#include "lib/stringinfo.h"
#include "nodes/nodes.h"
#include "nodes/execnodes.h"
#include "nodes/pg_list.h"
#include "parser/parse_type.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/array.h"
#include "utils/syscache.h"

#ifdef NamespaceRelationName
#include "catalog/namespace.h"
#endif   /* NamespaceRelationName */

/*
 * Max SQL statement size
 */
#define DBLINK_MAX_SQLSTATE_SIZE		16384

/*
 * This struct holds the results of the remote query.
 * Use fn_extra to hold a pointer to it across calls
 */
typedef struct
{
	/*
	 * last tuple number accessed
	 */
	int			tup_num;

	/*
	 * resource index number for this context
	 */
	int			res_id_index;

	/*
	 * the actual query results
	 */
	PGresult   *res;
}	dblink_results;


/*
 * This struct holds results in the form of an array.
 * Use fn_extra to hold a pointer to it across calls
 */
typedef struct
{
	/*
	 * elem being accessed
	 */
	int			elem_num;

	/*
	 * number of elems
	 */
	int			num_elems;

	/*
	 * the actual array
	 */
	void		*res;

}	dblink_array_results;

/*
 * External declarations
 */
extern Datum dblink(PG_FUNCTION_ARGS);
extern Datum dblink_tok(PG_FUNCTION_ARGS);
extern Datum dblink_strtok(PG_FUNCTION_ARGS);
extern Datum dblink_get_pkey(PG_FUNCTION_ARGS);
extern Datum dblink_last_oid(PG_FUNCTION_ARGS);
extern Datum dblink_build_sql_insert(PG_FUNCTION_ARGS);
extern Datum dblink_build_sql_delete(PG_FUNCTION_ARGS);
extern Datum dblink_build_sql_update(PG_FUNCTION_ARGS);
extern Datum dblink_current_query(PG_FUNCTION_ARGS);
extern Datum dblink_replace_text(PG_FUNCTION_ARGS);

extern char	*debug_query_string;

#endif   /* DBLINK_H */
