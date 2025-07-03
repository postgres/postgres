# Features

`pg_tde` is available for [Percona Server for PostgreSQL](https://docs.percona.com/postgresql/17/)
The Percona Server for PostgreSQL provides an extended Storage Manager API that allows integration with custom storage managers.

The following features are available for the extension:

* Table encryption, including:
    * Data tables
    * Index data for encrypted tables
    * TOAST tables
    * Temporary tables

!!! note
    Metadata of those tables is not encrypted.

* Single-tenancy support via a global keyring provider
* Multi-tenancy support
* Table-level granularity for encryption and access control
* Multiple Key management options

[Overview](index/index.md){.md-button} [Get Started](install.md){.md-button}
