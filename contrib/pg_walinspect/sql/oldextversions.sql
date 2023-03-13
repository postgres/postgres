-- test old extension version entry points

CREATE EXTENSION pg_walinspect WITH VERSION '1.0';

-- List what version 1.0 contains
\dx+ pg_walinspect

-- Move to new version 1.1
ALTER EXTENSION pg_walinspect UPDATE TO '1.1';

-- List what version 1.1 contains
\dx+ pg_walinspect

DROP EXTENSION pg_walinspect;
