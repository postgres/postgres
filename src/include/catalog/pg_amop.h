/*-------------------------------------------------------------------------
 *
 * pg_amop.h
 *	  definition of the "access method operator" system catalog (pg_amop)
 *
 * The amop table identifies the operators associated with each index operator
 * family and operator class (classes are subsets of families).  An associated
 * operator can be either a search operator or an ordering operator, as
 * identified by amoppurpose.
 *
 * The primary key for this table is <amopfamily, amoplefttype, amoprighttype,
 * amopstrategy>.  amoplefttype and amoprighttype are just copies of the
 * operator's oprleft/oprright, ie its declared input data types.  The
 * "default" operators for a particular opclass within the family are those
 * with amoplefttype = amoprighttype = opclass's opcintype.  An opfamily may
 * also contain other operators, typically cross-data-type operators.  All the
 * operators within a family are supposed to be compatible, in a way that is
 * defined by each individual index AM.
 *
 * We also keep a unique index on <amopopr, amoppurpose, amopfamily>, so that
 * we can use a syscache to quickly answer questions of the form "is this
 * operator in this opfamily, and if so what are its semantics with respect to
 * the family?"  This implies that the same operator cannot be listed for
 * multiple strategy numbers within a single opfamily, with the exception that
 * it's possible to list it for both search and ordering purposes (with
 * different strategy numbers for the two purposes).
 *
 * amopmethod is a copy of the owning opfamily's opfmethod field.  This is an
 * intentional denormalization of the catalogs to buy lookup speed.
 *
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_amop.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_AMOP_H
#define PG_AMOP_H

#include "catalog/genbki.h"
#include "catalog/pg_amop_d.h"

/* ----------------
 *		pg_amop definition.  cpp turns this into
 *		typedef struct FormData_pg_amop
 * ----------------
 */
CATALOG(pg_amop,2602,AccessMethodOperatorRelationId)
{
	Oid			oid;			/* oid */

	/* the index opfamily this entry is for */
	Oid			amopfamily BKI_LOOKUP(pg_opfamily);

	/* operator's left input data type */
	Oid			amoplefttype BKI_LOOKUP(pg_type);

	/* operator's right input data type */
	Oid			amoprighttype BKI_LOOKUP(pg_type);

	/* operator strategy number */
	int16		amopstrategy;

	/* is operator for 's'earch or 'o'rdering? */
	char		amoppurpose BKI_DEFAULT(s);

	/* the operator's pg_operator OID */
	Oid			amopopr BKI_LOOKUP(pg_operator);

	/* the index access method this entry is for */
	Oid			amopmethod BKI_LOOKUP(pg_am);

	/* ordering opfamily OID, or 0 if search op */
	Oid			amopsortfamily BKI_DEFAULT(0) BKI_LOOKUP(pg_opfamily);
} FormData_pg_amop;

/* ----------------
 *		Form_pg_amop corresponds to a pointer to a tuple with
 *		the format of pg_amop relation.
 * ----------------
 */
typedef FormData_pg_amop *Form_pg_amop;

#ifdef EXPOSE_TO_CLIENT_CODE

/* allowed values of amoppurpose: */
#define AMOP_SEARCH		's'		/* operator is for search */
#define AMOP_ORDER		'o'		/* operator is for ordering */

#endif							/* EXPOSE_TO_CLIENT_CODE */

#endif							/* PG_AMOP_H */
