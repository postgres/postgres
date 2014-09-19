/*-------------------------------------------------------------------------
 *
 * policy.h
 *	  prototypes for policy.c.
 *
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/commands/policy.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef POLICY_H
#define POLICY_H

#include "nodes/parsenodes.h"

extern void RelationBuildRowSecurity(Relation relation);

extern void RemovePolicyById(Oid policy_id);

extern Oid CreatePolicy(CreatePolicyStmt *stmt);
extern Oid AlterPolicy(AlterPolicyStmt *stmt);

Oid get_relation_policy_oid(Oid relid,
							const char *policy_name, bool missing_ok);

Oid rename_policy(RenameStmt *stmt);


#endif   /* POLICY_H */
