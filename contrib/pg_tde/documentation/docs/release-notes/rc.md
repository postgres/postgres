# pg_tde Release Candidate 1 ({{date.RC}})

`pg_tde` extension brings in [Transparent Data Encryption (TDE)](../index/index.md) to PostgreSQL and enables you to keep sensitive data safe and secure.

[Get started](../install.md){.md-button}

## Release Highlights

This release provides the following features and improvements:

* **Improved performance with redesigned WAL encryption**.

    The approach to WAL encryption has been redesigned. Now, `pg_tde` encrypts entire WAL files starting from the first WAL write after the server was started with the encryption turned on. The information about what is encrypted is stored in the internal key metadata. This change improves WAL encryption flow with native replication and increases performance for large scale databases. 

* **Default encryption key for single-tenancy**.

    The new functionality allows you to set a default principal key for the entire database cluster. This key is used to encrypt all databases and tables that do not have a custom principal key set. This feature simplifies encryption configuration and management in single-tenant environments where each user has their own database instance.

* **Ability to change key provider configuration**

    You no longer need to configure a new key provider and set a new principal key if the provider's configuration changed. Now can change the key provider configuration both for the current database and the entire PostgreSQL cluster using [functions](../functions.md#key-provider-management). This enhancement lifts existing limitations and is a native and common way to operate in PostgreSQL.

* **Key management permissions**

    The new functions allow you to manage permissions for global and database key management separately. This feature provides more granular control over key management operations and allows you to delegate key management tasks to different roles.

* **Additional information about principal keys and providers**

    The new functions allow you to display additional information about principal keys and providers. This feature helps you to understand the current key configuration and troubleshoot issues related to key management.

* **`tde_heap_basic` access method deprecation**

    The `tde_heap_basic` access method has limitations in encryption capabilities and affects performance. Also, it poses a potential security risk when used in production environments due to indexes remaining unencrypted. Considering all the above, we decided to deprecate this access method and remove it in future releases. Use the `tde_heap` access method instead that is available with Percona Server for PostgreSQL 17 - a drop-in replacement for PostgreSQL Community.

## Upgrade considerations

`pg_tde` Release Candidate is not backward compatible with `pg_tde` Beta2 due to significant changes in code. This means you cannot directly upgrade from one version to another. You must [uninstall](../how-to/uninstall.md) `pg_tde` Beta2 first and then [install](../install.md) and configure the new Release Candidate version.

## Known issues

* The default `mlock` limit on Rocky Linux 8 for ARM64-based architectures equals the memory page size and is 64 Kb. This results in the child process with `pg_tde` failing to allocate another memory page because the max memory limit is reached by the parent process.

    To prevent this, you can change the `mlock` limit to be at least twice bigger than the memory page size:

    * temporarily for the current session using the `ulimit -l <value>` command.
    * set a new hard limit in the `/etc/security/limits.conf` file. To do so, you require the superuser privileges.

    Adjust the limits with caution since it affects other processes running in your system.

* You can now delete global key providers even when their associated principal key is still in use. This known issue will be fixed in the next release. For now, avoid deleting global key providers. 

## Changelog

### New Features

