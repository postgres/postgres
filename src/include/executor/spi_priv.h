/*-------------------------------------------------------------------------
 *
 * spi_priv.h
 *				Server Programming Interface private declarations
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/executor/spi_priv.h,v 1.35 2010/02/26 02:01:24 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef SPI_PRIV_H
#define SPI_PRIV_H

#include "executor/spi.h"


#define _SPI_PLAN_MAGIC		569278163

typedef struct
{
	/* current results */
	uint32		processed;		/* by Executor */
	Oid			lastoid;
	SPITupleTable *tuptable;

	MemoryContext procCxt;		/* procedure context */
	MemoryContext execCxt;		/* executor context */
	MemoryContext savedcxt;		/* context of SPI_connect's caller */
	SubTransactionId connectSubid;		/* ID of connecting subtransaction */
} _SPI_connection;

/*
 * SPI plans have two states: saved or unsaved.
 *
 * For an unsaved plan, the _SPI_plan struct and all its subsidiary data are in
 * a dedicated memory context identified by plancxt.  An unsaved plan is good
 * at most for the current transaction, since the locks that protect it from
 * schema changes will be lost at end of transaction.  Hence the plancxt is
 * always a transient one.
 *
 * For a saved plan, the _SPI_plan struct and the argument type array are in
 * the plancxt (which can be really small).  All the other subsidiary state
 * is in plancache entries identified by plancache_list (note: the list cells
 * themselves are in plancxt).  We rely on plancache.c to keep the cache
 * entries up-to-date as needed.  The plancxt is a child of CacheMemoryContext
 * since it should persist until explicitly destroyed.
 *
 * To avoid redundant coding, the representation of unsaved plans matches
 * that of saved plans, ie, plancache_list is a list of CachedPlanSource
 * structs which in turn point to CachedPlan structs.  However, in an unsaved
 * plan all these structs are just created by spi.c and are not known to
 * plancache.c.  We don't try very hard to make all their fields valid,
 * only the ones spi.c actually uses.
 *
 * Note: if the original query string contained only whitespace and comments,
 * the plancache_list will be NIL and so there is no place to store the
 * query string.  We don't care about that, but we do care about the
 * argument type array, which is why it's seemingly-redundantly stored.
 */
typedef struct _SPI_plan
{
	int			magic;			/* should equal _SPI_PLAN_MAGIC */
	bool		saved;			/* saved or unsaved plan? */
	List	   *plancache_list; /* one CachedPlanSource per parsetree */
	MemoryContext plancxt;		/* Context containing _SPI_plan and data */
	int			cursor_options; /* Cursor options used for planning */
	int			nargs;			/* number of plan arguments */
	Oid		   *argtypes;		/* Argument types (NULL if nargs is 0) */
	ParserSetupHook parserSetup;	/* alternative parameter spec method */
	void	   *parserSetupArg;
} _SPI_plan;

#endif   /* SPI_PRIV_H */
