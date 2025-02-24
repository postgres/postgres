# Features

We provide `pg_tde` in two versions for both PostgreSQL Community and [Percona Server for PostgreSQL](https://docs.percona.com/postgresql/17/). The difference between the versions is in the set of included features which in its turn depends on the Storage Manager API. While PostgreSQL Community uses the default Storage Manager API, Percona Server for PostgreSQL extends the Storage Manager API enabling to integrate custom storage managers.

The following table provides features available for each version:

| Percona Server for PostgreSQL version | PostgreSQL Community version (deprecated)  |
|-------------------------------|----------------------|
| Table encryption: <br> - data tables, <br> - **Index data for encrypted tables**, <br> - TOAST tables, <br> - temporary tables created during the database operation.<br><br> Metadata of those tables is not encrypted.  | Table encryption: <br> - data tables, <br> - TOAST tables <br> - temporary tables created during the database operation.<br><br> Metadata of those tables is not encrypted. |
| **Global** Write-Ahead Log (WAL) encryption: for data in encrypted and non-encrypted tables | Write-Ahead Log (WAL) encryption of data in encrypted tables |
| Single-tenancy support via global keyring provider |   | 
| Multi-tenancy support | Multi-tenancy support |
| Table-level granularity | Table-level granularity |
| Key management via: <br> - HashiCorp Vault; <br> - KMIP server; <br> - Local keyfile | Key management via: <br> - HashiCorp Vault; <br> - Local keyfile |
| Logical replication of encrypted tables | |




[Get started](install.md){.md-button}