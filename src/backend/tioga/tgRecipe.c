/*-------------------------------------------------------------------------
 *
 * tgRecipe.c--
 *		Tioga recipe-related definitions
 *		these functions can be used in both the frontend and the
 *		backend
 *
 *		this file must be kept current with recipe-schema.sql
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/tioga/Attic/tgRecipe.c,v 1.6 1997/09/08 21:48:01 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include <stdlib.h>
#include "postgres.h"
#include "tioga/tgRecipe.h"

#include "catalog/catalog.h"	/* for newoid() */

static Arr_TgString *TextArray2ArrTgString(char *str);

#define ARRAY_LEFT_DELIM '{'
#define ARRAY_RIGHT_DELIM '}'
#define ARRAY_ELEM_LEFT '"'
#define ARRAY_ELEM_RIGHT '"'
#define ARRAY_ELEM_SEPARATOR ','

/* maximum length of query string */
#define MAX_QBUF_LENGTH 2048

/**** the queries being used ********/
#define Q_RETRIEVE_RECIPE_BYNAME \
	  "select * from Recipes where Recipes.elemName = '%s';"
#define Q_RETRIEVE_ELEMENTS_IN_RECIPE \
	  "select e.* from Element e, Node n where n.belongsTo = '%s' and n.nodeElem = e.elemName;"
#define Q_RETRIEVE_NODES_IN_RECIPE \
	  "select * from Node n where n.belongsTo = '%s'"
#define Q_LOOKUP_EDGES_IN_RECIPE \
	  "select * from Edge e where e.belongsTo = '%s'"

/* static functions only used here */
static void fillTgElement(TgElement * elem, PortalBuffer *pbuf, int tupno);
static void fillTgNode(TgRecipe * r, TgNode * node, PortalBuffer *pbuf, int tupno);
static TgRecipe *fillTgRecipe(PortalBuffer *pbuf, int tupno);
static void lookupEdges(TgRecipe * r, char *name);
static void fillAllNodes(TgRecipe * r, char *name);
static void fillAllElements(TgRecipe * r, char *name);
static TgNode *
connectTee(TgRecipe * r, TgNodePtr fromNode, TgNodePtr toNode,
		   int fromPort, int toPort);

/*
 * TextArray2ArrTgString --  take a string of the form:
 *						{"fooo", "bar", "xxxxx"} (for postgres)
 *						and parse it into a Array of TgString's
 *
 * always returns a valid Arr_TgString.  It could be a newly initialized one with
 * zero elements
 */
Arr_TgString *
TextArray2ArrTgString(char *str)
{
	Arr_TgString *result;

	char	   *beginQuote;
	char	   *endQuote;
	int			nextlen;
	char	   *word;

	result = newArr_TgString();

	if ((str == NULL) || (str[0] == '\0'))
		return result;

	if (*str != ARRAY_LEFT_DELIM)
	{
		elog(NOTICE, "TextArray2ArrTgString: badly formed string, must have %c as \
first character\n", ARRAY_LEFT_DELIM);
		return result;
	}

	str++;						/* skip the first { */
	while (*str != '}')
	{
		if (*str == '\0')
		{
			elog(NOTICE, "TextArray2ArrTgString: text string ended prematurely\n");
			return result;
		}

		if ((beginQuote = index(str, ARRAY_ELEM_LEFT)) == NULL)
		{
			elog(NOTICE, "textArray2ArrTgString:  missing a begin quote\n");
			return result;
		}
		if ((endQuote = index(beginQuote + 1, '"')) == NULL)
		{
			elog(NOTICE, "textArray2ArrTgString:  missing an end quote\n");
			return result;
		}
		nextlen = endQuote - beginQuote;		/* don't subtract one here
												 * because we need the
												 * extra character for \0
												 * anyway */
		word = (char *) malloc(nextlen);
		strNcpy(word, beginQuote + 1, nextlen - 1);
		addArr_TgString(result, (TgString *) & word);
		free(word);
		str = endQuote + 1;
	}
	return result;
}

/* -------------------------------------
findElemInRecipe()
   given an element name, find that element in the TgRecipe structure and return it.

   XXX Currently, this is done by linear search.  Change to using a hash table.
-------------------------------------- */

