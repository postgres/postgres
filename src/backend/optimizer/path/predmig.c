/*-------------------------------------------------------------------------
 *
 * predmig.c--
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/path/Attic/predmig.c,v 1.17 1999/02/12 17:24:50 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
** DESCRIPTION
** Main Routines to handle Predicate Migration (i.e. correct optimization
** of queries with expensive functions.)
**
**	  The reasoning behind some of these algorithms is rather detailed.
** Have a look at Sequoia Tech Report 92/13 for more info.	Also
** see Monma and Sidney's paper "Sequencing with Series-Parallel
** Precedence Constraints", in "Mathematics of Operations Research",
** volume 4 (1979),  pp. 215-224.
**
**	  The main thing that this code does that wasn't handled in xfunc.c is
** it considers the possibility that two joins in a stream may not
** be ordered by ascending rank -- in such a scenario, it may be optimal
** to pullup more restrictions than we did via xfunc_try_pullup.
**
**	  This code in some sense generalizes xfunc_try_pullup; if you
** run postgres -x noprune, you'll turn off xfunc_try_pullup, and this
** code will do everything that xfunc_try_pullup would have, and maybe
** more.  However, this results in no pruning, which may slow down the
** optimizer and/or cause the system to run out of memory.
**										   -- JMH, 11/13/92
*/

#include "nodes/pg_list.h"
#include "nodes/nodes.h"
#include "nodes/primnodes.h"
#include "nodes/relation.h"
#include "utils/palloc.h"
#include "utils/elog.h"
#include "optimizer/xfunc.h"
#include "optimizer/pathnode.h"
#include "optimizer/internal.h"
#include "optimizer/cost.h"
#include "optimizer/keys.h"
#include "optimizer/tlist.h"

#define is_clause(node) (get_cinfo(node))		/* a stream node
												 * represents a clause
												 * (not a join) iff it has
												 * a non-NULL cinfo field */

static void xfunc_predmig(JoinPath pathnode, Stream streamroot,
			  Stream laststream, bool *progressp);
static bool xfunc_series_llel(Stream stream);
static bool xfunc_llel_chains(Stream root, Stream bottom);
static Stream xfunc_complete_stream(Stream stream);
static bool xfunc_prdmig_pullup(Stream origstream, Stream pullme,
					JoinPath joinpath);
static void xfunc_form_groups(Stream root, Stream bottom);
static void xfunc_free_stream(Stream root);
static Stream xfunc_add_clauses(Stream current);
static void xfunc_setup_group(Stream node, Stream bottom);
static Stream xfunc_streaminsert(RestrictInfo restrictinfo, Stream current,
				   int clausetype);
static int	xfunc_num_relids(Stream node);
static StreamPtr xfunc_get_downjoin(Stream node);
static StreamPtr xfunc_get_upjoin(Stream node);
static Stream xfunc_stream_qsort(Stream root, Stream bottom);
static int	xfunc_stream_compare(void *arg1, void *arg2);
static bool xfunc_check_stream(Stream node);
static bool xfunc_in_stream(Stream node, Stream stream);

/* -----------------   MAIN FUNCTIONS ------------------------ */
/*
** xfunc_do_predmig
**	  wrapper for Predicate Migration.	It calls xfunc_predmig until no
** more progress is made.
**	  return value says if any changes were ever made.
*/
bool
xfunc_do_predmig(Path root)
{
	bool		progress,
				changed = false;

	if (is_join(root))
		do
		{
			progress = false;
			Assert(IsA(root, JoinPath));
			xfunc_predmig((JoinPath) root, (Stream) NULL, (Stream) NULL,
						  &progress);
			if (changed && progress)
				elog(DEBUG, "Needed to do a second round of predmig!\n");
			if (progress)
				changed = true;
		} while (progress);
	return changed;
}


/*
 ** xfunc_predmig
 **  The main routine for Predicate Migration.	It traverses a join tree,
 ** and for each root-to-leaf path in the plan tree it constructs a
 ** "Stream", which it passes to xfunc_series_llel for optimization.
 ** Destructively modifies the join tree (via predicate pullup).
 */
