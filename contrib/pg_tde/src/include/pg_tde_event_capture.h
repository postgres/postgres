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

typedef struct TdeCreateEvent
{
	bool		encryptMode;	/* true when the table uses encryption */
	Oid			baseTableOid;	/* Oid of table on which index is being
								 * created on. For create table statement this
								 * contains InvalidOid */
	RangeVar   *relation;		/* Reference to the parsed relation from
								 * create statement */
	bool		alterSequenceMode;	/* true when alter sequence is executed by
									 * pg_tde */
} TdeCreateEvent;

extern TdeCreateEvent *GetCurrentTdeCreateEvent(void);

#endif
