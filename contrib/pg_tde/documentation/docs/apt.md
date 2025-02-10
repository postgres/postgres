# Install `pg_tde` on Debian or Ubuntu

This tutorial shows how to install `pg_tde` with [Percona Distribution for PostgreSQL :octicons-link-external-16:](https://docs.percona.com/postgresql/latest/index.html).

Check the [list of supported platforms](install.md#__tabbed_1_1).

## Preconditions

1. Debian and other systems that use the `apt` package manager include the upstream PostgreSQL server package (`postgresql-{{pgversion17}}`) by default. You need to uninstall this package before you install Percona Server for PostgreSQL and `pg_tde` to avoid conflicts.
2. You need the `percona-release` repository management tool that enables the desired Percona repository for you.


### Install `percona-release`

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

    ```{.bash data-prompt="$"}
    $ sudo percona-release enable ppg-{{pgversion17}} 
    ```

6. Update the local cache

    ```bash
    sudo apt-get update
    ```

## Install `pg_tde`

!!! important

    The `pg_tde` {{release}} extension is a part of the `percona-postgresql-17` package. If you installed a previous version of `pg_tde` from the `percona-postgresql-17-pg-tde` package, do the following:

    1. Drop the extension using the `DROP EXTENSION` with `CASCADE` command.

       The use of the `CASCADE` parameter deletes all tables that were created in the database with `pg_tde` enabled and also all dependencies upon the encrypted table (e.g. foreign keys in a non-encrypted table used in the encrypted one).    

       ```sql
       DROP EXTENSION pg_tde CASCADE
       ```

    2. Uninstall the `percona-postgresql-17-pg-tde` package.  

After all [preconditions](#preconditions) are met, run the following command to install `pg_tde`:


```bash
sudo apt-get install -y percona-postgresql-17 
```


## Next step 

[Setup](setup.md){.md-button}
