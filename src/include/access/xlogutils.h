
#ifndef XLOG_UTILS_H

#include "utils/rel.h"

extern int XLogIsOwnerOfTuple(RelFileNode hnode, ItemPointer iptr, 
					TransactionId xid, CommandId cid);
extern bool XLogIsValidTuple(RelFileNode hnode, ItemPointer iptr);

extern void XLogOpenLogRelation(void);

extern Buffer XLogReadBuffer(bool extend, Relation reln, BlockNumber blkno);
extern void XLogCloseRelationCache(void);
extern Relation XLogOpenRelation(bool redo, RmgrId rmid, RelFileNode rnode);

#endif
