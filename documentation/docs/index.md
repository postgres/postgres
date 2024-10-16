# `pg_tde` documentation

`pg_tde` is the extension that brings in [Transparent Data Encryption (TDE)](tde.md) to PostgreSQL and enables users to keep sensitive data safe and secure. It enables users to configure encryption differently for each database, encrypting specific tables in some databases with different encryption keys, while keeping others non encrypted. 

!!! important 

    This is the {{release}} version of the extension and is not meant for production use yet. [Release notes](release-notes/release-notes.md)

## What's encrypted

`pg_tde` encrypts the following:

* User data in tables, including TOAST tables, that are created using the extension. Metadata of those tables is not encrypted. 
* Write-Ahead Log (WAL) data for tables created using the extension 
* Temporary tables created during the database operation for data tables created using the extension

## Known limitations

* Logical replication is not available as it doesn't work with encrypted tables.
* Keys in the local keyfile are stored unencrypted.
* Indexes and `NULL` bitmaps of tuples are currently not encrypted. 

<i warning>:material-alert: Warning:</i> Note that introducing encryption/decryption affects performance. Our benchmark tests show less than 10% performance overhead for most situations. However, in some specific applications such as those using JSONB operations, performance degradation might be higher.

[Get started](install.md){.md-button}

## Supported PostgreSQL versions

`pg_tde` is currently based on Percona Server for PostgreSQL 17.0 and upstream PostgreSQL 16.x. 

It is also supported for Percona Distribution for PostgreSQL 16.x and upstream PostgreSQL 16.x. 

## Future releases

The following is planned for future releases of `pg_tde`:

* Encryption of indexes and `NULL` bitmaps of tuples
* Logical replication support
* KMIP integration for key management
* Global principal key management



## Useful links

* [What is Transparent Data Encryption](tde.md)

