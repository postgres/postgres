/*-------------------------------------------------------------------------
 *
 * spi_priv.h
 *				Server Programming Interface private declarations
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/executor/spi_priv.h,v 1.25 2006/03/05 15:58:56 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef SPI_PRIV_H
#define SPI_PRIV_H

#include "executor/spi.h"


typedef struct
{
	/* current results */
	uint32		processed;		/* by Executor */
	Oid			lastoid;
	SPITupleTable *tuptable;

	MemoryContext procCxt;		/* procedure context */
	MemoryContext execCxt;		/* executor context */
	MemoryContext savedcxt;
	SubTransactionId connectSubid;		/* ID of connecting subtransaction */
} _SPI_connection;

typedef struct
{
	/* Context containing _SPI_plan itself as well as subsidiary data */
	MemoryContext plancxt;
	/* Original query string (used for error reporting) */
	const char *query;
	/* List of List of querytrees; one sublist per original parsetree */
	List	   *qtlist;
	/* List of plan trees --- length == # of querytrees, but flat list */
	List	   *ptlist;
	/* Argument types, if a prepared plan */
	int			nargs;
	Oid		   *argtypes;
} _SPI_plan;


#define _SPI_CPLAN_CURCXT	0
#define _SPI_CPLAN_PROCXT	1
#define _SPI_CPLAN_TOPCXT	2

#endif   /* SPI_PRIV_H */
