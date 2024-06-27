# Test Transparent Data Encryption

To check if the data is encrypted, do the following:

1. Create a table in the database for which you have [enabled `pg_tde`](setup.md). Enabling `pg_tde` extension creates the table access method `pg_tde`. To enable data encryption, create the table using this access method as follows:

    ```sql
    CREATE TABLE <table_name> (<field> <datatype>) USING pg_tde;
    ```

    !!! hint

        You can enable data encryption by default by setting the `default_table_access_method` to `pg_tde`:

        ```sql
        SET default_table_access_method = pg_tde;
        ```
    
2. Run the following function:

    ```sql
    SELECT pg_tde_is_encrypted('table_name');
    ```

    The function returns `t` if the table is encrypted and `f` - if not.

3. Rotate the principal key when needed:

    ```sql
    SELECT pg_tde_rotate_key(); -- uses automatic key versionin
    -- or
    SELECT pg_tde_rotate_key('new-principal-key', NULL); -- specify new key name
    -- or
    SELECT pg_tde_rotate_key('new-principal-key', 'new-provider'); -- change provider
    ```