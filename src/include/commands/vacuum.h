/*-------------------------------------------------------------------------
 *
 * vacuum.h
 *	  header file for postgres vacuum cleaner and statistics analyzer
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: vacuum.h,v 1.46.4.1 2009/11/10 18:01:46 alvherre Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef VACUUM_H
#define VACUUM_H

#include <time.h>
#include <sys/time.h>

#ifdef HAVE_GETRUSAGE
#include <sys/resource.h>
#else
#include "rusagestub.h"
#endif

#include "nodes/parsenodes.h"
#include "utils/rel.h"


/* State structure for vac_init_rusage/vac_show_rusage */
typedef struct VacRUsage
{
	struct timeval tv;
	struct rusage ru;
} VacRUsage;

/* Default statistics target (GUC parameter) */
extern int	default_statistics_target;


/* in commands/vacuum.c */
extern void vacuum(VacuumStmt *vacstmt);
extern void vac_open_indexes(Relation relation, int *nindexes,
				 Relation **Irel);
extern void vac_close_indexes(int nindexes, Relation *Irel);
extern void vac_update_relstats(Oid relid,
					BlockNumber num_pages,
					double num_tuples,
					bool hasindex);
extern void vacuum_set_xid_limits(VacuumStmt *vacstmt, bool sharedRel,
					  TransactionId *oldestXmin,
					  TransactionId *freezeLimit);
extern bool vac_is_partial_index(Relation indrel);
extern void vac_init_rusage(VacRUsage *ru0);
extern const char *vac_show_rusage(VacRUsage *ru0);

/* in commands/vacuumlazy.c */
extern bool lazy_vacuum_rel(Relation onerel, VacuumStmt *vacstmt);

/* in commands/analyze.c */
extern void analyze_rel(Oid relid, VacuumStmt *vacstmt);

#endif   /* VACUUM_H */
