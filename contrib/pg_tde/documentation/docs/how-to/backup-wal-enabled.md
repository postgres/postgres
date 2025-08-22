# Backup with WAL encryption enabled

To create a backup with WAL encryption enabled:

1. Copy the `pg_tde` directory from the source server’s data directory, for example `/var/lib/postgresql/data/pg_tde/`, including the `wal_keys` and `1664_providers` files, to the backup destination directory where `pg_basebackup` will write the backup.

Also copy any external files referenced by your providers configuration (such as certificate or key files) into the same relative paths under the backup destination, so that they are located and validated by `pg_basebackup -E`.

2. Run:

    ```bash
    pg_basebackup -D /path/to/backup -E
    ```

    Where:

    - `-D /path/to/backup` specifies the backup location where you have to copy `pg_tde`
    - `-E` (or `--encrypt-wal`) enables WAL encryption and validates that the copied `pg_tde` and provider files are present and that the server key is accessible (required)

!!! note
    - The `-E` flag only works with the `-X stream` option (default). It is not compatible with `-X none` or `-X fetch`. For more information, see [the other WAL methods topic](#other-wal-methods).
    - The `-E` flag is only supported with the plain output format (`-F p`). It cannot be used with the tar output format (`-F t`).

## Restore a backup created with WAL encryption

When you want to restore a backup created with `pg_basebackup -E`:

1. Ensure all external files referenced by your providers configuration (such as certificates or key files) are also present and accessible at the same relative paths.
2. Start PostgreSQL with the restored data directory.

## Other WAL methods

The `-X fetch` option works with encrypted WAL without requiring any additional flags.  
The `-X none` option excludes WAL from the backup and is unaffected by WAL encryption.

If the source server has `pg_tde/wal_keys`, running `pg_basebackup` with `-X none` or `-X fetch` produces warnings such as:

```sql
pg_basebackup: warning: the source has WAL keys, but no WAL encryption configured for the target backups
pg_basebackup: detail: This may lead to exposed data and broken backup
pg_basebackup: hint: Run pg_basebackup with -E to encrypt streamed WAL
```

You can safely ignore these warnings when using `-X none` or `-X fetch`, since in both cases WAL is not streamed.
