/*------------------------------------------------------------------------
 *
 * geqo_eval.c--
 *    Routines to evaluate query trees
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: geqo_eval.c,v 1.9 1997/06/06 00:38:17 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */

/* contributed by:
   =*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=
   *  Martin Utesch              * Institute of Automatic Control      *
   =                             = University of Mining and Technology =
   *  utesch@aut.tu-freiberg.de  * Freiberg, Germany                   *
   =*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=
 */

#include "postgres.h"

#include <math.h>
#ifdef HAVE_LIMITS_H
# include <limits.h>
# ifndef MAXINT
#  define MAXINT INT_MAX
# endif
#else
# include <values.h>
#endif 

#include "nodes/pg_list.h"
#include "nodes/relation.h"
#include "nodes/primnodes.h"

#include "utils/palloc.h"
#include "utils/elog.h"

#include "optimizer/internal.h"
#include "optimizer/paths.h"
#include "optimizer/pathnode.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/tlist.h"
#include "optimizer/joininfo.h"

#include "optimizer/geqo_gene.h"
#include "optimizer/geqo.h"
#include "optimizer/geqo_paths.h"


static List *gimme_clause_joins(Query *root, Rel *outer_rel, Rel *inner_rel);
static Rel *gimme_clauseless_join(Rel *outer_rel, Rel *inner_rel);
static Rel *init_join_rel(Rel *outer_rel, Rel *inner_rel, JInfo *joininfo);
static List *new_join_tlist(List *tlist, List *other_relids, int first_resdomno);
static List *new_joininfo_list(List *joininfo_list, List *join_relids);
static void add_superrels(Rel *rel, Rel *super_rel);
static bool nonoverlap_rels(Rel *rel1, Rel *rel2);
static bool nonoverlap_sets(List *s1, List *s2);
static void geqo_joinrel_size(Rel *joinrel, Rel *outer_rel, Rel *inner_rel);

static void geqo_add_new_joininfos(Query *root, List *joinrels, List *outerrels);
static List *geqo_final_join_rels(List *join_rel_list);

static Rel *geqo_nth(int stop, List *rels);

/*    
 * geqo_eval--
 *    
 * Returns cost of a query tree as an individual of the population.
 */
Cost
geqo_eval (Query *root, Gene *tour, int num_gene)
{
 Rel *joinrel;
 Cost fitness;
 List *temp;


/* remember root->join_relation_list_ ... */
/* because root->join_relation_list_ will be changed during the following */
 temp = listCopy(root->join_relation_list_);

/* joinrel is readily processed query tree -- left-sided ! */
 joinrel = gimme_tree(root, tour, 0, num_gene, NULL);

/* compute fitness */
 fitness = (Cost) joinrel->cheapestpath->path_cost;

 root->join_relation_list_ = listCopy(temp);

 pfree(joinrel);
 freeList(temp);

 return(fitness);

}

/*    
 * gimme-tree --
 *    this program presumes that only LEFT-SIDED TREES are considered!
 *    
 * 'outer_rel' is the preceeding join
 *    
 * Returns a new join relation incorporating all joins in a left-sided tree.
 */
Rel *
gimme_tree (Query *root, Gene *tour, int rel_count, int num_gene, Rel *outer_rel)
{
 Rel *inner_rel; /* current relation */
 int base_rel_index;

 List *new_rels = NIL;
 Rel *new_rel = NULL;

 if (rel_count < num_gene ) { /* tree not yet finished */

	/* tour[0] = 3; tour[1] = 1; tour[2] = 2 */
 	base_rel_index = (int) tour[rel_count];

 	inner_rel = (Rel *) geqo_nth(base_rel_index,root->base_relation_list_);

	if (rel_count == 0) { /* processing first join with base_rel_index = (int) tour[0] */
		rel_count++;
		return gimme_tree(root, tour, rel_count, num_gene, inner_rel);
		}
	else { /* tree main part */

		if(!(new_rels = gimme_clause_joins(root, outer_rel,inner_rel))) {
			if (BushyPlanFlag) {
				new_rels = lcons(gimme_clauseless_join(outer_rel,outer_rel),NIL); /* ??? MAU */
				}
			else {
				new_rels = lcons(gimme_clauseless_join(outer_rel,inner_rel),NIL);
				}
			}

		/* process new_rel->pathlist */
		find_all_join_paths(root, new_rels);

		/* prune new_rels */
		/* MAU: is this necessary? */
		/* what's the matter if more than one new rel is left till now? */
		/* joinrels in newrels with different ordering of relids are not possible */
		if (length(new_rels) > 1) new_rels = geqo_prune_rels(new_rels);

		if (length(new_rels) > 1) { /* should never be reached ... */
			elog(DEBUG,"gimme_tree: still %d relations left", length(new_rels));
			}

		/* get essential new relation */
		new_rel = (Rel *) lfirst(new_rels);
		rel_count++;

		/* process new_rel->cheapestpath, new_rel->unorderedpath */
		geqo_rel_paths(new_rel);

		/* processing of other new_rel attributes */
		new_rel->size = compute_rel_size(new_rel);
		new_rel->width = compute_rel_width(new_rel);

		root->join_relation_list_ = lcons(new_rel, NIL);

		return gimme_tree(root, tour, rel_count, num_gene, new_rel);
		}

	}

 return (outer_rel); /* tree finished ... */
}

