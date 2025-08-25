# pg_tde_restore_encrypt

The `pg_tde_restore_encrypt` tool wraps a normal restore command from the WAL archive and writes them to disk in a format compatible with `pg_tde`.

!!! note

    This command is often use together with [pg_tde_archive_decrypt](./pg-tde-archive-decrypt.md).

## How it works

1. Replaces `%f` and `%p` in the restore command with the WAL file name and temporary file path (in `/dev/shm`)
2. Runs the restore command to fetch the unencrypted WAL from the archive and write it to the temp file
3. Encrypts the temp file and writes it to the destination path in PostgreSQL’s data directory

## Usage

```bash
pg_tde_restore_encrypt [OPTION]
pg_tde_restore_encrypt SOURCE-NAME DEST-PATH RESTORE-COMMAND
```

## Parameter descriptions

* `SOURCE-NAME`: name of the WAL file to retrieve from the archive
* `DEST-PATH`: path where the encrypted WAL file should be written
* `RESTORE-COMMAND`: restore command to wrap; `%p` and `%f` are replaced with the WAL file name and path to write the unencrypted WAL, respectively

## Options

* `-V, --version`: show version information, then exit
* `-?, --help`: show help information, then exit

!!! note

    Any `%f` or `%p` parameter in `RESTORE-COMMAND` has to be escaped as `%%f` or `%%p` respectively if used as `restore_command` in `postgresql.conf`.

## Examples

### Using `cp`

```ini
restore_command='pg_tde_restore_encrypt %f %p "cp /mnt/server/archivedir/%%f %%p"'
```

### Using `PgBackRest`

```ini
restore_command='pg_tde_restore_encrypt %f %p "pgbackrest --stanza=your_stanza archive-get %%f \"%%p\""'
```

!!! warning
    When using PgBackRest with WAL encryption, disable PostgreSQL data checksums. Otherwise, PgBackRest may spam error messages, and in some package builds the log statement can cause crashes.
