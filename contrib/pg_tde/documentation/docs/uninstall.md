# Uninstall `pg_tde`

If you no longer wish to use TDE in your deployment, you can remove the `pg_tde` extension. To do so, your user must have the superuser privileges, or a database owner privileges in case you only want to remove it from a single database.

Here's how to do it:

1. Drop the extension using the `DROP EXTENSION` command:

    ```
    DROP EXTENSION pg_tde;
    ```

    This command will fail if there are still encrypted tables in the database.    

    In this case, you must drop the dependent objects manually. Alternatively, you can run the `DROP EXTENSION ... CASCADE` command to drop all dependent objects automatically.     

    Note that the `DROP EXTENSION` command does not delete the `pg_tde` data files related to the database.

2. Run the `DROP EXTENSION` command against every database where you have enabled the `pg_tde` extension, if the goal is to completely remove the extension. This also includes the template databases, in case `pg_tde` was previously enabled there.

3. Remove any reference to `pg_tde` GUC variables from the PostgreSQL configuration file.

4. Modify the `shared_preload_libraries` and remove the 'pg_tde' from it. Use the `ALTER SYSTEM` command for this purpose, or edit the configuration file.

    !!! warning    

        Once `pg_tde` is removed from the `shared_preload_libraries`, reading any leftover encrypted files will fail. Removing the extension from the `shared_preload_libraries` is also possible if the extension is still installed in some databases.    

        Make sure to do this only if the server has no encrypted files in its data directory.

5. Start or restart the `postgresql` cluster to apply the changes.

    * On Debian and Ubuntu:    

       ```sh
       sudo systemctl restart postgresql
       ```
    
    * On RHEL and derivatives

       ```sh
       sudo systemctl restart postgresql-17
       ```
