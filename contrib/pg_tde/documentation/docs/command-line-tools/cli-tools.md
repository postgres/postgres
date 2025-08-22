# Overview of pg_tde CLI tools

The `pg_tde` extension introduces new command-line utilities and extends some existing PostgreSQL tools to support encrypted WAL and tables.

## New tools

These tools are introduced by `pg_tde` to support key rotation and WAL encryption workflows:

* [pg_tde_change_key_provider](./pg-tde-change-key-provider.md): change the encryption key provider for a database
* [pg_tde_archive_decrypt](./pg-tde-archive-decrypt.md): decrypts WAL before archiving
* [pg_tde_restore_encrypt](./pg-tde-restore-encrypt.md): a custom restore command for making sure the restored WAL is encrypted

## Extended tools

These existing PostgreSQL tools are enhanced to support `pg_tde`:

* [pg_checksums](./pg-tde-checksums.md): verify data checksums (non-encrypted files only)
* [pg_waldump](./pg-waldump.md): inspect and decrypt WAL files
