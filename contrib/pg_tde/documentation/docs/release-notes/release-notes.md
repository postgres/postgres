# pg_tde release notes

`pg_tde` extension brings in [Transparent Data Encryption (TDE)](../tde.md) to PostgreSQL and enables you to keep sensitive data safe and secure.

[Get started](../install.md){.md-button}

## Beta 2 (2024-12-16)

With this release, `pg_tde` extension offers two database specific versions:

*  PostgreSQL Community version provides only the `tde_heap_basic` access method using which you can introduce table encryption and WAL encryption for data in the encrypted tables. Index data remains unencrypted.
* Version for Percona Server for PostgreSQL provides the `tde_heap`access method. using this method you can encrypt index data in encrypted tables thus increasing the safety of your sensitive data. For backward compatibility, the `tde_heap_basic` method is available in this version too. 

The Beta 2 version introduces the following features and improvements:

### New Features

* Added the `tde_heap` access method with which you can now enable index encryption for encrypted tables and global WAL data encryption. To use this access method, you must install Percona Server for PostgreSQL. Check the [installation guide](../install.md)
* Added event triggers to identify index creation operations on encrypted tables and store those in a custom storage. 
* Added support for secure transfer of keys using the [OASIS Key Management Interoperability Protocol (KMIP)](https://docs.oasis-open.org/kmip/kmip-spec/v2.0/os/kmip-spec-v2.0-os.html). The KMIP implementation was tested with the PyKMIP server and the HashiCorp Vault Enterprise KMIP Secrets Engine. 


### Improvements

* WAL encryption improvements:

   * Added a global key to encrypt WAL data in global space
   * Added WAL key management

* Keyring improvements:

    * Renamed functions to point their usage for principal key management
    * Improved keyring provider management across databases and the global space.
    * Keyring configuration now uses common JSON API. This simplifies code handling and enables frontend tools like `pg_waldump` to read the code thus improving debugging.

* The `pg_tde_is_encrypted` function now supports custom schemas in the format of `pg_tde_is_encrypted('schema.table');`
* Changed the location of internal TDE files: instead of the database directory, now all files are stored in ` $PGDATA/pg_tde`
* Improved error reporting when `pg_tde` is not added to the `shared_preload_libraries`
* Improved memory usage of `tde_heap_basic `during sequential reads
* Improved `tde_heap_basic` for select statements
* Added encryption support for (some) command line utilities

### Bugs fixed

* Fixed multiple bugs with `tde_heap_basic` and TOAST records
* Fixed various memory leaks

## Beta (2024-06-30)

With this version, the access method for `pg_tde` extension is renamed `tde_heap_basic`. Use this access method name to create tables. Find guidelines in [Test TDE](../test.md) tutorial.

The Beta version introduces the following bug fixes and improvements:

* Fixed the issue with `pg_tde` running out of memory used for decrypted tuples. The fix introduces the new component `TDEBufferHeapTupleTableSlot` that keeps track of the allocated memory for decrypted tuples and frees this memory when the tuple slot is no longer needed.

* Fixed the issue with adjusting a current position in a file by using raw file descriptor for the `lseek` function. (Thanks to user _rainhard_ for providing the fix)

* Enhanced the init script to consider a custom superuser for the POSTGRES_USER parameter when `pg_tde` is running via Docker (Thanks to _Alejandro Paredero_ for reporting the issue)



## Alpha 1 (2024-03-28)

### Release Highlights

The Alpha1 version of the extension introduces the following key features:

* You can now rotate principal keys used for data encryption. This reduces the risk of long-term exposure to potential attacks and helps you comply with security standards such as GDPR, HIPAA, and PCI DSS.

* You can now configure encryption differently for each database. For example, encrypt specific tables in some databases with different encryption keys while keeping others non-encrypted.

* Keyring configuration has undergone several improvements, namely:

    * You can define separate keyring configuration for each database
    * You can change keyring configuration dynamically, without having to restart the server
    * The keyring configuration is now stored in a catalog separately for each database, instead of a configuration file
    * Avoid storing secrets in the unencrypted catalog by configuring keyring parameters to be read from external sources (file, http(s) request)

### Improvements 

* Renamed the repository and Docker image from `postgres-tde-ext` to `pg_tde`. The extension name remains unchanged
* Changed the Initialization Vector (IV) calculation of both the data and internal keys

### Bugs fixed

* Fixed toast related crashes
* Fixed a crash with the DELETE statement 
* Fixed performance-related issues
* Fixed a bug where `pg_tde` sent many 404 requests to the Vault server
* Fixed сompatibility issues with old OpenSSL versions
* Fixed сompatibility with old Curl versions 

## MVP (2023-12-12)

The Minimum Viable Product (MVP) version introduces the following functionality:

* Encryption of heap tables, including TOAST
* Encryption keys are stored either in Hashicorp Vault server or in local keyring file (for development) 
* The key storage is configurable via separate JSON configuration files
* Replication support
