# pg_checksums

[`pg_checksums` :octicons-link-external-16:](https://www.postgresql.org/docs/current/app-pgchecksums.html) is a PostgreSQL command-line utility used to enable, disable, or verify data checksums on a PostgreSQL data directory. However, it cannot calculate checksums for encrypted files.

Encrypted files are skipped, and this is reported in the output.
