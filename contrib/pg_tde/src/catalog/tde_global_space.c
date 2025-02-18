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

void
TDEInitGlobalKeys(const char *dir)
{
	RelKeyData *key;

	if (dir != NULL)
		pg_tde_set_data_dir(dir);

	key = pg_tde_get_key_from_file(&GLOBAL_SPACE_RLOCATOR(XLOG_TDE_OID), TDE_KEY_TYPE_GLOBAL, true);

	/*
	 * Internal Key should be in the TopMemmoryContext because of SSL
	 * contexts. This context is being initialized by OpenSSL with the pointer
	 * to the encryption context which is valid only for the current backend.
	 * So new backends have to inherit a cached key with NULL SSL connext and
	 * any changes to it have to remain local ot the backend. (see
	 * https://github.com/percona-Lab/pg_tde/pull/214#discussion_r1648998317)
	 */
	if (key != NULL)
	{
		pg_tde_put_key_into_cache(&GLOBAL_SPACE_RLOCATOR(XLOG_TDE_OID), &key->internal_key);
	}

}

#endif							/* PERCONA_EXT */
