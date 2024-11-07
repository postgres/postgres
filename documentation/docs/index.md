# `pg_tde` documentation

`pg_tde` is the extension that brings in [Transparent Data Encryption (TDE)](tde.md) to PostgreSQL and enables users to keep sensitive data safe and secure. The encryption is transparent for users allowing them to access and manipulate the data and not to worry about the encryption process.

Users can configure encryption differently for each database, encrypting specific tables in some databases with different encryption keys, while keeping others non encrypted. 

!!! important 

    This extension is in the experimental phase and is under active development. It is not meant for production use yet. 
    
[What's new ](release-notes/release-notes.md){.md-button}

## What's encrypted:

`pg_tde` encrypts the following:

* User data in tables, including TOAST tables, that are created using the extension. Metadata of those tables is not encrypted. 
* Temporary tables created during the database operation for data tables created using the extension
* Write-Ahead Log (WAL) data for the entire database cluster. This includes WAL data in encrypted and non-encrypted tables
* Indexes on encrypted tables 
* Logical replication on encrypted tables

## Known limitations

* Keys in the local keyfile are stored unencrypted.
* System tables are currently not encrypted.

<i warning>:material-alert: Warning:</i> Note that introducing encryption/decryption affects performance. Our benchmark tests show less than 10% performance overhead for most situations. However, in some specific applications such as those using JSONB operations, performance degradation might be higher.

## Versions and supported PostgreSQL deployments

The `pg_tde` extension comes in two distinct versions with specific access methods to encrypt the data. These versions are database-specific and differ in terms of what they encrypt and with what access method. Each version is characterized by the database it supports, the access method it provides, and the scope of encryption it offers.

* **Version for Percona Server for PostgreSQL**

    This `pg_tde` version is based on and supported for [Percona Server for PostgreSQL 17.x :octicons-link-external-16:](https://docs.percona.com/postgresql/17/postgresql-server.html) - an open source binary drop-in replacement for PostgreSQL Community. It provides the `tde_heap` access method and offers [full encryption capabilities](#whats-encrypted). 

* **Community version**

    This version is supported for PostgreSQL Community 16 and 17, and Percona Distribution for PostgreSQL 16. It provides the `tde_heap_basic` access method, offering limited encryption features. The limitations are in encrypting WAL data only for tables created using the extension and no support of index encryption nor logical replication.

### Which version to chose?

The answer is pretty straightforward: if you don't use indexes and don't need index encryption, use the community version and the `tde_heap_basic` access method. Check the [upstream documentation :octicons-link-external-16:](https://github.com/percona/pg_tde/blob/main/README.md) how to get started.

Otherwise, enjoy full encryption with the Percona Server for PostgreSQL version and the `tde_heap` access method. 

Still not sure? [Contact our experts](https://www.percona.com/about/contact) to find the best solution for you.

[Get started](install.md){.md-button}

## Future releases

The following is planned for future releases of `pg_tde`:

* KMIP integration for key management
* Global principal key management



## Useful links

* [What is Transparent Data Encryption](tde.md)

