/*-------------------------------------------------------------------------
 *
 * lockcmds.c
 *	  Lock command support code
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/commands/lockcmds.c,v 1.21 2009/01/12 08:54:26 petere Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "catalog/namespace.h"
#include "commands/lockcmds.h"
#include "miscadmin.h"
#include "optimizer/prep.h"
#include "parser/parse_clause.h"
#include "utils/acl.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"


/*
 * LOCK TABLE
 */
void
LockTableCommand(LockStmt *lockstmt)
{
	ListCell   *p;

	/*
	 * Iterate over the list and open, lock, and close the relations one at a
	 * time
	 */

	foreach(p, lockstmt->relations)
	{
		RangeVar   *relation = lfirst(p);
		Oid			reloid;
		bool		recurse = interpretInhOption(relation->inhOpt);
		List	   *children_and_self;
		ListCell   *child;

		reloid = RangeVarGetRelid(relation, false);

		if (recurse)
			children_and_self = find_all_inheritors(reloid);
		else
			children_and_self = list_make1_oid(reloid);

		foreach(child, children_and_self)
		{
			Oid			childreloid = lfirst_oid(child);
			Relation	rel;
			AclResult	aclresult;

			/* We don't want to open the relation until we've checked privilege. */
			if (lockstmt->mode == AccessShareLock)
				aclresult = pg_class_aclcheck(childreloid, GetUserId(),
											  ACL_SELECT);
			else
				aclresult = pg_class_aclcheck(childreloid, GetUserId(),
											  ACL_UPDATE | ACL_DELETE | ACL_TRUNCATE);

			if (aclresult != ACLCHECK_OK)
				aclcheck_error(aclresult, ACL_KIND_CLASS,
							   get_rel_name(childreloid));

			if (lockstmt->nowait)
				rel = relation_open_nowait(childreloid, lockstmt->mode);
			else
				rel = relation_open(childreloid, lockstmt->mode);

			/* Currently, we only allow plain tables to be locked */
			if (rel->rd_rel->relkind != RELKIND_RELATION)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("\"%s\" is not a table",
								get_rel_name(childreloid))));

			relation_close(rel, NoLock);	/* close rel, keep lock */
		}
	}
}
