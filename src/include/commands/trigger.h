/*-------------------------------------------------------------------------
 *
 * trigger.h
 *	  Declarations for trigger handling.
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: trigger.h,v 1.25 2001/03/14 21:50:32 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef TRIGGER_H
#define TRIGGER_H

#include "nodes/execnodes.h"
#include "nodes/parsenodes.h"

/*
 * TriggerData is the node type that is passed as fmgr "context" info
 * when a function is called by the trigger manager.
 */

#define CALLED_AS_TRIGGER(fcinfo) \
	((fcinfo)->context != NULL && IsA((fcinfo)->context, TriggerData))

typedef uint32 TriggerEvent;

typedef struct TriggerData
{
	NodeTag		type;
	TriggerEvent tg_event;
	Relation	tg_relation;
	HeapTuple	tg_trigtuple;
	HeapTuple	tg_newtuple;
	Trigger    *tg_trigger;
} TriggerData;

/* TriggerEvent bit flags */

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

extern void RelationBuildTriggers(Relation relation);

extern void FreeTriggerDesc(TriggerDesc *trigdesc);

extern bool equalTriggerDescs(TriggerDesc *trigdesc1, TriggerDesc *trigdesc2);

extern HeapTuple ExecBRInsertTriggers(EState *estate,
									  Relation rel, HeapTuple tuple);
extern void ExecARInsertTriggers(EState *estate,
								 Relation rel, HeapTuple tuple);
extern bool ExecBRDeleteTriggers(EState *estate, ItemPointer tupleid);
extern void ExecARDeleteTriggers(EState *estate, ItemPointer tupleid);
extern HeapTuple ExecBRUpdateTriggers(EState *estate, ItemPointer tupleid,
									  HeapTuple tuple);
extern void ExecARUpdateTriggers(EState *estate, ItemPointer tupleid,
								 HeapTuple tuple);


/* ----------
 * Deferred trigger stuff
 * ----------
 */
typedef struct DeferredTriggerStatusData
{
	Oid			dts_tgoid;
	bool		dts_tgisdeferred;
} DeferredTriggerStatusData;

typedef struct DeferredTriggerStatusData *DeferredTriggerStatus;


typedef struct DeferredTriggerEventItem
{
	Oid			dti_tgoid;
	int32		dti_state;
} DeferredTriggerEventItem;


typedef struct DeferredTriggerEventData *DeferredTriggerEvent;

typedef struct DeferredTriggerEventData
{
	DeferredTriggerEvent dte_next; /* list link */
	int32		dte_event;
	Oid			dte_relid;
	ItemPointerData dte_oldctid;
	ItemPointerData dte_newctid;
	int32		dte_n_items;
	/* dte_item is actually a variable-size array, of length dte_n_items */
	DeferredTriggerEventItem dte_item[1];
} DeferredTriggerEventData;


extern void DeferredTriggerInit(void);
extern void DeferredTriggerBeginXact(void);
extern void DeferredTriggerEndQuery(void);
extern void DeferredTriggerEndXact(void);
extern void DeferredTriggerAbortXact(void);

extern void DeferredTriggerSetState(ConstraintsSetStmt *stmt);


/*
 * in utils/adt/ri_triggers.c
 *
 */
extern bool RI_FKey_keyequal_upd(TriggerData *trigdata);

#endif	 /* TRIGGER_H */
