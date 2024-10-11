# Test Transparent Data Encryption

To check if the data is encrypted, do the following:

=== "pg_tde Beta"

    1. Create a table in the database for which you have [enabled `pg_tde`](setup.md). Enabling `pg_tde`    extension creates the table access method `tde_heap_basic`. To enable data encryption, create the table using this access method as follows:

        ```sql
        CREATE TABLE <table_name> (<field> <datatype>) USING tde_heap_basic;
        ```

        !!! hint

            You can enable data encryption by default by setting the `default_table_access_method` to     `tde_heap_basic`:

            ```sql
            ALTER SYSTEM  SET default_table_access_method=tde_heap;
            ```

            Reload the configuration to apply the changes:

            ```
            SELECT pg_reload_conf();
            ```
    
    2. Run the following function:

        ```sql
        SELECT pg_tde_is_encrypted('table_name');
        ```

        The function returns `t` if the table is encrypted and `f` - if not.

    3. Rotate the principal key when needed:

        ```sql
        SELECT pg_tde_rotate_principal_key(); -- uses automatic key versionin
        -- or
        SELECT pg_tde_rotate_principal_key('new-principal-key', NULL); -- specify new key name
        -- or
        SELECT pg_tde_rotate_principal_key('new-principal-key', 'new-provider'); -- change provider
        ```

=== "pg_tde Tech preview"

    !!! warning

        This is the tech preview functionality. Its scope is not yet finalized and can change anytime.** Use it only for testing purposes.**

    1. Create a table in the database for which you have [enabled `pg_tde`](setup.md). Enabling `pg_tde`    extension creates the table access method `tde_heap`. To enable data encryption, create the table using this access method as follows:

        ```sql
        CREATE TABLE <table_name> (<field> <datatype>) USING tde_heap;
        ```

    2. Run the following function:

        ```sql
        SELECT pg_tde_is_encrypted('table_name');
        ```

        The function returns `t` if the table is encrypted and `f` - if not.

    3. Rotate the principal key when needed:

        ```sql
        SELECT pg_tde_rotate_principal_key(); -- uses automatic key versionin
        -- or
        SELECT pg_tde_rotate_principal_key('new-principal-key', NULL); -- specify new key name
        -- or
        SELECT pg_tde_rotate_principal_key('new-principal-key', 'new-provider'); -- change provider
        ```

!!! hint

    If you no longer wish to use `pg_tde` or wish to switch to using the `tde_heap_basic` access method, see how you can [decrypt your data](decrypt.md).