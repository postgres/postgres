# pg_tde release notes

`pg_tde` extension brings in [Transparent Data Encryption (TDE)](tde.md) to PostgreSQL and enables you to keep sensitive data safe and secure.

[Get started](../install.md){.md-button}

## Beta (2024-06-30)

With this version, the access method for `pg_tde` extension is renamed `pg_tde_basic`. Use this access method name to create tables. Find guidelines in [Test TDE](../test.md) tutorial.

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
