/*-------------------------------------------------------------------------
 *
 * value.c
 *	  implementation of value nodes
 *
 *
 * Copyright (c) 2003-2021, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/nodes/value.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/value.h"

/*
 *	makeInteger
 */
Integer *
makeInteger(int i)
{
	Integer	   *v = makeNode(Integer);

	v->val = i;
	return v;
}

/*
 *	makeFloat
 *
 * Caller is responsible for passing a palloc'd string.
 */
Float *
makeFloat(char *numericStr)
{
	Float	   *v = makeNode(Float);

	v->val = numericStr;
	return v;
}

/*
 *	makeString
 *
 * Caller is responsible for passing a palloc'd string.
 */
String *
makeString(char *str)
{
	String	   *v = makeNode(String);

	v->val = str;
	return v;
}

/*
 *	makeBitString
 *
 * Caller is responsible for passing a palloc'd string.
 */
BitString *
makeBitString(char *str)
{
	BitString  *v = makeNode(BitString);

	v->val = str;
	return v;
}
