# Test Transparent Data Encryption

To check if the data is encrypted, do the following:

1. Create a table in the database for which you have [enabled `pg_tde`](setup.md)
2. Run the following function:

    ```sql
    select pgtde_is_encrypted('table_name');
    ```

    The function returns `t` if the table is encrypted and `f` - if not.