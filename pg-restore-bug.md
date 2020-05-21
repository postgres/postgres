# there is a bug with pg restore on version 12.2, and likely other verisons.

I do this:

```bash

pg_dump --verbose -w --create \
   -U dev -d "$db_conn_str" > "$tmp_folder/dev_creatorpay_dump.sql"

echo 'the dump is done'

pg_restore --verbose --exit-on-error --create  -U postgres \
  -f "$tmp_folder/dev_creatorpay_dump.sql"

echo 'the restore is done'
```

and 

"the dumb is done" is logged

but nothing else is logged, even with the --verbose option passed to pg_restore, it's been hanging for 5 minutes etc.
