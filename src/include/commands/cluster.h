/*-------------------------------------------------------------------------
 *
 * cluster.h
 *	  header file for postgres cluster command stuff
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994-5, Regents of the University of California
 *
 * $Id: cluster.h,v 1.16 2002/11/15 03:09:39 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef CLUSTER_H
#define CLUSTER_H

#include <nodes/parsenodes.h>
/*
 * functions
 */
extern void cluster(ClusterStmt *stmt);

#endif   /* CLUSTER_H */