/*    
 * gimme-clause-joins--
 *
 * 'outer-rel' is the relation entry for the outer relation
 * 'inner-rel' is the relation entry for the inner relation
 *    
 * Returns a list of new join relations.
 */

static List *
gimme_clause_joins(Query *root, Rel *outer_rel, Rel *inner_rel)
{
 List *join_list = NIL;
 List *i = NIL;
 List *joininfo_list = (List *) outer_rel->joininfo;
    
 foreach (i, joininfo_list) {
	JInfo *joininfo = (JInfo*)lfirst(i);
	Rel *rel = NULL;

	if(!joininfo->inactive) {
		List *other_rels = (List *)joininfo->otherrels;

		if(other_rels != NIL) {
			if( (length(other_rels) == 1) ) {

				if( same(other_rels, inner_rel->relids) ) { /* look if inner_rel is it...*/
					rel = init_join_rel(outer_rel, inner_rel, joininfo);
					}
				}
		else if (BushyPlanFlag) { /* ?!? MAU */
			rel = init_join_rel(outer_rel, get_join_rel(root, other_rels), joininfo);
			}
		else {
			rel = NULL;
			}

		if (rel != NULL)
			join_list = lappend(join_list, rel);

			}
		}
	}

   return(join_list);
}

/*    
 * gimme-clauseless-join--
 *    Given an outer relation 'outer-rel' and an inner relation
 *    'inner-rel', create a join relation between 'outer-rel' and 'inner-rel'
 *    
 * Returns a new join relation.
 */

static Rel *
gimme_clauseless_join(Rel *outer_rel, Rel *inner_rel)
{
    return(init_join_rel(outer_rel, inner_rel, (JInfo*)NULL));
}

/*    
 * init-join-rel--
 *    Creates and initializes a new join relation.
 *    
 * 'outer-rel' and 'inner-rel' are relation nodes for the relations to be
 * 	joined
 * 'joininfo' is the joininfo node(join clause) containing both
 * 	'outer-rel' and 'inner-rel', if any exists
 *    
 * Returns the new join relation node.
 */
static Rel *
init_join_rel(Rel *outer_rel, Rel *inner_rel, JInfo *joininfo)
{
    Rel *joinrel = makeNode(Rel);
    List *joinrel_joininfo_list = NIL;
    List *new_outer_tlist;
    List *new_inner_tlist;

    /*
     * Create a new tlist by removing irrelevant elements from both
     * tlists of the outer and inner join relations and then merging
     * the results together.
     */
    new_outer_tlist = 
	new_join_tlist(outer_rel->targetlist,   /*   XXX 1-based attnos */
		       inner_rel->relids, 1);
    new_inner_tlist = 
	new_join_tlist(inner_rel->targetlist,   /*   XXX 1-based attnos */
		       outer_rel->relids,
		       length(new_outer_tlist) + 1);
    
    joinrel->relids = NIL;
    joinrel->indexed = false;
    joinrel->pages = 0;
    joinrel->tuples = 0;
    joinrel->width = 0;
/*    joinrel->targetlist = NIL;*/
    joinrel->pathlist = NIL;
    joinrel->unorderedpath = (Path *)NULL;
    joinrel->cheapestpath = (Path *)NULL;
    joinrel->pruneable = true;
    joinrel->classlist = NULL;
    joinrel->relam = InvalidOid;
    joinrel->ordering = NULL;
    joinrel->clauseinfo = NIL;
    joinrel->joininfo = NULL;
    joinrel->innerjoin = NIL;
    joinrel->superrels = NIL;

    joinrel->relids = lcons(outer_rel->relids, lcons(inner_rel->relids, NIL));

    new_outer_tlist = nconc(new_outer_tlist,new_inner_tlist);
    joinrel->targetlist = new_outer_tlist;
    
    if (joininfo) {
	joinrel->clauseinfo = joininfo->jinfoclauseinfo;
	if (BushyPlanFlag) joininfo->inactive = true;
    }
    
    joinrel_joininfo_list = 
	new_joininfo_list(append(outer_rel->joininfo, inner_rel->joininfo),
			  intAppend(outer_rel->relids, inner_rel->relids));
    
    joinrel->joininfo = joinrel_joininfo_list;

    geqo_joinrel_size(joinrel, outer_rel, inner_rel);

    return(joinrel);
}

