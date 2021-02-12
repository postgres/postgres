/*-------------------------------------------------------------------------
 *
 * subscriptioncmds.h
 *	  prototypes for subscriptioncmds.c.
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/commands/subscriptioncmds.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef SUBSCRIPTIONCMDS_H
#define SUBSCRIPTIONCMDS_H

#include "catalog/objectaddress.h"
#include "nodes/parsenodes.h"

extern ObjectAddress CreateSubscription(CreateSubscriptionStmt *stmt,
										bool isTopLevel);
extern ObjectAddress AlterSubscription(AlterSubscriptionStmt *stmt, bool isTopLevel);
extern void DropSubscription(DropSubscriptionStmt *stmt, bool isTopLevel);

extern ObjectAddress AlterSubscriptionOwner(const char *name, Oid newOwnerId);
extern void AlterSubscriptionOwner_oid(Oid subid, Oid newOwnerId);

#endif							/* SUBSCRIPTIONCMDS_H */
