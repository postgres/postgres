#ifndef PG_TDE_H
#define PG_TDE_H

#define PG_TDE_NAME "pg_tde"
#define PG_TDE_VERSION "1.0.0-rc"
#define PG_TDE_VERSION_STRING PG_TDE_NAME " " PG_TDE_VERSION

#define PG_TDE_DATA_DIR	"pg_tde"

#define TDE_TRANCHE_NAME "pg_tde_tranche"

typedef enum
{
	TDE_LWLOCK_ENC_KEY,
	TDE_LWLOCK_PI_FILES,

	/* Must be the last entry in the enum */
	TDE_LWLOCK_COUNT
}			TDELockTypes;

typedef struct XLogExtensionInstall
{
	Oid			database_id;
} XLogExtensionInstall;

extern void extension_install_redo(XLogExtensionInstall *xlrec);

#endif							/* PG_TDE_H */
