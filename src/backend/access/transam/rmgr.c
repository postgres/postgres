#include "postgres.h"
#include "access/xlog.h"

#ifdef XLOG
extern void xlog_redo(XLogRecPtr lsn, XLogRecord *rptr);
extern void xlog_undo(XLogRecPtr lsn, XLogRecord *rptr);
extern void xlog_desc(char *buf, uint8 xl_info, char* rec);

extern void xact_redo(XLogRecPtr lsn, XLogRecord *rptr);
extern void xact_undo(XLogRecPtr lsn, XLogRecord *rptr);
extern void xact_desc(char *buf, uint8 xl_info, char* rec);

extern void smgr_redo(XLogRecPtr lsn, XLogRecord *rptr);
extern void smgr_undo(XLogRecPtr lsn, XLogRecord *rptr);
extern void smgr_desc(char *buf, uint8 xl_info, char* rec);

extern void heap_redo(XLogRecPtr lsn, XLogRecord *rptr);
extern void heap_undo(XLogRecPtr lsn, XLogRecord *rptr);
extern void heap_desc(char *buf, uint8 xl_info, char* rec);

extern void btree_redo(XLogRecPtr lsn, XLogRecord *rptr);
extern void btree_undo(XLogRecPtr lsn, XLogRecord *rptr);
extern void btree_desc(char *buf, uint8 xl_info, char* rec);

extern void hash_redo(XLogRecPtr lsn, XLogRecord *rptr);
extern void hash_undo(XLogRecPtr lsn, XLogRecord *rptr);
extern void hash_desc(char *buf, uint8 xl_info, char* rec);

extern void rtree_redo(XLogRecPtr lsn, XLogRecord *rptr);
extern void rtree_undo(XLogRecPtr lsn, XLogRecord *rptr);
extern void rtree_desc(char *buf, uint8 xl_info, char* rec);

extern void gist_redo(XLogRecPtr lsn, XLogRecord *rptr);
extern void gist_undo(XLogRecPtr lsn, XLogRecord *rptr);
extern void gist_desc(char *buf, uint8 xl_info, char* rec);

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

#else

RmgrData   RmgrTable[] = {};

#endif
