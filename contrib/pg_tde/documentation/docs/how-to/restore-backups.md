# Restoring encrypted backups

To restore an encrypted backup created with an earlier key, ensure the key is still available in your Key Management System (KMS). The backup remains **encrypted** on disk, `pg_tde` uses the correct key to transparently access the data.

## How pg_tde uses old keys to load backups

* **KMS**: stores the principal key(s) that were active when the backup was made.
* **Encrypted backup**: contains data encrypted with the key available at that time.
* **Current server**: `pg_tde` automatically loads the backup if the key is accessible through the KMS.

The table (internal) keys are stored within the backup. At runtime, `pg_tde` retrieves the principal key(s) from the configured KMS and uses them to decrypt the internal keys, enabling access to the encrypted table data.

!!! note
    If the required key(s) get deleted or are missing, the backup **cannot** be read.

    If the key(s) still exists but the KMS configuration has changed, you can use [`pg_tde_change_key_provider`](../command-line-tools/pg-tde-change-key-provider.md) to update the configuration.

    However, if the key(s) got deleted from all accessible KMS sources, the encrypted backup is **unrecoverable**.
