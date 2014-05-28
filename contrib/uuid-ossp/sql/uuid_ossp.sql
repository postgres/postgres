CREATE EXTENSION "uuid-ossp";

SELECT uuid_nil();
SELECT uuid_ns_dns();
SELECT uuid_ns_url();
SELECT uuid_ns_oid();
SELECT uuid_ns_x500();

-- some quick and dirty field extraction functions

-- this is actually timestamp concatenated with clock sequence, per RFC 4122
CREATE FUNCTION uuid_timestamp_bits(uuid) RETURNS varbit AS
$$ SELECT ('x' || substr($1::text, 15, 4) || substr($1::text, 10, 4) ||
           substr($1::text, 1, 8) || substr($1::text, 20, 4))::bit(80)
          & x'0FFFFFFFFFFFFFFF3FFF' $$
LANGUAGE SQL STRICT IMMUTABLE;

CREATE FUNCTION uuid_version_bits(uuid) RETURNS varbit AS
$$ SELECT ('x' || substr($1::text, 15, 2))::bit(8) & '11110000' $$
LANGUAGE SQL STRICT IMMUTABLE;

CREATE FUNCTION uuid_reserved_bits(uuid) RETURNS varbit AS
$$ SELECT ('x' || substr($1::text, 20, 2))::bit(8) & '11000000' $$
LANGUAGE SQL STRICT IMMUTABLE;

CREATE FUNCTION uuid_multicast_bits(uuid) RETURNS varbit AS
$$ SELECT ('x' || substr($1::text, 25, 2))::bit(8) & '00000011' $$
LANGUAGE SQL STRICT IMMUTABLE;

CREATE FUNCTION uuid_node(uuid) RETURNS text AS
$$ SELECT substr($1::text, 25) $$
LANGUAGE SQL STRICT IMMUTABLE;

SELECT uuid_version_bits(uuid_generate_v1()),
       uuid_reserved_bits(uuid_generate_v1()),
       uuid_multicast_bits(uuid_generate_v1());

SELECT uuid_version_bits(uuid_generate_v1mc()),
       uuid_reserved_bits(uuid_generate_v1mc()),
       uuid_multicast_bits(uuid_generate_v1mc());

-- timestamp+clock sequence should be monotonic increasing in v1
SELECT uuid_timestamp_bits(uuid_generate_v1()) < uuid_timestamp_bits(uuid_generate_v1());
SELECT uuid_timestamp_bits(uuid_generate_v1mc()) < uuid_timestamp_bits(uuid_generate_v1mc());

-- node should be stable in v1, but not v1mc
SELECT uuid_node(uuid_generate_v1()) = uuid_node(uuid_generate_v1());
SELECT uuid_node(uuid_generate_v1()) <> uuid_node(uuid_generate_v1mc());
SELECT uuid_node(uuid_generate_v1mc()) <> uuid_node(uuid_generate_v1mc());

SELECT uuid_generate_v3(uuid_ns_dns(), 'www.widgets.com');
SELECT uuid_generate_v5(uuid_ns_dns(), 'www.widgets.com');

SELECT uuid_version_bits(uuid_generate_v4()),
       uuid_reserved_bits(uuid_generate_v4());

SELECT uuid_generate_v4() <> uuid_generate_v4();
