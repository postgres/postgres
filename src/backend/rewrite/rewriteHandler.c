/*-------------------------------------------------------------------------
 *
 * rewriteHandler.c
 *		Primary module of query rewriter.
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/rewrite/rewriteHandler.c,v 1.130.2.1 2004/01/14 03:39:29 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/prep.h"
#include "optimizer/var.h"
#include "parser/analyze.h"
#include "parser/parse_coerce.h"
#include "parser/parse_expr.h"
#include "parser/parse_oper.h"
#include "parser/parse_type.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteHandler.h"
#include "rewrite/rewriteManip.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"


/* We use a list of these to detect recursion in RewriteQuery */
typedef struct rewrite_event
{
	Oid			relation;		/* OID of relation having rules */
	CmdType		event;			/* type of rule being fired */
} rewrite_event;

static Query *rewriteRuleAction(Query *parsetree,
				  Query *rule_action,
				  Node *rule_qual,
				  int rt_index,
				  CmdType event);
static List *adjustJoinTreeList(Query *parsetree, bool removert, int rt_index);
static void rewriteTargetList(Query *parsetree, Relation target_relation);
static TargetEntry *process_matched_tle(TargetEntry *src_tle,
										TargetEntry *prior_tle,
										const char *attrName);
static void markQueryForUpdate(Query *qry, bool skipOldNew);
static List *matchLocks(CmdType event, RuleLock *rulelocks,
		   int varno, Query *parsetree);
static Query *fireRIRrules(Query *parsetree, List *activeRIRs);


/*
 * rewriteRuleAction -
 *	  Rewrite the rule action with appropriate qualifiers (taken from
 *	  the triggering query).
 */
static Query *
rewriteRuleAction(Query *parsetree,
				  Query *rule_action,
				  Node *rule_qual,
				  int rt_index,
				  CmdType event)
{
	int			current_varno,
				new_varno;
	int			rt_length;
	Query	   *sub_action;
	Query	  **sub_action_ptr;

	/*
	 * Make modifiable copies of rule action and qual (what we're passed
	 * are the stored versions in the relcache; don't touch 'em!).
	 */
	rule_action = (Query *) copyObject(rule_action);
	rule_qual = (Node *) copyObject(rule_qual);

	current_varno = rt_index;
	rt_length = length(parsetree->rtable);
	new_varno = PRS2_NEW_VARNO + rt_length;

	/*
	 * Adjust rule action and qual to offset its varnos, so that we can
	 * merge its rtable with the main parsetree's rtable.
	 *
	 * If the rule action is an INSERT...SELECT, the OLD/NEW rtable entries
	 * will be in the SELECT part, and we have to modify that rather than
	 * the top-level INSERT (kluge!).
	 */
	sub_action = getInsertSelectQuery(rule_action, &sub_action_ptr);

	OffsetVarNodes((Node *) sub_action, rt_length, 0);
	OffsetVarNodes(rule_qual, rt_length, 0);
	/* but references to *OLD* should point at original rt_index */
	ChangeVarNodes((Node *) sub_action,
				   PRS2_OLD_VARNO + rt_length, rt_index, 0);
	ChangeVarNodes(rule_qual,
				   PRS2_OLD_VARNO + rt_length, rt_index, 0);

	/*
	 * Generate expanded rtable consisting of main parsetree's rtable plus
	 * rule action's rtable; this becomes the complete rtable for the rule
	 * action.	Some of the entries may be unused after we finish
	 * rewriting, but we leave them all in place for two reasons:
	 *
	 *		* We'd have a much harder job to adjust the query's varnos
	 *		  if we selectively removed RT entries.
	 *
	 *		* If the rule is INSTEAD, then the original query won't be
	 *		  executed at all, and so its rtable must be preserved so that
	 *		  the executor will do the correct permissions checks on it.
	 *
	 * RT entries that are not referenced in the completed jointree will be
	 * ignored by the planner, so they do not affect query semantics.  But
	 * any permissions checks specified in them will be applied during
	 * executor startup (see ExecCheckRTEPerms()).  This allows us to check
	 * that the caller has, say, insert-permission on a view, when the view
	 * is not semantically referenced at all in the resulting query.
	 *
	 * When a rule is not INSTEAD, the permissions checks done on its copied
	 * RT entries will be redundant with those done during execution of the
	 * original query, but we don't bother to treat that case differently.
	 *
	 * NOTE: because planner will destructively alter rtable, we must ensure
	 * that rule action's rtable is separate and shares no substructure
	 * with the main rtable.  Hence do a deep copy here.
	 */
	sub_action->rtable = nconc((List *) copyObject(parsetree->rtable),
							   sub_action->rtable);

	/*
	 * Each rule action's jointree should be the main parsetree's jointree
	 * plus that rule's jointree, but usually *without* the original
	 * rtindex that we're replacing (if present, which it won't be for
	 * INSERT). Note that if the rule action refers to OLD, its jointree
	 * will add a reference to rt_index.  If the rule action doesn't refer
	 * to OLD, but either the rule_qual or the user query quals do, then
	 * we need to keep the original rtindex in the jointree to provide
	 * data for the quals.	We don't want the original rtindex to be
	 * joined twice, however, so avoid keeping it if the rule action
	 * mentions it.
	 *
	 * As above, the action's jointree must not share substructure with the
	 * main parsetree's.
	 */
	if (sub_action->commandType != CMD_UTILITY)
	{
		bool		keeporig;
		List	   *newjointree;

		Assert(sub_action->jointree != NULL);
		keeporig = (!rangeTableEntry_used((Node *) sub_action->jointree,
										  rt_index, 0)) &&
			(rangeTableEntry_used(rule_qual, rt_index, 0) ||
		  rangeTableEntry_used(parsetree->jointree->quals, rt_index, 0));
		newjointree = adjustJoinTreeList(parsetree, !keeporig, rt_index);
		if (newjointree != NIL)
		{
			/*
			 * If sub_action is a setop, manipulating its jointree will do
			 * no good at all, because the jointree is dummy.  (Perhaps
			 * someday we could push the joining and quals down to the
			 * member statements of the setop?)
			 */
			if (sub_action->setOperations != NULL)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("conditional UNION/INTERSECT/EXCEPT statements are not implemented")));

			sub_action->jointree->fromlist =
				nconc(newjointree, sub_action->jointree->fromlist);
		}
	}

	/*
	 * We copy the qualifications of the parsetree to the action and vice
	 * versa. So force hasSubLinks if one of them has it. If this is not
	 * right, the flag will get cleared later, but we mustn't risk having
	 * it not set when it needs to be.	(XXX this should probably be
	 * handled by AddQual and friends, not here...)
	 */
	if (parsetree->hasSubLinks)
		sub_action->hasSubLinks = TRUE;
	else if (sub_action->hasSubLinks)
		parsetree->hasSubLinks = TRUE;

	/*
	 * Event Qualification forces copying of parsetree and splitting into
	 * two queries one w/rule_qual, one w/NOT rule_qual. Also add user
	 * query qual onto rule action
	 */
	AddQual(sub_action, rule_qual);

	AddQual(sub_action, parsetree->jointree->quals);

	/*
	 * Rewrite new.attribute w/ right hand side of target-list entry for
	 * appropriate field name in insert/update.
	 *
	 * KLUGE ALERT: since ResolveNew returns a mutated copy, we can't just
	 * apply it to sub_action; we have to remember to update the sublink
	 * inside rule_action, too.
	 */
	if (event == CMD_INSERT || event == CMD_UPDATE)
	{
		sub_action = (Query *) ResolveNew((Node *) sub_action,
										  new_varno,
										  0,
										  parsetree->targetList,
										  event,
										  current_varno);
		if (sub_action_ptr)
			*sub_action_ptr = sub_action;
		else
			rule_action = sub_action;
	}

	return rule_action;
}

