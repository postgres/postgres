# `pg_tde` documentation

`pg_tde` is the extension that brings in [Transparent Data Encryption (TDE)](tde.md) to PostgreSQL and enables users to keep sensitive data safe and secure. 

!!! important 

    This is the MVP version of the extension and is not meant for production use yet.

[What's new](release-notes/tech-preview.md){.md-button}

## What's encrypted

`pg_tde` encrypts the following:

* User data in tables, including TOAST tables, that are created using the extension. Metadata of those tables is not encrypted. 
* Write-Ahead Log (WAL) data for tables created using the extension 
* Temporary tables created during the database operation for data tables created using the extension

## Known limitations

* Logical replication is not available as it doesn't work with encrypted tables.
* Keys in the local keyfile are stored unencrypted.
* Indexes and `NULL` bitmaps of tuples are currently not encrypted. 

<i warning>:material-alert: Warning:</i> Note that introducing encryption/decryption affects performance. Our benchmark tests show less than 10% performance overhead.

[Get started](install.md){.md-button}

## Supported PostgreSQL versions

`pg_tde` is currently based on PostgreSQL 16.0 and supported for Percona Distribution for PostgreSQL 16.x and upstream PostgreSQL 16.x. 

## Future releases

The following is planned for future releases of `pg_tde`:

* Encryption of indexes and `NULL` bitmaps of tuples
* Master key rotation
* Multi-tenancy support
* Logical replication support



## Useful links

* [What is Transparent Data Encryption](tde.md)

