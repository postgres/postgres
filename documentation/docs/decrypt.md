# Decrypt an encrypted table

## Method 1. Change the access method

If you encrypted a table with the `tde_heap` or `tde_heap_basic` access method and need to decrypt it, run the following command against the desired table (`mytable` in the example below):

```sql
ALTER TABLE mytable SET access method heap;
```

Check that the table is not encrypted:

```sql
SELECT pg_tde_is_encrypted('mytable');
```

The output returns `f` meaning that the table is no longer encrypted. 

!!! note ""

    In the same way you can re-encrypt the data with the `tde_heap_basic` access method. 
    
    ```sql
    ALTER TABLE mytable SET access method tde_heap_basic;
    ```
    
    Note that the indexes and WAL files will no longer be encrypted.

## Method 2. Create a new unencrypted table on the base of the encrypted one

Alternatively, you can create a new unencrypted table with the same structure and data as the initial table. For example, the original encrypted table is `EncryptedCustomers`. Use the following command to create a new table `Customers`: 

```sql
CREATE TABLE Customers AS
SELECT * FROM EncryptedCustomers;
```

The new table `Customers` inherits the structure and the data from `EncryptedCustomers`.

(Optional) If you no longer need the `EncryptedCustomers` table, you can delete it.

```sql
DROP TABLE EncryptedCustomers;
```