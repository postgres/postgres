/*-------------------------------------------------------------------------
 *
 * lockcmds.c
 *	  Lock command support code
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/commands/lockcmds.c,v 1.1 2002/04/15 05:22:03 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "catalog/namespace.h"
#include "commands/lockcmds.h"
#include "miscadmin.h"
#include "utils/acl.h"


/*
 * LOCK TABLE
 */
void
LockTableCommand(LockStmt *lockstmt)
{
	List	   *p;

	/*
	 * Iterate over the list and open, lock, and close the relations one
	 * at a time
	 */

	foreach(p, lockstmt->relations)
	{
		RangeVar   *relation = lfirst(p);
		Oid			reloid;
		int32		aclresult;
		Relation	rel;

		/*
		 * We don't want to open the relation until we've checked privilege.
		 * So, manually get the relation OID.
		 */
		reloid = RangeVarGetRelid(relation, false);

		if (lockstmt->mode == AccessShareLock)
			aclresult = pg_class_aclcheck(reloid, GetUserId(),
										  ACL_SELECT);
		else
			aclresult = pg_class_aclcheck(reloid, GetUserId(),
										  ACL_UPDATE | ACL_DELETE);

		if (aclresult != ACLCHECK_OK)
			elog(ERROR, "LOCK TABLE: permission denied");

		rel = relation_open(reloid, lockstmt->mode);

		/* Currently, we only allow plain tables to be locked */
		if (rel->rd_rel->relkind != RELKIND_RELATION)
			elog(ERROR, "LOCK TABLE: %s is not a table",
				 relation->relname);

		relation_close(rel, NoLock);	/* close rel, keep lock */
	}
}
