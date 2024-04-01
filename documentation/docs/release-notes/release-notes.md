# pg_tde release notes

 ## Alpha 1 (2024-03-28)

`pg_tde` extension brings in [Transparent Data Encryption (TDE)](tde.md) to PostgreSQL and enables you to keep sensitive data safe and secure.

[Get started](../install.md){.md-button}

## Release Highlights

The technical preview of the extension introduces the following key features:

* You can now rotate master keys used for data encryption. This reduces the risk of long-term exposure to potential attacks and helps you comply with security standards such as GDPR, HIPAA, and PCI DSS.

* You can now configure encryption differently for each database. For example, encrypt specific tables in some databases with different encryption keys while keeping others non-encrypted.

* Keyring configuration has undergone several improvements, namely:

    * You can define separate keyring configuration for each database
    * You can change keyring configuration dynamically, without having to restart the server
    * The keyring configuration is now stored in a catalog separately for each database, instead of a configuration file
    * Avoid storing secrets in the unencrypted catalog by configuring keyring parameters to be read from external sources (file, http(s) request)

## Improvements 

* Renamed the repository and Docker image from `postgres-tde-ext` to `pg_tde`. The extension name remains unchanged
* Changed the Initialization Vector (IV) calculation of both the data and internal keys

## Bugs fixed

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