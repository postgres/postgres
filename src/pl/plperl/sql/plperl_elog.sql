-- test warnings and errors from plperl

create or replace function perl_elog(text) returns void language plperl as $$

  my $msg = shift;
  elog(NOTICE,$msg);

$$;

select perl_elog('explicit elog');

create or replace function perl_warn(text) returns void language plperl as $$

  my $msg = shift;
  warn($msg);

$$;

select perl_warn('implicit elog via warn');

-- test strict mode on/off

SET plperl.use_strict = true;

create or replace function uses_global() returns text language plperl as $$

  $global = 1;
  $other_global = 2;
  return 'uses_global worked';

$$;

select uses_global();

SET plperl.use_strict = false;

create or replace function uses_global() returns text language plperl as $$

  $global = 1;
  $other_global=2;
  return 'uses_global worked';

$$;

select uses_global();
