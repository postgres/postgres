# Install `pg_tde` on Red Hat Enterprise Linux and derivatives

This tutorial shows how to install `pg_tde` with [Percona Distribution for PostgreSQL](https://docs.percona.com/postgresql/latest/index.html).

Check the [list of supported platforms](install.md#__tabbed_1_1).

## Install `percona-release`

You need the `percona-release` repository management tool that enables the desired Percona repository for you.

1. Install `percona-release`:

    ```bash
    sudo yum -y install https://repo.percona.com/yum/percona-release-latest.noarch.rpm 
    ```

2. Enable the repository.

    Percona provides [two repositories](repo-overview.md) for Percona Distribution for PostgreSQL. We recommend enabling the Major release repository to timely receive the latest updates.

    ```bash
    sudo percona-release enable-only ppg-{{pgversion17}} 
    ```

## Install `pg_tde`

!!! important

    The `pg_tde` {{release}} extension is a part of the `percona-postgresql17` package. If you installed a previous version of `pg_tde` from the `percona-pg_tde_17` package, do the following:

    1. Drop the extension using the `DROP EXTENSION` with `CASCADE` command.

       The use of the `CASCADE` parameter deletes all tables that were created in the database with `pg_tde` enabled and also all dependencies upon the encrypted table (e.g. foreign keys in a non-encrypted table used in the encrypted one).    

       ```sql
       DROP EXTENSION pg_tde CASCADE
       ```

    2. Uninstall the `percona-pg_tde_17` package.  
    

```bash
sudo yum -y install percona-postgresql17 
```

## Next steps

[Setup](setup.md){.md-button}
