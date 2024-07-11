/*-------------------------------------------------------------------------
 *
 * pg_tde.h
 * src/include/pg_tde.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_TDE_H
#define PG_TDE_H

typedef struct XLogExtensionInstall
{
    Oid database_id;
    Oid tablespace_id;
} XLogExtensionInstall;

typedef void (*pg_tde_on_ext_install_callback)(int tde_tbl_count, XLogExtensionInstall* ext_info, bool redo, void *arg);

extern void on_ext_install(pg_tde_on_ext_install_callback function, void* arg);

extern void extension_install_redo(XLogExtensionInstall *xlrec);
#endif /*PG_TDE_H*/