/* ----------
 * ri_triggers.c
 *
 *	Generic trigger procedures for referential integrity constraint
 *	checks.
 *
 *	1999 Jan Wieck
 *
 * $Header: /cvsroot/pgsql/src/backend/utils/adt/ri_triggers.c,v 1.1 1999/09/30 14:54:22 wieck Exp $
 *
 * ----------
 */

#include "postgres.h"
#include "fmgr.h"

#include "access/heapam.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/trigger.h"
#include "executor/spi.h"
#include "utils/builtins.h"
#include "utils/syscache.h"

/* ----------
 * RI_FKey_check_ins -
 *
 *	Check foreign key existance at insert event on FK table.
 * ----------
 */
HeapTuple
RI_FKey_check_ins (FmgrInfo *proinfo)
{
	CurrentTriggerData	= NULL;

	elog(NOTICE, "RI_FKey_check_ins() called\n");
	return NULL;
}


/* ----------
 * RI_FKey_check_upd -
 *
 *	Check foreign key existance at update event on FK table.
 * ----------
 */
HeapTuple
RI_FKey_check_upd (FmgrInfo *proinfo)
{
	CurrentTriggerData	= NULL;

	elog(NOTICE, "RI_FKey_check_upd() called\n");
	return NULL;
}


/* ----------
 * RI_FKey_cascade_del -
 *
 *	Cascaded delete foreign key references at delete event on PK table.
 * ----------
 */
HeapTuple
RI_FKey_cascade_del (FmgrInfo *proinfo)
{
	CurrentTriggerData	= NULL;

	elog(NOTICE, "RI_FKey_cascade_del() called\n");
	return NULL;
}


/* ----------
 * RI_FKey_cascade_upd -
 *
 *	Cascaded update/delete foreign key references at update event on PK table.
 * ----------
 */
HeapTuple
RI_FKey_cascade_upd (FmgrInfo *proinfo)
{
	CurrentTriggerData	= NULL;

	elog(NOTICE, "RI_FKey_cascade_upd() called\n");
	return NULL;
}


/* ----------
 * RI_FKey_restrict_del -
 *
 *	Restrict delete from PK table to rows unreferenced by foreign key.
 * ----------
 */
HeapTuple
RI_FKey_restrict_del (FmgrInfo *proinfo)
{
	CurrentTriggerData	= NULL;

	elog(NOTICE, "RI_FKey_restrict_del() called\n");
	return NULL;
}


/* ----------
 * RI_FKey_restrict_upd -
 *
 *	Restrict update of PK to rows unreferenced by foreign key.
 * ----------
 */
HeapTuple
RI_FKey_restrict_upd (FmgrInfo *proinfo)
{
	CurrentTriggerData	= NULL;

	elog(NOTICE, "RI_FKey_restrict_upd() called\n");
	return NULL;
}


/* ----------
 * RI_FKey_setnull_del -
 *
 *	Set foreign key references to NULL values at delete event on PK table.
 * ----------
 */
HeapTuple
RI_FKey_setnull_del (FmgrInfo *proinfo)
{
	CurrentTriggerData	= NULL;

	elog(NOTICE, "RI_FKey_setnull_del() called\n");
	return NULL;
}


/* ----------
 * RI_FKey_setnull_upd -
 *
 *	Set foreign key references to NULL at update event on PK table.
 * ----------
 */
HeapTuple
RI_FKey_setnull_upd (FmgrInfo *proinfo)
{
	CurrentTriggerData	= NULL;

	elog(NOTICE, "RI_FKey_setnull_upd() called\n");
	return NULL;
}


/* ----------
 * RI_FKey_setdefault_del -
 *
 *	Set foreign key references to defaults at delete event on PK table.
 * ----------
 */
HeapTuple
RI_FKey_setdefault_del (FmgrInfo *proinfo)
{
	CurrentTriggerData	= NULL;

	elog(NOTICE, "RI_FKey_setdefault_del() called\n");
	return NULL;
}


/* ----------
 * RI_FKey_setdefault_upd -
 *
 *	Set foreign key references to defaults at update event on PK table.
 * ----------
 */
HeapTuple
RI_FKey_setdefault_upd (FmgrInfo *proinfo)
{
	CurrentTriggerData	= NULL;

	elog(NOTICE, "RI_FKey_setdefault_upd() called\n");
	return NULL;
}


