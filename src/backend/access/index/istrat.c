/*-------------------------------------------------------------------------
 *
 * istrat.c--
 *	  index scan strategy manipulation code and index strategy manipulation
 *	  operator code.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/index/Attic/istrat.c,v 1.27 1998/09/01 03:21:10 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "miscadmin.h"
#include "access/heapam.h"
#include "access/istrat.h"
#include "catalog/catname.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_amproc.h"
#include "catalog/pg_index.h"
#include "catalog/pg_operator.h"
#include "utils/syscache.h"
#include "fmgr.h"
#include "utils/memutils.h"		/* could have been access/itup.h */

#ifdef USE_ASSERT_CHECKING
static bool StrategyEvaluationIsValid(StrategyEvaluation evaluation);
static bool
StrategyExpressionIsValid(StrategyExpression expression,
						  StrategyNumber maxStrategy);
static ScanKey StrategyMapGetScanKeyEntry(StrategyMap map,
						   StrategyNumber strategyNumber);
static bool
StrategyOperatorIsValid(StrategyOperator operator,
						StrategyNumber maxStrategy);
static bool
StrategyTermIsValid(StrategyTerm term,
					StrategyNumber maxStrategy);

#endif


/* ----------------------------------------------------------------
 *				   misc strategy support routines
 * ----------------------------------------------------------------
 */

/*
 *		StrategyNumberIsValid
 *		StrategyNumberIsInBounds
 *		StrategyMapIsValid
 *		StrategyTransformMapIsValid
 *		IndexStrategyIsValid
 *
 *				... are now macros in istrat.h -cim 4/27/91
 */

/*
 * StrategyMapGetScanKeyEntry --
 *		Returns a scan key entry of a index strategy mapping member.
 *
 * Note:
 *		Assumes that the index strategy mapping is valid.
 *		Assumes that the index strategy number is valid.
 *		Bounds checking should be done outside this routine.
 */
static ScanKey
StrategyMapGetScanKeyEntry(StrategyMap map,
						   StrategyNumber strategyNumber)
{
	Assert(StrategyMapIsValid(map));
	Assert(StrategyNumberIsValid(strategyNumber));
	return &map->entry[strategyNumber - 1];
}

/*
 * IndexStrategyGetStrategyMap --
 *		Returns an index strategy mapping of an index strategy.
 *
 * Note:
 *		Assumes that the index strategy is valid.
 *		Assumes that the number of index strategies is valid.
 *		Bounds checking should be done outside this routine.
 */
StrategyMap
IndexStrategyGetStrategyMap(IndexStrategy indexStrategy,
							StrategyNumber maxStrategyNum,
							AttrNumber attrNum)
{
	Assert(IndexStrategyIsValid(indexStrategy));
	Assert(StrategyNumberIsValid(maxStrategyNum));
	Assert(AttributeNumberIsValid(attrNum));

	maxStrategyNum = AMStrategies(maxStrategyNum);		/* XXX */
	return &indexStrategy->strategyMapData[maxStrategyNum * (attrNum - 1)];
}

/*
 * AttributeNumberGetIndexStrategySize --
 *		Computes the size of an index strategy.
 */
Size
AttributeNumberGetIndexStrategySize(AttrNumber maxAttributeNumber,
									StrategyNumber maxStrategyNumber)
{
	maxStrategyNumber = AMStrategies(maxStrategyNumber);		/* XXX */
	return maxAttributeNumber * maxStrategyNumber * sizeof(ScanKeyData);
}

#ifdef USE_ASSERT_CHECKING
/*
 * StrategyTransformMapIsValid is now a macro in istrat.h -cim 4/27/91
 */

/* ----------------
 *		StrategyOperatorIsValid
 * ----------------
 */
static bool
StrategyOperatorIsValid(StrategyOperator operator,
						StrategyNumber maxStrategy)
{
	return (bool)
	(PointerIsValid(operator) &&
	 StrategyNumberIsInBounds(operator->strategy, maxStrategy) &&
	 !(operator->flags & ~(SK_NEGATE | SK_COMMUTE)));
}

/* ----------------
 *		StrategyTermIsValid
 * ----------------
 */
static bool
StrategyTermIsValid(StrategyTerm term,
					StrategyNumber maxStrategy)
{
	Index		index;

	if (!PointerIsValid(term) || term->degree == 0)
		return false;

	for (index = 0; index < term->degree; index += 1)
	{
		if (!StrategyOperatorIsValid(&term->operatorData[index],
									 maxStrategy))
			return false;
	}

	return true;
}

