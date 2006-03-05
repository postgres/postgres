/*-------------------------------------------------------------------------
 *
 * relfilenode.h
 *	  Physical access information for relations.
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/storage/relfilenode.h,v 1.13 2006/03/05 15:59:00 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef RELFILENODE_H
#define RELFILENODE_H

/*
 * RelFileNode must provide all that we need to know to physically access
 * a relation.
 *
 * spcNode identifies the tablespace of the relation.  It corresponds to
 * pg_tablespace.oid.
 *
 * dbNode identifies the database of the relation.	It is zero for
 * "shared" relations (those common to all databases of a cluster).
 * Nonzero dbNode values correspond to pg_database.oid.
 *
 * relNode identifies the specific relation.  relNode corresponds to
 * pg_class.relfilenode (NOT pg_class.oid, because we need to be able
 * to assign new physical files to relations in some situations).
 * Notice that relNode is only unique within a particular database.
 *
 * Note: spcNode must be GLOBALTABLESPACE_OID if and only if dbNode is
 * zero.  We support shared relations only in the "global" tablespace.
 *
 * Note: in pg_class we allow reltablespace == 0 to denote that the
 * relation is stored in its database's "default" tablespace (as
 * identified by pg_database.dattablespace).  However this shorthand
 * is NOT allowed in RelFileNode structs --- the real tablespace ID
 * must be supplied when setting spcNode.
 */
typedef struct RelFileNode
{
	Oid			spcNode;		/* tablespace */
	Oid			dbNode;			/* database */
	Oid			relNode;		/* relation */
} RelFileNode;

/*
 * Note: RelFileNodeEquals compares relNode first since that is most likely
 * to be different in two unequal RelFileNodes.  It is probably redundant
 * to compare spcNode if the other two fields are found equal, but do it
 * anyway to be sure.
 */
#define RelFileNodeEquals(node1, node2) \
	((node1).relNode == (node2).relNode && \
	 (node1).dbNode == (node2).dbNode && \
	 (node1).spcNode == (node2).spcNode)

#endif   /* RELFILENODE_H */