TgElement  *
findElemInRecipe(TgRecipe * r, char *elemName)
{
	int			i;
	Arr_TgElementPtr *arr = r->elements;
	TgElement  *e;

	for (i = 0; i < arr->num; i++)
	{
		e = (TgElement *) arr->val[i];
		if (strcmp(e->elemName, elemName) == 0)
			return e;
	}
	elog(NOTICE, "Element named %s not found in recipe named %s", elemName, r->elmValue.elemName);
	return NULL;
}

/* -------------------------------------
findNodeInRecipe()
   given an node name, find that node in the TgRecipe structure and return it.

   XXX Currently, this is done by linear search.  Change to using a hash table.
-------------------------------------- */

TgNode	   *
findNodeInRecipe(TgRecipe * r, char *nodeName)
{
	int			i;
	Arr_TgNodePtr *arr = r->allNodes;
	TgNode	   *n;

	for (i = 0; i < arr->num; i++)
	{
		n = (TgNode *) arr->val[i];
		if (strcmp(n->nodeName, nodeName) == 0)
			return n;
	}
	elog(NOTICE, "Node named %s not found in recipe named %s", nodeName, r->elmValue.elemName);
	return NULL;
}


/* -------------------------------------
fillTgNode
	takes a query result in the PortalBuffer containing a Node
	and converts it to a C Node strcture.
	The Node structure passed in is 'filled' appropriately

-------------------------------------- */

void
fillTgNode(TgRecipe * r, TgNode * node, PortalBuffer *pbuf, int tupno)
{
	char	   *nodeType;
	char	   *nodeElem;
	char	   *locString;		/* ascii string rep of the point */
	static int	attnums_initialized = 0;
	static int	nodeName_attnum;
	static int	nodeElem_attnum;
	static int	nodeType_attnum;
	static int	loc_attnum;
	TgNodePtr	BlankNodePtr;
	int			i;

	if (!attnums_initialized)
	{

		/*
		 * the first time fillTgNode is called, we find out all the
		 * relevant attribute numbers in a TgNode so subsequent calls are
		 * speeded up, the assumption is that the schema won't change
		 * between calls
		 */
		nodeName_attnum = PQfnumber(pbuf, tupno, "nodeName");
		nodeElem_attnum = PQfnumber(pbuf, tupno, "nodeElem");
		nodeType_attnum = PQfnumber(pbuf, tupno, "nodeType");
		loc_attnum = PQfnumber(pbuf, tupno, "loc");
		attnums_initialized = 1;
	}
	node->nodeName = PQgetAttr(pbuf, tupno, nodeName_attnum);
	locString = PQgetvalue(pbuf, tupno, loc_attnum);
	if (locString == NULL || locString[0] == '\0')
	{
		node->loc.x = 0;
		node->loc.y = 0;		/* assign to zero for default */
	}
	else
	{
		float		x,
					y;

		sscanf(locString, "(%f, %f)", &x, &y);
		node->loc.x = x;
		node->loc.y = y;
	}
	nodeElem = PQgetvalue(pbuf, tupno, nodeElem_attnum);
	node->nodeElem = findElemInRecipe(r, nodeElem);
	node->inNodes = newArr_TgNodePtr();
	node->outNodes = newArr_TgNodePtr();

	/*
	 * fill the inNodes array with as many NULL's are there are inPorts in
	 * the underlying element
	 */
	BlankNodePtr = (TgNodePtr) NULL;
	for (i = 0; i < node->nodeElem->inPorts->num; i++)
		addArr_TgNodePtr(node->inNodes, &BlankNodePtr);

	/*
	 * fill the outNodes array with as many NULL's are there are inPorts
	 * in the underlying element
	 */
	for (i = 0; i < node->nodeElem->outPorts->num; i++)
		addArr_TgNodePtr(node->outNodes, &BlankNodePtr);

	nodeType = PQgetvalue(pbuf, tupno, nodeType_attnum);

	if (strcmp(nodeType, "Ingred") == 0)
		node->nodeType = TG_INGRED_NODE;
	else if (strcmp(nodeType, "Eye") == 0)
		node->nodeType = TG_EYE_NODE;
	else if (strcmp(nodeType, "Recipe") == 0)
		node->nodeType = TG_RECIPE_NODE;
	else
		elog(NOTICE, "fillTgNode: unknown nodeType field value : %s\n", nodeType);

}

/* -------------------------------------
fillTgElement
	takes a query result in the PortalBuffer containing a Element
	and converts it to a C TgElement strcture.
	The TgElement structure passed in is 'filled' appropriately
  ------------------------------------ */

