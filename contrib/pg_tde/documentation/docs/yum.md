# Install pg_tde on Red Hat Enterprise Linux and Derivatives

This tutorial shows how to install `pg_tde` with [Percona Distribution for PostgreSQL](https://docs.percona.com/postgresql/latest/index.html).

Make sure you check the [list of supported platforms](install.md#__tabbed_1_1) before continuing.

## Install percona-release {.power-number}

You need the `percona-release` repository management tool that enables the desired Percona repository for you.

1. Install `percona-release`:

    ```{.bash data-prompt="$"}
        sudo yum -y install https://repo.percona.com/yum/percona-release-latest.noarch.rpm 
    ```

2. Enable the repository.

    ```{.bash data-prompt="$"}
        sudo percona-release enable-only ppg-17.5
    ```

## Install pg_tde {.power-number}

!!! important

    The `pg_tde` extension is a part of the `percona-postgresql17` package. If you installed a previous version of `pg_tde` from the `percona-pg_tde_17` package, do the following:

    1. Drop the extension using the `DROP EXTENSION` with `CASCADE` command.

       The use of the `CASCADE` parameter deletes all tables that were created in the database with `pg_tde` enabled and also all dependencies upon the encrypted table (e.g. foreign keys in a non-encrypted table used in the encrypted one).    

    ```sql
       DROP EXTENSION pg_tde CASCADE
    ```

    2. Uninstall the `percona-pg_tde_17` package.  
    
Run the following command to install `pg_tde`:

```{.bash data-prompt="$"}
    sudo yum -y install percona-postgresql17-server percona-postgresql17-contrib 
```

## Next steps

[Configure pg_tde :material-arrow-right:](setup.md){.md-button}
