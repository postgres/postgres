/*-------------------------------------------------------------------------
 *
 * namespace.h
 *	  prototypes for functions in backend/catalog/namespace.c
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: namespace.h,v 1.27 2003/08/04 02:40:10 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NAMESPACE_H
#define NAMESPACE_H

#include "nodes/primnodes.h"


/*
 *	This structure holds a list of possible functions or operators
 *	found by namespace lookup.	Each function/operator is identified
 *	by OID and by argument types; the list must be pruned by type
 *	resolution rules that are embodied in the parser, not here.
 */
typedef struct _FuncCandidateList
{
	struct _FuncCandidateList *next;
	int			pathpos;		/* for internal use of namespace lookup */
	Oid			oid;			/* the function or operator's OID */
	int			nargs;			/* number of arg types returned */
	Oid			args[1];		/* arg types --- VARIABLE LENGTH ARRAY */
}	*FuncCandidateList;	/* VARIABLE LENGTH STRUCT */

/*
 *	This structure holds a list of opclass candidates found by namespace
 *	lookup.
 */
typedef struct _OpclassCandidateList
{
	struct _OpclassCandidateList *next;
	char	   *opcname_tmp;	/* for internal use of namespace lookup */
	int			pathpos;		/* for internal use of namespace lookup */
	Oid			oid;			/* the opclass's OID */
	Oid			opcintype;		/* type of input data for opclass */
	bool		opcdefault;		/* T if opclass is default for opcintype */
	Oid			opckeytype;		/* type of index data, or InvalidOid */
}	*OpclassCandidateList;


extern Oid	RangeVarGetRelid(const RangeVar *relation, bool failOK);
extern Oid	RangeVarGetCreationNamespace(const RangeVar *newRelation);
extern Oid	RelnameGetRelid(const char *relname);
extern bool RelationIsVisible(Oid relid);

extern Oid	TypenameGetTypid(const char *typname);
extern bool TypeIsVisible(Oid typid);

extern FuncCandidateList FuncnameGetCandidates(List *names, int nargs);
extern bool FunctionIsVisible(Oid funcid);

extern FuncCandidateList OpernameGetCandidates(List *names, char oprkind);
extern bool OperatorIsVisible(Oid oprid);

extern OpclassCandidateList OpclassGetCandidates(Oid amid);
extern Oid	OpclassnameGetOpcid(Oid amid, const char *opcname);
extern bool OpclassIsVisible(Oid opcid);

extern Oid	ConversionGetConid(const char *conname);
extern bool ConversionIsVisible(Oid conid);

extern void DeconstructQualifiedName(List *names,
						 char **nspname_p,
						 char **objname_p);
extern Oid	LookupExplicitNamespace(const char *nspname);

extern Oid	QualifiedNameGetCreationNamespace(List *names, char **objname_p);
extern RangeVar *makeRangeVarFromNameList(List *names);
extern char *NameListToString(List *names);
extern char *NameListToQuotedString(List *names);

extern bool isTempNamespace(Oid namespaceId);
extern bool isOtherTempNamespace(Oid namespaceId);

extern void PushSpecialNamespace(Oid namespaceId);
extern void PopSpecialNamespace(Oid namespaceId);

extern Oid	FindConversionByName(List *conname);
extern Oid	FindDefaultConversionProc(int4 for_encoding, int4 to_encoding);

/* initialization & transaction cleanup code */
extern void InitializeSearchPath(void);
extern void AtEOXact_Namespace(bool isCommit);

/* stuff for search_path GUC variable */
extern char *namespace_search_path;

extern const char *assign_search_path(const char *newval,
				   bool doit, bool interactive);

extern List *fetch_search_path(bool includeImplicit);

#endif   /* NAMESPACE_H */
