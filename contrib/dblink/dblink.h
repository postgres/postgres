/*
 * dblink.h
 *
 * Functions returning results from a remote database
 *
 * Joe Conway <mail@joeconway.com>
 *
 * Copyright (c) 2001, 2002 by PostgreSQL Global Development Group
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
 * External declarations
 */
/* deprecated */
extern Datum dblink(PG_FUNCTION_ARGS);
extern Datum dblink_tok(PG_FUNCTION_ARGS);

/* supported */
extern Datum dblink_connect(PG_FUNCTION_ARGS);
extern Datum dblink_disconnect(PG_FUNCTION_ARGS);
extern Datum dblink_open(PG_FUNCTION_ARGS);
extern Datum dblink_close(PG_FUNCTION_ARGS);
extern Datum dblink_fetch(PG_FUNCTION_ARGS);
extern Datum dblink_record(PG_FUNCTION_ARGS);
extern Datum dblink_exec(PG_FUNCTION_ARGS);
extern Datum dblink_get_pkey(PG_FUNCTION_ARGS);
extern Datum dblink_last_oid(PG_FUNCTION_ARGS);
extern Datum dblink_build_sql_insert(PG_FUNCTION_ARGS);
extern Datum dblink_build_sql_delete(PG_FUNCTION_ARGS);
extern Datum dblink_build_sql_update(PG_FUNCTION_ARGS);
extern Datum dblink_current_query(PG_FUNCTION_ARGS);

extern char	*debug_query_string;

#endif   /* DBLINK_H */
