--
-- Verify system catalog foreign key relationships
--
DO $doblock$
declare
  fk record;
  nkeys integer;
  cmd text;
  err record;
begin
  for fk in select * from pg_get_catalog_foreign_keys()
  loop
    raise notice 'checking % % => % %',
      fk.fktable, fk.fkcols, fk.pktable, fk.pkcols;
    nkeys := array_length(fk.fkcols, 1);
    cmd := 'SELECT ctid';
    for i in 1 .. nkeys loop
      cmd := cmd || ', ' || quote_ident(fk.fkcols[i]);
    end loop;
    if fk.is_array then
      cmd := cmd || ' FROM (SELECT ctid';
      for i in 1 .. nkeys-1 loop
        cmd := cmd || ', ' || quote_ident(fk.fkcols[i]);
      end loop;
      cmd := cmd || ', unnest(' || quote_ident(fk.fkcols[nkeys]);
      cmd := cmd || ') as ' || quote_ident(fk.fkcols[nkeys]);
      cmd := cmd || ' FROM ' || fk.fktable::text || ') fk WHERE ';
    else
      cmd := cmd || ' FROM ' || fk.fktable::text || ' fk WHERE ';
    end if;
    if fk.is_opt then
      for i in 1 .. nkeys loop
        cmd := cmd || quote_ident(fk.fkcols[i]) || ' != 0 AND ';
      end loop;
    end if;
    cmd := cmd || 'NOT EXISTS(SELECT 1 FROM ' || fk.pktable::text || ' pk WHERE ';
    for i in 1 .. nkeys loop
      if i > 1 then cmd := cmd || ' AND '; end if;
      cmd := cmd || 'pk.' || quote_ident(fk.pkcols[i]);
      cmd := cmd || ' = fk.' || quote_ident(fk.fkcols[i]);
    end loop;
    cmd := cmd || ')';
    -- raise notice 'cmd = %', cmd;
    for err in execute cmd loop
      raise warning 'FK VIOLATION IN %(%): %', fk.fktable, fk.fkcols, err;
    end loop;
  end loop;
end
$doblock$;
