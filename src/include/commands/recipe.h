/*-------------------------------------------------------------------------
 *
 * recipe.h--
 *	  recipe handling routines
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: recipe.h,v 1.6 1998/09/01 04:35:33 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef RECIPE_H
#define RECIPE_H

#include "nodes/parsenodes.h"

extern void beginRecipe(RecipeStmt *stmt);

#endif	 /* RECIPE_H */
