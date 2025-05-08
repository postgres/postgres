# pg_tde CLI Tools

The `pg_tde` extension introduces new command-line utilities and extends some existing PostgreSQL tools to support encrypted WAL and tables. These include:

* [pg_tde_change_key_provider](../command-line-tools/pg-tde-change-key-provider.md): change encryption key provider for a database
* [pg_waldump](../command-line-tools/pg-waldump.md): inspect and decrypt WAL files
* [pg_checksums](../command-line-tools/pg-tde-checksums.md): verify data checksums (non-encrypted files only)
