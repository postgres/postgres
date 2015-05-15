/*-------------------------------------------------------------------------
 *
 * pg_tablesample_method.h
 *	  definition of the table scan methods.
 *
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_tablesample_method.h
 *
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_TABLESAMPLE_METHOD_H
#define PG_TABLESAMPLE_METHOD_H

#include "catalog/genbki.h"
#include "catalog/objectaddress.h"

/* ----------------
 *		pg_tablesample_method definition.  cpp turns this into
 *		typedef struct FormData_pg_tablesample_method
 * ----------------
 */
#define TableSampleMethodRelationId	3330

CATALOG(pg_tablesample_method,3330)
{
	NameData	tsmname;		/* tablesample method name */
	bool		tsmseqscan;		/* does this method scan whole table sequentially? */
	bool		tsmpagemode;	/* does this method scan page at a time? */
	regproc		tsminit;		/* init scan function */
	regproc		tsmnextblock;	/* function returning next block to sample
								   or InvalidBlockOffset if finished */
	regproc		tsmnexttuple;	/* function returning next tuple offset from current block
								   or InvalidOffsetNumber if end of the block was reacher */
	regproc		tsmexaminetuple;	/* optional function which can examine tuple contents and
								   decide if tuple should be returned or not */
	regproc		tsmend;			/* end scan function*/
	regproc		tsmreset;		/* reset state - used by rescan */
	regproc		tsmcost;		/* costing function */
} FormData_pg_tablesample_method;

/* ----------------
 *		Form_pg_tablesample_method corresponds to a pointer to a tuple with
 *		the format of pg_tablesample_method relation.
 * ----------------
 */
typedef FormData_pg_tablesample_method *Form_pg_tablesample_method;

/* ----------------
 *		compiler constants for pg_tablesample_method
 * ----------------
 */
#define Natts_pg_tablesample_method					10
#define Anum_pg_tablesample_method_tsmname			1
#define Anum_pg_tablesample_method_tsmseqscan		2
#define Anum_pg_tablesample_method_tsmpagemode		3
#define Anum_pg_tablesample_method_tsminit			4
#define Anum_pg_tablesample_method_tsmnextblock		5
#define Anum_pg_tablesample_method_tsmnexttuple		6
#define Anum_pg_tablesample_method_tsmexaminetuple	7
#define Anum_pg_tablesample_method_tsmend			8
#define Anum_pg_tablesample_method_tsmreset			9
#define Anum_pg_tablesample_method_tsmcost			10

/* ----------------
 *		initial contents of pg_tablesample_method
 * ----------------
 */

DATA(insert OID = 3333 ( system false true tsm_system_init tsm_system_nextblock tsm_system_nexttuple - tsm_system_end tsm_system_reset tsm_system_cost ));
DESCR("SYSTEM table sampling method");
DATA(insert OID = 3334 ( bernoulli true false tsm_bernoulli_init tsm_bernoulli_nextblock tsm_bernoulli_nexttuple - tsm_bernoulli_end tsm_bernoulli_reset tsm_bernoulli_cost ));
DESCR("BERNOULLI table sampling method");

#endif   /* PG_TABLESAMPLE_METHOD_H */
