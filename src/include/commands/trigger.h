/*-------------------------------------------------------------------------
 *
 * trigger.h--
 *    prototypes for trigger.c.
 *
 *
 *-------------------------------------------------------------------------
 */
#ifndef TRIGGER_H
#define TRIGGER_H

#include "access/tupdesc.h"
#include "access/htup.h"
#include "utils/rel.h"

typedef uint32 TriggerAction;

#define TRIGGER_ACTION_INSERT		0x00000000  
#define TRIGGER_ACTION_DELETE		0x00000001   
#define TRIGGER_ACTION_UPDATE		0x00000010
#define TRIGGER_ACTION_OPMASK		0x00000011
#define TRIGGER_ACTION_ROW		4

#define TRIGGER_FIRED_BY_INSERT (action)	\
	(((TriggerAction) action & TRIGGER_ACTION_OPMASK) == \
						TRIGGER_ACTION_INSERT)

#define TRIGGER_FIRED_BY_DELETE (action)	\
	(((TriggerAction) action & TRIGGER_ACTION_OPMASK) == \
						TRIGGER_ACTION_DELETE)

#define TRIGGER_FIRED_BY_UPDATE (action)	\
	(((TriggerAction) action & TRIGGER_ACTION_OPMASK) == \
						TRIGGER_ACTION_UPDATE)

#define TRIGGER_FIRED_FOR_ROW (action)		\
	((TriggerAction) action & TRIGGER_ACTION_ROW)

#define TRIGGER_FIRED_FOR_STATEMENT (action)	\
	(!TRIGGER_FIRED_FOR_ROW (action))


extern void CreateTrigger (CreateTrigStmt *stmt);
extern void DropTrigger (DropTrigStmt *stmt);

extern HeapTuple ExecBRInsertTriggers (Relation rel, HeapTuple tuple);
extern void ExecARInsertTriggers (Relation rel, HeapTuple tuple);
extern bool ExecBRDeleteTriggers (Relation rel, ItemPointer tupleid);
extern void ExecARDeleteTriggers (Relation rel, ItemPointer tupleid);
extern HeapTuple ExecBRUpdateTriggers (Relation rel, ItemPointer tupleid, HeapTuple tuple);
extern void ExecARUpdateTriggers (Relation rel, ItemPointer tupleid, HeapTuple tuple);

#endif	/* TRIGGER_H */
