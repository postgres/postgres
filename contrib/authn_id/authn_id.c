/*
 * Extension to expose the current user's authn_id.
 *
 * contrib/authn_id/authn_id.c
 */

#include "postgres.h"

#include "fmgr.h"
#include "libpq/libpq-be.h"
#include "miscadmin.h"
#include "utils/builtins.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(authn_id);

/*
 * Returns the current user's authenticated identity.
 */
Datum
authn_id(PG_FUNCTION_ARGS)
{
	if (!MyProcPort->authn_id)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(cstring_to_text(MyProcPort->authn_id));
}
