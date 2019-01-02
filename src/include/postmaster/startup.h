/*-------------------------------------------------------------------------
 *
 * startup.h
 *	  Exports from postmaster/startup.c.
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 *
 * src/include/postmaster/startup.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _STARTUP_H
#define _STARTUP_H

extern void HandleStartupProcInterrupts(void);
extern void StartupProcessMain(void) pg_attribute_noreturn();
extern void PreRestoreCommand(void);
extern void PostRestoreCommand(void);
extern bool IsPromoteTriggered(void);
extern void ResetPromoteTriggered(void);

#endif							/* _STARTUP_H */
