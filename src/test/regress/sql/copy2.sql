CREATE TABLE x (
	a serial,
	b int,
	c text not null default 'stuff',
	d text not null,
	e text
);

CREATE FUNCTION fn_x_before () RETURNS OPAQUE AS '
  BEGIN
		NEW.e := ''before trigger fired''::text;
		return NEW;
	END;
' language 'plpgsql';

CREATE FUNCTION fn_x_after () RETURNS OPAQUE AS '
  BEGIN
		UPDATE x set e=''after trigger fired'' where c=''stuff'';
		return NULL;
	END;
' language 'plpgsql';

CREATE TRIGGER trg_x_after AFTER INSERT ON x
FOR EACH ROW EXECUTE PROCEDURE fn_x_after();

CREATE TRIGGER trg_x_before BEFORE INSERT ON x
FOR EACH ROW EXECUTE PROCEDURE fn_x_before();

COPY x (a,b,c,d,e) from stdin;
10000	21	31	41	51
\.

COPY x (b,d) from stdin;
1	test_1
\.

COPY x (b,d) from stdin;
2	test_2
3	test_3
4	test_4
5	test_5
\.

COPY x (a,b,c,d,e) from stdin;
10001	22	32	42	52
10002	23	33	43	53
10003	24	34	44	54
10004	25	35	45	55
10005	26	36	46	56
\.

COPY x TO stdout;
DROP TABLE x;
DROP SEQUENCE x_a_seq;
DROP FUNCTION fn_x_before();
DROP FUNCTION fn_x_after();