/* ----------------
 *		StrategyExpressionIsValid
 * ----------------
 */
static bool
StrategyExpressionIsValid(StrategyExpression expression,
						  StrategyNumber maxStrategy)
{
	StrategyTerm *termP;

	if (!PointerIsValid(expression))
		return true;

	if (!StrategyTermIsValid(expression->term[0], maxStrategy))
		return false;

	termP = &expression->term[1];
	while (StrategyTermIsValid(*termP, maxStrategy))
		termP += 1;

	return (bool)
		(!PointerIsValid(*termP));
}

/* ----------------
 *		StrategyEvaluationIsValid
 * ----------------
 */
static bool
StrategyEvaluationIsValid(StrategyEvaluation evaluation)
{
	Index		index;

	if (!PointerIsValid(evaluation) ||
		!StrategyNumberIsValid(evaluation->maxStrategy) ||
		!StrategyTransformMapIsValid(evaluation->negateTransform) ||
		!StrategyTransformMapIsValid(evaluation->commuteTransform) ||
		!StrategyTransformMapIsValid(evaluation->negateCommuteTransform))
	{

		return false;
	}

	for (index = 0; index < evaluation->maxStrategy; index += 1)
	{
		if (!StrategyExpressionIsValid(evaluation->expression[index],
									   evaluation->maxStrategy))
		{

			return false;
		}
	}
	return true;
}

#endif

/* ----------------
 *		StrategyTermEvaluate
 * ----------------
 */
static bool
StrategyTermEvaluate(StrategyTerm term,
					 StrategyMap map,
					 Datum left,
					 Datum right)
{
	Index		index;
	long		tmpres = 0;
	bool		result = 0;
	StrategyOperator operator;
	ScanKey		entry;

	for (index = 0, operator = &term->operatorData[0];
		 index < term->degree; index += 1, operator += 1)
	{

		entry = &map->entry[operator->strategy - 1];

		Assert(RegProcedureIsValid(entry->sk_procedure));

		switch (operator->flags ^ entry->sk_flags)
		{
			case 0x0:
				tmpres = (long) FMGR_PTR2(&entry->sk_func,
										  left, right);
				break;

			case SK_NEGATE:
				tmpres = (long) !FMGR_PTR2(&entry->sk_func,
										   left, right);
				break;

			case SK_COMMUTE:
				tmpres = (long) FMGR_PTR2(&entry->sk_func,
										  right, left);
				break;

			case SK_NEGATE | SK_COMMUTE:
				tmpres = (long) !FMGR_PTR2(&entry->sk_func,
										   right, left);
				break;

			default:
				elog(FATAL, "StrategyTermEvaluate: impossible case %d",
					 operator->flags ^ entry->sk_flags);
		}

		result = (bool) tmpres;
		if (!result)
			return result;
	}

	return result;
}


/* ----------------
 *		RelationGetStrategy
 * ----------------
 */
StrategyNumber
RelationGetStrategy(Relation relation,
					AttrNumber attributeNumber,
					StrategyEvaluation evaluation,
					RegProcedure procedure)
{
	StrategyNumber strategy;
	StrategyMap strategyMap;
	ScanKey		entry;
	Index		index;
	int			numattrs;

	Assert(RelationIsValid(relation));
	numattrs = RelationGetNumberOfAttributes(relation);

	Assert(relation->rd_rel->relkind == RELKIND_INDEX); /* XXX use accessor */
	Assert(AttributeNumberIsValid(attributeNumber));
	Assert((attributeNumber >= 1) && (attributeNumber < 1 + numattrs));

	Assert(StrategyEvaluationIsValid(evaluation));
	Assert(RegProcedureIsValid(procedure));

	strategyMap =
		IndexStrategyGetStrategyMap(RelationGetIndexStrategy(relation),
									evaluation->maxStrategy,
									attributeNumber);

	/* get a strategy number for the procedure ignoring flags for now */
	for (index = 0; index < evaluation->maxStrategy; index += 1)
	{
		if (strategyMap->entry[index].sk_procedure == procedure)
			break;
	}

	if (index == evaluation->maxStrategy)
		return InvalidStrategy;

	strategy = 1 + index;
	entry = StrategyMapGetScanKeyEntry(strategyMap, strategy);

	Assert(!(entry->sk_flags & ~(SK_NEGATE | SK_COMMUTE)));

	switch (entry->sk_flags & (SK_NEGATE | SK_COMMUTE))
	{
		case 0x0:
			return strategy;

		case SK_NEGATE:
			strategy = evaluation->negateTransform->strategy[strategy - 1];
			break;

		case SK_COMMUTE:
			strategy = evaluation->commuteTransform->strategy[strategy - 1];
			break;

		case SK_NEGATE | SK_COMMUTE:
			strategy = evaluation->negateCommuteTransform->strategy[strategy - 1];
			break;

		default:
			elog(FATAL, "RelationGetStrategy: impossible case %d", entry->sk_flags);
	}


	if (!StrategyNumberIsInBounds(strategy, evaluation->maxStrategy))
	{
		if (!StrategyNumberIsValid(strategy))
			elog(ERROR, "RelationGetStrategy: corrupted evaluation");
	}

	return strategy;
}