/*
 * Copy the query's jointree list, and optionally attempt to remove any
 * occurrence of the given rt_index as a top-level join item (we do not look
 * for it within join items; this is OK because we are only expecting to find
 * it as an UPDATE or DELETE target relation, which will be at the top level
 * of the join).  Returns modified jointree list --- this is a separate copy
 * sharing no nodes with the original.
 */
static List *
adjustJoinTreeList(Query *parsetree, bool removert, int rt_index)
{
	List	   *newjointree = copyObject(parsetree->jointree->fromlist);
	List	   *jjt;

	if (removert)
	{
		foreach(jjt, newjointree)
		{
			RangeTblRef *rtr = lfirst(jjt);

			if (IsA(rtr, RangeTblRef) &&
				rtr->rtindex == rt_index)
			{
				newjointree = lremove(rtr, newjointree);
				/* foreach is safe because we exit loop after lremove... */
				break;
			}
		}
	}
	return newjointree;
}


/*
 * rewriteTargetList - rewrite INSERT/UPDATE targetlist into standard form
 *
 * This has the following responsibilities:
 *
 * 1. For an INSERT, add tlist entries to compute default values for any
 * attributes that have defaults and are not assigned to in the given tlist.
 * (We do not insert anything for default-less attributes, however.  The
 * planner will later insert NULLs for them, but there's no reason to slow
 * down rewriter processing with extra tlist nodes.)  Also, for both INSERT
 * and UPDATE, replace explicit DEFAULT specifications with column default
 * expressions.
 *
 * 2. Merge multiple entries for the same target attribute, or declare error
 * if we can't.  Presently, multiple entries are only allowed for UPDATE of
 * an array field, for example "UPDATE table SET foo[2] = 42, foo[4] = 43".
 * We can merge such operations into a single assignment op.  Essentially,
 * the expression we want to produce in this case is like
 *		foo = array_set(array_set(foo, 2, 42), 4, 43)
 *
 * 3. Sort the tlist into standard order: non-junk fields in order by resno,
 * then junk fields (these in no particular order).
 *
 * We must do items 1 and 2 before firing rewrite rules, else rewritten
 * references to NEW.foo will produce wrong or incomplete results.	Item 3
 * is not needed for rewriting, but will be needed by the planner, and we
 * can do it essentially for free while handling items 1 and 2.
 */
