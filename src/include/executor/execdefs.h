/*-------------------------------------------------------------------------
 *
 * execdefs.h
 *
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: execdefs.h,v 1.14 2003/08/04 02:40:13 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef EXECDEFS_H
#define EXECDEFS_H

/* ----------------
 *		Merge Join states
 * ----------------
 */
#define EXEC_MJ_INITIALIZE				1
#define EXEC_MJ_JOINMARK				2
#define EXEC_MJ_JOINTEST				3
#define EXEC_MJ_JOINTUPLES				4
#define EXEC_MJ_NEXTOUTER				5
#define EXEC_MJ_TESTOUTER				6
#define EXEC_MJ_NEXTINNER				7
#define EXEC_MJ_SKIPOUTER_BEGIN			8
#define EXEC_MJ_SKIPOUTER_TEST			9
#define EXEC_MJ_SKIPOUTER_ADVANCE		10
#define EXEC_MJ_SKIPINNER_BEGIN			11
#define EXEC_MJ_SKIPINNER_TEST			12
#define EXEC_MJ_SKIPINNER_ADVANCE		13
#define EXEC_MJ_ENDOUTER				14
#define EXEC_MJ_ENDINNER				15

#endif   /* EXECDEFS_H */
