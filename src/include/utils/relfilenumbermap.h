/*-------------------------------------------------------------------------
 *
 * relfilenumbermap.h
 *	  relfilenumber to oid mapping cache.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/relfilenumbermap.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef RELFILENUMBERMAP_H
#define RELFILENUMBERMAP_H

#include "common/relpath.h"

extern Oid	RelidByRelfilenumber(Oid reltablespace,
								 RelFileNumber relfilenumber);

#endif							/* RELFILENUMBERMAP_H */