static void
rewriteTargetList(Query *parsetree, Relation target_relation)
{
	CmdType		commandType = parsetree->commandType;
	List	   *tlist = parsetree->targetList;
	List	   *new_tlist = NIL;
	int			attrno,
				numattrs;
	List	   *temp;

	/*
	 * Scan the tuple description in the relation's relcache entry to make
	 * sure we have all the user attributes in the right order.
	 */
	numattrs = RelationGetNumberOfAttributes(target_relation);

	for (attrno = 1; attrno <= numattrs; attrno++)
	{
		Form_pg_attribute att_tup = target_relation->rd_att->attrs[attrno - 1];
		TargetEntry *new_tle = NULL;

		/* We can ignore deleted attributes */
		if (att_tup->attisdropped)
			continue;

		/*
		 * Look for targetlist entries matching this attr.
		 *
		 * Junk attributes are not candidates to be matched.
		 */
		foreach(temp, tlist)
		{
			TargetEntry *old_tle = (TargetEntry *) lfirst(temp);
			Resdom	   *resdom = old_tle->resdom;

			if (!resdom->resjunk && resdom->resno == attrno)
			{
				new_tle = process_matched_tle(old_tle, new_tle,
											  NameStr(att_tup->attname));
				/* keep scanning to detect multiple assignments to attr */
			}
		}

		/*
		 * Handle the two cases where we need to insert a default
		 * expression: it's an INSERT and there's no tlist entry for the
		 * column, or the tlist entry is a DEFAULT placeholder node.
		 */
		if ((new_tle == NULL && commandType == CMD_INSERT) ||
		  (new_tle && new_tle->expr && IsA(new_tle->expr, SetToDefault)))
		{
			Node	   *new_expr;

			new_expr = build_column_default(target_relation, attrno);

			/*
			 * If there is no default (ie, default is effectively NULL),
			 * we can omit the tlist entry in the INSERT case, since the
			 * planner can insert a NULL for itself, and there's no point
			 * in spending any more rewriter cycles on the entry.  But in
			 * the UPDATE case we've got to explicitly set the column to
			 * NULL.
			 */
			if (!new_expr)
			{
				if (commandType == CMD_INSERT)
					new_tle = NULL;
				else
				{
					new_expr = (Node *) makeConst(att_tup->atttypid,
												  att_tup->attlen,
												  (Datum) 0,
												  true, /* isnull */
												  att_tup->attbyval);
					/* this is to catch a NOT NULL domain constraint */
					new_expr = coerce_to_domain(new_expr,
												InvalidOid,
												att_tup->atttypid,
												COERCE_IMPLICIT_CAST);
				}
			}

			if (new_expr)
				new_tle = makeTargetEntry(makeResdom(attrno,
													 att_tup->atttypid,
													 att_tup->atttypmod,
									  pstrdup(NameStr(att_tup->attname)),
													 false),
										  (Expr *) new_expr);
		}

		if (new_tle)
			new_tlist = lappend(new_tlist, new_tle);
	}

	/*
	 * Copy all resjunk tlist entries to the end of the new tlist, and
	 * assign them resnos above the last real resno.
	 *
	 * Typical junk entries include ORDER BY or GROUP BY expressions (are
	 * these actually possible in an INSERT or UPDATE?), system attribute
	 * references, etc.
	 */
	foreach(temp, tlist)
	{
		TargetEntry *old_tle = (TargetEntry *) lfirst(temp);
		Resdom	   *resdom = old_tle->resdom;

		if (resdom->resjunk)
		{
			/* Get the resno right, but don't copy unnecessarily */
			if (resdom->resno != attrno)
			{
				resdom = (Resdom *) copyObject((Node *) resdom);
				resdom->resno = attrno;
				old_tle = makeTargetEntry(resdom, old_tle->expr);
			}
			new_tlist = lappend(new_tlist, old_tle);
			attrno++;
		}
		else
		{
			/* Let's just make sure we processed all the non-junk items */
			if (resdom->resno < 1 || resdom->resno > numattrs)
				elog(ERROR, "bogus resno %d in targetlist", resdom->resno);
		}
	}

	parsetree->targetList = new_tlist;
}


/*
 * Convert a matched TLE from the original tlist into a correct new TLE.
 *
 * This routine detects and handles multiple assignments to the same target
 * attribute.  (The attribute name is needed only for error messages.)
 */
static TargetEntry *
process_matched_tle(TargetEntry *src_tle,
					TargetEntry *prior_tle,
					const char *attrName)
{
	Resdom	   *resdom = src_tle->resdom;
	Node	   *priorbottom;
	ArrayRef   *newexpr;

	if (prior_tle == NULL)
	{
		/*
		 * Normal case where this is the first assignment to the
		 * attribute.
		 */
		return src_tle;
	}

	/*
	 * Multiple assignments to same attribute.	Allow only if all are
	 * array-assign operators with same bottom array object.
	 */
	if (src_tle->expr == NULL || !IsA(src_tle->expr, ArrayRef) ||
		((ArrayRef *) src_tle->expr)->refassgnexpr == NULL ||
		prior_tle->expr == NULL || !IsA(prior_tle->expr, ArrayRef) ||
		((ArrayRef *) prior_tle->expr)->refassgnexpr == NULL ||
		((ArrayRef *) src_tle->expr)->refrestype !=
		((ArrayRef *) prior_tle->expr)->refrestype)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("multiple assignments to same column \"%s\"",
						attrName)));

	/*
	 * Prior TLE could be a nest of ArrayRefs if we do this more than
	 * once.
	 */
	priorbottom = (Node *) ((ArrayRef *) prior_tle->expr)->refexpr;
	while (priorbottom != NULL && IsA(priorbottom, ArrayRef) &&
		   ((ArrayRef *) priorbottom)->refassgnexpr != NULL)
		priorbottom = (Node *) ((ArrayRef *) priorbottom)->refexpr;
	if (!equal(priorbottom, ((ArrayRef *) src_tle->expr)->refexpr))
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("multiple assignments to same column \"%s\"",
						attrName)));

	/*
	 * Looks OK to nest 'em.
	 */
	newexpr = makeNode(ArrayRef);
	memcpy(newexpr, src_tle->expr, sizeof(ArrayRef));
	newexpr->refexpr = prior_tle->expr;

	return makeTargetEntry(resdom, (Expr *) newexpr);
}