/*    
 * new-join-tlist--
 *    Builds a join relations's target list by keeping those elements that 
 *    will be in the final target list and any other elements that are still 
 *    needed for future joins.  For a target list entry to still be needed 
 *    for future joins, its 'joinlist' field must not be empty after removal 
 *    of all relids in 'other-relids'.
 *    
 * 'tlist' is the target list of one of the join relations
 * 'other-relids' is a list of relids contained within the other
 *    		join relation
 * 'first-resdomno' is the resdom number to use for the first created
 *    		target list entry
 *    
 * Returns the new target list.
 */
static List *
new_join_tlist(List *tlist,
	       List *other_relids,
	       int first_resdomno)
{
    int resdomno = first_resdomno - 1;
    TargetEntry *xtl = NULL;
    List *temp_node = NIL;
    List *t_list = NIL;
    List *i = NIL;
    List *join_list = NIL;
    bool in_final_tlist =false;
    
    
    foreach(i,tlist) {
	xtl= lfirst(i);
	in_final_tlist = (join_list==NIL);
	if( in_final_tlist)  {
	    resdomno += 1;
	    temp_node = 
		lcons(create_tl_element(get_expr(xtl),
				       resdomno),
		     NIL);
	    t_list = nconc(t_list,temp_node);
	} 
    }

    return(t_list);
}

/*    
 * new-joininfo-list--
 *    Builds a join relation's joininfo list by checking for join clauses
 *    which still need to used in future joins involving this relation.  A
 *    join clause is still needed if there are still relations in the clause
 *    not contained in the list of relations comprising this join relation.
 *    New joininfo nodes are only created and added to
 *    'current-joininfo-list' if a node for a particular join hasn't already
 *    been created.
 *
 * 'current-joininfo-list' contains a list of those joininfo nodes that 
 * 	have already been built 
 * 'joininfo-list' is the list of join clauses involving this relation
 * 'join-relids' is a list of relids corresponding to the relations 
 * 	currently being joined
 *    
 * Returns a list of joininfo nodes, new and old.
 */
static List *
new_joininfo_list(List *joininfo_list, List *join_relids)
{
    List *current_joininfo_list = NIL;
    List *new_otherrels = NIL;
    JInfo *other_joininfo = (JInfo*)NULL;
    List *xjoininfo = NIL;
    
    foreach (xjoininfo, joininfo_list) {
	List *or;
	JInfo *joininfo = (JInfo*)lfirst(xjoininfo);

	new_otherrels = joininfo->otherrels;
	foreach (or, new_otherrels)
	{
	    if ( intMember (lfirsti(or), join_relids) )
	    	new_otherrels = lremove ((void*)lfirst(or), new_otherrels);
	}
	joininfo->otherrels = new_otherrels;
	if ( new_otherrels != NIL )
	{
	    other_joininfo = joininfo_member(new_otherrels,
					     current_joininfo_list);
	    if(other_joininfo) {
		other_joininfo->jinfoclauseinfo =
		    (List*)LispUnion(joininfo->jinfoclauseinfo,
				     other_joininfo->jinfoclauseinfo);
	    }else {
		other_joininfo = makeNode(JInfo);

		other_joininfo->otherrels =
		    joininfo->otherrels;
		other_joininfo->jinfoclauseinfo =
		    joininfo->jinfoclauseinfo;
		other_joininfo->mergesortable =
		    joininfo->mergesortable;
		other_joininfo->hashjoinable =
		    joininfo->hashjoinable;
		other_joininfo->inactive = false;
					   
		current_joininfo_list = lcons(other_joininfo,
					     current_joininfo_list);
	    }
	}
    }

    return(current_joininfo_list);
}

