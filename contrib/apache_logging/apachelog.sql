drop table access;
CREATE TABLE access (host char(200), ident char(200), authuser char(200), accdate datetime, request char(500), ttime int2, status int2, bytes int4) archive = none;
grant all on access to nobody;