/*
 * Make an expression tree for the default value for a column.
 *
 * If there is no default, return a NULL instead.
 */
Node *
build_column_default(Relation rel, int attrno)
{
	TupleDesc	rd_att = rel->rd_att;
	Form_pg_attribute att_tup = rd_att->attrs[attrno - 1];
	Oid			atttype = att_tup->atttypid;
	int32		atttypmod = att_tup->atttypmod;
	Node	   *expr = NULL;
	Oid			exprtype;

	/*
	 * Scan to see if relation has a default for this column.
	 */
	if (rd_att->constr && rd_att->constr->num_defval > 0)
	{
		AttrDefault *defval = rd_att->constr->defval;
		int			ndef = rd_att->constr->num_defval;

		while (--ndef >= 0)
		{
			if (attrno == defval[ndef].adnum)
			{
				/*
				 * Found it, convert string representation to node tree.
				 */
				expr = stringToNode(defval[ndef].adbin);
				break;
			}
		}
	}

	if (expr == NULL)
	{
		/*
		 * No per-column default, so look for a default for the type
		 * itself.
		 */
		if (att_tup->attisset)
		{
			/*
			 * Set attributes are represented as OIDs no matter what the
			 * set element type is, and the element type's default is
			 * irrelevant too.
			 */
		}
		else
			expr = get_typdefault(atttype);
	}

	if (expr == NULL)
		return NULL;			/* No default anywhere */

	/*
	 * Make sure the value is coerced to the target column type; this will
	 * generally be true already, but there seem to be some corner cases
	 * involving domain defaults where it might not be true. This should
	 * match the parser's processing of non-defaulted expressions --- see
	 * updateTargetListEntry().
	 */
	exprtype = exprType(expr);

	expr = coerce_to_target_type(NULL,	/* no UNKNOWN params here */
								 expr, exprtype,
								 atttype, atttypmod,
								 COERCION_ASSIGNMENT,
								 COERCE_IMPLICIT_CAST);
	if (expr == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("column \"%s\" is of type %s"
						" but default expression is of type %s",
						NameStr(att_tup->attname),
						format_type_be(atttype),
						format_type_be(exprtype)),
		   errhint("You will need to rewrite or cast the expression.")));

	return expr;
}


/*
 * matchLocks -
 *	  match the list of locks and returns the matching rules
 */
static List *
matchLocks(CmdType event,
		   RuleLock *rulelocks,
		   int varno,
		   Query *parsetree)
{
	List	   *matching_locks = NIL;
	int			nlocks;
	int			i;

	if (rulelocks == NULL)
		return NIL;

	if (parsetree->commandType != CMD_SELECT)
	{
		if (parsetree->resultRelation != varno)
			return NIL;
	}

	nlocks = rulelocks->numLocks;

	for (i = 0; i < nlocks; i++)
	{
		RewriteRule *oneLock = rulelocks->rules[i];

		if (oneLock->event == event)
		{
			if (parsetree->commandType != CMD_SELECT ||
				(oneLock->attrno == -1 ?
				 rangeTableEntry_used((Node *) parsetree, varno, 0) :
				 attribute_used((Node *) parsetree,
								varno, oneLock->attrno, 0)))
				matching_locks = lappend(matching_locks, oneLock);
		}
	}

	return matching_locks;
}