void
fillTgElement(TgElement * elem, PortalBuffer *pbuf, int tupno)
{
	char	   *srcLang,
			   *elemType;
	static int	attnums_initialized = 0;
	static int	elemName_attnum;
	static int	elemType_attnum;
	static int	inPorts_attnum;
	static int	inTypes_attnum;
	static int	outPorts_attnum;
	static int	outTypes_attnum;
	static int	doc_attnum;
	static int	keywords_attnum;
	static int	icon_attnum;
	static int	srcLang_attnum;
	static int	src_attnum;
	static int	owner_attnum;

	if (!attnums_initialized)
	{

		/*
		 * the first time fillTgElement is called, we find out all the
		 * relevant attribute numbers in a TgElement so subsequent calls
		 * are speeded up, the assumption is that the schema won't change
		 * between calls
		 */
		elemName_attnum = PQfnumber(pbuf, tupno, "elemName");
		elemType_attnum = PQfnumber(pbuf, tupno, "elemType");
		inPorts_attnum = PQfnumber(pbuf, tupno, "inPorts");
		inTypes_attnum = PQfnumber(pbuf, tupno, "inTypes");
		outPorts_attnum = PQfnumber(pbuf, tupno, "outPorts");
		outTypes_attnum = PQfnumber(pbuf, tupno, "outTypes");
		doc_attnum = PQfnumber(pbuf, tupno, "doc");
		keywords_attnum = PQfnumber(pbuf, tupno, "keywords");
		icon_attnum = PQfnumber(pbuf, tupno, "icon");
		srcLang_attnum = PQfnumber(pbuf, tupno, "srcLang");
		src_attnum = PQfnumber(pbuf, tupno, "src");
		attnums_initialized = 1;
	}

	elem->elemName = PQgetAttr(pbuf, tupno, elemName_attnum);
	elem->inPorts = TextArray2ArrTgString(PQgetvalue(pbuf, tupno, inPorts_attnum));
	elem->inTypes = TextArray2ArrTgString(PQgetvalue(pbuf, tupno, inTypes_attnum));
	elem->outPorts = TextArray2ArrTgString(PQgetvalue(pbuf, tupno, outPorts_attnum));
	elem->outTypes = TextArray2ArrTgString(PQgetvalue(pbuf, tupno, outTypes_attnum));
	elem->doc = PQgetAttr(pbuf, tupno, doc_attnum);
	elem->keywords = TextArray2ArrTgString(PQgetvalue(pbuf, tupno, keywords_attnum));
	elem->icon = PQgetAttr(pbuf, tupno, icon_attnum);
	elem->src = PQgetAttr(pbuf, tupno, src_attnum);
	elem->owner = PQgetAttr(pbuf, tupno, owner_attnum);

	/*
	 * we don't need to keep the value returned so use PQgetvalue()
	 * instead of PQgetAttr()
	 */
	srcLang = PQgetvalue(pbuf, tupno, srcLang_attnum);

	if (strcmp(srcLang, "SQL") == 0)
		elem->srcLang = TG_SQL;
	else if (strcmp(srcLang, "C") == 0)
		elem->srcLang = TG_C;
	else if (strcmp(srcLang, "RecipeGraph") == 0)
		elem->srcLang = TG_RECIPE_GRAPH;
	else if (strcmp(srcLang, "Compiled") == 0)
		elem->srcLang = TG_COMPILED;
	else
		elog(NOTICE, "fillTgElement(): unknown srcLang field value : %s\n", srcLang);

	elemType = PQgetvalue(pbuf, tupno, elemType_attnum);
	if (strcmp(elemType, "Ingred") == 0)
		elem->elemType = TG_INGRED;
	else if (strcmp(elemType, "Eye") == 0)
		elem->elemType = TG_EYE;
	else if (strcmp(elemType, "Recipe") == 0)
		elem->elemType = TG_RECIPE;
	else
		elog(NOTICE, "fillTgElement(): unknown elemType field value : %s\n", elemType);


}

