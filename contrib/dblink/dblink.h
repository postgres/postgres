/*
 * dblink.h
 *
 * Functions returning results from a remote database
 *
 * Copyright (c) Joseph Conway <joe.conway@mail.com>, 2001;
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
#include "executor/executor.h"
#include "nodes/nodes.h"
#include "nodes/execnodes.h"
#include "utils/builtins.h"

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
	 * the actual query results
	 */
	PGresult   *res;

}	dblink_results;

/*
 * External declarations
 */
extern Datum dblink(PG_FUNCTION_ARGS);
extern Datum dblink_tok(PG_FUNCTION_ARGS);

/*
 * Internal declarations
 */
dblink_results *init_dblink_results(MemoryContext fn_mcxt);

#endif   /* DBLINK_H */
