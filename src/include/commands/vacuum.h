/*-------------------------------------------------------------------------
 *
 * vacuum.h
 *	  header file for postgres vacuum cleaner and statistics analyzer
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: vacuum.h,v 1.35 2001/05/07 00:43:25 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef VACUUM_H
#define VACUUM_H

#include "nodes/parsenodes.h"


/* in commands/vacuum.c */
extern void vacuum(VacuumStmt *vacstmt);
extern void vac_update_relstats(Oid relid, long num_pages, double num_tuples,
								bool hasindex);
/* in commands/analyze.c */
extern void analyze_rel(Oid relid, VacuumStmt *vacstmt);

#endif	 /* VACUUM_H */
