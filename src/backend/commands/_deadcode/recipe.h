/*-------------------------------------------------------------------------
 *
 * recipe.h
 *	  recipe handling routines
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: recipe.h,v 1.6 2001/11/05 17:46:25 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef RECIPE_H
#define RECIPE_H

#include "nodes/parsenodes.h"

extern void beginRecipe(RecipeStmt *stmt);

#endif   /* RECIPE_H */
