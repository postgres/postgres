#include "postgres.h"
#include "access/gist.h"
#include "access/hash.h"
#include "access/heapam.h"
#include "access/nbtree.h"
#include "access/rtree.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "storage/smgr.h"

#ifdef XLOG

RmgrData   RmgrTable[] = {
{"XLOG", xlog_redo, xlog_undo, xlog_desc},
{"Transaction", xact_redo, xact_undo, xact_desc},
{"Storage", smgr_redo, smgr_undo, smgr_desc},
{"Reserved 3", NULL, NULL, NULL},
{"Reserved 4", NULL, NULL, NULL},
{"Reserved 5", NULL, NULL, NULL},
{"Reserved 6", NULL, NULL, NULL},
{"Reserved 7", NULL, NULL, NULL},
{"Reserved 8", NULL, NULL, NULL},
{"Reserved 9", NULL, NULL, NULL},
{"Heap", heap_redo, heap_undo, heap_desc},
{"Btree", btree_redo, btree_undo, btree_desc},
{"Hash", hash_redo, hash_undo, hash_desc},
{"Rtree", rtree_redo, rtree_undo, rtree_desc},
{"Gist", gist_redo, gist_undo, gist_desc}
};

#else /* not XLOG */

/*
 * This is a dummy, but don't write RmgrTable[] = {} here,
 * that's not accepted by some compilers. -- petere
 */
RmgrData   RmgrTable[1];

#endif /* not XLOG */
