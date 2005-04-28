/*
 * multixact.h
 *
 * PostgreSQL multi-transaction-log manager
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/access/multixact.h,v 1.1 2005/04/28 21:47:17 tgl Exp $
 */
#ifndef MULTIXACT_H
#define MULTIXACT_H

#define InvalidMultiXactId	((MultiXactId) 0)
#define FirstMultiXactId	((MultiXactId) 1)

#define MultiXactIdIsValid(multi) ((multi) != InvalidMultiXactId)

extern void MultiXactIdWait(MultiXactId multi);
extern MultiXactId MultiXactIdExpand(MultiXactId multi, bool isMulti,
									 TransactionId xid);
extern bool MultiXactIdIsRunning(MultiXactId multi);
extern void MultiXactIdSetOldestMember(void);

extern void AtEOXact_MultiXact(void);

extern int	MultiXactShmemSize(void);
extern void MultiXactShmemInit(void);
extern void BootStrapMultiXact(void);
extern void StartupMultiXact(void);
extern void ShutdownMultiXact(void);
extern MultiXactId MultiXactGetCheckptMulti(bool is_shutdown);
extern void CheckPointMultiXact(void);
extern void MultiXactSetNextMXact(MultiXactId nextMulti);
extern void MultiXactAdvanceNextMXact(MultiXactId minMulti);

#endif   /* MULTIXACT_H */