static void
xfunc_predmig(JoinPath pathnode,/* root of the join tree */
			  Stream streamroot,
			  Stream laststream,/* for recursive calls -- these are the
								 * root of the stream under construction,
								 * and the lowest node created so far */
			  bool *progressp)
{
	Stream		newstream;

	/*
	 * * traverse the join tree dfs-style, constructing a stream as you
	 * go. * When you hit a scan node, pass the stream off to
	 * xfunc_series_llel.
	 */

	/* sanity check */
	if ((!streamroot && laststream) ||
		(streamroot && !laststream))
		elog(ERROR, "called xfunc_predmig with bad inputs");
	if (streamroot)
		Assert(xfunc_check_stream(streamroot));

	/* add path node to stream */
	newstream = RMakeStream();
	if (!streamroot)
		streamroot = newstream;
	set_upstream(newstream, (StreamPtr) laststream);
	if (laststream)
		set_downstream(laststream, (StreamPtr) newstream);
	set_downstream(newstream, (StreamPtr) NULL);
	set_pathptr(newstream, (pathPtr) pathnode);
	set_cinfo(newstream, (RestrictInfo) NULL);
	set_clausetype(newstream, XFUNC_UNKNOWN);

	/* base case: we're at a leaf, call xfunc_series_llel */
	if (!is_join(pathnode))
	{
		/* form a fleshed-out copy of the stream */
		Stream		fullstream = xfunc_complete_stream(streamroot);

		/* sort it via series-llel */
		if (xfunc_series_llel(fullstream))
			*progressp = true;

		/* free up the copy */
		xfunc_free_stream(fullstream);
	}
	else
	{
		/* visit left child */
		xfunc_predmig((JoinPath) get_outerjoinpath(pathnode),
					  streamroot, newstream, progressp);

		/* visit right child */
		xfunc_predmig((JoinPath) get_innerjoinpath(pathnode),
					  streamroot, newstream, progressp);
	}

	/* remove this node */
	if (get_upstream(newstream))
		set_downstream((Stream) get_upstream(newstream), (StreamPtr) NULL);
	pfree(newstream);
}

/*
 ** xfunc_series_llel
 **    A flavor of Monma and Sidney's Series-Parallel algorithm.
 ** Traverse stream downwards.	When you find a node with restrictions on it,
 ** call xfunc_llel_chains on the substream from root to that node.
 */
static bool
xfunc_series_llel(Stream stream)
{
	Stream		temp,
				next;
	bool		progress = false;

	for (temp = stream; temp != (Stream) NULL; temp = next)
	{
		next = (Stream) xfunc_get_downjoin(temp);

		/*
		 * * if there are restrictions/secondary join clauses above this *
		 * node, call xfunc_llel_chains
		 */
		if (get_upstream(temp) && is_clause((Stream) get_upstream(temp)))
			if (xfunc_llel_chains(stream, temp))
				progress = true;
	}
	return progress;
}

/*
 ** xfunc_llel_chains
 **    A flavor of Monma and Sidney's Parallel Chains algorithm.
 ** Given a stream which has been well-ordered except for its lowermost
 ** restrictions/2-ary joins, pull up the restrictions/2-arys as appropriate.
 ** What that means here is to form groups in the chain above the lowest
 ** join node above bottom inclusive, and then take all the restrictions
 ** following bottom, and try to pull them up as far as possible.
 */
static bool
xfunc_llel_chains(Stream root, Stream bottom)
{
	bool		progress = false;
	Stream		origstream;
	Stream		tmpstream,
				pathstream;
	Stream		rootcopy = root;

	Assert(xfunc_check_stream(root));

	/* xfunc_prdmig_pullup will need an unmodified copy of the stream */
	origstream = (Stream) copyObject((Node) root);

	/* form groups among ill-ordered nodes */
	xfunc_form_groups(root, bottom);

	/* sort chain by rank */
	Assert(xfunc_in_stream(bottom, root));
	rootcopy = xfunc_stream_qsort(root, bottom);

	/*
	 * * traverse sorted stream -- if any restriction has moved above a
	 * join, * we must pull it up in the plan.	That is, make plan tree *
	 * reflect order of sorted stream.
	 */
	for (tmpstream = rootcopy,
		 pathstream = (Stream) xfunc_get_downjoin(rootcopy);
		 tmpstream != (Stream) NULL && pathstream != (Stream) NULL;
		 tmpstream = (Stream) get_downstream(tmpstream))
	{
		if (is_clause(tmpstream)
			&& get_pathptr(pathstream) != get_pathptr(tmpstream))
		{

			/*
			 * * If restriction moved above a Join after sort, we pull it *
			 * up in the join plan. *	 If restriction moved down, we
			 * ignore it. * This is because Joey's Sequoia paper proves
			 * that * restrictions should never move down.	If this * one
			 * were moved down, it would violate "semantic correctness", *
			 * i.e. it would be lower than the attributes it references.
			 */
			Assert(xfunc_num_relids(pathstream) > xfunc_num_relids(tmpstream));
			progress = xfunc_prdmig_pullup(origstream, tmpstream,
									(JoinPath) get_pathptr(pathstream));
		}
		if (get_downstream(tmpstream))
			pathstream = (Stream) xfunc_get_downjoin((Stream) get_downstream(tmpstream));
	}

	/* free up origstream */
	xfunc_free_stream(origstream);
	return progress;
}

