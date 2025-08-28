# Limitations of pg_tde

Limitations of `pg_tde` {{release}}:

* PostgreSQL’s internal system tables, which include statistics and metadata, are not encrypted.
* Temporary files created when queries exceed `work_mem` are not encrypted. These files may persist during long-running queries or after a server crash which can expose sensitive data in plaintext on disk.

## Currently unsupported WAL tools

The following tools are currently unsupported with `pg_tde` WAL encryption:

* `pg_createsubscriber`
* `pg_receivewal`
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
    scope: tde
    name: pg1
    restapi:
      listen: 0.0.0.0:8008
      connect_address: pg1:8008
    etcd3:
      host: etcd1:2379
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
            archive_command: "/lib/postgresql/17/bin/pg_tde_archive_decrypt %f %p \"pgbackrest --stanza=tde archive-push %%p\""
            archive_timeout: 600s
            archive_mode: "on"
            logging_collector: "on"
            restore_command: "/lib/postgresql/17/bin/pg_tde_restore_encrypt %f %p \"pgbackrest --stanza=tde archive-get %%f \\\"%%p\\\"\""
          pg_hba:
            - local all all peer
            - host all all 0.0.0.0/0 scram-sha-256
            - host all all ::/0 scram-sha-256
            - local replication all peer
            - host replication all 0.0.0.0/0 scram-sha-256
            - host replication all ::/0 scram-sha-256
      initdb:
        - encoding: UTF8
        - data-checksums
        - set: shared_preload_libraries=pg_tde
      post_init: /usr/local/bin/setup_cluster.sh
    postgresql:
      listen: 0.0.0.0:5432
      connect_address: pg1:5432
      data_dir: /var/lib/postgresql/patroni-17
      bin_dir: /lib/postgresql/17/bin
      pgpass: /var/lib/postgresql/patronipass
      authentication:
        replication:
          username: replicator
          password: rep-pass
        superuser:
          username: postgres
          password: secretpassword
      parameters:
        unix_socket_directories: /tmp
        # Use unix_socket_directories: /var/run/postgresql for Debian/Ubuntu distributions
    watchdog:
      mode: off
    tags:
      nofailover: false
      noloadbalance: false
      clonefrom: false
      nosync: false
    ```

!!! warning  
    The above example is Percona-tested, but Patroni versions differ, especially with discovery backends such as `etcd`. Ensure you adjust the configuration to match your environment, version, and security requirements.

## Next steps

Check which PostgreSQL versions and deployment types are compatible with `pg_tde` before planning your installation.

[View the versions and supported deployments :material-arrow-right:](supported-versions.md){.md-button}

Begin the installation process when you're ready to set up encryption.

[Start installing `pg_tde`](../install.md){.md-button}
