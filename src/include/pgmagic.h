/*-------------------------------------------------------------------------
 *
 * pgmagic.h
 *	Defines a magic block that can mark a module in a way so show that
 *      it is compatible with the server it is being loaded into.
 *
 * This file is intended to be included into modules that wish to load
 * themselves into the backend. All they need to do is include this header
 * into one of the source files and include the line:
 *
 * PG_MODULE_MAGIC;
 *
 * The trailing semi-colon is optional. To work with versions of PostgreSQL
 * that do not support this, you may put an #ifdef/endif block around it.
 *
 * Note, there is space available, particularly in the bitfield part. If it
 * turns out that a change has happened within a major release that would
 * require all modules to be recompiled, just setting one unused bit there
 * will do the trick.
 *
 * Originally written by Martijn van Oosterhout <kleptog@svana.org>
 *
 * $PostgreSQL: pgsql/src/include/pgmagic.h,v 1.1 2006/05/30 14:09:32 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
 
#ifndef PGMAGIC_H
#define PGMAGIC_H

#include "c.h"

/* The main structure in which the magic is stored. the length field is used
 * to detect major changes */

typedef struct {
  int len;
  int version;
  int magic;
} Pg_magic_struct;

/* Declare the module magic function. It needs to be a function as the dlsym
 * in the backend is only guarenteed to work on functions, not data */

typedef Pg_magic_struct *(*PGModuleMagicFunction) (void);

#define PG_MAGIC_FUNCTION_NAME Pg_magic_func
#define PG_MAGIC_FUNCTION_NAME_STRING "Pg_magic_func"

#define PG_MODULE_MAGIC     \
extern DLLIMPORT Pg_magic_struct *PG_MAGIC_FUNCTION_NAME(void);       \
Pg_magic_struct *                                            \
PG_MAGIC_FUNCTION_NAME(void) \
{ \
  static Pg_magic_struct Pg_magic_data = PG_MODULE_MAGIC_DATA; \
  return &Pg_magic_data; \
}

    /* Common user adjustable constants */
#define PG_MODULE_MAGIC_CONST \
   ((INDEX_MAX_KEYS <<  0) +                                    \
    (FUNC_MAX_ARGS  <<  8) +                                    \
    (NAMEDATALEN    << 16))

/* Finally, the actual data block */
#define PG_MODULE_MAGIC_DATA                                    \
{                                                               \
  sizeof(Pg_magic_struct),                                      \
  PG_VERSION_NUM / 100,       /* Major version of postgres */   \
  PG_MODULE_MAGIC_CONST,   /* Constants users can configure */  \
}

#endif  /* PGMAGIC_H */
