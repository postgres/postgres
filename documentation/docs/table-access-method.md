# Table access method

A table access method is the way how PostgreSQL stores the data in a table. The default table access method is `heap`. PostgreSQL organizes data in a heap structure, meaning there is no particular order to the rows in the table. Each row is stored independently and identified by its unique row identifier (TID).

## How the `heap` access method works

**Insertion**: When a new row is inserted, PostgreSQL finds a free space in the tablespace and stores the row there.

**Deletion**: When a row is deleted, PostgreSQL marks the space occupied by the row as free, but the data remains until it is overwritten by a new insertion.

**Updates**: PostgreSQL handles updates by deleting the old row and inserting a new row with the updated values.

## Custom access method

Custom access methods allow you to implement and define your own way of organizing data in PostgreSQL. This is useful if the default table access method doesn't meet your needs.

Custom access methods are typically available with PostgreSQL extensions. When you install an extension and enable it in PostgreSQL, a custom access method is created.

An example of such an approach is the `tde_heap` access method. It is automatically created **only** for the databases where you [enabled the `pg_tde` extension](setup.md) and configured the key provider, enabling you to encrypt the data.

To use a custom access method, specify the `USING` clause for the `CREATE TABLE` command:

```sql
CREATE TABLE table_name (
    column1 data_type,
    column2 data_type,
    ...
) USING tde_heap;
```

### How `tde_heap` works

The `tde_heap` access method works on top of the default `heap` access method and is a marker to point which tables require encryption. It uses the custom storage manager TDE SMGR, which becomes active only after you installed the `pg_tde` extension. 

When a table requires encryption, every data block is encrypted before it is written to disk and decrypted after reading before it is sent to the PostgreSQL core and then to the client. The encryption is done at the storage manager level. 

## Changing the default table access method

You can change the default table access method so that every table in the entire database cluster is created using the custom access method. For example, you can enable data encryption by default by defining the `tde_heap` as the default table access method. 

However, consider the following before making this change:

* This is a global setting and applies across the entire database cluster and not just a single database. 
We recommend setting it with caution because all tables and materialized views created without an explicit access method in their `CREATE` statement will default to the specified table access method. 
* You must create the `pg_tde` extension and configure the key provider for all databases before you modify the configuration. Otherwise PostgreSQL won't find the specified access method and will throw an error.

Here's how you can set the new default table access method:

1. Add the access method to the `default_table_access_method` parameter.        

    === "via the SQL statement"

        Use the `ALTER SYSTEM SET` command. This requires superuser or ALTER SYSTEM privileges.

        This example shows how to set the `tde_heap` access method. Replace it with the `tde_heap_basic` if needed. 
    

        ```sql
        ALTER SYSTEM SET default_table_access_method=tde_heap;
        ```

    === "via the configuration file"

        Edit the `postgresql.conf` configuration file and add the value for the `default_table_access_method` parameter.
        
        This example shows how to set the `tde_heap` access method. Replace it with the `tde_heap_basic` if needed.

        ```ini
        default_table_access_method = 'tde_heap'
        ```  

    === "via the SET command"

        You can use the SET command to change the default table access method temporarily, for the current session. 
        
        Unlike modifying the `postgresql.conf` file or using the ALTER SYSTEM SET command, the changes you make via the SET command don't persist after the session ends.

        You also don't need to have the superuser privileges to run the SET command.

        You can run the SET command anytime during the session. This example shows how to set the `tde_heap` access method. Replace it with the `tde_heap_basic` if needed.

        ```sql
        SET default_table_access_method = tde_heap;
        ```

2. Reload the configuration to apply the changes:

    ```sql
    SELECT pg_reload_conf();
    ```