static Query *
ApplyRetrieveRule(Query *parsetree,
				  RewriteRule *rule,
				  int rt_index,
				  bool relation_level,
				  Relation relation,
				  bool relIsUsed,
				  List *activeRIRs)
{
	Query	   *rule_action;
	RangeTblEntry *rte,
			   *subrte;

	if (length(rule->actions) != 1)
		elog(ERROR, "expected just one rule action");
	if (rule->qual != NULL)
		elog(ERROR, "cannot handle qualified ON SELECT rule");
	if (!relation_level)
		elog(ERROR, "cannot handle per-attribute ON SELECT rule");

	/*
	 * Make a modifiable copy of the view query, and recursively expand
	 * any view references inside it.
	 */
	rule_action = copyObject(lfirst(rule->actions));

	rule_action = fireRIRrules(rule_action, activeRIRs);

	/*
	 * VIEWs are really easy --- just plug the view query in as a
	 * subselect, replacing the relation's original RTE.
	 */
	rte = rt_fetch(rt_index, parsetree->rtable);

	rte->rtekind = RTE_SUBQUERY;
	rte->relid = InvalidOid;
	rte->subquery = rule_action;
	rte->inh = false;			/* must not be set for a subquery */

	/*
	 * We move the view's permission check data down to its rangetable.
	 * The checks will actually be done against the *OLD* entry therein.
	 */
	subrte = rt_fetch(PRS2_OLD_VARNO, rule_action->rtable);
	Assert(subrte->relid == relation->rd_id);
	subrte->checkForRead = rte->checkForRead;
	subrte->checkForWrite = rte->checkForWrite;
	subrte->checkAsUser = rte->checkAsUser;

	rte->checkForRead = false;	/* no permission check on subquery itself */
	rte->checkForWrite = false;
	rte->checkAsUser = InvalidOid;

	/*
	 * FOR UPDATE of view?
	 */
	if (intMember(rt_index, parsetree->rowMarks))
	{
		/*
		 * Remove the view from the list of rels that will actually be
		 * marked FOR UPDATE by the executor.  It will still be access-
		 * checked for write access, though.
		 */
		parsetree->rowMarks = lremovei(rt_index, parsetree->rowMarks);

		/*
		 * Set up the view's referenced tables as if FOR UPDATE.
		 */
		markQueryForUpdate(rule_action, true);
	}

	return parsetree;
}

/*
 * Recursively mark all relations used by a view as FOR UPDATE.
 *
 * This may generate an invalid query, eg if some sub-query uses an
 * aggregate.  We leave it to the planner to detect that.
 *
 * NB: this must agree with the parser's transformForUpdate() routine.
 */
static void
markQueryForUpdate(Query *qry, bool skipOldNew)
{
	Index		rti = 0;
	List	   *l;

	foreach(l, qry->rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(l);

		rti++;

		/* Ignore OLD and NEW entries if we are at top level of view */
		if (skipOldNew &&
			(rti == PRS2_OLD_VARNO || rti == PRS2_NEW_VARNO))
			continue;

		if (rte->rtekind == RTE_RELATION)
		{
			if (!intMember(rti, qry->rowMarks))
				qry->rowMarks = lappendi(qry->rowMarks, rti);
			rte->checkForWrite = true;
		}
		else if (rte->rtekind == RTE_SUBQUERY)
		{
			/* FOR UPDATE of subquery is propagated to subquery's rels */
			markQueryForUpdate(rte->subquery, false);
		}
	}
}


/*
 * fireRIRonSubLink -
 *	Apply fireRIRrules() to each SubLink (subselect in expression) found
 *	in the given tree.
 *
 * NOTE: although this has the form of a walker, we cheat and modify the
 * SubLink nodes in-place.	It is caller's responsibility to ensure that
 * no unwanted side-effects occur!
 *
 * This is unlike most of the other routines that recurse into subselects,
 * because we must take control at the SubLink node in order to replace
 * the SubLink's subselect link with the possibly-rewritten subquery.
 */
static bool
fireRIRonSubLink(Node *node, List *activeRIRs)
{
	if (node == NULL)
		return false;
	if (IsA(node, SubLink))
	{
		SubLink    *sub = (SubLink *) node;

		/* Do what we came for */
		sub->subselect = (Node *) fireRIRrules((Query *) sub->subselect,
											   activeRIRs);
		/* Fall through to process lefthand args of SubLink */
	}

	/*
	 * Do NOT recurse into Query nodes, because fireRIRrules already
	 * processed subselects of subselects for us.
	 */
	return expression_tree_walker(node, fireRIRonSubLink,
								  (void *) activeRIRs);
}


/*
 * fireRIRrules -
 *	Apply all RIR rules on each rangetable entry in a query
 */
