-- test plperl.on_plperl_init errors are fatal

-- Avoid need for custom_variable_classes = 'plperl'
LOAD 'plperl';

SET SESSION plperl.on_plperl_init = ' eval "1+1" ';

SHOW plperl.on_plperl_init;

DO $$ warn 42 $$ language plperl;