* [PG-1234](https://perconadev.atlassian.net/browse/PG-1234) - Added functions for separate global and database key management permissions.

* [PG-1255](https://perconadev.atlassian.net/browse/PG-1255) - Added functionality to delete key providers.

* [PG-1256](https://perconadev.atlassian.net/browse/PG-1256) - Added single-tenant support via the default principal key functionality.

* [PG-1258](https://perconadev.atlassian.net/browse/PG-1258) - Added functions to display additional information  about principal keys / providers.

* [PG-1294](https://perconadev.atlassian.net/browse/PG-1294) - Redesigned WAL encryption.

* [PG-1303](https://perconadev.atlassian.net/browse/PG-1303) - Deprecated tde_heap_basic access method.

## Improvements

* [PG-858](https://perconadev.atlassian.net/browse/PG-858) - Refactored internal/principal key LWLocks to make local databases inherit a global key provider.

* [PG-1243](https://perconadev.atlassian.net/browse/PG-1243) - Investigated performance issues at a specific threshold and large databases and updated documentation about handling hint bits.

* [PG-1310](https://perconadev.atlassian.net/browse/PG-1310) - Added access method enforcement via the GUC variable.

* [PG-1361](https://perconadev.atlassian.net/browse/PG-1361) - Fixed pg_tde relocatability.

* [PG-1380](https://perconadev.atlassian.net/browse/PG-1380) - Added support for `pg_tde_is_encrypted()` function on indexes and sequences.

### Bugs Fixed

* [PG-821](https://perconadev.atlassian.net/browse/PG-821) - Fixed the issue with `pg_basebackup` failing when configuring replication.

* [PG-847](https://perconadev.atlassian.net/browse/PG-847) - Fixed the issue with `pg_basebackup` and `pg_checksum` throwing an error on files created by `pg_tde` when the checksum is enabled on the database cluster.

* [PG-1004](https://perconadev.atlassian.net/browse/PG-1004) - Fixed the issue with `pg_checksums` utility failing during checksum verification on `pg_tde` tables. Now `pg_checksum` skips encrypted relations by looking if the relation has a custom storage manager (SMGR) key.

* [PG-1373](https://perconadev.atlassian.net/browse/PG-1373) - Fixed the issue with potential unterminated strings by using the `memcpy()` or `strlcpy()` instead of the `strncpy()` function.

* [PG-1378](https://perconadev.atlassian.net/browse/PG-1378) - Fixed the issue with toast tables created by the `ALTER TABLE` command not being encrypted.

* [PG-1379](https://perconadev.atlassian.net/browse/PG-1379) - Fixed sequence and alter table handling in the event trigger.

* [PG-1222](https://perconadev.atlassian.net/browse/PG-1222) - Fixed the bug with  confused relations with the same `RelFileNumber` in different databases.

* [PG-1400](https://perconadev.atlassian.net/browse/PG-1400) - Corrected the pg_tde_change_key_provider naming in help.

* [PG-1401](https://perconadev.atlassian.net/browse/PG-1401) - Fixed the issue with inheriting an encryption status during the ALTER TABLE SET access method command execution by basing a new encryption status only on the new encryption setting.

* [PG-1414](https://perconadev.atlassian.net/browse/PG-1414) - Fixed the error message wording when configuring WAL encryption by referencing to a correct function.

* [PG-1450](https://perconadev.atlassian.net/browse/PG-1450) - Fixed the `pg_tde_delete_key_provider()` function behavior when called multiple times by ignoring already deleted records.

* [PG-1451](https://perconadev.atlassian.net/browse/PG-1451) -Fixed the issue with the repeating error message about inability to retrieve a principal key even when a user creates non-encrypted tables by checking the current transaction ID in both the event trigger start function and during a file creation. If the transaction changed during the setup of the current event trigger data, the event trigger is reset.

* [PG-1473](https://perconadev.atlassian.net/browse/PG-1473) - Allowed only users with key viewer privileges to execute `pg_tde_verify_principal_key()` and `pg_tde_verify_global_principal_key()` functions.

* [PG-1474](https://perconadev.atlassian.net/browse/PG-1474) - Fixed the issue with the principal key reference corruption when reassigning it to a key provider with the same name by setting the key name in vault/kmip getters.

* [PG-1476](https://perconadev.atlassian.net/browse/PG-1476) - Fixed the issue with the server failing to start when WAL encryption is enabled by creating a new principal key for WAL in case only one default key exists in the database.

* [PG-1479](https://perconadev.atlassian.net/browse/PG-1479), [PG-1480](https://perconadev.atlassian.net/browse/PG-1480) - Fixed the issue with the lost access to data after the global key provider change and the server restart by fixing the incorrect parameter order in default key rotation.

* [PG-1489](https://perconadev.atlassian.net/browse/PG-1489) - Fixed the issue with replicating the keys and key provider configuration by creating the `pg_tde` directory on the replica server.
/browse/PG-1476) - Fixed the issue with the server failing to start when WAL encryption is enabled by creating a new principal key for WAL in case only one default key exists in the database.

* [PG-1479](https://perconadev.atlassian.net/browse/PG-1479), [PG-1480](https://perconadev.atlassian.net/browse/PG-1480) - Fixed the issue with the lost access to data after the global key provider change and the server restart by fixing the incorrect parameter order in default key rotation.

* [PG-1489](https://perconadev.atlassian.net/browse/PG-1489) - Fixed the issue with replicating the keys and key provider configuration by creating the `pg_tde` directory on the replica server.
