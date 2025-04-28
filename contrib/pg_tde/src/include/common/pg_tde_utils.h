/*-------------------------------------------------------------------------
 *
 * pg_tde_utils.h
 * src/include/common/pg_tde_utils.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_TDE_UTILS_H
#define PG_TDE_UTILS_H

extern void pg_tde_set_data_dir(const char *dir);
extern const char *pg_tde_get_data_dir(void);

#endif							/* PG_TDE_UTILS_H */
