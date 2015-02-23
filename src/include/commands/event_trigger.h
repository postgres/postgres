/*-------------------------------------------------------------------------
 *
 * event_trigger.h
 *	  Declarations for command trigger handling.
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/commands/event_trigger.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef EVENT_TRIGGER_H
#define EVENT_TRIGGER_H

#include "catalog/dependency.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_event_trigger.h"
#include "nodes/parsenodes.h"

typedef struct EventTriggerData
{
	NodeTag		type;
	const char *event;			/* event name */
	Node	   *parsetree;		/* parse tree */
	const char *tag;			/* command tag */
} EventTriggerData;

#define AT_REWRITE_ALTER_PERSISTENCE	0x01
#define AT_REWRITE_DEFAULT_VAL			0x02
#define AT_REWRITE_COLUMN_REWRITE		0x04
#define AT_REWRITE_ALTER_OID			0x08

/*
 * EventTriggerData is the node type that is passed as fmgr "context" info
 * when a function is called by the event trigger manager.
 */
#define CALLED_AS_EVENT_TRIGGER(fcinfo) \
	((fcinfo)->context != NULL && IsA((fcinfo)->context, EventTriggerData))

extern Oid	CreateEventTrigger(CreateEventTrigStmt *stmt);
extern void RemoveEventTriggerById(Oid ctrigOid);
extern Oid	get_event_trigger_oid(const char *trigname, bool missing_ok);

extern Oid	AlterEventTrigger(AlterEventTrigStmt *stmt);
extern Oid	AlterEventTriggerOwner(const char *name, Oid newOwnerId);
extern void AlterEventTriggerOwner_oid(Oid, Oid newOwnerId);

extern bool EventTriggerSupportsObjectType(ObjectType obtype);
extern bool EventTriggerSupportsObjectClass(ObjectClass objclass);
extern bool EventTriggerSupportsGrantObjectType(GrantObjectType objtype);
extern void EventTriggerDDLCommandStart(Node *parsetree);
extern void EventTriggerDDLCommandEnd(Node *parsetree);
extern void EventTriggerSQLDrop(Node *parsetree);
extern void EventTriggerTableRewrite(Node *parsetree, Oid tableOid, int reason);

extern bool EventTriggerBeginCompleteQuery(void);
extern void EventTriggerEndCompleteQuery(void);
extern bool trackDroppedObjectsNeeded(void);
extern void EventTriggerSQLDropAddObject(const ObjectAddress *object,
							 bool original, bool normal);

#endif   /* EVENT_TRIGGER_H */
