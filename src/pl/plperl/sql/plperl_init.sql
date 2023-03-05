-- test plperl.on_plperl_init

-- This test tests setting on_plperl_init after loading plperl
LOAD 'plperl';

SET SESSION plperl.on_plperl_init = ' system("/nonesuch"); ';

SHOW plperl.on_plperl_init;

DO $$ warn 42 $$ language plperl;

--
-- Reconnect (to unload plperl), then test setting on_plperl_init
-- as an unprivileged user
--

\c -

CREATE ROLE regress_plperl_user;

SET ROLE regress_plperl_user;

-- this succeeds, since the GUC isn't known yet
SET SESSION plperl.on_plperl_init = 'test';

RESET ROLE;

LOAD 'plperl';

SHOW plperl.on_plperl_init;

DO $$ warn 42 $$ language plperl;

-- now we won't be allowed to set it in the first place
SET ROLE regress_plperl_user;

SET SESSION plperl.on_plperl_init = 'test';

RESET ROLE;

DROP ROLE regress_plperl_user;
