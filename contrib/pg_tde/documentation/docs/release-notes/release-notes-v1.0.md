# pg_tde 1.0 ({{date.GA10}})

The `pg_tde` by Percona extension brings in [Transparent Data Encryption (TDE)](../index/index.md) to PostgreSQL and enables you to keep sensitive data safe and secure.

[Get Started](../install.md){.md-button}

## Release Highlights

* **`pg_tde` 1.0 is now GA (Generally Available)**

And **stable** for encrypting relational data in PostgreSQL using [Transparent Data Encryption (TDE)](../index/index.md). This milestone brings production-level data protection to PostgreSQL workloads.

* **WAL encryption is still in Beta**

The WAL encryption feature is currently still in beta and is not effective unless explicitly enabled. **It is not yet production ready.** Do **not** enable this feature in production environments.

## Upgrade considerations

`pg_tde` {{tdeversion}} is **not** backward compatible with previous `pg_tde` versions, like Release Candidate 2, due to significant changes in code. This means you **cannot** directly upgrade from one version to another. You must do **a clean installation** of `pg_tde`.

## Known issues

* The default `mlock` limit on Rocky Linux 8 for ARM64-based architectures equals the memory page size and is 64 Kb. This results in the child process with `pg_tde` failing to allocate another memory page because the max memory limit is reached by the parent process.

To prevent this, you can change the `mlock` limit to be at least twice bigger than the memory page size:

* temporarily for the current session using the `ulimit -l <value>` command.
* set a new hard limit in the `/etc/security/limits.conf` file. To do so, you require the superuser privileges.

Adjust the limits with caution since it affects other processes running in your system.

## Changelog

### New Features

- [PG-1257](https://perconadev.atlassian.net/browse/PG-1257) – Added SQL function to remove the current principal key  

### Improvements

- [PG-1617](https://perconadev.atlassian.net/browse/PG-1617) – Removed relation key cache
- [PG-1635](https://perconadev.atlassian.net/browse/PG-1635) – User-facing TDE functions now return void
- [PG-1605](https://perconadev.atlassian.net/browse/PG-1605) – Removed undeclared dependencies for `pg_tde_grant_database_key_management_to_role()`

### Bugs Fixed

- [PG-1581](https://perconadev.atlassian.net/browse/PG-1581) – Fixed PostgreSQL crashes on table access when KMIP key is unavailable after restart  
- [PG-1583](https://perconadev.atlassian.net/browse/PG-1583) – Fixed a crash when dropping the `pg_tde` extension with CASCADE after changing the key provider file  
- [PG-1585](https://perconadev.atlassian.net/browse/PG-1585) – Fixed the vault provider re-addition that failed after server restart with a new token  
- [PG-1592](https://perconadev.atlassian.net/browse/PG-1592) – Improve error logs when Server Key Info is requested without being created  
- [PG-1593](https://perconadev.atlassian.net/browse/PG-1593) – Fixed runtime failures when invalid Vault tokens are allowed during key provider creation
- [PG-1600](https://perconadev.atlassian.net/browse/PG-1600) – Fixed Postmaster error when dropping a table with an unavailable key provider  
- [PG-1606](https://perconadev.atlassian.net/browse/PG-1606) – Fixed missing superuser check in role grant function leads to misleading errors  
- [PG-1607](https://perconadev.atlassian.net/browse/PG-1607) – Improved CA parameter order and surrounding documentation for clearer interpretation
- [PG-1608](https://perconadev.atlassian.net/browse/PG-1608) – Updated and fixed global key configuration parameters in documentation  
- [PG-1613](https://perconadev.atlassian.net/browse/PG-1613) – Tested and improved the `pg_tde_change_key_provider` CLI utility
- [PG-1637](https://perconadev.atlassian.net/browse/PG-1637) – Fixed unused keys in key files which caused issues after OID wraparound  
- [PG-1651](https://perconadev.atlassian.net/browse/PG-1651) – Fixed the CLI tool when working with Vault key export/import  
- [PG-1652](https://perconadev.atlassian.net/browse/PG-1652) – Fixed when the server fails to find encryption keys after CLI-based provider change  
- [PG-1662](https://perconadev.atlassian.net/browse/PG-1662) – Fixed the creation of inconsistent encryption status when altering partitioned tables
- [PG-1663](https://perconadev.atlassian.net/browse/PG-1663) – Fixed the indexes on partitioned tables which were not encrypted
- [PG-1700](https://perconadev.atlassian.net/browse/PG-1700) – Fixed the error hint when the principal key is missing
