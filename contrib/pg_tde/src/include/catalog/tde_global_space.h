/*-------------------------------------------------------------------------
 *
 * tde_global_space.h
 *	  Global catalog key management
 *
 * src/include/catalog/tde_global_space.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef TDE_GLOBAL_CATALOG_H
#define TDE_GLOBAL_CATALOG_H

#include "postgres.h"
#include "catalog/pg_tablespace_d.h"

/*
 * Needed for global data (WAL etc) keys identification in caches and storage.
 * We take Oids of the sql operators, so there is no overlap with the "real"
 * catalog objects possible.
 */
#define GLOBAL_DATA_TDE_OID	607 // TODO: why not repeat GLOBALTABLESPACE_OID ?
#define XLOG_TDE_OID        608

#define GLOBAL_SPACE_RLOCATOR(_obj_oid) (RelFileLocator) { \
	GLOBALTABLESPACE_OID, \
	GLOBAL_DATA_TDE_OID, \
	_obj_oid \
}

/*  Needed for using the same default key for multiple databases */
#define DEFAULT_DATA_TDE_OID	DEFAULTTABLESPACE_OID

#define TDEisInGlobalSpace(dbOid) 	(dbOid == GLOBAL_DATA_TDE_OID)

#endif							/* TDE_GLOBAL_CATALOG_H */
