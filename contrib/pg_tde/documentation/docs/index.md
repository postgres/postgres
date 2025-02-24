# `pg_tde` documentation

`pg_tde` is the open source PostgreSQL extension that provides Transparent Data Encryption (TDE) to protect data at rest. This ensures that the data stored on disk is encrypted, and no one can read it without the proper encryption keys, even if they gain access to the physical storage media. 

Learn more [what is Transparent Data Encryption](tde.md#how-does-it-work) and [why you need it](tde.md#why-do-you-need-tde).

!!! important 

    This is the {{release}} version of the extension and it is not meant for production use yet. We encourage you to use it in testing environments and [provide your feedback](https://forums.percona.com/c/postgresql/pg-tde-transparent-data-encryption-tde/82). 
    
[Get started](install.md){.md-button}
[What's new in pg_tde {{release}}](release-notes/release-notes.md){.md-button}

## What's encrypted:

* User data in tables, including TOAST tables, that are created using the extension. Metadata of those tables is not encrypted. 
* Temporary tables created during the database operation for data tables created using the extension
* Write-Ahead Log (WAL) data for the entire database cluster. This includes WAL data in encrypted and non-encrypted tables
* Indexes on encrypted tables 
* Logical replication on encrypted tables

[Check the full feature list](features.md){.md-button}

## Known limitations

* Keys in the local keyfile are stored unencrypted. For better security we recommend using the Key management storage. 
* System tables are currently not encrypted. This means that statistics data and database metadata are currently not encrypted.
   
* `pg_rewind` doesn't work with encrypted WAL for now. We plan to fix it in future releases.


<i warning>:material-alert: Warning:</i> Note that introducing encryption/decryption affects performance. Our benchmark tests show less than 10% performance overhead for most situations. However, in some specific applications such as those using JSONB operations, performance degradation might be higher.

## Versions and supported PostgreSQL deployments

The `pg_tde` extension comes in two distinct versions with specific access methods to encrypt the data. These versions are database-specific and differ in terms of what they encrypt and with what access method. Each version is characterized by the database it supports, the access method it provides, and the scope of encryption it offers.

* **Version for Percona Server for PostgreSQL**

    This `pg_tde` version is based on and supported for [Percona Server for PostgreSQL 17.x :octicons-link-external-16:](https://docs.percona.com/postgresql/17/postgresql-server.html) - an open source binary drop-in replacement for PostgreSQL Community. It provides the `tde_heap` access method and offers [full encryption capabilities](features.md). 

* **Community version** (deprecated)

    This version is available with PostgreSQL Community 16 and 17, and Percona Distribution for PostgreSQL 16. It provides the `tde_heap_basic` access method, offering limited encryption features. The limitations are in encrypting WAL data only for tables created using the extension and no support of index encryption nor logical replication.

### Which version to choose?

Enjoy full encryption with the Percona Server for PostgreSQL version and the `tde_heap` access method. The Community version is deprecated and is planned to be removed in future releases. 

Still not sure? [Contact our experts](https://www.percona.com/about/contact) to find the best solution for you.

