/*-------------------------------------------------------------------------
 *
 * nodes.c
 *	  support code for nodes (now that we get rid of the home-brew
 *	  inheritance system, our support code for nodes get much simpler)
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/nodes/nodes.c,v 1.19 2002/12/16 16:22:46 tgl Exp $
 *
 * HISTORY
 *	  Andrew Yu			Oct 20, 1994	file creation
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/nodes.h"

/*
 * Support for newNode() macro
 */

Node *newNodeMacroHolder;
