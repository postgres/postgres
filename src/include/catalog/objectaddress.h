/*-------------------------------------------------------------------------
 *
 * objectaddress.h
 *	  functions for working with object addresses
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/objectaddress.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef OBJECTADDRESS_H 
#define OBJECTADDRESS_H

#include "nodes/parsenodes.h"
#include "storage/lock.h"
#include "utils/rel.h"

/*
 * An ObjectAddress represents a database object of any type.
 */
typedef struct ObjectAddress
{
	Oid			classId;		/* Class Id from pg_class */
	Oid			objectId;		/* OID of the object */
	int32		objectSubId;	/* Subitem within object (eg column), or 0 */
} ObjectAddress;

ObjectAddress get_object_address(ObjectType objtype, List *objname,
				   List *objargs, Relation *relp, LOCKMODE lockmode);

#endif   /* PARSE_OBJECT_H */
