# Features

`pg_tde` is available for [Percona Server for PostgreSQL](https://docs.percona.com/postgresql/17/)
The Percona Server for PostgreSQL provides an extended Storage Manager API that allows integration with custom storage managers.

The following features are available for the extension:

* [Table encryption](test.md#encrypt-data-in-a-new-table), including:
    * Data tables
    * Index data for encrypted tables
    * TOAST tables
    * Temporary tables

!!! note
    Metadata of those tables is not encrypted.

* Single-tenancy support via a [global keyring provider](global-key-provider-configuration/set-principal-key.md)
* [Multi-tenancy support](how-to/multi-tenant-setup.md)
* Table-level granularity for encryption and access control
* Multiple [Key management options](global-key-provider-configuration/index.md)

[What is Transparent Data Encryption (TDE)?](index/index.md){.md-button} [Install pg_tde to get started](install.md){.md-button}