/*
 ** xfunc_complete_stream --
 **   Given a stream composed of join nodes only, make a copy containing the
 ** join nodes along with the associated restriction nodes.
 */
static Stream
xfunc_complete_stream(Stream stream)
{
	Stream		tmpstream,
				copystream,
				curstream = (Stream) NULL;

	copystream = (Stream) copyObject((Node) stream);
	Assert(xfunc_check_stream(copystream));

	curstream = copystream;
	Assert(!is_clause(curstream));

	/* curstream = (Stream)xfunc_get_downjoin(curstream); */

	while (curstream != (Stream) NULL)
	{
		xfunc_add_clauses(curstream);
		curstream = (Stream) xfunc_get_downjoin(curstream);
	}

	/* find top of stream and return it */
	for (tmpstream = copystream; get_upstream(tmpstream) != (StreamPtr) NULL;
		 tmpstream = (Stream) get_upstream(tmpstream))
		 /* no body in for loop */ ;

	return tmpstream;
}

/*
 ** xfunc_prdmig_pullup
 **    pullup a clause in a path above joinpath.  Since the JoinPath tree
 ** doesn't have upward pointers, it's difficult to deal with.	Thus we
 ** require the original stream, which maintains pointers to all the path
 ** nodes.	We use the original stream to find out what joins are
 ** above the clause.
 */
static bool
xfunc_prdmig_pullup(Stream origstream, Stream pullme, JoinPath joinpath)
{
	RestrictInfo	restrictinfo = get_cinfo(pullme);
	bool		progress = false;
	Stream		upjoin,
				orignode,
				temp;
	int			whichchild;

	/* find node in origstream that contains clause */
	for (orignode = origstream;
		 orignode != (Stream) NULL
		 && get_cinfo(orignode) != restrictinfo;
		 orignode = (Stream) get_downstream(orignode))
		 /* empty body in for loop */ ;
	if (!orignode)
		elog(ERROR, "Didn't find matching node in original stream");


	/* pull up this node as far as it should go */
	for (upjoin = (Stream) xfunc_get_upjoin(orignode);
		 upjoin != (Stream) NULL
		 && (JoinPath) get_pathptr((Stream) xfunc_get_downjoin(upjoin))
		 != joinpath;
		 upjoin = (Stream) xfunc_get_upjoin(upjoin))
	{
#ifdef DEBUG
		elog(DEBUG, "pulling up in xfunc_predmig_pullup!");
#endif
		/* move clause up in path */
		if (get_pathptr((Stream) get_downstream(upjoin))
		  == (pathPtr) get_outerjoinpath((JoinPath) get_pathptr(upjoin)))
			whichchild = OUTER;
		else
			whichchild = INNER;
		restrictinfo = xfunc_pullup((Path) get_pathptr((Stream) get_downstream(upjoin)),
								  (JoinPath) get_pathptr(upjoin),
								  restrictinfo,
								  whichchild,
								  get_clausetype(orignode));
		set_pathptr(pullme, get_pathptr(upjoin));
		/* pullme has been moved into locrestrictinfo */
		set_clausetype(pullme, XFUNC_LOCPRD);

		/*
		 * * xfunc_pullup makes new path nodes for children of *
		 * get_pathptr(current). We must modify the stream nodes to point *
		 * to these path nodes
		 */
		if (whichchild == OUTER)
		{
			for (temp = (Stream) get_downstream(upjoin); is_clause(temp);
				 temp = (Stream) get_downstream(temp))
				set_pathptr
					(temp, (pathPtr)
					 get_outerjoinpath((JoinPath) get_pathptr(upjoin)));
			set_pathptr
				(temp,
			(pathPtr) get_outerjoinpath((JoinPath) get_pathptr(upjoin)));
		}
		else
		{
			for (temp = (Stream) get_downstream(upjoin); is_clause(temp);
				 temp = (Stream) get_downstream(temp))
				set_pathptr
					(temp, (pathPtr)
					 get_innerjoinpath((JoinPath) get_pathptr(upjoin)));
			set_pathptr
				(temp, (pathPtr)
				 get_innerjoinpath((JoinPath) get_pathptr(upjoin)));
		}
		progress = true;
	}
	if (!progress)
		elog(DEBUG, "didn't succeed in pulling up in xfunc_prdmig_pullup");
	return progress;
}

