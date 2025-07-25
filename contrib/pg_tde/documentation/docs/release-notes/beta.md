# pg_tde Beta (2024-06-30)

`pg_tde` extension brings in [Transparent Data Encryption (TDE)](../index/about-tde.md) to PostgreSQL and enables you to keep sensitive data safe and secure.

[Get started](../install.md){.md-button}

!!! important

    This version of Percona Transparent Data Encryption extension **is 
    not recommended for production environments yet**. We encourage you to test it and [give your feedback](https://forums.percona.com/c/postgresql/pg-tde-transparent-data-encryption-tde/82).
  
    This will help us improve the product and make it production-ready faster.

## Release Highlights

Starting with `pg_tde` Beta, the access method for `pg_tde` extension is renamed `tde_heap_basic`. Use this access method name to create tables. Find guidelines in [Test TDE](../test.md) tutorial.

## Changelog

* Fixed the issue with `pg_tde` running out of memory used for decrypted tuples. The fix introduces the new component `TDEBufferHeapTupleTableSlot` that keeps track of the allocated memory for decrypted tuples and frees this memory when the tuple slot is no longer needed.

* Fixed the issue with adjusting a current position in a file by using raw file descriptor for the `lseek` function. (Thanks to user _rainhard_ for providing the fix)

* Enhanced the init script to consider a custom superuser for the POSTGRES_USER parameter when `pg_tde` is running via Docker (Thanks to _Alejandro Paredero_ for reporting the issue)
