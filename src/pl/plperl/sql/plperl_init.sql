-- test plperl.on_plperl_init errors are fatal

-- This test tests setting on_plperl_init after loading plperl
LOAD 'plperl';

SET SESSION plperl.on_plperl_init = ' system("/nonesuch"); ';

SHOW plperl.on_plperl_init;

DO $$ warn 42 $$ language plperl;
