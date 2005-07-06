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