static Query *
fireRIRrules(Query *parsetree, List *activeRIRs)
{
	int			rt_index;

	/*
	 * don't try to convert this into a foreach loop, because rtable list
	 * can get changed each time through...
	 */
	rt_index = 0;
	while (rt_index < length(parsetree->rtable))
	{
		RangeTblEntry *rte;
		Relation	rel;
		List	   *locks;
		RuleLock   *rules;
		RewriteRule *rule;
		LOCKMODE	lockmode;
		bool		relIsUsed;
		int			i;

		++rt_index;

		rte = rt_fetch(rt_index, parsetree->rtable);

		/*
		 * A subquery RTE can't have associated rules, so there's nothing
		 * to do to this level of the query, but we must recurse into the
		 * subquery to expand any rule references in it.
		 */
		if (rte->rtekind == RTE_SUBQUERY)
		{
			rte->subquery = fireRIRrules(rte->subquery, activeRIRs);
			continue;
		}

		/*
		 * Joins and other non-relation RTEs can be ignored completely.
		 */
		if (rte->rtekind != RTE_RELATION)
			continue;

		/*
		 * If the table is not referenced in the query, then we ignore it.
		 * This prevents infinite expansion loop due to new rtable entries
		 * inserted by expansion of a rule. A table is referenced if it is
		 * part of the join set (a source table), or is referenced by any
		 * Var nodes, or is the result table.
		 */
		relIsUsed = rangeTableEntry_used((Node *) parsetree, rt_index, 0);

		if (!relIsUsed && rt_index != parsetree->resultRelation)
			continue;

		/*
		 * This may well be the first access to the relation during the
		 * current statement (it will be, if this Query was extracted from
		 * a rule or somehow got here other than via the parser).
		 * Therefore, grab the appropriate lock type for the relation, and
		 * do not release it until end of transaction.	This protects the
		 * rewriter and planner against schema changes mid-query.
		 *
		 * If the relation is the query's result relation, then
		 * RewriteQuery() already got the right lock on it, so we need no
		 * additional lock. Otherwise, check to see if the relation is
		 * accessed FOR UPDATE or not.
		 */
		if (rt_index == parsetree->resultRelation)
			lockmode = NoLock;
		else if (intMember(rt_index, parsetree->rowMarks))
			lockmode = RowShareLock;
		else
			lockmode = AccessShareLock;

		rel = heap_open(rte->relid, lockmode);

		/*
		 * Collect the RIR rules that we must apply
		 */
		rules = rel->rd_rules;
		if (rules == NULL)
		{
			heap_close(rel, NoLock);
			continue;
		}
		locks = NIL;
		for (i = 0; i < rules->numLocks; i++)
		{
			rule = rules->rules[i];
			if (rule->event != CMD_SELECT)
				continue;

			if (rule->attrno > 0)
			{
				/* per-attr rule; do we need it? */
				if (!attribute_used((Node *) parsetree, rt_index,
									rule->attrno, 0))
					continue;
			}

			locks = lappend(locks, rule);
		}

		/*
		 * If we found any, apply them --- but first check for recursion!
		 */
		if (locks != NIL)
		{
			List	   *newActiveRIRs;
			List	   *l;

			if (oidMember(RelationGetRelid(rel), activeRIRs))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("infinite recursion detected in rules for relation \"%s\"",
								RelationGetRelationName(rel))));
			newActiveRIRs = lconso(RelationGetRelid(rel), activeRIRs);

			foreach(l, locks)
			{
				rule = lfirst(l);

				parsetree = ApplyRetrieveRule(parsetree,
											  rule,
											  rt_index,
											  rule->attrno == -1,
											  rel,
											  relIsUsed,
											  newActiveRIRs);
			}
		}

		heap_close(rel, NoLock);
	}

	/*
	 * Recurse into sublink subqueries, too.  But we already did the ones
	 * in the rtable.
	 */
	if (parsetree->hasSubLinks)
		query_tree_walker(parsetree, fireRIRonSubLink, (void *) activeRIRs,
						  QTW_IGNORE_RT_SUBQUERIES);

	/*
	 * If the query was marked having aggregates, check if this is still
	 * true after rewriting.  Ditto for sublinks.  Note there should be no
	 * aggs in the qual at this point.	(Does this code still do anything
	 * useful?	The view-becomes-subselect-in-FROM approach doesn't look
	 * like it could remove aggs or sublinks...)
	 */
	if (parsetree->hasAggs)
	{
		parsetree->hasAggs = checkExprHasAggs((Node *) parsetree);
		if (parsetree->hasAggs)
			if (checkExprHasAggs((Node *) parsetree->jointree))
				elog(ERROR, "failed to remove aggregates from qual");
	}
	if (parsetree->hasSubLinks)
		parsetree->hasSubLinks = checkExprHasSubLink((Node *) parsetree);

	return parsetree;
}


/*
 * Modify the given query by adding 'AND rule_qual IS NOT TRUE' to its
 * qualification.  This is used to generate suitable "else clauses" for
 * conditional INSTEAD rules.  (Unfortunately we must use "x IS NOT TRUE",
 * not just "NOT x" which the planner is much smarter about, else we will
 * do the wrong thing when the qual evaluates to NULL.)
 *
 * The rule_qual may contain references to OLD or NEW.	OLD references are
 * replaced by references to the specified rt_index (the relation that the
 * rule applies to).  NEW references are only possible for INSERT and UPDATE
 * queries on the relation itself, and so they should be replaced by copies
 * of the related entries in the query's own targetlist.
 */
static Query *
CopyAndAddInvertedQual(Query *parsetree,
					   Node *rule_qual,
					   int rt_index,
					   CmdType event)
{
	Query	   *new_tree = (Query *) copyObject(parsetree);
	Node	   *new_qual = (Node *) copyObject(rule_qual);

	/* Fix references to OLD */
	ChangeVarNodes(new_qual, PRS2_OLD_VARNO, rt_index, 0);
	/* Fix references to NEW */
	if (event == CMD_INSERT || event == CMD_UPDATE)
		new_qual = ResolveNew(new_qual,
							  PRS2_NEW_VARNO,
							  0,
							  parsetree->targetList,
							  event,
							  rt_index);
	/* And attach the fixed qual */
	AddInvertedQual(new_tree, new_qual);

	return new_tree;
}


/*
 *	fireRules -
 *	   Iterate through rule locks applying rules.
 *
 * Input arguments:
 *	parsetree - original query
 *	rt_index - RT index of result relation in original query
 *	event - type of rule event
 *	locks - list of rules to fire
 * Output arguments:
 *	*instead_flag - set TRUE if any unqualified INSTEAD rule is found
 *					(must be initialized to FALSE)
 *	*qual_product - filled with modified original query if any qualified
 *					INSTEAD rule is found (must be initialized to NULL)
 * Return value:
 *	list of rule actions adjusted for use with this query
 *
 * Qualified INSTEAD rules generate their action with the qualification
 * condition added.  They also generate a modified version of the original
 * query with the negated qualification added, so that it will run only for
 * rows that the qualified action doesn't act on.  (If there are multiple
 * qualified INSTEAD rules, we AND all the negated quals onto a single
 * modified original query.)  We won't execute the original, unmodified
 * query if we find either qualified or unqualified INSTEAD rules.	If
 * we find both, the modified original query is discarded too.
 */
