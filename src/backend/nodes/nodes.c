/*-------------------------------------------------------------------------
 *
 * nodes.c--
 *	  support code for nodes (now that we get rid of the home-brew
 *	  inheritance system, our support code for nodes get much simpler)
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/nodes/nodes.c,v 1.2 1997/09/07 04:42:52 momjian Exp $
 *
 * HISTORY
 *	  Andrew Yu			Oct 20, 1994	file creation
 *
 *-------------------------------------------------------------------------
 */
#include <string.h>
#include "postgres.h"
#include "utils/palloc.h"
#include "utils/elog.h"
#include "nodes/nodes.h"		/* where func declarations of this file
								 * goes */

/*
 * newNode -
 *	  create a new node of the specified size and tag the node with the
 *	  specified tag.
 *
 * !WARNING!: Avoid using newNode directly. You should be using the
 *	  macro makeNode. eg. to create a Resdom node, use makeNode(Resdom)
 *
 */
Node		   *
newNode(Size size, NodeTag tag)
{
	Node		   *newNode;

	Assert(size >= 4);			/* need the tag, at least */

	newNode = (Node *) palloc(size);
	memset((char *) newNode, 0, size);
	newNode->type = tag;
	return (newNode);
}
