# Limitations of pg_tde

Limitations of `pg_tde` {{release}}:

* PostgreSQL’s internal system tables, which include statistics and metadata, are not encrypted.
* Temporary files created when queries exceed `work_mem` are not encrypted. These files may persist during long-running queries or after a server crash which can expose sensitive data in plaintext on disk.

## Currently unsupported WAL tools

The following tools are currently unsupported with `pg_tde` WAL encryption:

* `pg_createsubscriber`
* `pg_verifybackup` (checksum mismatch with encrypted WAL)

The following tools and extensions in Percona Distribution for PostgreSQL have been tested and verified to work with `pg_tde` WAL encryption:

## Supported WAL tools

The following tools have been tested and verified by Percona to work with `pg_tde` WAL encryption:

* Patroni, for an example configuration see the following [Patroni configuration file](#example-patroni-configuration)
* `pg_basebackup` (with `--wal-method=stream` or `--wal-method=none`), for details on using `pg_basebackup` with WAL encryption, see [Backup with WAL encryption enabled](../how-to/backup-wal-enabled.md)
* `pg_resetwal`
* `pg_rewind`
* `pg_upgrade`
* `pg_waldump`
* pgBackRest

## Example Patroni configuration

The following is a Percona-tested example configuration.

??? example "Click to expand the Percona-tested Patroni configuration"
    ```yaml
    # Example Patroni configuration file maintained by Percona
    # Source: https://github.com/jobinau/pgscripts/blob/main/patroni/patroni.yml
    scope: postgres
    namespace: /db/
    name: postgresql0

    restapi:
      listen: 0.0.0.0:8008
      connect_address: 127.0.0.1:8008

    etcd:
      host: 127.0.0.1:2379

    bootstrap:
      dcs:
        ttl: 30
        loop_wait: 10
        retry_timeout: 10
        maximum_lag_on_failover: 1048576
        postgresql:
          use_pg_rewind: true
          use_slots: true
          parameters:
            max_connections: 100
            shared_buffers: 1GB
            wal_level: replica
            hot_standby: "on"
            wal_keep_size: 256MB
            max_wal_senders: 10
            max_replication_slots: 10

      initdb:
      - encoding: UTF8
      - data-checksums

      pg_hba:
      - host replication replicator 127.0.0.1/32 md5
      - host all all 0.0.0.0/0 md5

    postgresql:
      listen: 0.0.0.0:5432
      connect_address: 127.0.0.1:5432
      data_dir: /var/lib/postgresql/data
      bin_dir: /usr/lib/postgresql/14/bin
      authentication:
        replication:
          username: replicator
          password: rep-pass
        superuser:
          username: postgres
          password: secretpassword
    ```

!!! warning  
    The above example is Percona-tested, but Patroni versions differ, especially with discovery backends such as `etcd`. Ensure you adjust the configuration to match your environment, version, and security requirements.

## Next steps

Check which PostgreSQL versions and deployment types are compatible with `pg_tde` before planning your installation.

[View the versions and supported deployments :material-arrow-right:](supported-versions.md){.md-button}

Begin the installation process when you're ready to set up encryption.

[Start installing `pg_tde`](../install.md){.md-button}
