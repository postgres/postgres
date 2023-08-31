/*-------------------------------------------------------------------------
 *
 * pg_tde.c       
 *      Main file: setup GUCs, shared memory, hooks and other general-purpose
 *      routines.
 *
 * IDENTIFICATION        
 *    contrib/pg_tde/src/pg_tde.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "funcapi.h"
#include "transam/pg_tde_xact_handler.h"

PG_MODULE_MAGIC;
void        _PG_init(void);

void
 _PG_init(void)
{
    RegisterXactCallback(pg_tde_xact_callback, NULL);
    RegisterSubXactCallback(pg_tde_subxact_callback, NULL);
}