/*
 ** xfunc_form_groups --
 **    A group is a pair of stream nodes a,b such that a is constrained to
 ** precede b (for instance if a and b are both joins), but rank(a) > rank(b).
 ** In such a situation, Monma and Sidney prove that no clauses should end
 ** up between a and b, and therefore we may treat them as a group, with
 ** selectivity equal to the product of their selectivities, and cost
 ** equal to the cost of the first plus the selectivity of the first times the
 ** cost of the second.  We define each node to be in a group by itself,
 ** and then repeatedly find adjacent groups which are ordered by descending
 ** rank, and make larger groups.  You know that two adjacent nodes are in a
 ** group together if the lower has groupup set to true.  They will both have
 ** the same groupcost and groupsel (since they're in the same group!)
 */
static void
xfunc_form_groups(Query *queryInfo, Stream root, Stream bottom)
{
	Stream		temp,
				parent;
	int			lowest = xfunc_num_relids((Stream) xfunc_get_upjoin(bottom));
	bool		progress;
	LispValue	primjoin;
	int			whichchild;

	if (!lowest)
		return;					/* no joins in stream, so no groups */

	/* initialize groups to be single nodes */
	for (temp = root;
		 temp != (Stream) NULL && temp != bottom;
		 temp = (Stream) get_downstream(temp))
	{
		/* if a Join node */
		if (!is_clause(temp))
		{
			if (get_pathptr((Stream) get_downstream(temp))
			== (pathPtr) get_outerjoinpath((JoinPath) get_pathptr(temp)))
				whichchild = OUTER;
			else
				whichchild = INNER;
			set_groupcost(temp,
						  xfunc_join_expense((JoinPath) get_pathptr(temp),
											 whichchild));
			if (primjoin = xfunc_primary_join((JoinPath) get_pathptr(temp)))
			{
				set_groupsel(temp,
							 compute_clause_selec(queryInfo,
												  primjoin, NIL));
			}
			else
				set_groupsel(temp, 1.0);
		}
		else
/* a restriction, or 2-ary join pred */
		{
			set_groupcost(temp,
						  xfunc_expense(queryInfo,
										get_clause(get_cinfo(temp))));
			set_groupsel(temp,
						 compute_clause_selec(queryInfo,
											  get_clause(get_cinfo(temp)),
											  NIL));
		}
		set_groupup(temp, false);
	}

	/* make passes upwards, forming groups */
	do
	{
		progress = false;
		for (temp = (Stream) get_upstream(bottom);
			 temp != (Stream) NULL;
			 temp = (Stream) get_upstream(temp))
		{
			/* check for grouping with node upstream */
			if (!get_groupup(temp) &&	/* not already grouped */
				(parent = (Stream) get_upstream(temp)) != (Stream) NULL &&
			/* temp is a join or temp is the top of a group */
				(is_join((Path) get_pathptr(temp)) ||
				 get_downstream(temp) &&
				 get_groupup((Stream) get_downstream(temp))) &&
				get_grouprank(parent) < get_grouprank(temp))
			{
				progress = true;/* we formed a new group */
				set_groupup(temp, true);
				set_groupcost(temp,
							  get_groupcost(temp) +
							  get_groupsel(temp) * get_groupcost(parent));
				set_groupsel(temp, get_groupsel(temp) * get_groupsel(parent));

				/* fix costs and sels of all members of group */
				xfunc_setup_group(temp, bottom);
			}
		}
	} while (progress);
}


/* -------------------			   UTILITY FUNCTIONS	 ------------------------- */

/*
 ** xfunc_free_stream --
 **   walk down a stream and pfree it
 */
static void
xfunc_free_stream(Stream root)
{
	Stream		cur,
				next;

	Assert(xfunc_check_stream(root));

	if (root != (Stream) NULL)
		for (cur = root; cur != (Stream) NULL; cur = next)
		{
			next = (Stream) get_downstream(cur);
			pfree(cur);
		}
}

