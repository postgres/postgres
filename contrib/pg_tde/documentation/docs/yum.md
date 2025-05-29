# Install pg_tde on Red Hat Enterprise Linux and Derivatives

This tutorial shows how to install `pg_tde` with [Percona Distribution for PostgreSQL](https://docs.percona.com/postgresql/latest/index.html).

Make sure you check the [list of supported platforms](install.md#__tabbed_1_1) before continuing.

## Memory limits for pg_tde keys

The `pg_tde` uses memory locks (mlocks) to keep internal encryption keys in RAM, both for WAL and for user data.  

A memory lock (`mlock`) is a system call to lock a specified memory range in RAM for a process. The maximum amount of memory that can be locked differs between systems. You can check the current setting with this command:

```bash
    ulimit -a 
```

Memory locking is done only in memory pages. This means that when a process uses `mlocks`, it locks the entire memory page.

A process can have child processes that share the `mlock` limits of their parent. In PostgreSQL, the parent process is the one that runs the server. And its child backend processes handle client connections to the server.

If the `mlock` limit is greater than the page size, a child process locks another page for its operation. However, when the `mlock` limit equals the page size, the child process cannot run because the max memory limit is already reached by the parent process that used it for reading WAL files. This results in `pg_tde` failing with the error.

To prevent this, you can change the `mlock` limit to be at least twice bigger than the memory page size:

* temporarily for the current session using the `ulimit -l <value>` command.
* set a new hard limit in the `/etc/security/limits.conf` file. To do so, you require the superuser privileges.

Adjust the limits with caution since it affects other processes running in your system.

## Install percona-release {.power-number}

You need the `percona-release` repository management tool that enables the desired Percona repository for you.

1. Install `percona-release`:

    ```{.bash data-prompt="$"}
        $ sudo yum -y install https://repo.percona.com/yum/percona-release-latest.noarch.rpm 
    ```

2. Enable the repository.

    ```{.bash data-prompt="$"}
        $ sudo percona-release enable-only ppg-{{pgversion17}} 
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
    $ sudo yum -y install percona-postgresql17 percona-postgresql17-contrib 
```

## Next steps

[Configure pg_tde :material-arrow-right:](setup.md){.md-button}
