# Uninstall pg_tde

If you no longer wish to use TDE in your deployment, you can remove the `pg_tde` extension. To do so, you must have superuser privileges, or database owner privileges in case you only want to remove it from a single database.

!!! warning
    This process removes the extension, but does not decrypt data automatically.  Only uninstall the extension after all encrypted data **has been removed or decrypted**.

To uninstall `pg_tde`, follow these steps:

1. Decrypt or drop encrypted tables:

    Before removing the extension, you must either **decrypt** or **drop** all encrypted tables:

    - To decrypt a table, run:

    ```sql
    ALTER TABLE <table name> SET ACCESS METHOD heap;
    ```

    - To discard data, drop the encrypted tables.

2. Drop the extension using the `DROP EXTENSION` command:

    ```sql
    DROP EXTENSION pg_tde;
    ```

    Alternatively, to remove everything at once:

    ```sql
    DROP EXTENSION pg_tde CASCADE;
    ```

    !!! note
        The `DROP EXTENSION` command does not delete the underlying `pg_tde`-specific data files from disk.

3. Run the `DROP EXTENSION` command against every database where you have enabled the `pg_tde` extension, if the goal is to completely remove the extension. This also includes the template databases, in case `pg_tde` was previously enabled there.

4. Remove any reference to `pg_tde` GUC variables from the PostgreSQL configuration file.

5. Modify the `shared_preload_libraries` and remove the 'pg_tde' from it. Use the `ALTER SYSTEM` command for this purpose, or edit the configuration file.

    !!! warning
        Once `pg_tde` is removed from the `shared_preload_libraries`, reading any leftover encrypted files will fail. Removing the extension from the `shared_preload_libraries` is also possible if the extension is still installed in some databases.
        Make sure to do this only if the server has no encrypted files in its data directory.

6. Start or restart the `postgresql` cluster to apply the changes.

    * On Debian and Ubuntu:

       ```sh
       sudo systemctl restart postgresql
       ```

    * On RHEL and derivatives

       ```sh
       sudo systemctl restart postgresql-17
       ```
