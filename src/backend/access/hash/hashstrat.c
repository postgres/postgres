/*-------------------------------------------------------------------------
 *
 * btstrat.c--
 *    Srategy map entries for the btree indexed access method
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/access/hash/Attic/hashstrat.c,v 1.3 1996/10/20 08:31:51 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
 
#include "catalog/pg_attribute.h"
#include "access/attnum.h"
#include "nodes/pg_list.h" 
#include "access/tupdesc.h"  
#include "storage/fd.h" 
#include "catalog/pg_am.h"
#include "catalog/pg_class.h"
#include "nodes/nodes.h" 
#include "rewrite/prs2lock.h"
#include "access/skey.h"
#include "access/strat.h"
#include "utils/rel.h"
 
#include "storage/block.h"  
#include "storage/off.h"
#include "storage/itemptr.h"
#include <time.h>
#include "utils/nabstime.h"
#include "access/htup.h"
#include "access/itup.h"   
#include "storage/itemid.h"
#include "storage/item.h"
#include "storage/buf.h"   
#include "storage/bufpage.h" 
#include "access/sdir.h"
#include "access/funcindex.h"
#include "utils/tqual.h"
#include "storage/buf.h" 
#include "access/relscan.h"
#include "access/hash.h"

/* 
 *  only one valid strategy for hash tables: equality. 
 */

static StrategyNumber	HTNegate[1] = {
    InvalidStrategy
};

static StrategyNumber	HTCommute[1] = {
    HTEqualStrategyNumber
};

static StrategyNumber	HTNegateCommute[1] = {
    InvalidStrategy
};

static StrategyEvaluationData	HTEvaluationData = {
    /* XXX static for simplicity */

    HTMaxStrategyNumber,
    (StrategyTransformMap)HTNegate,
    (StrategyTransformMap)HTCommute,
    (StrategyTransformMap)HTNegateCommute,
    {NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL}
};

/* ----------------------------------------------------------------
 *	RelationGetHashStrategy
 * ----------------------------------------------------------------
 */

StrategyNumber
_hash_getstrat(Relation rel,
	       AttrNumber attno,
	       RegProcedure proc)
{
    StrategyNumber	strat;

    strat = RelationGetStrategy(rel, attno, &HTEvaluationData, proc);

    Assert(StrategyNumberIsValid(strat));

    return (strat);
}

bool
_hash_invokestrat(Relation rel,
		  AttrNumber attno,
		  StrategyNumber strat,
		  Datum left,
		  Datum right)
{
    return (RelationInvokeStrategy(rel, &HTEvaluationData, attno, strat, 
				   left, right));
}

























