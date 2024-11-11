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

#ifndef FRONTEND
#include "nodes/pg_list.h"

extern Oid	get_tde_basic_table_am_oid(void);
extern Oid	get_tde_table_am_oid(void);
extern List *get_all_tde_tables(void);
extern int	get_tde_tables_count(void);
#endif /* !FRONTEND */

extern char *pg_tde_get_tde_file_dir(Oid dbOid, Oid spcOid);
extern void pg_tde_set_globalspace_dir(const char *dir);
#endif /* PG_TDE_UTILS_H */