/* -------------------------------------
lookupEdges -
	look up the edges of a recipe and fill in the inNodes
	and outNodes of each node.
	In the process of connecting edges, we detect tee's and create
	teeNodes.  We add the teeNodes to the allNodes field of r as well
------------------------------------ */
void
lookupEdges(TgRecipe * r, char *name)
{
	char		qbuf[MAX_QBUF_LENGTH];
	int			i;
	char	   *pqres;
	char	   *pbufname;
	PortalBuffer *pbuf;
	int			ntups;
	int			fromNode_attnum;
	int			fromPort_attnum;
	int			toPort_attnum;
	int			toNode_attnum;
	char	   *toNode,
			   *fromNode;
	char	   *toPortStr,
			   *fromPortStr;
	int			toPort,
				fromPort;

	TgNodePtr	fromNodePtr,
				toNodePtr;

	sprintf(qbuf, Q_LOOKUP_EDGES_IN_RECIPE, name);
	pqres = PQexec(qbuf);
	pqres = PQexec(qbuf);
	if (*pqres == 'R' || *pqres == 'E')
	{
		elog(NOTICE, "lookupEdges(): Error while executing query : %s\n", qbuf);
		elog(NOTICE, "result = %s, error is %s\n", pqres, PQerrormsg);
		return;
	}
	pbufname = ++pqres;
	pbuf = PQparray(pbufname);
	ntups = PQntuplesGroup(pbuf, 0);

	if (ntups == 0)
	{
		return;
	}

	fromNode_attnum = PQfnumber(pbuf, 0, "fromNode");
	fromPort_attnum = PQfnumber(pbuf, 0, "fromPort");
	toNode_attnum = PQfnumber(pbuf, 0, "toNode");
	toPort_attnum = PQfnumber(pbuf, 0, "toPort");

	for (i = 0; i < ntups; i++)
	{

		fromNode = PQgetvalue(pbuf, i, fromNode_attnum);
		toNode = PQgetvalue(pbuf, i, toNode_attnum);
		fromPortStr = PQgetvalue(pbuf, i, fromPort_attnum);
		toPortStr = PQgetvalue(pbuf, i, toPort_attnum);

		if (!fromPortStr || fromPortStr[0] == '\0')
		{
			elog(NOTICE, "lookupEdges():  SANITY CHECK failed.  Edge with invalid fromPort value!");
			return;
		}
		if (!toPortStr || toPortStr[0] == '\0')
		{
			elog(NOTICE, "lookupEdges():  SANITY CHECK failed.  Edge with invalid toPort value!!");
			return;
		}
		fromPort = atoi(fromPortStr);
		toPort = atoi(toPortStr);

		fromNodePtr = findNodeInRecipe(r, fromNode);
		if (!fromNodePtr)
		{
			elog(NOTICE, "lookupEdges():  SANITY CHECK failed.  Edge with bad fromNode value!");
			return;
		}
		toNodePtr = findNodeInRecipe(r, toNode);
		if (!toNodePtr)
		{
			elog(NOTICE, "lookupEdges():  SANITY CHECK failed.  Edge with bad toNode value!");
			return;
		}

		/*
		 * check to see if the from port is already connected. if it is,
		 * then this means we should construct a Tee node
		 */
		if (fromNodePtr->outNodes->val[fromPort - 1] != NULL)
		{
			TgNodePtr	tn;

			tn = connectTee(r, fromNodePtr, toNodePtr, fromPort, toPort);
			addArr_TgNodePtr(r->allNodes, &tn);
		}
		else
		{
			fromNodePtr->outNodes->val[fromPort - 1] = toNodePtr;
			toNodePtr->inNodes->val[toPort - 1] = fromNodePtr;
		}
	}

	PQclear(pbufname);
}

/*
   handle tee connections here
   Everytime an output port is connected multiply,
   we explicitly insert TgTeeNode

   returns the teeNode created
*/
static TgNode *
connectTee(TgRecipe * r, TgNodePtr fromNode, TgNodePtr toNode,
		   int fromPort, int toPort)
{
	TgNodePtr	origToNode;
	TgNodePtr	tn;
	TgNodePtr	BlankNodePtr;
	int			origToPort;
	int			i;

	/* the toNode formerly pointed to */
	origToNode = fromNode->outNodes->val[fromPort - 1];

	if (origToNode == NULL)
	{
		elog(NOTICE, "Internal Error: connectTee() called with a null origToNode");
		return;
	}

	for (i = 0; i < origToNode->inNodes->num; i++)
	{
		if (origToNode->inNodes->val[i] == fromNode)
			break;
	}

	/* the inport of the former toNode	*/
	/* ports start with 1, array indices start from 0 */
	origToPort = i + 1;

	/* add a tee node now. */
	tn = malloc(sizeof(TgNode));
	/* generate a name for the tee node table */
	tn->nodeName = malloc(50);
	sprintf(tn->nodeName, "tee_%d", newoid());
/*	  tn->nodeName = NULL; */

	tn->nodeType = TG_TEE_NODE;
	tn->nodeElem = NULL;
	tn->inNodes = newArr_TgNodePtr();
	tn->outNodes = newArr_TgNodePtr();

	BlankNodePtr = (TgNodePtr) NULL;
	/* each TgTeeNode has one input and two outputs, NULL them initiallly */
	addArr_TgNodePtr(tn->inNodes, &BlankNodePtr);
	addArr_TgNodePtr(tn->outNodes, &BlankNodePtr);
	addArr_TgNodePtr(tn->outNodes, &BlankNodePtr);

	/*
	 * make the old toNode the left parent of the tee node add the new
	 * toNode as the right parent of the tee node
	 */
	tn->outNodes->val[0] = origToNode;
	origToNode->inNodes->val[origToPort - 1] = tn;

	tn->outNodes->val[1] = toNode;
	toNode->inNodes->val[toPort - 1] = tn;

	/* connect the fromNode to the new tee node */
	fromNode->outNodes->val[fromPort - 1] = tn;
	tn->inNodes->val[0] = fromNode;

	return tn;
}

