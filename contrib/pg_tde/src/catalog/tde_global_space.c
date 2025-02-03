/*-------------------------------------------------------------------------
 *
 * tde_global_space.c
 *	  Global catalog key management
 *
 *
 * IDENTIFICATION
 *	  src/catalog/tde_global_space.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#ifdef PERCONA_EXT

#include "utils/memutils.h"

#include "access/pg_tde_tdemap.h"
#include "catalog/tde_global_space.h"
#include "catalog/tde_keyring.h"
#include "common/pg_tde_utils.h"

#ifdef FRONTEND
#include "pg_tde_fe.h"
#endif

#include <unistd.h>
#include <openssl/rand.h>
#include <openssl/err.h>
#include <sys/time.h>

#define PRINCIPAL_KEY_DEFAULT_NAME	"tde-global-catalog-key"
#define KEYRING_DEFAULT_NAME "default_global_tablespace_keyring"
#define KEYRING_DEFAULT_FILE_NAME "pg_tde_default_keyring_CHANGE_AND_REMOVE_IT"


void
TDEInitGlobalKeys(const char *dir)
{
	RelKeyData *ikey;

	if (dir != NULL)
		pg_tde_set_data_dir(dir);

	ikey = pg_tde_get_key_from_file(&GLOBAL_SPACE_RLOCATOR(XLOG_TDE_OID), TDE_KEY_TYPE_GLOBAL, true);

	/*
	 * Internal Key should be in the TopMemmoryContext because of SSL
	 * contexts. This context is being initialized by OpenSSL with the pointer
	 * to the encryption context which is valid only for the current backend.
	 * So new backends have to inherit a cached key with NULL SSL connext and
	 * any changes to it have to remain local ot the backend. (see
	 * https://github.com/percona-Lab/pg_tde/pull/214#discussion_r1648998317)
	 */
	if (ikey != NULL)
	{
		pg_tde_put_key_into_cache(XLOG_TDE_OID, ikey);
	}

}

#endif							/* PERCONA_EXT */
