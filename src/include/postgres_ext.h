/*-------------------------------------------------------------------------
 *
 * postgres_ext.h
 *
 *	   This file contains declarations of things that are visible everywhere
 *	in PostgreSQL *and* are visible to clients of frontend interface libraries.
 *	For example, the Oid type is part of the API of libpq and other libraries.
 *
 *	   Declarations which are specific to a particular interface should
 *	go in the header file for that interface (such as libpq-fe.h).	This
 *	file is only for fundamental Postgres declarations.
 *
 *	   User-written C functions don't count as "external to Postgres."
 *	Those function much as local modifications to the backend itself, and
 *	use header files that are otherwise internal to Postgres to interface
 *	with the backend.
 *
 * $Id: postgres_ext.h,v 1.12 2003/03/18 17:21:07 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#ifndef POSTGRES_EXT_H
#define POSTGRES_EXT_H

/*
 * Object ID is a fundamental type in Postgres.
 */
typedef unsigned int Oid;

#ifdef __cplusplus
#define InvalidOid		(Oid(0))
#else
#define InvalidOid		((Oid) 0)
#endif

#define OID_MAX  UINT_MAX
/* you will need to include <limits.h> to use the above #define */


/*
 * NAMEDATALEN is the max length for system identifiers (e.g. table names,
 * attribute names, function names, etc).  It must be a multiple of
 * sizeof(int) (typically 4).
 *
 * NOTE that databases with different NAMEDATALEN's cannot interoperate!
 */
#define NAMEDATALEN 64

#endif
