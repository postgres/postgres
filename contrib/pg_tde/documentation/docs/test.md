# Test Transparent Data Encryption

Enabling `pg_tde` extension for a database creates the table access method `tde_heap` . This access method enables you to encrypt the data.

Here's how to do it:

## Encrypt data in a new table

1. Create a table in the database for which you have [enabled `pg_tde`](setup.md) using the `tde_heap` access method as follows:

    ```
    CREATE TABLE <table_name> (<field> <datatype>) USING tde_heap;
    ```

    <i warning>:material-information: Warning:</i> Example for testing purposes only:

    ```
    CREATE TABLE albums (
    album_id INTEGER GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    artist_id INTEGER,
    title TEXT NOT NULL,
    released DATE NOT NULL
    ) USING tde_heap;
    ```
    
    Learn more about table access methods and how you can enable data encryption by default in the [Table access methods](table-access-method.md) section.

2. To check if the data is encrypted, run the following function:

    ```
    SELECT pg_tde_is_encrypted('table_name');
    ```

    The function returns `t` if the table is encrypted and `f` - if not.

3. Rotate the principal key when needed:

    ```
    SELECT pg_tde_rotate_principal_key(); -- uses automatic key versionin
    -- or
    SELECT pg_tde_rotate_principal_key('new-principal-key', NULL); -- specify new key name
    -- or
    SELECT pg_tde_rotate_principal_key('new-principal-key', 'new-provider'); -- changeprovider
    ```

## Encrypt existing table

You can encrypt an existing table. It requires rewriting the table, so for large tables, it might take a considerable amount of time.

Run the following command:

```
ALTER TABLE table_name SET access method  tde_heap;
```

Note that the `ALTER TABLE SET` command drops hint bits and this may affect the performance. Running a plain `SELECT, count(*)`, or `VACUUM` commands on the entire table will check every tuple for visibility and set its hint bits. Therefore, after executing the ALTER command, run a simple "count(*)" on your tables:

```
SELECT COUNT(*) FROM table_name;
```

!!! hint

    If you no longer wish to use `pg_tde` or wish to switch to using the `tde_heap_basic` access method, see how you can [decrypt your data](decrypt.md).
