DROP FUNCTION biteq(bits,bits);
DROP OPERATOR = (bits,bits);
DROP FUNCTION bitne(bits,bits);
DROP OPERATOR <> (bits,bits);
DROP FUNCTION bitlt(bits,bits);
DROP OPERATOR < (bits,bits);
DROP FUNCTION bitle(bits,bits);
DROP OPERATOR <= (bits,bits);
DROP FUNCTION bitgt(bits,bits);
DROP OPERATOR > (bits,bits);
DROP FUNCTION bitge(bits,bits);
DROP OPERATOR >= (bits,bits);
DROP FUNCTION bitcmp(bits,bits);
DROP OPERATOR <=> (bits,bits);

DROP FUNCTION bitor(bits,bits);
DROP OPERATOR | (bits,bits);
DROP FUNCTION bitand(bits,bits);
DROP OPERATOR & (bits,bits);
DROP FUNCTION bitxor(bits,bits);
DROP OPERATOR ^ (bits,bits);
DROP FUNCTION bitnot(bits);
DROP OPERATOR ~ (none,bits);

DROP FUNCTION bitshiftleft(bits,int4);
DROP OPERATOR << (bits,int4);
DROP FUNCTION bitshiftright(bits,int4);
DROP OPERATOR >> (bits,int4);

DROP FUNCTION bitsubstr(bits,integer,integer);
DROP OPERATOR || (bits,bits);
DROP FUNCTION bitcat(bits,bits);

DROP FUNCTION varbit_in(opaque);
DROP FUNCTION varbit_out(opaque);
DROP TYPE bits;