/* -------------------------------------
fillAllNodes
	fill out the nodes of a recipe
  ------------------------------------ */
void
fillAllNodes(TgRecipe * r, char *name)
{
	char		qbuf[MAX_QBUF_LENGTH];
	int			i;
	char	   *pqres;
	char	   *pbufname;
	PortalBuffer *pbuf;
	int			ntups;
	TgElement  *elem;
	TgNode	   *node;

	/* 1) fill out the elements that are in the recipe */
	sprintf(qbuf, Q_RETRIEVE_ELEMENTS_IN_RECIPE, name);
	pqres = PQexec(qbuf);
	if (*pqres == 'R' || *pqres == 'E')
	{
		elog(NOTICE, "fillAllNodes(): Error while executing query : %s\n", qbuf);
		elog(NOTICE, "result = %s, error is %s\n", pqres, PQerrormsg);
		return;
	}
	pbufname = ++pqres;
	pbuf = PQparray(pbufname);
	ntups = PQntuplesGroup(pbuf, 0);
	for (i = 0; i < ntups; i++)
	{
		elem = malloc(sizeof(TgElement));
		fillTgElement(elem, pbuf, i);
		addArr_TgElementPtr(r->elements, &elem);
	}
	PQclear(pbufname);

	sprintf(qbuf, Q_RETRIEVE_NODES_IN_RECIPE, name);
	pqres = PQexec(qbuf);
	if (*pqres == 'R' || *pqres == 'E')
	{
		elog(NOTICE, "fillAllNodes(): Error while executing query : %s\n", qbuf);
		elog(NOTICE, "result = %s, error is %s\n", pqres, PQerrormsg);
		return;
	}
	pbufname = ++pqres;
	pbuf = PQparray(pbufname);
	ntups = PQntuplesGroup(pbuf, 0);
	for (i = 0; i < ntups; i++)
	{
		node = malloc(sizeof(TgNode));
		fillTgNode(r, node, pbuf, i);
		addArr_TgNodePtr(r->allNodes, &node);
	}
	PQclear(pbufname);

}


/* -------------------------------------
fillAllElements
	fill out the elements of a recipe
  ------------------------------------ */
void
fillAllElements(TgRecipe * r, char *name)
{
	char		qbuf[MAX_QBUF_LENGTH];
	int			i;
	char	   *pqres;
	char	   *pbufname;
	PortalBuffer *pbuf;
	int			ntups;
	TgElement  *elem;

	sprintf(qbuf, Q_RETRIEVE_ELEMENTS_IN_RECIPE, name);
	pqres = PQexec(qbuf);
	if (*pqres == 'R' || *pqres == 'E')
	{
		elog(NOTICE, "fillAllElements(): Error while executing query : %s\n", qbuf);
		elog(NOTICE, "result = %s, error is %s\n", pqres, PQerrormsg);
		return;
	}
	pbufname = ++pqres;
	pbuf = PQparray(pbufname);
	ntups = PQntuplesGroup(pbuf, 0);
	for (i = 0; i < ntups; i++)
	{
		elem = malloc(sizeof(TgElement));
		fillTgElement(elem, pbuf, i);
		addArr_TgElementPtr(r->elements, &elem);
	}
	PQclear(pbufname);

}


