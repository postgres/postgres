/*-------------------------------------------------------------------------
 *
 * parse_type.h
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: parse_type.h,v 1.1 1997/11/25 22:07:07 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSE_TYPE_H
#define PARSE_TYPE_H

#include "access/htup.h"

typedef HeapTuple Type;

bool typeidIsValid(Oid id);
Type typeidType(Oid id);
Type typenameType(char *s);
char *typeidTypeName(Oid id);
Oid typeTypeId(Type tp);
int16 typeLen(Type t);
bool typeByVal(Type t);
char *typeTypeName(Type t);
char typeTypeFlag(Type t);
char *stringTypeString(Type tp, char *string, int typlen);
Oid typeidRetoutfunc(Oid type_id);
Oid typeidTypeRelid(Oid type_id);
Oid typeTypeRelid(Type typ);
Oid typeidTypElem(Oid type_id);
Oid GetArrayElementType(Oid typearray);
Oid typeidRetinfunc(Oid type_id);

#endif							/* PARSE_TYPE_H */
