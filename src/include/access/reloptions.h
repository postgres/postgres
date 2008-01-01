/*-------------------------------------------------------------------------
 *
 * reloptions.h
 *	  Core support for relation options (pg_class.reloptions)
 *
 * Note: the functions dealing with text-array reloptions values declare
 * them as Datum, not ArrayType *, to avoid needing to include array.h
 * into a lot of low-level code.
 *
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/access/reloptions.h,v 1.5 2008/01/01 19:45:56 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef RELOPTIONS_H
#define RELOPTIONS_H

#include "nodes/pg_list.h"

extern Datum transformRelOptions(Datum oldOptions, List *defList,
					bool ignoreOids, bool isReset);

extern List *untransformRelOptions(Datum options);

extern void parseRelOptions(Datum options, int numkeywords,
				const char *const * keywords,
				char **values, bool validate);

extern bytea *default_reloptions(Datum reloptions, bool validate,
				   int minFillfactor, int defaultFillfactor);

extern bytea *heap_reloptions(char relkind, Datum reloptions, bool validate);

extern bytea *index_reloptions(RegProcedure amoptions, Datum reloptions,
				 bool validate);

#endif   /* RELOPTIONS_H */
