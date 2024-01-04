/*-------------------------------------------------------------------------
 *
 * pg_class.c
 *	  routines to support manipulation of the pg_class relation
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/catalog/pg_class.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_class.h"

/*
 * Issue an errdetail() informing that the relkind is not supported for this
 * operation.
 */
int
errdetail_relkind_not_supported(char relkind)
{
	switch (relkind)
	{
		case RELKIND_RELATION:
			return errdetail("This operation is not supported for tables.");
		case RELKIND_INDEX:
			return errdetail("This operation is not supported for indexes.");
		case RELKIND_SEQUENCE:
			return errdetail("This operation is not supported for sequences.");
		case RELKIND_TOASTVALUE:
			return errdetail("This operation is not supported for TOAST tables.");
		case RELKIND_VIEW:
			return errdetail("This operation is not supported for views.");
		case RELKIND_MATVIEW:
			return errdetail("This operation is not supported for materialized views.");
		case RELKIND_COMPOSITE_TYPE:
			return errdetail("This operation is not supported for composite types.");
		case RELKIND_FOREIGN_TABLE:
			return errdetail("This operation is not supported for foreign tables.");
		case RELKIND_PARTITIONED_TABLE:
			return errdetail("This operation is not supported for partitioned tables.");
		case RELKIND_PARTITIONED_INDEX:
			return errdetail("This operation is not supported for partitioned indexes.");
		default:
			elog(ERROR, "unrecognized relkind: '%c'", relkind);
			return 0;
	}
}