/* ----------------
 *		RelationInvokeStrategy
 * ----------------
 */
bool							/* XXX someday, this may return Datum */
RelationInvokeStrategy(Relation relation,
					   StrategyEvaluation evaluation,
					   AttrNumber attributeNumber,
					   StrategyNumber strategy,
					   Datum left,
					   Datum right)
{
	StrategyNumber newStrategy;
	StrategyMap strategyMap;
	ScanKey		entry;
	StrategyTermData termData;
	int			numattrs;

	Assert(RelationIsValid(relation));
	Assert(relation->rd_rel->relkind == RELKIND_INDEX); /* XXX use accessor */
	numattrs = RelationGetNumberOfAttributes(relation);

	Assert(StrategyEvaluationIsValid(evaluation));
	Assert(AttributeNumberIsValid(attributeNumber));
	Assert((attributeNumber >= 1) && (attributeNumber < 1 + numattrs));

	Assert(StrategyNumberIsInBounds(strategy, evaluation->maxStrategy));

	termData.degree = 1;

	strategyMap = IndexStrategyGetStrategyMap(RelationGetIndexStrategy(relation),
												evaluation->maxStrategy,
												attributeNumber);

	entry = StrategyMapGetScanKeyEntry(strategyMap, strategy);

	if (RegProcedureIsValid(entry->sk_procedure))
	{
		termData.operatorData[0].strategy = strategy;
		termData.operatorData[0].flags = 0x0;

		return StrategyTermEvaluate(&termData, strategyMap, left, right);
	}


	newStrategy = evaluation->negateTransform->strategy[strategy - 1];
	if (newStrategy != strategy && StrategyNumberIsValid(newStrategy))
	{

		entry = StrategyMapGetScanKeyEntry(strategyMap, newStrategy);

		if (RegProcedureIsValid(entry->sk_procedure))
		{
			termData.operatorData[0].strategy = newStrategy;
			termData.operatorData[0].flags = SK_NEGATE;

			return StrategyTermEvaluate(&termData, strategyMap, left, right);
		}
	}

	newStrategy = evaluation->commuteTransform->strategy[strategy - 1];
	if (newStrategy != strategy && StrategyNumberIsValid(newStrategy))
	{

		entry = StrategyMapGetScanKeyEntry(strategyMap, newStrategy);

		if (RegProcedureIsValid(entry->sk_procedure))
		{
			termData.operatorData[0].strategy = newStrategy;
			termData.operatorData[0].flags = SK_COMMUTE;

			return StrategyTermEvaluate(&termData, strategyMap, left, right);
		}
	}

	newStrategy = evaluation->negateCommuteTransform->strategy[strategy - 1];
	if (newStrategy != strategy && StrategyNumberIsValid(newStrategy))
	{

		entry = StrategyMapGetScanKeyEntry(strategyMap, newStrategy);

		if (RegProcedureIsValid(entry->sk_procedure))
		{
			termData.operatorData[0].strategy = newStrategy;
			termData.operatorData[0].flags = SK_NEGATE | SK_COMMUTE;

			return StrategyTermEvaluate(&termData, strategyMap, left, right);
		}
	}

	if (PointerIsValid(evaluation->expression[strategy - 1]))
	{
		StrategyTerm *termP;

		termP = &evaluation->expression[strategy - 1]->term[0];
		while (PointerIsValid(*termP))
		{
			Index		index;

			for (index = 0; index < (*termP)->degree; index += 1)
			{
				entry = StrategyMapGetScanKeyEntry(strategyMap,
								 (*termP)->operatorData[index].strategy);

				if (!RegProcedureIsValid(entry->sk_procedure))
					break;
			}

			if (index == (*termP)->degree)
			{
				return StrategyTermEvaluate(*termP, strategyMap, left, right);
			}

			termP += 1;
		}
	}

	elog(ERROR, "RelationInvokeStrategy: cannot evaluate strategy %d",
		 strategy);

	/* not reached, just to make compiler happy */
	return FALSE;


}