#ifdef	NOTUSED
/*
 * add-new-joininfos--
 *    For each new join relation, create new joininfos that
 *    use the join relation as inner relation, and add
 *    the new joininfos to those rel nodes that still
 *    have joins with the join relation.
 *
 * 'joinrels' is a list of join relations.
 *
 * Modifies the joininfo field of appropriate rel nodes.
 */
static void
geqo_add_new_joininfos(Query *root, List *joinrels, List *outerrels)
{
    List *xjoinrel = NIL;
    List *xrelid = NIL;
    List *xrel = NIL;
    List *xjoininfo = NIL;
    
    Rel *rel;
    List *relids;

    List *super_rels;
    List *xsuper_rel = NIL;
    JInfo *new_joininfo;
						
    foreach(xjoinrel, joinrels) {
	Rel *joinrel = (Rel *)lfirst(xjoinrel);
        foreach(xrelid, joinrel->relids) {
	/* length(joinrel->relids) should always be greater that 1, because of *JOIN* */
	    /* ! BUG BUG !
	    Relid relid = (Relid)lfirst(xrelid);
	    Rel *rel = get_join_rel(root, relid);
	    */

	    /*
            if ( (root->join_relation_list_) != NIL ) {
	    	rel = get_join_rel(root, xrelid);
	    }
	    else {
	    	rel = get_base_rel(root, lfirsti(xrelid));
	    }
	    */

       	/* NOTE: STILL BUGGY FOR CLAUSE-JOINS: */
	/*
        relids = lconsi(lfirsti(xrelid), NIL);
        rel = rel_member(relids, outerrels);
	*/

	    relids = lconsi(lfirsti(xrelid), NIL);
	    rel = rel_member(relids, root->base_relation_list_);

	    add_superrels(rel,joinrel);
	}
    }
    foreach(xjoinrel, joinrels) {
	Rel *joinrel = (Rel *)lfirst(xjoinrel);

	foreach(xjoininfo, joinrel->joininfo) {
	    JInfo *joininfo = (JInfo*)lfirst(xjoininfo);
	    List *other_rels = joininfo->otherrels;
	    List *clause_info = joininfo->jinfoclauseinfo;
	    bool mergesortable = joininfo->mergesortable;
	    bool hashjoinable = joininfo->hashjoinable;

	    foreach(xrelid, other_rels) {
	        /* ! BUG BUG !
		Relid relid = (Relid)lfirst(xrelid);
		Rel *rel = get_join_rel(root, relid);
		*/

		/*
                if ( (root->join_relation_list_) != NIL ) {
	    		rel = get_join_rel(root, xrelid);
	        }
	        else {
	    		rel = get_base_rel(root, lfirsti(xrelid));
	    	}
		*/

		/* NOTE: STILL BUGGY FOR CLAUSE-JOINS: */
		/*
	        relids = lconsi(lfirsti(xrelid), NIL);
	        rel = rel_member(relids, outerrels);
		*/

	        relids = lconsi(lfirsti(xrelid), NIL);
	        rel = rel_member(relids, root->base_relation_list_);

		super_rels = rel->superrels;
		new_joininfo = makeNode(JInfo);
						
		new_joininfo->otherrels = joinrel->relids;
		new_joininfo->jinfoclauseinfo = clause_info;
		new_joininfo->mergesortable = mergesortable;
		new_joininfo->hashjoinable = hashjoinable;
		new_joininfo->inactive = false;
		rel->joininfo = 
		    lappend(rel->joininfo, new_joininfo);

		foreach(xsuper_rel, super_rels) {
		    Rel *super_rel = (Rel *)lfirst(xsuper_rel);

		    if( nonoverlap_rels(super_rel,joinrel) ) {
			List *new_relids = super_rel->relids;
			JInfo *other_joininfo = 
			    joininfo_member(new_relids,
					    joinrel->joininfo);

			if (other_joininfo) {
			    other_joininfo->jinfoclauseinfo =
				(List*)LispUnion(clause_info,
						 other_joininfo->jinfoclauseinfo);
			} else {
			    JInfo *new_joininfo = makeNode(JInfo);

			    new_joininfo->otherrels = new_relids;
			    new_joininfo->jinfoclauseinfo = clause_info; 
			    new_joininfo->mergesortable = mergesortable;
			    new_joininfo->hashjoinable = hashjoinable;
			    new_joininfo->inactive = false;
			    joinrel->joininfo =
				lappend(joinrel->joininfo,
					 new_joininfo);
			}
		    }
		}
	    }
	}
    }
    foreach(xrel, outerrels)  {
	rel = (Rel *)lfirst(xrel);
	rel->superrels = NIL;
    }
}

