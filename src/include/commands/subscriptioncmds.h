/*-------------------------------------------------------------------------
 *
 * subscriptioncmds.h
 *	  prototypes for subscriptioncmds.c.
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/commands/subscriptioncmds.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef SUBSCRIPTIONCMDS_H
#define SUBSCRIPTIONCMDS_H

#include "catalog/objectaddress.h"
#include "parser/parse_node.h"

struct WalReceiverConn;			/* avoid pulling in walreceiver.h here */

extern ObjectAddress CreateSubscription(ParseState *pstate, CreateSubscriptionStmt *stmt,
										bool isTopLevel);
extern ObjectAddress AlterSubscription(ParseState *pstate, AlterSubscriptionStmt *stmt, bool isTopLevel);
extern void DropSubscription(DropSubscriptionStmt *stmt, bool isTopLevel);

extern ObjectAddress AlterSubscriptionOwner(const char *name, Oid newOwnerId);
extern void AlterSubscriptionOwner_oid(Oid subid, Oid newOwnerId);

extern char defGetStreamingMode(DefElem *def);

extern ObjectAddress AlterSubscription(ParseState *pstate, AlterSubscriptionStmt *stmt, bool isTopLevel);

extern void CheckSubDeadTupleRetention(bool check_guc, bool sub_disabled,
									   int elevel_for_sub_disabled,
									   bool retain_dead_tuples,
									   bool retention_active,
									   bool max_retention_set);

extern void CheckPubDeadTupleRetention(struct WalReceiverConn *wrconn);

#endif							/* SUBSCRIPTIONCMDS_H */
