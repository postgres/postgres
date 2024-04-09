# Install `pg_tde` on Red Hat Enterprise Linux and derivatives

In version Aplha1, `pg_tde` packages are available in the testing repository for Percona Distribution for PostgreSQL 16.2. Check the [list of supported platforms](install.md#__tabbed_1_2).

This tutorial shows how to install `pg_tde` with [Percona Distribution for PostgreSQL](https://docs.percona.com/postgresql/latest/index.html).

## Preconditions

### Enable / disable modules

=== "CentOS 7"

    Install the `epel-release` package:

    ```bash
    sudo yum -y install epel-release
    sudo yum repolist
    ```

=== "RHEL8/Oracle Linux 8/Rocky Linux 8"

    Disable the ``postgresql``  and ``llvm-toolset``modules:    

    ```bash
    sudo dnf module disable postgresql llvm-toolset
    ```

### Install `percona-release`

You need the `percona-release` repository management tool that enables the desired Percona repository for you.

1. Install `percona-release`:

    ```bash
    sudo yum -y install https://repo.percona.com/yum/percona-release-latest.noarch.rpm 
    ```

2. Enable the repository

    ```bash
    sudo percona-release enable-only ppg-16.2 testing
    ```

## Install Percona Distribution for PostgreSQL

To install Percona Distribution for PostgreSQL 16 and the required packages, run the following command:

```bash
sudo yum -y install percona-postgresql-client-common percona-postgresql-common percona-postgresql-server-dev-all percona-postgresql16 percona-postgresql16-contrib percona-postgresql16-devel percona-postgresql16-libs
```

## Install `pg_tde`

To install `pg_tde` packages, run the following command:

```bash
sudo yum install percona-pg_tde_16
```

## Next steps

[Setup](setup.md){.md-button}