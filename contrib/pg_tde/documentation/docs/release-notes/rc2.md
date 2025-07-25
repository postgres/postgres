# pg_tde Release Candidate 2 ({{date.RC2}})

`pg_tde` extension brings in [Transparent Data Encryption (TDE)](../index/about-tde.md) to PostgreSQL and enables you to keep sensitive data safe and secure.

[Get Started](../install.md){.md-button}

## Release Highlights

This release provides the following features and improvements:

* **Restricted key provider configuration to superusers**

    The database owners can no longer configure key providers directly. Instead, they must refer to the superuser who manages the provider setup. This security improvement clearly separates the responsibilities between users and administrators.

* **WAL encryption supports Vault**.

    `pg_tde` now supports using the Vault keyring for secure storage and management of WAL encryption keys.

* **Automatic WAL internal key generation at server startup**.

    On each server start, a new internal key is generated for encrypting subsequent WAL records (assuming WAL encryption is enabled). The existing WAL records and their keys remain unchanged, this ensures continuity and secure key management without affecting historical data.

* **Proper removal of relation-level encryption keys on table drop**

    Previously, encrypted relation keys persisted even after dropping the associated tables, potentially leaving orphaned entries in the map file. This is now corrected, when an encrypted table is dropped, its corresponding key is also removed from the key map.

* **Fixed external tablespace data loss with encrypted partitions**

    An issue was fixed where data could be lost when the encrypted partitioned tables were moved to external tablespaces.  

* **New visibility and verification functions for default principal keys**

    Added additional functions to help you verify and inspect the state of default principal keys more easily.

* **Fixed SQL failures caused by inconsistent key provider switching**

    An issue was resolved where SQL queries could fail after switching key providers while the server was running.
    This occurred because principal keys became inaccessible when spread across multiple keyring backends, triggering the single-provider-at-a-time design constraint.
    `pg_tde` now enforces consistency during provider changes to prevent a corrupted key state and query errors.

## Upgrade considerations

`pg_tde` Release Candidate 2 is not backward compatible with `pg_tde` Beta2 due to significant changes in code. This means you cannot directly upgrade from one version to another. You must [uninstall](../how-to/uninstall.md) `pg_tde` Beta2 first and then [install](../install.md) and configure the new Release Candidate version.

## Known issues

* The default `mlock` limit on Rocky Linux 8 for ARM64-based architectures equals the memory page size and is 64 Kb. This results in the child process with `pg_tde` failing to allocate another memory page because the max memory limit is reached by the parent process.

To prevent this, you can change the `mlock` limit to be at least twice bigger than the memory page size:

* temporarily for the current session using the `ulimit -l <value>` command.
* set a new hard limit in the `/etc/security/limits.conf` file. To do so, you require the superuser privileges.

Adjust the limits with caution since it affects other processes running in your system.

## Changelog

### New Features

