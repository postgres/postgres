/*-------------------------------------------------------------------------
 *
 * relfilelocator.h
 *	  Physical access information for relations.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/relfilelocator.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef RELFILELOCATOR_H
#define RELFILELOCATOR_H

#include "common/relpath.h"
#include "storage/procnumber.h"

/*
 * RelFileLocator must provide all that we need to know to physically access
 * a relation, with the exception of the backend's proc number, which can be
 * provided separately.  Note, however, that a "physical" relation is
 * comprised of multiple files on the filesystem, as each fork is stored as
 * a separate file, and each fork can be divided into multiple segments. See
 * md.c.
 *
 * spcOid identifies the tablespace of the relation.  It corresponds to
 * pg_tablespace.oid.
 *
 * dbOid identifies the database of the relation.  It is zero for
 * "shared" relations (those common to all databases of a cluster).
 * Nonzero dbOid values correspond to pg_database.oid.
 *
 * relNumber identifies the specific relation.  relNumber corresponds to
 * pg_class.relfilenode (NOT pg_class.oid, because we need to be able
 * to assign new physical files to relations in some situations).
 * Notice that relNumber is only unique within a database in a particular
 * tablespace.
 *
 * Note: spcOid must be GLOBALTABLESPACE_OID if and only if dbOid is
 * zero.  We support shared relations only in the "global" tablespace.
 *
 * Note: in pg_class we allow reltablespace == 0 to denote that the
 * relation is stored in its database's "default" tablespace (as
 * identified by pg_database.dattablespace).  However this shorthand
 * is NOT allowed in RelFileLocator structs --- the real tablespace ID
 * must be supplied when setting spcOid.
 *
 * Note: in pg_class, relfilenode can be zero to denote that the relation
 * is a "mapped" relation, whose current true filenode number is available
 * from relmapper.c.  Again, this case is NOT allowed in RelFileLocators.
 *
 * Note: various places use RelFileLocator in hashtable keys.  Therefore,
 * there *must not* be any unused padding bytes in this struct.  That
 * should be safe as long as all the fields are of type Oid.
 */
typedef struct RelFileLocator
{
	Oid			spcOid;			/* tablespace */
	Oid			dbOid;			/* database */
	RelFileNumber relNumber;	/* relation */
} RelFileLocator;

/*
 * Augmenting a relfilelocator with the backend's proc number provides all the
 * information we need to locate the physical storage.  'backend' is
 * INVALID_PROC_NUMBER for regular relations (those accessible to more than
 * one backend), or the owning backend's proc number for backend-local
 * relations.  Backend-local relations are always transient and removed in
 * case of a database crash; they are never WAL-logged or fsync'd.
 */
typedef struct RelFileLocatorBackend
{
	RelFileLocator locator;
	ProcNumber	backend;
} RelFileLocatorBackend;

#define RelFileLocatorBackendIsTemp(rlocator) \
	((rlocator).backend != INVALID_PROC_NUMBER)

/*
 * Note: RelFileLocatorEquals and RelFileLocatorBackendEquals compare relNumber
 * first since that is most likely to be different in two unequal
 * RelFileLocators.  It is probably redundant to compare spcOid if the other
 * fields are found equal, but do it anyway to be sure.  Likewise for checking
 * the backend number in RelFileLocatorBackendEquals.
 */
#define RelFileLocatorEquals(locator1, locator2) \
	((locator1).relNumber == (locator2).relNumber && \
	 (locator1).dbOid == (locator2).dbOid && \
	 (locator1).spcOid == (locator2).spcOid)

#define RelFileLocatorBackendEquals(locator1, locator2) \
	((locator1).locator.relNumber == (locator2).locator.relNumber && \
	 (locator1).locator.dbOid == (locator2).locator.dbOid && \
	 (locator1).backend == (locator2).backend && \
	 (locator1).locator.spcOid == (locator2).locator.spcOid)

#endif							/* RELFILELOCATOR_H */
