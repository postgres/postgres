/*-------------------------------------------------------------------------
 *
 * namespace.h
 *	  prototypes for functions in backend/catalog/namespace.c
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: namespace.h,v 1.8 2002/04/16 23:08:11 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NAMESPACE_H
#define NAMESPACE_H

#include "nodes/primnodes.h"


/*
 *	This structure holds a list of possible functions or operators
 *	found by namespace lookup.  Each function/operator is identified
 *	by OID and by argument types; the list must be pruned by type
 *	resolution rules that are embodied in the parser, not here.
 *	The number of arguments is assumed to be known a priori.
 */
typedef struct _FuncCandidateList
{
	struct _FuncCandidateList *next;
	int			pathpos;		/* for internal use of namespace lookup */
	Oid			oid;			/* the function or operator's OID */
	Oid			args[1];		/* arg types --- VARIABLE LENGTH ARRAY */
} *FuncCandidateList;			/* VARIABLE LENGTH STRUCT */


extern Oid	RangeVarGetRelid(const RangeVar *relation, bool failOK);

extern Oid	RangeVarGetCreationNamespace(const RangeVar *newRelation);

extern Oid	RelnameGetRelid(const char *relname);

extern Oid	TypenameGetTypid(const char *typname);

extern FuncCandidateList FuncnameGetCandidates(List *names, int nargs);

extern FuncCandidateList OpernameGetCandidates(List *names, char oprkind);

extern Oid	QualifiedNameGetCreationNamespace(List *names, char **objname_p);

extern RangeVar *makeRangeVarFromNameList(List *names);

extern char *NameListToString(List *names);

extern bool isTempNamespace(Oid namespaceId);

/* stuff for search_path GUC variable */
extern char *namespace_search_path;

extern bool check_search_path(const char *proposed);
extern void assign_search_path(const char *newval);
extern void InitializeSearchPath(void);

#endif   /* NAMESPACE_H */
