--
-- This removes the type (and a test table)
-- It's used just for development
--

-- remove our test table
drop table a;

-- now drop any sql based functions associated with the lo type
drop function oid(lo);

-- now drop the type
drop type lo;

-- as the type is gone, remove the C based functions
drop function lo_in(opaque);
drop function lo_out(opaque);
drop function lo(oid);
drop function lo_manage();

-- the lo stuff is now removed from the system
