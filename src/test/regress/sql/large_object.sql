
-- This is more-or-less DROP IF EXISTS LARGE OBJECT 3001;
WITH unlink AS (SELECT lo_unlink(loid) FROM pg_largeobject WHERE loid = 3001) SELECT 1;

-- Test creation of a large object and leave it for testing pg_upgrade
SELECT lo_create(3001);

COMMENT ON LARGE OBJECT 3001 IS 'testing comments';
