# Backup with WAL encryption enabled

To create a backup with WAL encryption enabled:

1. Copy the `pg_tde` directory from the source server’s data directory, for example `/var/lib/postgresql/data/pg_tde/`, including the `wal_keys` and `1664_providers` files, to the backup destination directory where `pg_basebackup` will write the backup.

Also copy any external files referenced by your providers configuration (such as certificate or key files) into the same relative paths under the backup destination, so that they are located and validated by `pg_basebackup -E`.

2. Run:

    ```bash
    pg_basebackup -D /path/to/backup -E
    ```

    Where:

    - `-D /path/to/backup` specifies the backup location where you have to copy `pg_tde`.
    - `-E` (or `--encrypt-wal`) enables WAL encryption and validates that the copied `pg_tde` and provider files are present and that the server key is accessible (required).

!!! note
    - The `-E` flag only works with the `-X stream` option (default). It is not compatible with `-X none` or `-X fetch`. For more information, see [the other WAL methods topic](#other-wal-methods).
    - The `-E` flag is only supported with the plain output format (`-F p`). It cannot be used with the tar output format (`-F t`).

## Key rotation during backups

!!! warning
    Do not create, change, or rotate global key providers (or their keys) while `pg_basebackup` is running. Standbys or standalone clusters created from such backups may fail to start during WAL replay and may also lead to the corruption of encrypted data (tables, indexes, and other relations).

Creating, changing, or rotating global key providers (or their keys) during a base backup can leave the standby in an inconsistent state where it cannot retrieve the correct key history.

For example, you may see errors such as:

```sql
FATAL: failed to retrieve principal key "database_keyXXXX" from key provider "providerYYYY"
CONTEXT: WAL redo at ... ROTATE_PRINCIPAL_KEY ...
```

To ensure standby recoverability, plan key rotations outside backup windows or take a new full backup after rotation completes.

## Restore a backup created with WAL encryption

When you want to restore a backup created with `pg_basebackup -E`:

1. Ensure all external files referenced by your providers configuration (such as certificates or key files) are also present and accessible at the same relative paths.
2. Start PostgreSQL with the restored data directory.

## Backup method compatibility with WAL encryption

Tar format (`-F t`):

* Works with `-X fetch`.
* Does not support `-X stream` when WAL encryption is enabled. Using `pg_basebackup -F t -X stream` will create a broken replica.

Streaming mode (`-X stream`):

* **Must** specify `-E` (`--encrypt-wal`).
* Without `-E`, backups may contain decrypted WAL while `wal_encryption=on` remains in `postgresql.conf` and `pg_tde/wal_keys` are copied. This leads to **startup failures and compromised data in the backup**.

Fetch mode (`-X fetch`):

* Compatible with encrypted WAL without requiring any additional flags.

None (`-X none`):

* Excludes WAL from the backup and is unaffected by WAL encryption.


!!! note
    If the source server has `pg_tde/wal_keys`, running `pg_basebackup` with `-X none` or `-X fetch` produces warnings such as:

    ```sql
    pg_basebackup: warning: the source has WAL keys, but no WAL encryption configured for the target backups
    pg_basebackup: detail: This may lead to exposed data and broken backup
    pg_basebackup: hint: Run pg_basebackup with -E to encrypt streamed WAL
    ```

    You can safely ignore the warnings with `-X none` or `-X fetch`, since no WAL streaming occurs.
