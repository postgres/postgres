# Install `pg_tde` on Debian or Ubuntu

The packages for the tech preview `pg_tde` are available in the experimental repository for Percona Distribution for PostgreSQL 17. 

Check the [list of supported platforms](install.md#__tabbed_1_2).

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

    Percona provides [two repositories](repo-overview.md) for Percona Distribution for PostgreSQL. We recommend enabling the Major release repository to timely receive the latest updates. Since the `tde_heap` access method is still in the experimental stage, the `pg_tde` package is currently available from the experimental repository.

    ```{.bash data-prompt="$"}
    $ sudo percona-release enable ppg-{{pgversion17}} experimental
    ```

6. Update the local cache

    ```bash
    sudo apt-get update
    ```

## Install `pg_tde`

After all [preconditions](#preconditions) are met, install the extension.

1. Install Percona Distribution for PostgreSQL. 
    
    Run the following command to install Percona Distribution for PostgreSQL and the required packages:

    ```bash
    sudo apt-get install -y percona-postgresql-17 percona-postgresql-contrib percona-postgresql-server-dev-all
    ```

2. Install `pg_tde` packages
        
    ```bash
    sudo apt-get install percona-postgresql-17-pg-tde
    ```


## Next step 

[Setup](setup.md){.md-button}
