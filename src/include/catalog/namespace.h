/*-------------------------------------------------------------------------
 *
 * namespace.h
 *	  prototypes for functions in backend/catalog/namespace.c
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: namespace.h,v 1.3 2002/03/30 01:02:42 tgl Exp $
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

#endif   /* NAMESPACE_H */