/*
 ** xfunc_add<_clauses
 **    find any clauses above current, and insert them into stream as
 ** appropriate.  Return uppermost clause inserted, or current if none.
 */
static Stream
xfunc_add_clauses(Stream current)
{
	Stream		topnode = current;
	LispValue	temp;
	LispValue	primjoin;

	/* first add in the local clauses */
	foreach(temp, get_loc_restrictinfo((Path) get_pathptr(current)))
	{
		topnode = xfunc_streaminsert((RestrictInfo) lfirst(temp), topnode,
							   XFUNC_LOCPRD);
	}

	/* and add in the join clauses */
	if (IsA(get_pathptr(current), JoinPath))
	{
		primjoin = xfunc_primary_join((JoinPath) get_pathptr(current));
		foreach(temp, get_pathrestrictinfo((JoinPath) get_pathptr(current)))
		{
			if (!equal(get_clause((RestrictInfo) lfirst(temp)), primjoin))
				topnode = xfunc_streaminsert((RestrictInfo) lfirst(temp), topnode,
									   XFUNC_JOINPRD);
		}
	}
	return topnode;
}


/*
 ** xfunc_setup_group
 **   find all elements of stream that are grouped with node and are above
 ** bottom, and set their groupcost and groupsel to be the same as node's.
 */
static void
xfunc_setup_group(Stream node, Stream bottom)
{
	Stream		temp;

	if (node != bottom)
		/* traverse downwards */
		for (temp = (Stream) get_downstream(node);
			 temp != (Stream) NULL && temp != bottom;
			 temp = (Stream) get_downstream(temp))
		{
			if (!get_groupup(temp))
				break;
			else
			{
				set_groupcost(temp, get_groupcost(node));
				set_groupsel(temp, get_groupsel(node));
			}
		}

	/* traverse upwards */
	for (temp = (Stream) get_upstream(node); temp != (Stream) NULL;
		 temp = (Stream) get_upstream(temp))
	{
		if (!get_groupup((Stream) get_downstream(temp)))
			break;
		else
		{
			set_groupcost(temp, get_groupcost(node));
			set_groupsel(temp, get_groupsel(node));
		}
	}
}


/*
 ** xfunc_streaminsert
 **    Make a new Stream node to hold clause, and insert it above current.
 ** Return new node.
 */
static Stream
xfunc_streaminsert(RestrictInfo restrictinfo,
				   Stream current,
				   int clausetype)		/* XFUNC_LOCPRD or XFUNC_JOINPRD */
{
	Stream		newstream = RMakeStream();

	set_upstream(newstream, get_upstream(current));
	if (get_upstream(current))
		set_downstream((Stream) (get_upstream(current)), (StreamPtr) newstream);
	set_upstream(current, (StreamPtr) newstream);
	set_downstream(newstream, (StreamPtr) current);
	set_pathptr(newstream, get_pathptr(current));
	set_cinfo(newstream, restrictinfo);
	set_clausetype(newstream, clausetype);
	return newstream;
}

/*
 ** Given a Stream node, find the number of relids referenced in the pathnode
 ** associated with the stream node.  The number of relids gives a unique
 ** ordering on the joins in a stream, which we use to compare the height of
 ** join nodes.
 */
static int
xfunc_num_relids(Stream node)
{
	if (!node || !IsA(get_pathptr(node), JoinPath))
		return 0;
	else
		return (length
				(get_relids(get_parent((JoinPath) get_pathptr(node)))));
}

/*
 ** xfunc_get_downjoin --
 **    Given a stream node, find the next lowest node which points to a
 ** join predicate or a scan node.
 */
static StreamPtr
xfunc_get_downjoin(Stream node)
{
	Stream		temp;

	if (!is_clause(node))		/* if this is a join */
		node = (Stream) get_downstream(node);
	for (temp = node; temp && is_clause(temp);
		 temp = (Stream) get_downstream(temp))
		 /* empty body in for loop */ ;

	return (StreamPtr) temp;
}

/*
 ** xfunc_get_upjoin --
 **   same as above, but upwards.
 */
static StreamPtr
xfunc_get_upjoin(Stream node)
{
	Stream		temp;

	if (!is_clause(node))		/* if this is a join */
		node = (Stream) get_upstream(node);
	for (temp = node; temp && is_clause(temp);
		 temp = (Stream) get_upstream(temp))
		 /* empty body in for loop */ ;

	return (StreamPtr) temp;
}

