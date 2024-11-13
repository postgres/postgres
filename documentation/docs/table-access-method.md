# Table access method

A table access method is the way how PostgreSQL stores the data in a table. The default table access method is `heap`. PostgreSQL organizes data in a heap structure, meaning there is no particular order to the rows in the table. Each row is stored independently, and rows are identified by their unique row identifier (TID).

## How does the heap access method work?

**Insertion**: When a new row is inserted, PostgreSQL finds a free space in the tablespace and stores the row there.

**Deletion**: When a row is deleted, PostgreSQL marks the space occupied by the row as free, but the data remains until it is overwritten by a new insertion.

**Updates**: Updates are handled by deleting the old row and inserting a new row with the updated values

## Custom access method

You can create a custom table access method for each table and instruct PostgreSQL how to store the data for you. For example, you can tailor the table access method to better suit your specific workload or data access patterns.

To define an access method, use the `CREATE ACCESS METHOD` with the `TYPE` clause set to `table`:

```sql
CREATE ACCESS METHOD access_method_name TYPE table;
```

To use your access method, specify the `USING` clause for the `CREATE TABLE` command:

```sql
CREATE TABLE table_name (
    column1 data_type,
    column2 data_type,
    ...
) USING access_method_name;
```

## `tde_heap` access method

The `tde_heap` is a custom table access method that comes with the `pg_tde` extension to provide data encryption. It is automatically created **only** for the databases where you [enabled the `pg_tde` extension](setup.md) and configured the key provider.


## Changing the default table access method

You can change the default table access method so that every table in the entire database cluster is created using the custom access method. For example, you can enable data encryption by default by defining either `tde_heap_basic` or the  `tde_heap` as the default table access method. 

However, consider the following before doing so:

* This is a global setting and applies across the entire database cluster and not just a single database. We recommend setting it with caution only if you created the `pg_tde` extensions for all databases. Otherwise PostgreSQL throws an error.
* You must create the `pg_tde` extension and configure the key provider for all databases before you modify the configuration. Otherwise PostgreSQL won't find the specified access method and will throw an error.

Here's how you can set the new default table access method:

1. Add the access method to the `default_table_access_method` parameter.        

    === "via the SQL statement"

        Use the `ALTER SYSTEM SET` command. This requires superuser privileges.

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