static List *
fireRules(Query *parsetree,
		  int rt_index,
		  CmdType event,
		  List *locks,
		  bool *instead_flag,
		  Query **qual_product)
{
	List	   *results = NIL;
	List	   *i;

	foreach(i, locks)
	{
		RewriteRule *rule_lock = (RewriteRule *) lfirst(i);
		Node	   *event_qual = rule_lock->qual;
		List	   *actions = rule_lock->actions;
		QuerySource qsrc;
		List	   *r;

		/* Determine correct QuerySource value for actions */
		if (rule_lock->isInstead)
		{
			if (event_qual != NULL)
				qsrc = QSRC_QUAL_INSTEAD_RULE;
			else
			{
				qsrc = QSRC_INSTEAD_RULE;
				*instead_flag = true;	/* report unqualified INSTEAD */
			}
		}
		else
			qsrc = QSRC_NON_INSTEAD_RULE;

		if (qsrc == QSRC_QUAL_INSTEAD_RULE)
		{
			/*
			 * If there are INSTEAD rules with qualifications, the
			 * original query is still performed. But all the negated rule
			 * qualifications of the INSTEAD rules are added so it does
			 * its actions only in cases where the rule quals of all
			 * INSTEAD rules are false. Think of it as the default action
			 * in a case. We save this in *qual_product so RewriteQuery()
			 * can add it to the query list after we mangled it up enough.
			 *
			 * If we have already found an unqualified INSTEAD rule, then
			 * *qual_product won't be used, so don't bother building it.
			 */
			if (!*instead_flag)
			{
				if (*qual_product == NULL)
					*qual_product = parsetree;
				*qual_product = CopyAndAddInvertedQual(*qual_product,
													   event_qual,
													   rt_index,
													   event);
			}
		}

		/* Now process the rule's actions and add them to the result list */
		foreach(r, actions)
		{
			Query	   *rule_action = lfirst(r);

			if (rule_action->commandType == CMD_NOTHING)
				continue;

			rule_action = rewriteRuleAction(parsetree, rule_action,
											event_qual, rt_index, event);

			rule_action->querySource = qsrc;
			rule_action->canSetTag = false;		/* might change later */

			results = lappend(results, rule_action);
		}
	}

	return results;
}


/*
 * RewriteQuery -
 *	  rewrites the query and apply the rules again on the queries rewritten
 *
 * rewrite_events is a list of open query-rewrite actions, so we can detect
 * infinite recursion.
 */
static List *
RewriteQuery(Query *parsetree, List *rewrite_events)
{
	CmdType		event = parsetree->commandType;
	bool		instead = false;
	Query	   *qual_product = NULL;
	List	   *rewritten = NIL;

	/*
	 * If the statement is an update, insert or delete - fire rules on it.
	 *
	 * SELECT rules are handled later when we have all the queries that
	 * should get executed.  Also, utilities aren't rewritten at all (do
	 * we still need that check?)
	 */
	if (event != CMD_SELECT && event != CMD_UTILITY)
	{
		int			result_relation;
		RangeTblEntry *rt_entry;
		Relation	rt_entry_relation;
		List	   *locks;

		result_relation = parsetree->resultRelation;
		Assert(result_relation != 0);
		rt_entry = rt_fetch(result_relation, parsetree->rtable);
		Assert(rt_entry->rtekind == RTE_RELATION);

		/*
		 * This may well be the first access to the result relation during
		 * the current statement (it will be, if this Query was extracted
		 * from a rule or somehow got here other than via the parser).
		 * Therefore, grab the appropriate lock type for a result
		 * relation, and do not release it until end of transaction.  This
		 * protects the rewriter and planner against schema changes
		 * mid-query.
		 */
		rt_entry_relation = heap_open(rt_entry->relid, RowExclusiveLock);

		/*
		 * If it's an INSERT or UPDATE, rewrite the targetlist into
		 * standard form.  This will be needed by the planner anyway, and
		 * doing it now ensures that any references to NEW.field will
		 * behave sanely.
		 */
		if (event == CMD_INSERT || event == CMD_UPDATE)
			rewriteTargetList(parsetree, rt_entry_relation);

		/*
		 * Collect and apply the appropriate rules.
		 */
		locks = matchLocks(event, rt_entry_relation->rd_rules,
						   result_relation, parsetree);

		if (locks != NIL)
		{
			List	   *product_queries;

			product_queries = fireRules(parsetree,
										result_relation,
										event,
										locks,
										&instead,
										&qual_product);

			/*
			 * If we got any product queries, recursively rewrite them ---
			 * but first check for recursion!
			 */
			if (product_queries != NIL)
			{
				List	   *n;
				rewrite_event *rev;

				foreach(n, rewrite_events)
				{
					rev = (rewrite_event *) lfirst(n);
					if (rev->relation == RelationGetRelid(rt_entry_relation) &&
						rev->event == event)
						ereport(ERROR,
							 (errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							  errmsg("infinite recursion detected in rules for relation \"%s\"",
						   RelationGetRelationName(rt_entry_relation))));
				}

				rev = (rewrite_event *) palloc(sizeof(rewrite_event));
				rev->relation = RelationGetRelid(rt_entry_relation);
				rev->event = event;
				rewrite_events = lcons(rev, rewrite_events);

				foreach(n, product_queries)
				{
					Query	   *pt = (Query *) lfirst(n);
					List	   *newstuff;

					newstuff = RewriteQuery(pt, rewrite_events);
					rewritten = nconc(rewritten, newstuff);
				}
			}
		}

		heap_close(rt_entry_relation, NoLock);	/* keep lock! */
	}

	/*
	 * For INSERTs, the original query is done first; for UPDATE/DELETE,
	 * it is done last.  This is needed because update and delete rule
	 * actions might not do anything if they are invoked after the update
	 * or delete is performed. The command counter increment between the
	 * query executions makes the deleted (and maybe the updated) tuples
	 * disappear so the scans for them in the rule actions cannot find
	 * them.
	 *
	 * If we found any unqualified INSTEAD, the original query is not done at
	 * all, in any form.  Otherwise, we add the modified form if qualified
	 * INSTEADs were found, else the unmodified form.
	 */
	if (!instead)
	{
		if (parsetree->commandType == CMD_INSERT)
		{
			if (qual_product != NULL)
				rewritten = lcons(qual_product, rewritten);
			else
				rewritten = lcons(parsetree, rewritten);
		}
		else
		{
			if (qual_product != NULL)
				rewritten = lappend(rewritten, qual_product);
			else
				rewritten = lappend(rewritten, parsetree);
		}
	}

	return rewritten;
}


