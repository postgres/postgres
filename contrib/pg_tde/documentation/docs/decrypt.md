# Decrypt an encrypted table

## Method 1. Change the access method

If you encrypted a table with the `tde_heap` access method and need to decrypt it, run the following command against the desired table (`mytable` in the example below):

```
ALTER TABLE mytable SET ACCESS METHOD heap;
```

Note that the `SET ACCESS METHOD` command drops hint bits and this may affect the performance. Running a plain `SELECT count(*)` or `VACUUM` commands on the entire table will check every tuple for visibility and set its hint bits. Therefore, after executing the `ALTER TABLE` command, run a simple `count(*)` on your tables:

```
SELECT count(*) FROM mytable;
```

Check that the table is not encrypted:

```
SELECT pg_tde_is_encrypted('mytable');
```

The output returns `f` meaning that the table is no longer encrypted. 



## Method 2. Create a new unencrypted table on the base of the encrypted one

Alternatively, you can create a new unencrypted table with the same structure and data as the initial table. For example, the original encrypted table is `EncryptedCustomers`. Use the following command to create a new table `Customers`: 

```
CREATE TABLE Customers AS
SELECT * FROM EncryptedCustomers;
```

The new table `Customers` inherits the structure and the data from `EncryptedCustomers`.

(Optional) If you no longer need the `EncryptedCustomers` table, you can delete it.

```
DROP TABLE EncryptedCustomers;
```
