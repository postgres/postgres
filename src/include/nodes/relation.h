/*-------------------------------------------------------------------------
 *
 * relation.h--
 *    Definitions for internal planner nodes.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: relation.h,v 1.1 1996/08/28 01:57:49 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef RELATION_H
#define RELATION_H

#include "c.h"
#include "nodes/pg_list.h"
#include "nodes/primnodes.h"
#include "nodes/parsenodes.h"
#include "nodes/nodes.h"

/*
 * Relid
 *	List of relation identifiers (indexes into the rangetable).
 */

typedef	List	*Relid;

/*
 * Rel
 *	Per-base-relation information
 *
 *	Parts of this data structure are specific to various scan and join
 *	mechanisms.  It didn't seem worth creating new node types for them.
 *
 *	relids - List of relation indentifiers
 *	indexed - true if the relation has secondary indices
 *	pages - number of pages in the relation
 *	tuples - number of tuples in the relation
 *	size - number of tuples in the relation after restrictions clauses
 *	       have been applied
 *	width - number of bytes per tuple in the relation after the
 *		appropriate projections have been done
 *	targetlist - List of TargetList nodes
 *	pathlist - List of Path nodes, one for each possible method of
 *		   generating the relation
 *	unorderedpath - a Path node generating this relation whose resulting
 *			tuples are unordered (this isn't necessarily a
 *			sequential scan path, e.g., scanning with a hash index
 *			leaves the tuples unordered)
 *	cheapestpath -  least expensive Path (regardless of final order)
 *      pruneable - flag to let the planner know whether it can prune the plan
 *                  space of this Rel or not.  -- JMH, 11/11/92
 *
 *   * If the relation is a (secondary) index it will have the following
 *	three fields:
 *
 *	classlist - List of PG_AMOPCLASS OIDs for the index
 *	indexkeys - List of base-relation attribute numbers that are index keys
 *	ordering - List of PG_OPERATOR OIDs which order the indexscan result
 *      relam     - the OID of the pg_am of the index
 *
 *   * The presence of the remaining fields depends on the restrictions
 *	and joins which the relation participates in:
 *
 *	clauseinfo - List of ClauseInfo nodes, containing info about each
 *		     qualification clause in which this relation participates
 *	joininfo  - List of JoinInfo nodes, containing info about each join
 *		    clause in which this relation participates
 *	innerjoin - List of Path nodes that represent indices that may be used
 *		    as inner paths of nestloop joins
 *
 * NB. the last element of the arrays classlist, indexkeys and ordering
 *     is always 0.				2/95 - ay
 */

typedef	struct Rel {
    NodeTag	type;

    /* all relations: */
    Relid	relids;

    /* catalog statistics information */
    bool	indexed;
    int		pages;
    int		tuples;
    int		size;
    int		width;

    /* materialization information */
    List	*targetlist;
    List	*pathlist;
    struct Path	*unorderedpath;
    struct Path	*cheapestpath;
    bool    	pruneable;

    /* used solely by indices: */
    Oid		*classlist;		/* classes of AM operators */
    int		*indexkeys;		/* keys over which we're indexing */
    Oid         relam;  /* OID of the access method (in pg_am) */
				   
    Oid		indproc;
    List	*indpred;

    /* used by various scans and joins: */
    Oid		*ordering;		/* OID of operators in sort order */
    List	*clauseinfo;		/* restriction clauses */
    List	*joininfo;		/* join clauses */
    List	*innerjoin;
    List	*superrels;
} Rel;

extern Var *get_expr(TargetEntry *foo);

typedef struct MergeOrder {
    NodeTag	type;
    Oid 	join_operator;
    Oid 	left_operator;
    Oid 	right_operator;
    Oid 	left_type;
    Oid 	right_type;
} MergeOrder;

typedef enum OrderType {
    MERGE_ORDER, SORTOP_ORDER
} OrderType;

typedef struct PathOrder {
    OrderType	ordtype;
    union {
	Oid		*sortop;
	MergeOrder	*merge;
    } ord;
} PathOrder;

typedef struct Path {
    NodeTag	type;

    Rel		*parent;
    Cost	path_cost;

    NodeTag	pathtype;

    PathOrder	p_ordering;

    List	*keys;
    Cost	outerjoincost;
    Relid	joinid;
    List        *locclauseinfo;
} Path;

typedef struct IndexPath {
    Path	path;
    List	*indexid;
    List	*indexqual;
} IndexPath;

typedef struct JoinPath {
    Path	path;
    List	*pathclauseinfo;
    Path	*outerjoinpath;
    Path	*innerjoinpath;
} JoinPath;

typedef struct MergePath {
    JoinPath	jpath;
    List	*path_mergeclauses;
    List	*outersortkeys;
    List	*innersortkeys;
} MergePath;

typedef struct HashPath {
    JoinPath	jpath;
    List	*path_hashclauses;
    List	*outerhashkeys;
    List	*innerhashkeys;
} HashPath;

/******
 * Keys
 ******/

typedef struct OrderKey {
    NodeTag	type;
    int 	attribute_number;
    Index	array_index;
} OrderKey;

typedef struct JoinKey {
    NodeTag	type;
    Var 	*outer;
    Var  	*inner;
} JoinKey;

/*******
 * clause info
 *******/

typedef struct CInfo {
    NodeTag	type;
    Expr	*clause;	/* should be an OP clause */
    Cost	selectivity;
    bool	notclause;
    List	*indexids;

    /* mergesort only */
    MergeOrder	*mergesortorder;

    /* hashjoin only */
    Oid		hashjoinoperator;
    Relid	cinfojoinid;
} CInfo;

typedef struct JoinMethod {
    NodeTag	type;
    List        *jmkeys;
    List        *clauses;
} JoinMethod;

typedef struct HInfo {
    JoinMethod	jmethod;
    Oid        	hashop;
} HInfo;

typedef struct MInfo {
    JoinMethod	jmethod;
    MergeOrder	*m_ordering;
} MInfo;

typedef struct JInfo {
    NodeTag	type;
    List	*otherrels;
    List	*jinfoclauseinfo;
    bool	mergesortable;
    bool	hashjoinable;
    bool	inactive;
} JInfo;

typedef struct Iter {
    NodeTag	type;
    Node	*iterexpr;
    Oid		itertype;	/* type of the iter expr (use for type
				   checking) */
} Iter;

/*
** Stream:
**   A stream represents a root-to-leaf path in a plan tree (i.e. a tree of
** JoinPaths and Paths).  The stream includes pointers to all Path nodes,
** as well as to any clauses that reside above Path nodes.  This structure
** is used to make Path nodes and clauses look similar, so that Predicate
** Migration can run.
**
**     pathptr -- pointer to the current path node
**       cinfo -- if NULL, this stream node referes to the path node.
**                Otherwise this is a pointer to the current clause.
**  clausetype -- whether cinfo is in locclauseinfo or pathclauseinfo in the 
**                path node
**    upstream -- linked list pointer upwards
**  downstream -- ditto, downwards
**     groupup -- whether or not this node is in a group with the node upstream
**   groupcost -- total cost of the group that node is in
**    groupsel -- total selectivity of the group that node is in
*/
typedef struct Stream *StreamPtr;

typedef struct Stream {
    NodeTag	type;
    Path	*pathptr;
    CInfo 	*cinfo;
    int		*clausetype;
    struct Stream *upstream;
    struct Stream *downstream;
    bool 	groupup;
    Cost 	groupcost;
    Cost	 groupsel;
} Stream;

#endif /* RELATION_H */
