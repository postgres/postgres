/*-------------------------------------------------------------------------
 *
 * heap.h--
 *    prototypes for functions in lib/catalog/heap.c
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: heap.h,v 1.1.1.1 1996/07/09 06:21:15 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef HEAP_H
#define HEAP_H

extern Relation heap_creatr(char *relname, unsigned smgr, TupleDesc att);

extern int RelationAlreadyExists(Relation pg_class_desc, char relname[]);
extern void addNewRelationType(char *typeName, Oid new_rel_oid);

extern void AddPgRelationTuple(Relation pg_class_desc,
	Relation new_rel_desc, Oid new_rel_oid, int arch, unsigned natts);

extern Oid heap_create(char relname[], 
		       char *typename,
		       int arch, 
		       unsigned smgr, TupleDesc tupdesc);

extern void RelationRemoveInheritance(Relation relation);
extern void RelationRemoveIndexes(Relation relation);
extern void DeletePgRelationTuple(Relation rdesc);
extern void DeletePgAttributeTuples(Relation rdesc);
extern void DeletePgTypeTuple(Relation rdesc);
extern void heap_destroy(char relname[]);
extern void heap_destroyr(Relation r);
 
extern void InitTempRelList();
extern void AddToTempRelList(Relation r);
extern void RemoveFromTempRelList(Relation r);
extern void DestroyTempRels();

#endif	/* HEAP_H */
