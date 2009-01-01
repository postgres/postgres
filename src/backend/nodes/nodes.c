/*-------------------------------------------------------------------------
 *
 * nodes.c
 *	  support code for nodes (now that we have removed the home-brew
 *	  inheritance system, our support code for nodes is much simpler)
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/nodes/nodes.c,v 1.30 2009/01/01 17:23:43 momjian Exp $
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
 *
 * In a GCC build there is no need for the global variable newNodeMacroHolder.
 * However, we create it anyway, to support the case of a non-GCC-built
 * loadable module being loaded into a GCC-built backend.
 */

Node	   *newNodeMacroHolder;
