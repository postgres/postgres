/*-------------------------------------------------------------------------
 *
 * namespace.h
 *	  prototypes for functions in backend/catalog/namespace.c
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: namespace.h,v 1.5 2002/04/01 03:34:27 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NAMESPACE_H
#define NAMESPACE_H

#include "nodes/primnodes.h"


extern Oid	RangeVarGetRelid(const RangeVar *relation, bool failOK);

extern Oid	RangeVarGetCreationNamespace(const RangeVar *newRelation);

extern Oid	RelnameGetRelid(const char *relname);

extern Oid	TypenameGetTypid(const char *typname);

extern Oid	QualifiedNameGetCreationNamespace(List *names, char **objname_p);

extern RangeVar *makeRangeVarFromNameList(List *names);

extern bool isTempNamespace(Oid namespaceId);

/* stuff for search_path GUC variable */
extern char *namespace_search_path;

extern bool check_search_path(const char *proposed);
extern void assign_search_path(const char *newval);
extern void InitializeSearchPath(void);

#endif   /* NAMESPACE_H */
