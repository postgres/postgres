# Validate Encryption with pg_tde

After enabling the `pg_tde` extension for a database, you can begin encrypting data using the `tde_heap` table access method.

## Encrypt data in a new table

1. Create a table in the database for which you have [enabled `pg_tde`](setup.md) using the `tde_heap` access method as follows:

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

    Learn more about table access methods and how you can enable data encryption by default in the [Table Access Methods and TDE](index/table-access-method.md) section.

2. To check if the data is encrypted, run the following function:

    ```sql
    SELECT pg_tde_is_encrypted('table_name');
    ```

    The function returns `true` or `false`.

3. (Optional) Rotate the principal key.

To re-encrypt the data using a new key, see [Principal key management](functions.md#principal-key-management).

## Encrypt existing table

You can encrypt an existing table. It requires rewriting the table, so for large tables, it might take a considerable amount of time.

Run the following command:

```sql
ALTER TABLE table_name SET ACCESS METHOD tde_heap;
```

!!! important
    Using `SET ACCESS METHOD` drops hint bits which can impact query performance. To restore performance, run:

    ```sql
    SELECT count(*) FROM table_name;
    ```

    This forces PostgreSQL to check every tuple for visibility and reset the hint bits.

!!! hint
    Want to remove encryption later? See how to [decrypt your data](how-to/decrypt.md).

## Next steps

[Configure WAL encryption :material-arrow-right:](wal-encryption.md){.md-button}
