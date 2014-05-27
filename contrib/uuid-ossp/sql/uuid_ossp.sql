CREATE EXTENSION "uuid-ossp";

SELECT uuid_nil();
SELECT uuid_ns_dns();
SELECT uuid_ns_url();
SELECT uuid_ns_oid();
SELECT uuid_ns_x500();

SELECT uuid_generate_v1() < uuid_generate_v1();
SELECT uuid_generate_v1() < uuid_generate_v1mc();

SELECT substr(uuid_generate_v1()::text, 25) = substr(uuid_generate_v1()::text, 25);
SELECT substr(uuid_generate_v1()::text, 25) <> substr(uuid_generate_v1mc()::text, 25);
SELECT substr(uuid_generate_v1mc()::text, 25) <> substr(uuid_generate_v1mc()::text, 25);

SELECT ('x' || substr(uuid_generate_v1mc()::text, 25, 2))::bit(8) & '00000011';

SELECT uuid_generate_v3(uuid_ns_dns(), 'www.widgets.com');
SELECT uuid_generate_v5(uuid_ns_dns(), 'www.widgets.com');

SELECT uuid_generate_v4()::text ~ '^[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}$';
SELECT uuid_generate_v4() <> uuid_generate_v4();