/* -------------------------------------
fillTgRecipe
	takes a query result in the PortalBuffer containing a Recipe
	and converts it to a C TgRecipe strcture
  ------------------------------------ */
TgRecipe   *
fillTgRecipe(PortalBuffer *pbuf, int tupno)
{
	TgRecipe   *r;
	int			i,
				j;

	/* 1) set up the recipe structure */
	r = (TgRecipe *) malloc(sizeof(TgRecipe));
	fillTgElement(&r->elmValue, pbuf, 0);
	r->elmValue.elemType = TG_RECIPE;
	r->allNodes = newArr_TgNodePtr();
	r->rootNodes = newArr_TgNodePtr();
	r->eyes = newArr_TgNodePtr();
	r->tees = newArr_TgNodePtr();
	r->elements = newArr_TgElementPtr();

	/*
	 * 2) find all the elements. There may be less elements than nodes
	 * because you can have multiple instantiations of an element in a
	 * recipe
	 */
	fillAllElements(r, r->elmValue.elemName);

	/* 3) find all the nodes in the recipe */
	fillAllNodes(r, r->elmValue.elemName);

	/*
	 * 4) find all the edges, and connect the nodes, may also add tee
	 * nodes to the allNodes field
	 */
	lookupEdges(r, r->elmValue.elemName);

	/* 5) find all the rootNodes in the recipe */

	/*
	 * root nodes are nodes with no incoming nodes or whose incoming nodes
	 * are all null
	 */
	/* 6) find all the eyes in the recipe */
	/* eye nodes are nodes with the node type TG_EYE_NODE */
	/* 7) find all the tee nodes in the recipe */
	/* tee nodes are nodes with the node type TG_TEE_NODE */
	for (i = 0; i < r->allNodes->num; i++)
	{
		TgNode	   *nptr = r->allNodes->val[i];

		if (nptr->nodeType == TG_EYE_NODE)
			addArr_TgNodePtr(r->eyes, &nptr);
		else if (nptr->nodeType == TG_TEE_NODE)
			addArr_TgNodePtr(r->tees, &nptr);

		if (nptr->inNodes->num == 0)
			addArr_TgNodePtr(r->rootNodes, &nptr);
		else
		{
			for (j = 0;
			   j < nptr->inNodes->num && (nptr->inNodes->val[j] == NULL);
				 j++);
			if (j == nptr->inNodes->num)
				addArr_TgNodePtr(r->rootNodes, &nptr);
		}
	}

	return r;

}


/* -------------------------------------
retrieveRecipe
   find the recipe with the given name
  ------------------------------------ */
TgRecipe   *
retrieveRecipe(char *name)
{
	char		qbuf[MAX_QBUF_LENGTH];
	TgRecipe   *recipe;
	char	   *pqres;
	char	   *pbufname;
	PortalBuffer *pbuf;
	int			ntups;

	sprintf(qbuf, Q_RETRIEVE_RECIPE_BYNAME, name);

	pqres = PQexec(qbuf);
	if (*pqres == 'R' || *pqres == 'E')
	{
		elog(NOTICE, "retrieveRecipe: Error while executing query : %s\n", qbuf);
		elog(NOTICE, "result = %s, error is %s\n", pqres, PQerrormsg);
		return NULL;
	}
	pbufname = ++pqres;
	pbuf = PQparray(pbufname);
	ntups = PQntuplesGroup(pbuf, 0);
	if (ntups == 0)
	{
		elog(NOTICE, "retrieveRecipe():  No recipe named %s exists\n", name);
		return NULL;
	}
	if (ntups != 1)
	{
		elog(NOTICE, "retrieveRecipe():  Multiple (%d) recipes named %s exists\n", ntups, name);
		return NULL;
	}

	recipe = fillTgRecipe(pbuf, 0);

	PQclear(pbufname);
	return recipe;

}

/* -------------------- copyXXX functions ----------------------- */
void
copyTgElementPtr(TgElementPtr * from, TgElementPtr * to)
{
	*to = *from;
}

void
copyTgNodePtr(TgNodePtr * from, TgNodePtr * to)
{
	*to = *from;
}

void
copyTgRecipePtr(TgRecipePtr * from, TgRecipePtr * to)
{
	*to = *from;
}

void
copyTgString(TgString * from, TgString * to)
{
	TgString	fromTgString = *from;
	TgString	toTgString;

	toTgString = (TgString) malloc(strlen(fromTgString) + 1);
	strcpy(toTgString, fromTgString);
	*to = toTgString;
}
