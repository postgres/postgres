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

#endif							/* TDE_GLOBAL_CATALOG_H */
