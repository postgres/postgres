/*-------------------------------------------------------------------------
 *
 * objectaddress.h
 *	  functions for working with object addresses
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
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
#include "utils/relcache.h"

/*
 * An ObjectAddress represents a database object of any type.
 */
typedef struct ObjectAddress
{
	Oid			classId;		/* Class Id from pg_class */
	Oid			objectId;		/* OID of the object */
	int32		objectSubId;	/* Subitem within object (eg column), or 0 */
} ObjectAddress;

extern ObjectAddress get_object_address(ObjectType objtype, List *objname,
				   List *objargs, Relation *relp,
				   LOCKMODE lockmode, bool missing_ok);

extern void check_object_ownership(Oid roleid,
					   ObjectType objtype, ObjectAddress address,
					   List *objname, List *objargs, Relation relation);

extern Oid	get_object_namespace(const ObjectAddress *address);

#endif   /* PARSE_OBJECT_H */
