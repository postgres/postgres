# Use external reference to parameters

To allow storing secrets or any other parameters in a more secure, external location, `pg_tde`
allows users to specify an external reference instead of hardcoded parameters.

In Alpha1 version, `pg_tde` supports the following external storage methods:

* `file`, which just stores the data in a simple file specified by a `path`. The file should be
readable to the postgres process.
* `remote`, which uses a HTTP request to retrieve the parameter from the specified `url`.

## Examples

To use the file provider with a file location specified by the `remote` method,
use the following command:

```sql
SELECT pg_tde_add_key_provider_file(
    'file-provider', 
    json_object( 'type' VALUE 'remote', 'url' VALUE 'http://localhost:8888/hello' )
    );"
```

Or to use the `file` method, use the following command:

```sql
SELECT pg_tde_add_key_provider_file(
    'file-provider', 
    json_object( 'type' VALUE 'remote', 'path' VALUE '/tmp/datafile-location' )
    );"
```

Any parameter specified to the `add_key_provider` function can be a `json_object` instead of the string, 
similar to the above examples.