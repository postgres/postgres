/*-------------------------------------------------------------------------
 *
 * trigger.h
 *	  prototypes for trigger.c.
 *
 *
 *-------------------------------------------------------------------------
 */
#ifndef TRIGGER_H
#define TRIGGER_H

#include "nodes/execnodes.h"
#include "nodes/parsenodes.h"

typedef uint32 TriggerEvent;

typedef struct TriggerData
{
	TriggerEvent tg_event;
	Relation	tg_relation;
	HeapTuple	tg_trigtuple;
	HeapTuple	tg_newtuple;
	Trigger    *tg_trigger;
} TriggerData;

extern DLLIMPORT TriggerData *CurrentTriggerData;

#define TRIGGER_EVENT_INSERT			0x00000000
#define TRIGGER_EVENT_DELETE			0x00000001
#define TRIGGER_EVENT_UPDATE			0x00000002
#define TRIGGER_EVENT_OPMASK			0x00000003
#define TRIGGER_EVENT_ROW				0x00000004
#define TRIGGER_EVENT_BEFORE			0x00000008

#define TRIGGER_FIRED_BY_INSERT(event)	\
		(((TriggerEvent) (event) & TRIGGER_EVENT_OPMASK) == \
												TRIGGER_EVENT_INSERT)

#define TRIGGER_FIRED_BY_DELETE(event)	\
		(((TriggerEvent) (event) & TRIGGER_EVENT_OPMASK) == \
												TRIGGER_EVENT_DELETE)

#define TRIGGER_FIRED_BY_UPDATE(event)	\
		(((TriggerEvent) (event) & TRIGGER_EVENT_OPMASK) == \
												TRIGGER_EVENT_UPDATE)

#define TRIGGER_FIRED_FOR_ROW(event)			\
		((TriggerEvent) (event) & TRIGGER_EVENT_ROW)

#define TRIGGER_FIRED_FOR_STATEMENT(event)		\
		(!TRIGGER_FIRED_FOR_ROW (event))

#define TRIGGER_FIRED_BEFORE(event)				\
		((TriggerEvent) (event) & TRIGGER_EVENT_BEFORE)

#define TRIGGER_FIRED_AFTER(event)				\
		(!TRIGGER_FIRED_BEFORE (event))


extern void CreateTrigger(CreateTrigStmt *stmt);
extern void DropTrigger(DropTrigStmt *stmt);
extern void RelationRemoveTriggers(Relation rel);

extern HeapTuple ExecBRInsertTriggers(Relation rel, HeapTuple tuple);
extern void ExecARInsertTriggers(Relation rel, HeapTuple tuple);
extern bool ExecBRDeleteTriggers(EState *estate, ItemPointer tupleid);
extern void ExecARDeleteTriggers(EState *estate, ItemPointer tupleid);
extern HeapTuple ExecBRUpdateTriggers(EState *estate, ItemPointer tupleid, HeapTuple tuple);
extern void ExecARUpdateTriggers(EState *estate, ItemPointer tupleid, HeapTuple tuple);

#endif	 /* TRIGGER_H */
