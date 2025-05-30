# Encrypted Data Scope

`pg_tde` encrypts the following components:

* **User data** in tables using the extension, including associated TOAST data. The table metadata (column names, data types, etc.) is not encrypted.
* **Temporary tables** created during the query execution, for data tables created using the extension.
* **Write-Ahead Log (WAL) data** for the entire database cluster. This includes WAL data in encrypted and non-encrypted tables.
* **Indexes** on encrypted tables.
* **Logical replication data** for encrypted tables (ensures encrypted content is preserved across replicas).

[Table Access Methods and TDE](table-access-method.md){.md-button}
