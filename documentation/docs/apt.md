# Install `pg_tde` on Debian or Ubuntu

In version Aplha1, `pg_tde` packages are available in the testing repository for Percona Distribution for PostgreSQL 16.2. Check the [list of supported platforms](install.md#__tabbed_1_2).

This tutorial shows how to install `pg_tde` with [Percona Distribution for PostgreSQL](https://docs.percona.com/postgresql/latest/index.html).

## Preconditions

You need the `percona-release` repository management tool that enables the desired Percona repository for you.

1. You need the following dependencies to install `percona-release`:
    
    - `wget`
    - `gnupg2`
    - `curl`
    - `lsb-release`
    
    Install them with the following command:
    
    ```bash
    sudo apt-get install -y wget gnupg2 curl lsb-release
    ```
    
2. Fetch the `percona-release` package

   ```bash
   sudo wget https://repo.percona.com/apt/percona-release_latest.generic_all.deb
   ```

3. Install `percona-release`

    ```bash
    sudo dpkg -i percona-release_latest.generic_all.deb
    ```

4. Enable the Percona Distribution for PostgreSQL repository

    ```bash
    sudo percona-release enable-only ppg-16.2 testing
    ```

5. Update the local cache

    ```bash
    sudo apt-get update
    ```

## Install Percona Distribution for PostgreSQL

To install Percona Distribution for PostgreSQL 16 and the required packages, run the following command:

```bash
sudo apt-get install -y percona-postgresql-16 percona-postgresql-contrib percona-postgresql-server-dev-all
```

## Install `pg_tde` packages

To install `pg_tde` packages, run the following command:

```bash
sudo apt-get install percona-postgresql-16-pg-tde
```

## Next step 

[Setup](setup.md){.md-button}