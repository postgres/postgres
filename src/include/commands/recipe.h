/*-------------------------------------------------------------------------
 *
 * recipe.h--
 *	  recipe handling routines
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: recipe.h,v 1.5 1997/11/26 01:12:24 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef RECIPE_H
#define RECIPE_H

#include "nodes/parsenodes.h"

extern void beginRecipe(RecipeStmt *stmt);

#endif							/* RECIPE_H */
