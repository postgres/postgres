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

extern void CreateTrigger (CreateTrigStmt *stmt);
extern void DropTrigger (DropTrigStmt *stmt);

#endif	/* TRIGGER_H */