* [PG-817](https://perconadev.atlassian.net/browse/PG-817) – Added fuzz testing to `pstress` to strengthen validation and resilience.
* [PG-824](https://perconadev.atlassian.net/browse/PG-824) – Ensured fsync is called on `pg_tde.map`, `pg_tde.dat`, and FS key provider files.
* [PG-830](https://perconadev.atlassian.net/browse/PG-830) – Implemented full WAL encryption using Vault keyring.
* [PG-831](https://perconadev.atlassian.net/browse/PG-831) – Tested WAL recovery and both streaming and logical replication compatibility.
* [PG-855](https://perconadev.atlassian.net/browse/PG-855) – Added a contributor guide to help new developers engage with pg_tde.
* [PG-938](https://perconadev.atlassian.net/browse/PG-938) – Evaluated use of `pg_basebackup` for automated backup validation with pg_tde.
* [PG-962](https://perconadev.atlassian.net/browse/PG-962) – Automated test cases to validate data integrity after PostgreSQL restart.
* [PG-1001](https://perconadev.atlassian.net/browse/PG-1001) – Verified encryption behavior of temporary tables.
* [PG-1099](https://perconadev.atlassian.net/browse/PG-1099) – Developed automation for bare-metal performance benchmarking.
* [PG-1289](https://perconadev.atlassian.net/browse/PG-1289) – Added test cases for verifying compatibility with different PostgreSQL versions.
* [PG-1444](https://perconadev.atlassian.net/browse/PG-1444) – Implemented support for removing relation-level encryption keys when dropping tables.
* [PG-1455](https://perconadev.atlassian.net/browse/PG-1455) – Introduced random base numbers in encryption IVs for enhanced security.
* [PG-1458](https://perconadev.atlassian.net/browse/PG-1458) – Added visibility and verification functions for default principal keys.
* [PG-1460](https://perconadev.atlassian.net/browse/PG-1460) – Enabled automatic rotation of WAL internal keys on server start.
* [PG-1461](https://perconadev.atlassian.net/browse/PG-1461) – Implemented random IV initialization for WAL keys.
* [PG-1506](https://perconadev.atlassian.net/browse/PG-1506) – Added parameter support for client certificates in KMIP provider configuration.

## Improvements

* [PG-826](https://perconadev.atlassian.net/browse/PG-826) – Documented how to encrypt and decrypt existing tables using pg_tde.
* [PG-827](https://perconadev.atlassian.net/browse/PG-827) – Fixed CI pipeline tests on the smgr branch.
* [PG-834](https://perconadev.atlassian.net/browse/PG-834) – Resolved issues with `CREATE ... USING pg_tde` on the smgr branch.
* [PG-1427](https://perconadev.atlassian.net/browse/PG-1427) – Tested and fixed KMIP implementation for Thales support.
* [PG-1507](https://perconadev.atlassian.net/browse/PG-1507) – Handled ALTER TYPE operations in the TDE event trigger.
* [PG-1508](https://perconadev.atlassian.net/browse/PG-1508) – Fixed encryption state inconsistencies when altering inherited tables.
* [PG-1550](https://perconadev.atlassian.net/browse/PG-1550) – Restricted database owners from creating key providers to improve security.
* [PG-1586](https://perconadev.atlassian.net/browse/PG-1586) – Verified and fixed KMIP compatibility with Fortanix HSM.

### Bugs Fixed

* [PG-1397](https://perconadev.atlassian.net/browse/PG-1397) – Fixed segmentation fault during replication with WAL encryption enabled.
* [PG-1413](https://perconadev.atlassian.net/browse/PG-1413) – Resolved invalid WAL magic number errors after toggling encryption.
* [PG-1416](https://perconadev.atlassian.net/browse/PG-1416) – Fixed SQL query failures caused by inconsistent key provider switching.
* [PG-1468](https://perconadev.atlassian.net/browse/PG-1468) – Fixed WAL read failures on replicas after key rotation.
* [PG-1491](https://perconadev.atlassian.net/browse/PG-1491) – Corrected `pg_tde_is_encrypted()` behavior for partitioned tables.
* [PG-1493](https://perconadev.atlassian.net/browse/PG-1493) – Fixed data loss when encrypted partitioned tables were moved to external tablespaces.
* [PG-1503](https://perconadev.atlassian.net/browse/PG-1503) – Blocked deletion of global key providers still associated with principal keys.
* [PG-1504](https://perconadev.atlassian.net/browse/PG-1504) – Ensured correct encryption inheritance in partitioned `tde_heap` tables.
* [PG-1510](https://perconadev.atlassian.net/browse/PG-1510) – Used different keys and IVs for PostgreSQL forks to prevent conflicts.
* [PG-1530](https://perconadev.atlassian.net/browse/PG-1530) – Fixed inability to read WAL after toggling WAL encryption.
* [PG-1532](https://perconadev.atlassian.net/browse/PG-1532) – Resolved errors rewriting owned sequences when pg_tde isn't in the default schema.
* [PG-1535](https://perconadev.atlassian.net/browse/PG-1535) – Prevented server crash on calling `pg_tde_principal_key_info()`.
* [PG-1537](https://perconadev.atlassian.net/browse/PG-1537) – Fixed crash on NULL input in user-facing functions.
* [PG-1539](https://perconadev.atlassian.net/browse/PG-1539) – Handled principal key header verification errors gracefully.
* [PG-1540](https://perconadev.atlassian.net/browse/PG-1540) – Ensured sequences are assigned correct encryption status.
* [PG-1541](https://perconadev.atlassian.net/browse/PG-1541) – Resolved WAL decryption failure after key rotation.
* [PG-1543](https://perconadev.atlassian.net/browse/PG-1543) – Fixed validation error when multiple server keys exist.
* [PG-1545](https://perconadev.atlassian.net/browse/PG-1545) – Resolved error from `pg_tde_grant_grant_management_to_role()` execution.
* [PG-1546](https://perconadev.atlassian.net/browse/PG-1546) – Fixed incorrect behavior in role grant function.
* [PG-1551](https://perconadev.atlassian.net/browse/PG-1551) – Improved handling of short reads and errors in WAL storage code.
* [PG-1571](https://perconadev.atlassian.net/browse/PG-1571) – Fixed WAL decryption failure due to corrupted or mismatched principal keys.
* [PG-1573](https://perconadev.atlassian.net/browse/PG-1573) – Prevented crash during WAL replay when lock was not held.
* [PG-1574](https://perconadev.atlassian.net/browse/PG-1574) – Ensured encrypted WAL is readable by streaming replica.
* [PG-1576](https://perconadev.atlassian.net/browse/PG-1576) – Resolved crash from malformed JSON in user-facing functions.