/*
 * QueryRewrite -
 *	  Primary entry point to the query rewriter.
 *	  Rewrite one query via query rewrite system, possibly returning 0
 *	  or many queries.
 *
 * NOTE: The code in QueryRewrite was formerly in pg_parse_and_plan(), and was
 * moved here so that it would be invoked during EXPLAIN.
 */
List *
QueryRewrite(Query *parsetree)
{
	List	   *querylist;
	List	   *results = NIL;
	List	   *l;
	CmdType		origCmdType;
	bool		foundOriginalQuery;
	Query	   *lastInstead;

	/*
	 * Step 1
	 *
	 * Apply all non-SELECT rules possibly getting 0 or many queries
	 */
	querylist = RewriteQuery(parsetree, NIL);

	/*
	 * Step 2
	 *
	 * Apply all the RIR rules on each query
	 */
	foreach(l, querylist)
	{
		Query	   *query = (Query *) lfirst(l);

		query = fireRIRrules(query, NIL);

		/*
		 * If the query target was rewritten as a view, complain.
		 */
		if (query->resultRelation)
		{
			RangeTblEntry *rte = rt_fetch(query->resultRelation,
										  query->rtable);

			if (rte->rtekind == RTE_SUBQUERY)
			{
				switch (query->commandType)
				{
					case CMD_INSERT:
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("cannot insert into a view"),
								 errhint("You need an unconditional ON INSERT DO INSTEAD rule.")));
						break;
					case CMD_UPDATE:
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("cannot update a view"),
								 errhint("You need an unconditional ON UPDATE DO INSTEAD rule.")));
						break;
					case CMD_DELETE:
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("cannot delete from a view"),
								 errhint("You need an unconditional ON DELETE DO INSTEAD rule.")));
						break;
					default:
						elog(ERROR, "unrecognized commandType: %d",
							 (int) query->commandType);
						break;
				}
			}
		}

		results = lappend(results, query);
	}

	/*
	 * Step 3
	 *
	 * Determine which, if any, of the resulting queries is supposed to set
	 * the command-result tag; and update the canSetTag fields
	 * accordingly.
	 *
	 * If the original query is still in the list, it sets the command tag.
	 * Otherwise, the last INSTEAD query of the same kind as the original
	 * is allowed to set the tag.  (Note these rules can leave us with no
	 * query setting the tag.  The tcop code has to cope with this by
	 * setting up a default tag based on the original un-rewritten query.)
	 *
	 * The Asserts verify that at most one query in the result list is marked
	 * canSetTag.  If we aren't checking asserts, we can fall out of the
	 * loop as soon as we find the original query.
	 */
	origCmdType = parsetree->commandType;
	foundOriginalQuery = false;
	lastInstead = NULL;

	foreach(l, results)
	{
		Query	   *query = (Query *) lfirst(l);

		if (query->querySource == QSRC_ORIGINAL)
		{
			Assert(query->canSetTag);
			Assert(!foundOriginalQuery);
			foundOriginalQuery = true;
#ifndef USE_ASSERT_CHECKING
			break;
#endif
		}
		else
		{
			Assert(!query->canSetTag);
			if (query->commandType == origCmdType &&
				(query->querySource == QSRC_INSTEAD_RULE ||
				 query->querySource == QSRC_QUAL_INSTEAD_RULE))
				lastInstead = query;
		}
	}

	if (!foundOriginalQuery && lastInstead != NULL)
		lastInstead->canSetTag = true;

	return results;
}
