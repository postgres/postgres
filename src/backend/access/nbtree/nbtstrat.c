/*-------------------------------------------------------------------------
 *
 * btstrat.c--
 *    Srategy map entries for the btree indexed access method
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/access/nbtree/Attic/nbtstrat.c,v 1.2 1996/10/20 10:53:16 scrappy Exp $
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
#include "access/itup.h"
#include "access/funcindex.h"
#include "storage/itemid.h"
#include "storage/item.h"
#include "storage/buf.h"
#include "storage/bufpage.h"
#include <time.h>
#include "utils/nabstime.h"
#include "access/htup.h"
#include "utils/tqual.h"
#include "access/relscan.h"
#include "access/sdir.h"
#include "access/nbtree.h"

/*
 * Note:
 *	StrategyNegate, StrategyCommute, and StrategyNegateCommute
 *	assume <, <=, ==, >=, > ordering.
 */
static StrategyNumber	BTNegate[5] = {
    BTGreaterEqualStrategyNumber,
    BTGreaterStrategyNumber,
    InvalidStrategy,
    BTLessStrategyNumber,
    BTLessEqualStrategyNumber
};

static StrategyNumber	BTCommute[5] = {
    BTGreaterStrategyNumber,
    BTGreaterEqualStrategyNumber,
    InvalidStrategy,
    BTLessEqualStrategyNumber,
    BTLessStrategyNumber
};

static StrategyNumber	BTNegateCommute[5] = {
    BTLessEqualStrategyNumber,
    BTLessStrategyNumber,
    InvalidStrategy,
    BTGreaterStrategyNumber,
    BTGreaterEqualStrategyNumber
};

static uint16	BTLessTermData[] = {		/* XXX type clash */
    2,
    BTLessStrategyNumber,
    SK_NEGATE,
    BTLessStrategyNumber,
    SK_NEGATE | SK_COMMUTE
};

static uint16	BTLessEqualTermData[] = {	/* XXX type clash */
    2,
    BTLessEqualStrategyNumber,
    0x0,
    BTLessEqualStrategyNumber,
    SK_COMMUTE
};

static uint16	BTGreaterEqualTermData[] = {	/* XXX type clash */
    2,
    BTGreaterEqualStrategyNumber,
    0x0,
    BTGreaterEqualStrategyNumber,
    SK_COMMUTE
    };

static uint16	BTGreaterTermData[] = {		/* XXX type clash */
    2,
    BTGreaterStrategyNumber,
    SK_NEGATE,
    BTGreaterStrategyNumber,
    SK_NEGATE | SK_COMMUTE
};

static StrategyTerm	BTEqualExpressionData[] = {
    (StrategyTerm)BTLessTermData,		/* XXX */
    (StrategyTerm)BTLessEqualTermData,		/* XXX */
    (StrategyTerm)BTGreaterEqualTermData,	/* XXX */
    (StrategyTerm)BTGreaterTermData,		/* XXX */
    NULL
};

static StrategyEvaluationData	BTEvaluationData = {
    /* XXX static for simplicity */
    
    BTMaxStrategyNumber,
    (StrategyTransformMap)BTNegate,	/* XXX */
    (StrategyTransformMap)BTCommute,	/* XXX */
    (StrategyTransformMap)BTNegateCommute,	/* XXX */

    { NULL, NULL, (StrategyExpression)BTEqualExpressionData, NULL, NULL,
      NULL,NULL,NULL,NULL,NULL,NULL,NULL}
};

/* ----------------------------------------------------------------
 *	RelationGetBTStrategy
 * ----------------------------------------------------------------
 */

StrategyNumber
_bt_getstrat(Relation rel,
	     AttrNumber attno,
	     RegProcedure proc)
{
    StrategyNumber	strat;
    
    strat = RelationGetStrategy(rel, attno, &BTEvaluationData, proc);
    
    Assert(StrategyNumberIsValid(strat));
    
    return (strat);
}

bool
_bt_invokestrat(Relation rel,
		AttrNumber attno,
		StrategyNumber strat,
		Datum left,
		Datum right)
{
    return (RelationInvokeStrategy(rel, &BTEvaluationData, attno, strat, 
				   left, right));
}
