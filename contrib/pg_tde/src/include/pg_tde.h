/*-------------------------------------------------------------------------
 *
 * pg_tde.h
 * src/include/pg_tde.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_TDE_H
#define PG_TDE_H

#define PG_TDE_NAME "pg_tde"
#define PG_TDE_VERSION "1.0.0-rc"
#define PG_TDE_VERSION_STRING PG_TDE_NAME " " PG_TDE_VERSION

#define PG_TDE_DATA_DIR	"pg_tde"

typedef struct XLogExtensionInstall
{
	Oid			database_id;
} XLogExtensionInstall;

typedef void (*pg_tde_on_ext_install_callback) (XLogExtensionInstall *ext_info, bool redo);

extern void on_ext_install(pg_tde_on_ext_install_callback function);
extern void extension_install_redo(XLogExtensionInstall *xlrec);

#endif							/* PG_TDE_H */