/* ----------------
 *		OperatorRelationFillScanKeyEntry
 * ----------------
 */
static void
OperatorRelationFillScanKeyEntry(Relation operatorRelation,
								 Oid operatorObjectId,
								 ScanKey entry)
{
	HeapTuple	tuple;
	HeapScanDesc scan = NULL;

	if (!IsBootstrapProcessingMode())
	{
		tuple = SearchSysCacheTuple(OPROID,
									ObjectIdGetDatum(operatorObjectId),
									0, 0, 0);
	}
	else
	{
		ScanKeyData scanKeyData;
	
		ScanKeyEntryInitialize(&scanKeyData, 0,
							   ObjectIdAttributeNumber,
							   F_OIDEQ,
							   ObjectIdGetDatum(operatorObjectId));

		scan = heap_beginscan(operatorRelation, false, SnapshotNow,
							  1, &scanKeyData);
	
		tuple = heap_getnext(scan, 0);
	}

	if (!HeapTupleIsValid(tuple))
	{
		if (IsBootstrapProcessingMode())
			heap_endscan(scan);
		elog(ERROR, "OperatorObjectIdFillScanKeyEntry: unknown operator %lu",
			 (uint32) operatorObjectId);
	}

	entry->sk_flags = 0;
	entry->sk_procedure = ((Form_pg_operator) GETSTRUCT(tuple))->oprcode;
	fmgr_info(entry->sk_procedure, &entry->sk_func);
	entry->sk_nargs = entry->sk_func.fn_nargs;

	if (IsBootstrapProcessingMode())
		heap_endscan(scan);
	
	if (!RegProcedureIsValid(entry->sk_procedure))
	{
		elog(ERROR,
		"OperatorObjectIdFillScanKeyEntry: no procedure for operator %lu",
			 (uint32) operatorObjectId);
	}
}


/*
 * IndexSupportInitialize --
 *		Initializes an index strategy and associated support procedures.
 */
