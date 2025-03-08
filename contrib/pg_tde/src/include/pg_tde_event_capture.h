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
	bool		alterAccessMethodMode;	/* during ALTER ... SET ACCESS METHOD,
										 * new file permissions shouldn't be
										 * based on earlier encryption status. */
} TdeCreateEvent;

extern TdeCreateEvent *GetCurrentTdeCreateEvent(void);

#endif
