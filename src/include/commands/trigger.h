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

#define TRIGGER_DEFERRED_DONE			0x00000010
#define TRIGGER_DEFERRED_CANCELED		0x00000020
#define TRIGGER_DEFERRED_DEFERRABLE		0x00000040
#define TRIGGER_DEFERRED_INITDEFERRED	0x00000080
#define TRIGGER_DEFERRED_HAS_BEFORE		0x00000100
#define TRIGGER_DEFERRED_ROW_INSERTED	0x00000200
#define TRIGGER_DEFERRED_KEY_CHANGED	0x00000400
#define TRIGGER_DEFERRED_MASK			0x000007F0

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



/* ----------
 * Deferred trigger stuff
 * ----------
 */
typedef struct DeferredTriggerStatusData {
	Oid				dts_tgoid;
	bool			dts_tgisdeferred;
} DeferredTriggerStatusData;
typedef struct DeferredTriggerStatusData *DeferredTriggerStatus;


typedef struct DeferredTriggerEventItem {
	Oid				dti_tgoid;
	int32			dti_state;
} DeferredTriggerEventItem;


typedef struct DeferredTriggerEventData {
	int32			dte_event;
	Oid				dte_relid;
	ItemPointerData	dte_oldctid;
	ItemPointerData	dte_newctid;
	int32			dte_n_items;
	DeferredTriggerEventItem	dte_item[1];
} DeferredTriggerEventData;
typedef struct DeferredTriggerEventData *DeferredTriggerEvent;


extern int  DeferredTriggerInit(void);
extern void DeferredTriggerBeginXact(void);
extern void DeferredTriggerEndQuery(void);
extern void DeferredTriggerEndXact(void);
extern void DeferredTriggerAbortXact(void);

extern void DeferredTriggerSetState(ConstraintsSetStmt *stmt);

extern void DeferredTriggerSaveEvent(Relation rel, int event,
					HeapTuple oldtup, HeapTuple newtup);


/*
 * in utils/adt/ri_triggers.c
 *
 */
extern bool RI_FKey_keyequal_upd(void);

#endif	 /* TRIGGER_H */
