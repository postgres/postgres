/*-------------------------------------------------------------------------
 *
 * parse_type.h
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: parse_type.h,v 1.6 1998/02/26 04:42:50 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSE_TYPE_H
#define PARSE_TYPE_H

#include "access/htup.h"

typedef HeapTuple Type;

extern bool typeidIsValid(Oid id);
extern Type typeidType(Oid id);
extern Type typenameType(char *s);
extern char *typeidTypeName(Oid id);
extern Oid	typeTypeId(Type tp);
extern int16 typeLen(Type t);
extern bool typeByVal(Type t);
extern char *typeTypeName(Type t);
extern char typeTypeFlag(Type t);
extern char *stringTypeString(Type tp, char *string, int16 atttypmod);
extern Oid	typeidOutfunc(Oid type_id);
extern Oid	typeidTypeRelid(Oid type_id);
extern Oid	typeTypeRelid(Type typ);
extern Oid	typeidTypElem(Oid type_id);
extern Oid	GetArrayElementType(Oid typearray);
extern Oid	typeidInfunc(Oid type_id);

#endif							/* PARSE_TYPE_H */