void
IndexSupportInitialize(IndexStrategy indexStrategy,
					   RegProcedure *indexSupport,
					   Oid indexObjectId,
					   Oid accessMethodObjectId,
					   StrategyNumber maxStrategyNumber,
					   StrategyNumber maxSupportNumber,
					   AttrNumber maxAttributeNumber)
{
	Relation	relation = NULL;
	HeapScanDesc scan = NULL;
	ScanKeyData entry[2];
	Relation	operatorRelation;
	HeapTuple	tuple;
	StrategyMap map;
	AttrNumber	attributeNumber;
	int			attributeIndex;
	Oid			operatorClassObjectId[MaxIndexAttributeNumber];

	if (!IsBootstrapProcessingMode())
	{
		tuple = SearchSysCacheTuple(INDEXRELID,
									ObjectIdGetDatum(indexObjectId),
									0, 0, 0);
	}
	else
	{
		ScanKeyEntryInitialize(&entry[0], 0, Anum_pg_index_indexrelid,
							   F_OIDEQ,
							   ObjectIdGetDatum(indexObjectId));
	
		relation = heap_openr(IndexRelationName);
		scan = heap_beginscan(relation, false, SnapshotNow, 1, entry);
		tuple = heap_getnext(scan, 0);
	}

	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "IndexSupportInitialize: corrupted catalogs");

	maxStrategyNumber = AMStrategies(maxStrategyNumber);

	/*
	 * XXX note that the following assumes the INDEX tuple is well formed
	 * and that the *key and *class are 0 terminated.
	 */
	for (attributeIndex = 0; attributeIndex < maxAttributeNumber; attributeIndex++)
	{
		Form_pg_index iform;

		iform = (Form_pg_index) GETSTRUCT(tuple);

		if (!OidIsValid(iform->indkey[attributeIndex]))
		{
			if (attributeIndex == 0)
				elog(ERROR, "IndexSupportInitialize: no pg_index tuple");
			break;
		}

		operatorClassObjectId[attributeIndex] = iform->indclass[attributeIndex];
	}

	if (IsBootstrapProcessingMode())
	{
		heap_endscan(scan);
		heap_close(relation);
	}
	
	/* if support routines exist for this access method, load them */
	if (maxSupportNumber > 0)
	{
		ScanKeyEntryInitialize(&entry[0], 0, Anum_pg_amproc_amid,
							   F_OIDEQ,
							   ObjectIdGetDatum(accessMethodObjectId));

		ScanKeyEntryInitialize(&entry[1], 0, Anum_pg_amproc_amopclaid,
							   F_OIDEQ, 0);

		relation = heap_openr(AccessMethodProcedureRelationName);

		for (attributeNumber = 1; attributeNumber <= maxAttributeNumber;
			attributeNumber++)
		{
			int16		support;
			Form_pg_amproc form;
			RegProcedure *loc;

			loc = &indexSupport[((attributeNumber - 1) * maxSupportNumber)];

			for (support = 0; support < maxSupportNumber; ++support)
				loc[support] = InvalidOid;

			entry[1].sk_argument =
				ObjectIdGetDatum(operatorClassObjectId[attributeNumber - 1]);

			scan = heap_beginscan(relation, false, SnapshotNow, 2, entry);

			while (HeapTupleIsValid(tuple = heap_getnext(scan, 0)))
			{
				form = (Form_pg_amproc) GETSTRUCT(tuple);
				loc[(form->amprocnum - 1)] = form->amproc;
			}

			heap_endscan(scan);
		}
		heap_close(relation);
	}

	ScanKeyEntryInitialize(&entry[0], 0,
						   Anum_pg_amop_amopid,
						   F_OIDEQ,
						   ObjectIdGetDatum(accessMethodObjectId));

	ScanKeyEntryInitialize(&entry[1], 0,
						   Anum_pg_amop_amopclaid,
						   F_OIDEQ, 0);

	relation = heap_openr(AccessMethodOperatorRelationName);
	operatorRelation = heap_openr(OperatorRelationName);

	for (attributeNumber = maxAttributeNumber; attributeNumber > 0;
		 attributeNumber--)
	{
		StrategyNumber strategy;

		entry[1].sk_argument =
			ObjectIdGetDatum(operatorClassObjectId[attributeNumber - 1]);

		map = IndexStrategyGetStrategyMap(indexStrategy,
										  maxStrategyNumber,
										  attributeNumber);

		for (strategy = 1; strategy <= maxStrategyNumber; strategy++)
			ScanKeyEntrySetIllegal(StrategyMapGetScanKeyEntry(map, strategy));

		scan = heap_beginscan(relation, false, SnapshotNow, 2, entry);

		while (HeapTupleIsValid(tuple = heap_getnext(scan, 0)))
		{
			Form_pg_amop form;

			form = (Form_pg_amop) GETSTRUCT(tuple);
			OperatorRelationFillScanKeyEntry(operatorRelation,
											 form->amopopr,
					StrategyMapGetScanKeyEntry(map, form->amopstrategy));
		}

		heap_endscan(scan);
	}

	heap_close(operatorRelation);
	heap_close(relation);
}

/* ----------------
 *		IndexStrategyDisplay
 * ----------------
 */
#ifdef	ISTRATDEBUG
int
IndexStrategyDisplay(IndexStrategy indexStrategy,
					 StrategyNumber numberOfStrategies,
					 int numberOfAttributes)
{
	StrategyMap strategyMap;
	AttrNumber	attributeNumber;
	StrategyNumber strategyNumber;

	for (attributeNumber = 1; attributeNumber <= numberOfAttributes;
		 attributeNumber += 1)
	{

		strategyMap = IndexStrategyGetStrategyMap(indexStrategy,
												  numberOfStrategies,
												  attributeNumber);

		for (strategyNumber = 1;
			 strategyNumber <= AMStrategies(numberOfStrategies);
			 strategyNumber += 1)
		{

			printf(":att %d\t:str %d\t:opr 0x%x(%d)\n",
				   attributeNumber, strategyNumber,
				   strategyMap->entry[strategyNumber - 1].sk_procedure,
				   strategyMap->entry[strategyNumber - 1].sk_procedure);
		}
	}
}

#endif							/* defined(ISTRATDEBUG) */
