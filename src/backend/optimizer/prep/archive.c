/*-------------------------------------------------------------------------
 *
 * archive.c--
 *	  Support for planning scans on archived relations
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/prep/Attic/archive.c,v 1.4 1997/09/08 21:45:29 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>				/* for sprintf() */
#include <sys/types.h>			/* for u_int in relcache.h */
#include "postgres.h"

#include "utils/rel.h"
#include "utils/elog.h"
#include "utils/palloc.h"
#include "utils/relcache.h"
#include "catalog/pg_class.h"
#include "nodes/pg_list.h"
#include "nodes/parsenodes.h"
#include "optimizer/prep.h"
#include "commands/creatinh.h"

void
plan_archive(List *rt)
{
	List	   *rtitem;
	RangeTblEntry *rte;
	TimeRange  *trange;
	Relation	r;
	Oid			reloid;

	foreach(rtitem, rt)
	{
		rte = lfirst(rtitem);
		trange = rte->timeRange;
		if (trange)
		{
			reloid = rte->relid;
			r = RelationIdGetRelation(reloid);
			if (r->rd_rel->relarch != 'n')
			{
				rte->archive = true;
			}
		}
	}
}


/*
 *	find_archive_rels -- Given a particular relid, find the archive
 *						 relation's relid.
 */
List	   *
find_archive_rels(Oid relid)
{
	Relation	arel;
	char	   *arelName;

	arelName = MakeArchiveName(relid);
	arel = RelationNameGetRelation(arelName);
	pfree(arelName);

	return lconsi(arel->rd_id, lconsi(relid, NIL));
}