/*
 * final-join-rels--
 *     Find the join relation that includes all the original
 *     relations, i.e. the final join result.
 *
 * 'join-rel-list' is a list of join relations.
 *
 * Returns the list of final join relations.
 */
static List *
geqo_final_join_rels(List *join_rel_list)
{
    List *xrel = NIL;
    List *temp = NIL;
    List *t_list = NIL;
    
    /*
     * find the relations that has no further joins,
     * i.e., its joininfos all have otherrels nil.
     */
    foreach(xrel,join_rel_list)  {
	Rel *rel = (Rel *)lfirst(xrel);
	List *xjoininfo = NIL;
	bool final = true;

	foreach (xjoininfo, rel->joininfo) {
	    JInfo *joininfo = (JInfo*)lfirst(xjoininfo);

	    if (joininfo->otherrels != NIL)  {
		final = false;
		break;
	    }
	}
	if (final)  {
	    temp = lcons(rel, NIL);
	    t_list = nconc(t_list, temp);
	}
    }

    return(t_list);
}

/*
 * add_superrels--
 *    add rel to the temporary property list superrels.
 *
 * 'rel' a rel node
 * 'super-rel' rel node of a join relation that includes rel
 *
 * Modifies the superrels field of rel
 */
static void
add_superrels(Rel *rel, Rel *super_rel)
{
    rel->superrels = lappend(rel->superrels, super_rel);
}

/*
 * nonoverlap-rels--
 *    test if two join relations overlap, i.e., includes the same
 *    relation.
 *
 * 'rel1' and 'rel2' are two join relations
 *
 * Returns non-nil if rel1 and rel2 do not overlap.
 */
static bool
nonoverlap_rels(Rel *rel1, Rel *rel2)
{
    return(nonoverlap_sets(rel1->relids, rel2->relids));
}

static bool
nonoverlap_sets(List *s1, List *s2)
{
    List *x = NIL;
    
    foreach(x,s1)  {
	int e = lfirsti(x);
	if(intMember(e,s2))
	    return(false);
    }
    return(true);
}
#endif	/* NOTUSED */

/*
 * geqo_joinrel_size--
 *    compute estimate for join relation tuples, even for
 *    long join queries; so get logarithm of size when MAXINT overflow;
 */
static void
geqo_joinrel_size(Rel *joinrel, Rel *outer_rel, Rel *inner_rel)
{
	Cost temp;
	int ntuples;
	
	temp = (Cost) inner_rel->tuples * (Cost) outer_rel->tuples; /* cartesian product */

	if (joinrel->clauseinfo) {
		temp = temp * product_selec(joinrel->clauseinfo);
		}
    
   if (temp >= (MAXINT -1)) {
		ntuples = ceil( geqo_log((double)temp, (double) GEQO_LOG_BASE) ); 
		}
	else {
		ntuples = ceil((double)temp);
    	}

	if (ntuples < 1) ntuples = 1; /* make the best case 1 instead of 0 */

	joinrel->tuples = ntuples;
}

double
geqo_log(double x, double b)
{
    return(log(x)/log(b));
}

static Rel *
geqo_nth(int stop, List *rels)
{
	List *r;
	int i=1;

	foreach(r, rels) {
		if (i == stop) return lfirst(r);
		i++;
		}
	elog(WARN,"geqo_nth: Internal error - ran off end of list");
	return NULL;	/* to keep compiler happy */
}
