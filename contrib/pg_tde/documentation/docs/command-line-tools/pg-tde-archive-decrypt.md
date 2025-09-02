# pg_tde_archive_decrypt

The `pg_tde_archive_decrypt` tool wraps an archive command and decrypts WAL files before archiving. It allows external tools to access unencrypted WAL data, which is required because WAL encryption keys in the two-key hierarchy are host-specific and may not be available on the replay host.

!!! tip
    For more information on the encryption architecture and key hierarchy, see [Architecture](../architecture/architecture.md).

This tool is often used in conjunction with [pg_tde_restore_encrypt](./pg-tde-restore-encrypt.md) to support WAL archive.

## How it works

1. Decrypts the WAL segment to a temporary file on a RAM disk (`/dev/shm`)
2. Replaces `%p` and `%f` in the archive command with the path and name of the decrypted file
3. Executes the archive command

!!! note

    To ensure security, encrypt the files stored in your WAL archive using tools like `PgBackRest`.

## Usage

```bash
pg_tde_archive_decrypt [OPTION]
pg_tde_archive_decrypt DEST-NAME SOURCE-PATH ARCHIVE-COMMAND
```

## Parameter descriptions

* `DEST-NAME`: name of the WAL file to send to the archive
* `SOURCE-PATH`: path to the original encrypted WAL file
* `ARCHIVE-COMMAND`: archive command to wrap. `%p` and `%f` are replaced with the decrypted WAL file path and WAL file name, respectively.

## Options

* `-V, --version`: show version information, then exit
* `-?, --help`: show help information, then exit

!!! note

    Any `%f` or `%p` parameter in `ARCHIVE-COMMAND` has to be escaped as `%%f` or `%%p` respectively if used as `archive_command` in `postgresql.conf`.

## Examples

### Using `cp`

```ini
archive_command='pg_tde_archive_decrypt %f %p "cp %%p /mnt/server/archivedir/%%f"'
```

### Using `PgBackRest`

```ini
archive_command='pg_tde_archive_decrypt %f %p "pgbackrest --stanza=your_stanza archive-push %%p"'
```

!!! warning
    When using PgBackRest with WAL encryption, disable PostgreSQL data checksums. Otherwise, PgBackRest may spam error messages, and in some package builds the log statement can cause crashes.
