/*-------------------------------------------------------------------------
 *
 * cluster.h
 *	  header file for postgres cluster command stuff
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994-5, Regents of the University of California
 *
 * $Id: cluster.h,v 1.15 2002/08/10 21:00:34 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef CLUSTER_H
#define CLUSTER_H

/*
 * functions
 */
extern void cluster(RangeVar *oldrelation, char *oldindexname);

#endif   /* CLUSTER_H */
