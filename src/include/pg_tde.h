/*-------------------------------------------------------------------------
 *
 * pg_tde.h
 * src/include/pg_tde.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_TDE_H
#define PG_TDE_H

typedef void (*pg_tde_on_ext_install_callback)(int tde_tbl_count, void* arg);

extern void on_ext_install(pg_tde_on_ext_install_callback function, void* arg);

#endif /*PG_TDE_UTILS_H*/