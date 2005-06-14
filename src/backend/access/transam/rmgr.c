/*
 * rmgr.c
 *
 * Resource managers definition
 *
 * $PostgreSQL: pgsql/src/backend/access/transam/rmgr.c,v 1.20 2005/06/14 11:45:14 teodor Exp $
 */
#include "postgres.h"

#include "access/clog.h"
#include "access/gist_private.h"
#include "access/hash.h"
#include "access/heapam.h"
#include "access/multixact.h"
#include "access/nbtree.h"
#include "access/rtree.h"
#include "access/xact.h"
#include "access/xlog_internal.h"
#include "commands/dbcommands.h"
#include "commands/sequence.h"
#include "commands/tablespace.h"
#include "storage/smgr.h"


const RmgrData RmgrTable[RM_MAX_ID + 1] = {
	{"XLOG", xlog_redo, xlog_desc, NULL, NULL},
	{"Transaction", xact_redo, xact_desc, NULL, NULL},
	{"Storage", smgr_redo, smgr_desc, NULL, NULL},
	{"CLOG", clog_redo, clog_desc, NULL, NULL},
	{"Database", dbase_redo, dbase_desc, NULL, NULL},
	{"Tablespace", tblspc_redo, tblspc_desc, NULL, NULL},
	{"MultiXact", multixact_redo, multixact_desc, NULL, NULL},
	{"Reserved 7", NULL, NULL, NULL, NULL},
	{"Reserved 8", NULL, NULL, NULL, NULL},
	{"Reserved 9", NULL, NULL, NULL, NULL},
	{"Heap", heap_redo, heap_desc, NULL, NULL},
	{"Btree", btree_redo, btree_desc, btree_xlog_startup, btree_xlog_cleanup},
	{"Hash", hash_redo, hash_desc, NULL, NULL},
	{"Rtree", rtree_redo, rtree_desc, NULL, NULL},
	{"Gist", gist_redo, gist_desc, gist_xlog_startup, gist_xlog_cleanup},
	{"Sequence", seq_redo, seq_desc, NULL, NULL}
};
