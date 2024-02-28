/*-------------------------------------------------------------------------
 *
 * pg_tde_utils.h
 * src/include/common/pg_tde_utils.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_TDE_UTILS_H
#define PG_TDE_UTILS_H

#include "postgres.h"
#include "nodes/pg_list.h"

extern Oid get_tde_table_am_oid(void);
extern List *get_all_tde_tables(void);
extern int get_tde_tables_count(void);

#endif /*PG_TDE_UTILS_H*/