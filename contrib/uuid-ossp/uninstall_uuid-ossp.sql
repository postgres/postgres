/* $PostgreSQL: pgsql/contrib/uuid-ossp/uninstall_uuid-ossp.sql,v 1.1 2007/04/21 17:26:17 petere Exp $ */

SET search_path = public;

DROP FUNCTION uuid_nil();
DROP FUNCTION uuid_ns_dns();
DROP FUNCTION uuid_ns_url();
DROP FUNCTION uuid_ns_oid();
DROP FUNCTION uuid_ns_x500();

DROP FUNCTION uuid_generate_v1();
DROP FUNCTION uuid_generate_v1mc();
DROP FUNCTION uuid_generate_v3(namespace uuid, name text);
DROP FUNCTION uuid_generate_v4();
DROP FUNCTION uuid_generate_v5(namespace uuid, name text);
