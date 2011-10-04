-- test plperl.on_plperl_init errors are fatal

-- Must load plperl before we can set on_plperl_init
LOAD 'plperl';

SET SESSION plperl.on_plperl_init = ' system("/nonesuch") ';

SHOW plperl.on_plperl_init;

DO $$ warn 42 $$ language plperl;
