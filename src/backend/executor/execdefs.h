/*-------------------------------------------------------------------------
 *
 * execdefs.h--
 *    
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: execdefs.h,v 1.1.1.1 1996/07/09 06:21:25 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef EXECDEFS_H
#define EXECDEFS_H

/* ----------------
 *	executor scan direction definitions
 * ----------------
 */
#define EXEC_FRWD		1		/* Scan forward */
#define EXEC_BKWD		-1		/* Scan backward */

/* ----------------
 *	ExecutePlan() tuplecount definitions
 * ----------------
 */
#define ALL_TUPLES		0		/* return all tuples */
#define ONE_TUPLE		1		/* return only one tuple */

/* ----------------
 *	constants used by ExecMain
 * ----------------
 */
#define EXEC_RUN		        3
#define EXEC_FOR 			4
#define EXEC_BACK			5
#define EXEC_RETONE  			6
#define EXEC_RESULT  			7

/* ----------------
 *	Merge Join states
 * ----------------
 */
#define EXEC_MJ_INITIALIZE		1
#define EXEC_MJ_JOINMARK		2
#define EXEC_MJ_JOINTEST		3
#define EXEC_MJ_JOINTUPLES		4
#define EXEC_MJ_NEXTOUTER		5
#define EXEC_MJ_TESTOUTER		6
#define EXEC_MJ_NEXTINNER		7
#define EXEC_MJ_SKIPINNER		8
#define EXEC_MJ_SKIPOUTER		9

#endif /* EXECDEFS_H */
