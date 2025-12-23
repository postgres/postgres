-- directory paths and dlsuffix are passed to us in environment variables
\getenv libdir PG_LIBDIR
\getenv dlsuffix PG_DLSUFFIX

\set regresslib :libdir '/regress' :dlsuffix

CREATE FUNCTION test_translation()
    RETURNS void
    AS :'regresslib'
    LANGUAGE C;

-- There's less standardization in locale name spellings than one could wish.
-- While some platforms insist on having a codeset name in lc_messages,
-- fortunately it seems that it need not match the actual database encoding.
-- However, if no es_ES locale is installed at all, this'll fail.
SET lc_messages = 'C';

do $$
declare locale text; ok bool;
begin
  for locale in values('es_ES'), ('es_ES.UTF-8'), ('es_ES.utf8')
  loop
    ok = true;
    begin
      execute format('set lc_messages = %L', locale);
    exception when invalid_parameter_value then
      ok = false;
    end;
    exit when ok;
  end loop;
  -- Don't clutter the expected results with this info, just log it
  raise log 'NLS regression test: lc_messages = %',
    current_setting('lc_messages');
end $$;

SELECT test_translation();

RESET lc_messages;
