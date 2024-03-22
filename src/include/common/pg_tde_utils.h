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

extern const char *extract_json_cstr(Datum json, const char* field_name);
const char *extract_json_option_value(Datum top_json, const char* field_name);

#endif /*PG_TDE_UTILS_H*/