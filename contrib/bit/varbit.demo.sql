create table bit_example (a bit, b bit);
copy bit_example from stdin;
X0F	X10
X1F	X11
X2F	X12
X3F	X13
X8F	X04
X000F	X0010
X0123	XFFFF
X2468	X2468
XFA50	X05AF
X12345	XFFF
\.

select a,b,a||b as "a||b", bitsubstr(a,4,4) as "sub(a,4,4)", 
	bitsubstr(b,2,4) as "sub(b,2,4)", 
	bitsubstr(b,5,5) as "sub(b,5,5)"
	from bit_example;
select a,b,~a as "~ a",~b as "~ b",a & b as "a & b", 
	a|b as "a | b", a^b as "a ^ b" from bit_example;
select a,b,a<b as "a<b",a<=b as "a<=b",a=b as "a=b",
        a>=b as "a>=b",a>b as "a>b",a<=>b as "a<=>b" from bit_example;
select a,a<<4 as "a<<4",b,b>>2 as "b>>2" from bit_example;
select a,b,a||b as "a||b", bitsubstr(a,4,4) as "sub(a,4,4)", 
	bitsubstr(b,2,4) as "sub(b,2,4)", 
	bitsubstr(b,5,5) as "sub(b,5,5)"
	from bit_example;

drop table bit_example;
