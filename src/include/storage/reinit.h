/*-------------------------------------------------------------------------
 *
 * reinit.h
 *	  Reinitialization of unlogged relations
 *
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/fd.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef REINIT_H
#define REINIT_H

extern void ResetUnloggedRelations(int op);

#define UNLOGGED_RELATION_CLEANUP		0x0001
#define UNLOGGED_RELATION_INIT			0x0002

#endif   /* REINIT_H */
