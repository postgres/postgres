/*-------------------------------------------------------------------------
 *
 * pg_tde_event_capture.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_TDE_EVENT_CAPTURE_H
#define PG_TDE_EVENT_CAPTURE_H

#include "postgres.h"
#include "nodes/parsenodes.h"

typedef enum TdeCreateEventType
{
	TDE_UNKNOWN_CREATE_EVENT,
	TDE_TABLE_CREATE_EVENT,
	TDE_INDEX_CREATE_EVENT
}			TdeCreateEventType;

typedef struct TdeCreateEvent
{
	TdeCreateEventType eventType;	/* DDL statement type */
	bool		encryptMode;	/* true when the table uses encryption */
	Oid			baseTableOid;	/* Oid of table on which index is being
								 * created on. For create table statement this
								 * contains InvalidOid */
	RangeVar   *relation;		/* Reference to the parsed relation from
								 * create statement */
}			TdeCreateEvent;

extern TdeCreateEvent * GetCurrentTdeCreateEvent(void);

#endif
