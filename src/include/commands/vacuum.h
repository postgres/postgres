/*-------------------------------------------------------------------------
 *
 * vacuum.h
 *	  header file for postgres vacuum cleaner and statistics analyzer
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: vacuum.h,v 1.37 2001/07/12 04:11:13 tgl Exp $
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
#include "storage/block.h"


/* State structure for vac_init_rusage/vac_show_rusage */
typedef struct VacRUsage
{
	struct timeval	tv;
	struct rusage	ru;
} VacRUsage;


/* in commands/vacuum.c */
extern void vacuum(VacuumStmt *vacstmt);
extern void vac_update_relstats(Oid relid,
								BlockNumber num_pages,
								double num_tuples,
								bool hasindex);
extern void vac_init_rusage(VacRUsage *ru0);
extern const char *vac_show_rusage(VacRUsage *ru0);

/* in commands/analyze.c */
extern void analyze_rel(Oid relid, VacuumStmt *vacstmt);

#endif	 /* VACUUM_H */
