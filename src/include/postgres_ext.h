/*-------------------------------------------------------------------------
 *
 * postgres_ext.h--
 *
 *	   This file contains declarations of things that are visible
 *	external to Postgres.  For example, the Oid type is part of a
 *	structure that is passed to the front end via libpq, and is
 *	accordingly referenced in libpq-fe.h.
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
 * $Id: postgres_ext.h,v 1.2 1997/09/07 04:55:41 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#ifndef POSTGRES_EXT_H
#define POSTGRES_EXT_H

typedef unsigned int Oid;

/* NAMEDATALEN is the max length for system identifiers (e.g. table names,
 * attribute names, function names, etc.)
 *
 * NOTE that databases with different NAMEDATALEN's cannot interoperate!
 */
#define NAMEDATALEN 32

/* OIDNAMELEN should be set to NAMEDATALEN + sizeof(Oid) */
#define OIDNAMELEN	36

#endif
