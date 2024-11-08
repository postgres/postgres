# Uninstall `pg_tde`

If you no longer wish to use TDE in your deployment, you can remove the `pg_tde` extension. To do that, your user must have the privileges of the superuser or a database owner.

Here's how to do it:

1. Drop the extension using the `DROP EXTENSION` with `CASCADE` command.

   <i warning>:material-alert: Warning:</i> The use of the CASCADE parameter deletes all tables that were created in the database with `pg_tde` enabled and also all dependencies upon the encrypted table (e.g. foreign keys in a non-encrypted table used in the encrypted one).

   ```sql
   DROP EXTENSION pg_tde CASCADE
   ```

2. Run the `DROP EXTENSION` command against every database where you have enabled the `pg_tde` extension

3. Modify the `shared_preload_libraries` and remove the 'pg_tde' from it. Use the `ALTER SYSTEM SET` command for this purpose

4. Start or restart the `postgresql` instance to apply the changes.

    * On Debian and Ubuntu:    

       ```sh
       sudo systemctl restart postgresql.service
       ```
    
    * On RHEL and derivatives

       ```sh
       sudo systemctl restart postgresql-17
       ```
