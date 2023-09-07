/*-------------------------------------------------------------------------
 *
 * pg_tde_tdemap.c
 *	  tde relation fork manager code
 *
 *
 * IDENTIFICATION
 *	  src/access/pg_tde_tdemap.c
 *
 *-------------------------------------------------------------------------
 */

#define TDE_FORK_DEBUG 1

#include "postgres.h"
#include "access/pg_tde_tdemap.h"
#include "transam/pg_tde_xact_handler.h"
#include "storage/fd.h"
#include "utils/wait_event.h"
#include "utils/memutils.h"

#include "access/pg_tde_tdemap.h"

#include <openssl/rand.h>
#include <openssl/err.h>

/*
 * Creates a relation fork file relfilenode.tde that contains the
 * encryption key for the relation.
 */
void
pg_tde_create_key_fork(const RelFileLocator *newrlocator, Relation rel)
{
	/* TODO: should be a user defined */ 
	static const char *MasterKeyName = "master-key";

	char		*rel_file_path;
	char		*key_file_path;
	File		file = -1;
	InternalKey int_key = {0};
	RelKeysData *data;
	size_t 		sz;

	/* We get a relation name for MAIN fork and manually append the
	 * .tde postfix to the file name
	 */
	rel_file_path = relpathperm(*newrlocator, MAIN_FORKNUM);
	key_file_path = psprintf("%s."TDE_FORK_EXT, rel_file_path);
	pfree(rel_file_path);

	file = PathNameOpenFile(key_file_path, O_RDWR | O_CREAT | PG_BINARY);
	if (file < 0)
		ereport(FATAL,
        		(errcode_for_file_access(),
        		errmsg("could not open tde key file \"%s\": %m",
				  		key_file_path)));


	if (!RAND_bytes(int_key.key, INTERNAL_KEY_LEN))
		ereport(FATAL,
				(errcode(ERRCODE_INTERNAL_ERROR),
				errmsg("could not generate internal key for relation \"%s\": %s",
                		RelationGetRelationName(rel), ERR_error_string(ERR_get_error(), NULL))));

#if TDE_FORK_DEBUG
	ereport(DEBUG2,
		(errmsg("internal_key: %s", tde_sprint_key(&int_key))));
#endif

	data = (RelKeysData *) palloc(SizeOfRelKeysData(2));

	strcpy(data->master_key_name, MasterKeyName);
	data->internal_key[0] = int_key;
	data->internal_keys_len = 1;

	sz = SizeOfRelKeysData(data->internal_keys_len);
	/* 
	 * TODO: internal key(s) should be encrypted
	 */
	if (FileWrite(file, data, sz, 0, WAIT_EVENT_DATA_FILE_WRITE) != sz)
    	ereport(FATAL,
				(errcode_for_file_access(),
                errmsg("could not write key data to file \"%s\": %m",
                		key_file_path)));

	pfree(key_file_path);
	pfree(data);
	FileClose(file);
}

/*
 * Reads tde keys for the relatoin from a fork file.
 */
RelKeysData *
pg_tde_get_keys_from_fork(const RelFileLocator *rlocator)
{
	char		*rel_file_path;
	char		*key_file_path;
	File		file = -1;
	Size		sz;
	int			nbytes;
	RelKeysData *keys;

	rel_file_path = relpathperm(*rlocator, MAIN_FORKNUM);
	key_file_path = psprintf("%s."TDE_FORK_EXT, rel_file_path);
	pfree(rel_file_path);

	file = PathNameOpenFile(key_file_path, O_RDONLY | PG_BINARY);
	if (file < 0)
		ereport(FATAL,
                (errcode_for_file_access(),
                errmsg("could not open tde key file \"%s\": %m",
						key_file_path)));

	
	sz = (Size) FileSize(file);
	keys = (RelKeysData *) MemoryContextAlloc(TopMemoryContext, sz);

	/* 
	 * TODO: internal key(s) should be encrypted
	 */
	nbytes = FileRead(file, keys, sz, 0, WAIT_EVENT_DATA_FILE_READ);
	if (nbytes < 0)
		ereport(FATAL,
				(errcode_for_file_access(),
				errmsg("could not read key data file \"%s\": %m",
						key_file_path)));
	else if (nbytes < SizeOfRelKeysData(1) || 
				(nbytes - SizeOfRelKeysDataHeader) % sizeof(InternalKey) != 0)
		ereport(FATAL,
				(errcode(ERRCODE_DATA_CORRUPTED),
				errmsg("corrupted key data in file \"%s\"",
						key_file_path)));

#if TDE_FORK_DEBUG
	for (Size i = 0; i < keys->internal_keys_len; i++) 
		ereport(DEBUG2,
			(errmsg("fork file keys: [%lu] %s: %s", i+1, keys->master_key_name, tde_sprint_key(&keys->internal_key[i]))));
#endif

	/* Register the file for delete in case transaction Aborts */
	RegisterFileForDeletion(key_file_path, false);

	pfree(key_file_path);
	/* For now just close the key file.*/
	FileClose(file);

	return keys;
}

/* Head of the keys cache (linked list) */
RelKeys *tde_rel_keys_map = NULL;

/*
 * Returns TDE keys for a given relation.
 * First it looks in a cache. If nothing found in the cache, it reads data from
 * the tde fork file and populates cache.
 */
RelKeysData *
GetRelationKeys(Relation rel)
{
	RelKeys		*curr;
	RelKeys		*prev = NULL;
	RelKeys		*new;
	RelKeysData *keys;

	Oid rel_id = RelationGetRelid(rel);
	for (curr = tde_rel_keys_map; curr != NULL; curr = curr->next)
	{
		if (curr->rel_id == rel_id) {
#if TDE_FORK_DEBUG
			ereport(DEBUG2,
					(errmsg("TDE: cache hit, \"%s\" %s |  rel %s (%d)",
							curr->keys->master_key_name,
							tde_sprint_key(&curr->keys->internal_key[0]), 
							RelationGetRelationName(rel), 
							rel_id)));
#endif
			return curr->keys;
		}
		prev = curr;
	}

	keys = pg_tde_get_keys_from_fork(&rel->rd_locator);
	new = (RelKeys *) MemoryContextAlloc(TopMemoryContext, sizeof(RelKeys));
	new->rel_id = rel_id;
	new->keys = keys;
	new->next = NULL; 

	if (prev == NULL)
		tde_rel_keys_map = new;
	else
		prev->next = new;

	return keys;
}

const char *
tde_sprint_key(InternalKey *k)
{
	static char buf[256];
	int 	i;

	for (i = 0; i < sizeof(k->key); i++)
		sprintf(buf+i, "%02X", k->key[i]);

	sprintf(buf+i, "[%lu, %lu]", k->start_loc, k->end_loc);

	return buf;
}
