/*
 * Global catalog key management
 */

#ifndef TDE_GLOBAL_CATALOG_H
#define TDE_GLOBAL_CATALOG_H

#include "catalog/pg_tablespace_d.h"

/*
 * We pick magical database oids from the tablespace oid which avoids
 * collissions with any real database oid.
 */
#define GLOBAL_DATA_TDE_OID		GLOBALTABLESPACE_OID
#define DEFAULT_DATA_TDE_OID	DEFAULTTABLESPACE_OID

/*
 * This oid can be anything since the database oid is gauranteed to not be a
 * real database.
 */
#define XLOG_TDE_OID 1

#define GLOBAL_SPACE_RLOCATOR(_obj_oid) (RelFileLocator) { \
	GLOBALTABLESPACE_OID, \
	GLOBAL_DATA_TDE_OID, \
	_obj_oid \
}

#endif							/* TDE_GLOBAL_CATALOG_H */
