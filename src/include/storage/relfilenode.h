/*-------------------------------------------------------------------------
 *
 * relfilenode.h
 *	  Physical access information for relations.
 *
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/storage/relfilenode.h,v 1.25 2010/02/07 20:48:13 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef RELFILENODE_H
#define RELFILENODE_H

/*
 * The physical storage of a relation consists of one or more forks. The
 * main fork is always created, but in addition to that there can be
 * additional forks for storing various metadata. ForkNumber is used when
 * we need to refer to a specific fork in a relation.
 */
typedef enum ForkNumber
{
	InvalidForkNumber = -1,
	MAIN_FORKNUM = 0,
	FSM_FORKNUM,
	VISIBILITYMAP_FORKNUM

	/*
	 * NOTE: if you add a new fork, change MAX_FORKNUM below and update the
	 * forkNames array in catalog.c
	 */
} ForkNumber;

#define MAX_FORKNUM		VISIBILITYMAP_FORKNUM

/*
 * RelFileNode must provide all that we need to know to physically access
 * a relation. Note, however, that a "physical" relation is comprised of
 * multiple files on the filesystem, as each fork is stored as a separate
 * file, and each fork can be divided into multiple segments. See md.c.
 *
 * spcNode identifies the tablespace of the relation.  It corresponds to
 * pg_tablespace.oid.
 *
 * dbNode identifies the database of the relation.  It is zero for
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
 *
 * Note: in pg_class, relfilenode can be zero to denote that the relation
 * is a "mapped" relation, whose current true filenode number is available
 * from relmapper.c.  Again, this case is NOT allowed in RelFileNodes.
 *
 * Note: various places use RelFileNode in hashtable keys.  Therefore,
 * there *must not* be any unused padding bytes in this struct.  That
 * should be safe as long as all the fields are of type Oid.
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
