# pg_tde 2.0 ({{date.GA20}})

The `pg_tde` by Percona extension brings [Transparent Data Encryption (TDE)](../index/about-tde.md) to PostgreSQL and enables you to keep sensitive data safe and secure.

[Get Started](../install.md){.md-button}

## Release Highlights

### WAL encryption is now generally available

The WAL (Write-Ahead Logging) encryption feature is now fully supported and production-ready, it adds secure write-ahead logging to `pg_tde`, expanding Percona's PostgreSQL encryption coverage by enabling secure, transparent encryption of write-ahead logs using the same key infrastructure as data encryption.

### WAL encryption upgrade limitation

Clusters that used WAL encryption in the beta release (`pg_tde` 1.0 or older) cannot be upgraded to `pg_tde` 2.0. The following error indicates that WAL encryption was enabled:

```sql
FATAL: principal key not configured
HINT: Use pg_tde_set_server_key_using_global_key_provider() to configure one.
```

Clusters that did not use WAL encryption in beta can be upgraded normally.

### Documentation updates

* Updated the [Limitations](../index/tde-limitations.md) topic, it now includes WAL encryption limitations and both supported and unsupported WAL tools
* [PG-1858 :octicons-link-external-16:](https://perconadev.atlassian.net/browse/PG-1858) - Added a new topic for [Backup with WAL encryption enabled](../how-to/backup-wal-enabled.md) that includes restoring a backup created with WAL encryption
* [PG-1832 :octicons-link-external-16:](https://perconadev.atlassian.net/browse/PG-1858) - Added documentation for using the `pg_tde_archive_decrypt` and `pg_tde_restore_encrypt` utilities. These tools are now covered in [CLI Tools](../command-line-tools/cli-tools.md) to guide users on how to archive and restore encrypted WAL segments securely
* [PG-1740 :octicons-link-external-16:](https://perconadev.atlassian.net/browse/PG-1740) - Updated documentation for [uninstalling `pg_tde`](../how-to/uninstall.md) with WAL encryption enabled and improved the uninstall instructions to cover cases where TDE is disabled while WAL encryption remains active

## Known issues

* Creating, changing, or rotating global key providers (or their keys) while `pg_basebackup` is running may cause standbys or standalone clusters initialized from the backup to fail during WAL replay and may also lead to the corruption of encrypted data (tables, indexes, and other relations).

    Avoid making these actions during backup windows. Run a new full backup after completing a rotation or provider update.

* Using `pg_basebackup` with `--wal-method=fetch` produces warnings.

    This behavior is expected and will be addressed in a future release.

* The default `mlock` limit on Rocky Linux 8 for ARM64-based architectures equals the memory page size and is 64 Kb. This results in the child process with `pg_tde` failing to allocate another memory page because the max memory limit is reached by the parent process.

    To prevent this, you can change the `mlock` limit to be at least twice bigger than the memory page size:

    * temporarily for the current session using the `ulimit -l <value>` command.
    * set a new hard limit in the `/etc/security/limits.conf` file. To do so, you require the superuser privileges.

    Adjust the limits with caution since it affects other processes running in your system.

## Changelog

### New Features

* [PG-1497 :octicons-link-external-16:](https://perconadev.atlassian.net/browse/PG-1497) WAL encryption is now generally available (GA)
* [PG-1037 :octicons-link-external-16:](https://perconadev.atlassian.net/browse/PG-1037) - Added support for `pg_rewind` with encrypted WAL
* [PG-1411 :octicons-link-external-16:](https://perconadev.atlassian.net/browse/PG-1497) - Added support for `pg_resetwal` with encrypted WAL
* [PG-1603 :octicons-link-external-16:](https://perconadev.atlassian.net/browse/PG-1603) - Added support for `pg_basebackup` with encrypted WAL
* [PG-1710 :octicons-link-external-16:](https://perconadev.atlassian.net/browse/PG-1710) - Added support for WAL archiving with encrypted WAL
* [PG-1711 :octicons-link-external-16:](https://perconadev.atlassian.net/browse/PG-1711) - Added support for incremental backups with encrypted WAL, compatibility has been verified with `pg_combinebackup` and the WAL summarizer tool.
* [PG-1712 :octicons-link-external-16:](https://perconadev.atlassian.net/browse/PG-1712) - Added support for `pg_createsubscriber` with encrypted WAL
* [PG-1833 :octicons-link-external-16:](https://perconadev.atlassian.net/browse/PG-1833) - Added verified support for using `pg_waldump` with encrypted WAL
* [PG-1834 :octicons-link-external-16:](https://perconadev.atlassian.net/browse/PG-1834) - Verified `pg_upgrade` with encryption

### Improvements

* [PG-1661 :octicons-link-external-16:](https://perconadev.atlassian.net/browse/PG-1661) - Added validation for key material received from providers
* [PG-1667 :octicons-link-external-16:](https://perconadev.atlassian.net/browse/PG-1667) - Validated Vault keyring engine type

### Bugs Fixed

* [PG-1391 :octicons-link-external-16:](https://perconadev.atlassian.net/browse/PG-1391) - Fixed unencrypted checkpoint segment on replica with encrypted key
* [PG-1412 :octicons-link-external-16:](https://perconadev.atlassian.net/browse/PG-1412) – Fixed an issue where `XLogFileCopy` failed with encrypted WAL during PITR and `pg_rewind`
* [PG-1452 :octicons-link-external-16:](https://perconadev.atlassian.net/browse/PG-1452) - Fixed an issue where `pg_tde_change_key_provider` did not work without the `-D` flag even if `PGDATA` was set
* [PG-1485 :octicons-link-external-16:](https://perconadev.atlassian.net/browse/PG-1485) - Fixed an issue where streaming replication failed with an invalid magic number in WAL when `wal_encryption` was enabled
* [PG-1604 :octicons-link-external-16:](https://perconadev.atlassian.net/browse/PG-1604) - Fixed a crash during standby promotion caused by an invalid magic number when replaying two-phase transactions from WAL
* [PG-1658 :octicons-link-external-16:](https://perconadev.atlassian.net/browse/PG-1658) - Fixed an issue where the global key provider could not be deleted after server restart
* [PG-1835 :octicons-link-external-16:](https://perconadev.atlassian.net/browse/PG-1835) - Fixed an issue where `pg_resetwal` corrupted encrypted WAL, causing PostgreSQL to fail at startup with an invalid checkpoint
* [PG-1842 :octicons-link-external-16:](https://perconadev.atlassian.net/browse/PG-1842) - Fixed a delay in replica startup with encrypted tables in streaming replication setups
* [PG-1843 :octicons-link-external-16:](https://perconadev.atlassian.net/browse/PG-1843) - Fixed performance issues when creating encrypted tables
* [PG-1863 :octicons-link-external-16:](https://perconadev.atlassian.net/browse/PG-1863) - Fixed an issue where unnecessary WAL was generated when creating temporary tables
* [PG-1866 :octicons-link-external-16:](https://perconadev.atlassian.net/browse/PG-1866) - Fixed an issue where automatic restart after crash sometimes failed with WAL encryption enabled
* [PG-1867 :octicons-link-external-16:](https://perconadev.atlassian.net/browse/PG-1867) - Fixed archive recovery with encrypted WAL
