/* contrib/chkpass/chkpass--unpackaged--1.0.sql */

ALTER EXTENSION chkpass ADD type chkpass;
ALTER EXTENSION chkpass ADD function chkpass_in(cstring);
ALTER EXTENSION chkpass ADD function chkpass_out(chkpass);
ALTER EXTENSION chkpass ADD function raw(chkpass);
ALTER EXTENSION chkpass ADD function eq(chkpass,text);
ALTER EXTENSION chkpass ADD function ne(chkpass,text);
ALTER EXTENSION chkpass ADD operator <>(chkpass,text);
ALTER EXTENSION chkpass ADD operator =(chkpass,text);
