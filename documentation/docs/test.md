# Test Transparent Data Encryption

To check if the data is encrypted, do the following:

=== "pg_tde Tech preview"

    !!! warning

        This is the tech preview functionality. Its scope is not yet finalized and can change anytime.** Use it only for testing purposes.**

To check if the data is encrypted, do the following:

1. Create a table in the database for which you have [enabled `pg_tde`](setup.md). Enabling `pg_tde`    extension creates the table access method `tde_heap`. To enable data encryption, create the table using this access method as follows:

    ```sql
    CREATE TABLE <table_name> (<field> <datatype>) USING tde_heap;
    ```

    <i warning>:material-information: Warning:</i> Example for testing purposes only:

    ```sql
    CREATE TABLE albums (
    album_id INTEGER GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    artist_id INTEGER,
    title TEXT NOT NULL,
    released DATE NOT NULL
    ) USING tde_heap;
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
    SELECT pg_tde_rotate_principal_key('new-principal-key', 'new-provider'); -- changeprovider
    ```

4. You can encrypt existing table. It requires rewriting the table, so for large tables, it might take a considerable amount of time.

    ```sql
    ALTER TABLE table_name SET access method  tde_heap;
    ```

!!! hint

    If you no longer wish to use `pg_tde` or wish to switch to using the `tde_heap_basic` access method, see how you can [decrypt your data](decrypt.md).