/*
 ** xfunc_stream_qsort --
 **   Given a stream, sort by group rank the elements in the stream from the
 ** node "bottom" up.  DESTRUCTIVELY MODIFIES STREAM!  Returns new root.
 */
static Stream
xfunc_stream_qsort(Stream root, Stream bottom)
{
	int			i;
	size_t		num;
	Stream	   *nodearray,
				output;
	Stream		tmp;

	/* find size of list */
	for (num = 0, tmp = root; tmp != bottom;
		 tmp = (Stream) get_downstream(tmp))
		num++;
	if (num <= 1)
		return root;

	/* copy elements of the list into an array */
	nodearray = (Stream *) palloc(num * sizeof(Stream));

	for (tmp = root, i = 0; tmp != bottom;
		 tmp = (Stream) get_downstream(tmp), i++)
		nodearray[i] = tmp;

	/* sort the array */
	qsort(nodearray, num, sizeof(LispValue), xfunc_stream_compare);

	/* paste together the array elements */
	output = nodearray[num - 1];
	set_upstream(output, (StreamPtr) NULL);
	for (i = num - 2; i >= 0; i--)
	{
		set_downstream(nodearray[i + 1], (StreamPtr) nodearray[i]);
		set_upstream(nodearray[i], (StreamPtr) nodearray[i + 1]);
	}
	set_downstream(nodearray[0], (StreamPtr) bottom);
	if (bottom)
		set_upstream(bottom, (StreamPtr) nodearray[0]);

	Assert(xfunc_check_stream(output));
	return output;
}

/*
 ** xfunc_stream_compare
 **    comparison function for xfunc_stream_qsort.
 ** Compare nodes by group rank.  If group ranks are equal, ensure that
 ** join nodes appear in same order as in plan tree.
 */
static int
xfunc_stream_compare(void *arg1, void *arg2)
{
	Stream		stream1 = *(Stream *) arg1;
	Stream		stream2 = *(Stream *) arg2;
	Cost		rank1,
				rank2;

	rank1 = get_grouprank(stream1);
	rank2 = get_grouprank(stream2);

	if (rank1 > rank2)
		return 1;
	else if (rank1 < rank2)
		return -1;
	else
	{
		if (is_clause(stream1) && is_clause(stream2))
			return 0;			/* doesn't matter what order if both are
								 * restrictions */
		else if (!is_clause(stream1) && !is_clause(stream2))
		{
			if (xfunc_num_relids(stream1) < xfunc_num_relids(stream2))
				return -1;
			else
				return 1;
		}
		else if (is_clause(stream1) && !is_clause(stream2))
		{
			if (xfunc_num_relids(stream1) == xfunc_num_relids(stream2))
				/* stream1 is a restriction over stream2 */
				return 1;
			else
				return -1;
		}
		else if (!is_clause(stream1) && is_clause(stream2))
		{
			/* stream2 is a restriction over stream1: never push down */
			return -1;
		}
	}
}

/* ------------------  DEBUGGING ROUTINES ---------------------------- */

/*
 ** Make sure all pointers in stream make sense.  Make sure no joins are
 ** out of order.
 */
static bool
xfunc_check_stream(Stream node)
{
	Stream		temp;
	int			numrelids,
				tmp;

	/* set numrelids higher than max */
	if (!is_clause(node))
		numrelids = xfunc_num_relids(node) + 1;
	else if (xfunc_get_downjoin(node))
		numrelids = xfunc_num_relids((Stream) xfunc_get_downjoin(node)) + 1;
	else
		numrelids = 1;

	for (temp = node; get_downstream(temp); temp = (Stream) get_downstream(temp))
	{
		if ((Stream) get_upstream((Stream) get_downstream(temp)) != temp)
		{
			elog(ERROR, "bad pointers in stream");
			return false;
		}
		if (!is_clause(temp))
		{
			if ((tmp = xfunc_num_relids(temp)) >= numrelids)
			{
				elog(ERROR, "Joins got reordered!");
				return false;
			}
			numrelids = tmp;
		}
	}

	return true;
}

/*
 ** xfunc_in_stream
 **   check if node is in stream
 */
static bool
xfunc_in_stream(Stream node, Stream stream)
{
	Stream		temp;

	for (temp = stream; temp; temp = (Stream) get_downstream(temp))
		if (temp == node)
			return 1;
	return 0;
}
