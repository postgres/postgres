/*-------------------------------------------------------------------------
 *
 * tgRecipe.h--
 *		Tioga recipe-related definitions and declarations
 *		these functions can be used in both the frontend and the
 *		backend
 *
 *	   to use this header, you must also include
 *			"utils/geo-decls.h"
 *		and "libpq/libpq.h"
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: tgRecipe.h,v 1.7 1998/09/01 04:32:21 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "libpq/libpq.h"
#ifndef TIOGA_FRONTEND
#include "libpq/libpq-be.h"
#include "utils/elog.h"
#include "utils/geo-decls.h"
#else
#include "libpq-fe.h"
typedef struct
{
	double		x,
				y;
} Point;						/* this should match whatever is in

								 *
								 *
								 *
								 *
								 *
								 *
								 * geo-decls.h */

#endif	 /* TIOGA_FRONTEND */

typedef enum
{
	TG_INGRED,
	TG_EYE,
	TG_RECIPE
}			TgElemType;

typedef enum
{
	TG_SQL,
	TG_C,
	TG_RECIPE_GRAPH,
	TG_COMPILED
}			TgSrcLangType;

typedef enum
{
	TG_INGRED_NODE,
	TG_EYE_NODE,
	TG_RECIPE_NODE,
	TG_TEE_NODE					/* tee nodes are not stored in the db we
								 * create them when we read the recipe
								 * back */
}			TgNodeType;

/* -- type definition for setting up in memory Tioga recipe structure -- */
/* -- see 'recipe-schema.sql' for their corresponding database types  -- */

typedef char *TgString;

typedef struct _tgelement *TgElementPtr;
typedef struct _tgnode *TgNodePtr;
typedef struct _tgrecipe *TgRecipePtr;

/* auto-generated header containing Arr_TgString, Arr_TgElementPtr,
   and Arr_TgNodePtr */
#include "tioga/Arr_TgRecipe.h"

/* C structure representation of a Tioga Element */
typedef struct _tgelement
{
	char	   *elemName;		/* name of function this element represent */
	TgElemType	elemType;		/* type of this element */
	Arr_TgString *inPorts;		/* names of inputs */
	Arr_TgString *inTypes;		/* name of input types */
	Arr_TgString *outPorts;		/* type of output */
	Arr_TgString *outTypes;		/* name of output types */
	char	   *doc;			/* description	of this element */
	Arr_TgString *keywords;		/* keywords used to search for this
								 * element */
	char	   *icon;			/* iconic representation */
	char	   *src;			/* source code for this element */
	TgSrcLangType srcLang;		/* source language */
	char	   *owner;			/* owner recipe name */
}			TgElement;


/* C structure representation of a Tioga Node */
typedef struct _tgnode
{
	char	   *nodeName;		/* name of this node */
	TgNodeType	nodeType;		/* type of this node */
	Point		loc;			/* screen location of the node. */
	TgElement  *nodeElem;		/* the underlying element of this node */
	Arr_TgNodePtr *inNodes;		/* variable array of in node pointers a
								 * NULL TgNodePtr indicates a run-time
								 * parameter */
	Arr_TgNodePtr *outNodes;	/* variable array of out node pointers. */
}			TgNode;

/* C structure representation of a Tioga Recipe */
typedef struct _tgrecipe
{
	TgElement	elmValue;		/* "inherits" TgElement attributes. */
	Arr_TgNodePtr *allNodes;	/* array of all nodes for this recipe. */
	Arr_TgNodePtr *rootNodes;	/* array of root nodes for this recipe. --
								 * root nodes are nodes with no parents */
	Arr_TgNodePtr *eyes;		/* array of pointers for the browser nodes
								 * recipe, execution of recipe starts by
								 * traversing the recipe C structure from
								 * the eye nodes pointed by these
								 * pointers. */
	Arr_TgNodePtr *tees;		/* array of pointers of all the tee nodes */
	Arr_TgElementPtr *elements; /* array of all the elements in this
								 * recipe, elements may be shared by
								 * multiple nodes */

}			TgRecipe;

/* functions defined in tgRecipe.c */
extern TgRecipe *retrieveRecipe(char *name);
extern TgElement *findElemInRecipe(TgRecipe * r, char *elemName);
extern TgNode *findNodeInRecipe(TgRecipe * r, char *nodeName);

/* ---- copyXXX functions ---- */
extern void copyTgElementPtr(TgElementPtr *, TgElementPtr *);
extern void copyTgNodePtr(TgNodePtr *, TgNodePtr *);
extern void copyTgRecipePtr(TgRecipePtr *, TgRecipePtr *);
extern void copyTgString(TgString *, TgString *);
