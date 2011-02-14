/* contrib/spi/timetravel--unpackaged--1.0.sql */

ALTER EXTENSION timetravel ADD function timetravel();
ALTER EXTENSION timetravel ADD function set_timetravel(name,integer);
ALTER EXTENSION timetravel ADD function get_timetravel(name);
