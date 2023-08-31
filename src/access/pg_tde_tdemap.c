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

#include "postgres.h"
#include "access/pg_tde_tdemap.h"
#include "transam/pg_tde_xact_handler.h"
#include "storage/fd.h"
#include "utils/wait_event.h"

/*
 * Creates a relation fork file relfilenode.tde that contains the
 * encryption key for the relation.
 */
void
pg_tde_create_key_fork(const RelFileLocator *newrlocator, Relation rel)
{
	char 	*rel_file_path;
	char 	*key_file_path;
	File	file = -1;
	char enc_key[256]; /* Dummy key */

	/* We get a relation name for MAIN fork and manually append the
	 * .tde postfix to the file name
	 */
	rel_file_path = relpathperm(*newrlocator, MAIN_FORKNUM);
	key_file_path = psprintf("%s.tde", rel_file_path);
	pfree(rel_file_path);

	file = PathNameOpenFile(key_file_path, O_RDWR | O_CREAT | PG_BINARY);
	if (file < 0)
	{
		ereport(FATAL,
                 (errcode_for_file_access(),
                  errmsg("could not open tde key file %s", key_file_path)));
	}
	/* TODO:
	 * For now just write a dummy data to the file. We will write the actual
	 * key later.
	 */
	snprintf(enc_key, sizeof(enc_key), "Percona TDE Dummy key for relation:%s", RelationGetRelationName(rel));
	if (FileWrite(file, enc_key, sizeof(enc_key),
                    0, WAIT_EVENT_DATA_FILE_WRITE) != sizeof(enc_key))
         ereport(FATAL, (errcode_for_file_access(),
                         errmsg("Could not write key data to file: %s",
                                key_file_path)));

	/* Register the file for delete in case transaction Aborts */
	RegisterFileForDeletion(key_file_path, false);

	pfree(key_file_path);
	/* For now just close the key file.*/
	FileClose(file);
}
