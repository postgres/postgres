--
-- PLPGSQL
--

create table Room (
    roomno	char(8),
    comment	text
);

create unique index Room_rno on Room using btree (roomno bpchar_ops);


create table WSlot (
    slotname	char(20),
    roomno	char(8),
    slotlink	char(20),
    backlink	char(20)
);

create unique index WSlot_name on WSlot using btree (slotname bpchar_ops);


create table PField (
    name	text,
    comment	text
);

create unique index PField_name on PField using btree (name text_ops);


create table PSlot (
    slotname	char(20),
    pfname	text,
    slotlink	char(20),
    backlink	char(20)
);

create unique index PSlot_name on PSlot using btree (slotname bpchar_ops);


create table PLine (
    slotname	char(20),
    phonenumber	char(20),
    comment	text,
    backlink	char(20)
);

create unique index PLine_name on PLine using btree (slotname bpchar_ops);


create table Hub (
    name	char(14),
    comment	text,
    nslots	integer
);

create unique index Hub_name on Hub using btree (name bpchar_ops);


create table HSlot (
    slotname	char(20),
    hubname	char(14),
    slotno	integer,
    slotlink	char(20)
);

create unique index HSlot_name on HSlot using btree (slotname bpchar_ops);
create index HSlot_hubname on HSlot using btree (hubname bpchar_ops);


create table System (
    name	text,
    comment	text
);

create unique index System_name on System using btree (name text_ops);


create table IFace (
    slotname	char(20),
    sysname	text,
    ifname	text,
    slotlink	char(20)
);

create unique index IFace_name on IFace using btree (slotname bpchar_ops);


create table PHone (
    slotname	char(20),
    comment	text,
    slotlink	char(20)
);

create unique index PHone_name on PHone using btree (slotname bpchar_ops);


-- ************************************************************
-- * 
-- * Trigger procedures and functions for the patchfield
-- * test of PL/pgSQL
-- * 
-- ************************************************************


-- ************************************************************
-- * AFTER UPDATE on Room
-- *	- If room no changes let wall slots follow
-- ************************************************************
create function tg_room_au() returns opaque as '
begin
    if new.roomno != old.roomno then
        update WSlot set roomno = new.roomno where roomno = old.roomno;
    end if;
    return new;
end;
' language 'plpgsql';

create trigger tg_room_au after update
    on Room for each row execute procedure tg_room_au();


-- ************************************************************
-- * AFTER DELETE on Room
-- *	- delete wall slots in this room
-- ************************************************************
create function tg_room_ad() returns opaque as '
begin
    delete from WSlot where roomno = old.roomno;
    return old;
end;
' language 'plpgsql';

create trigger tg_room_ad after delete
    on Room for each row execute procedure tg_room_ad();


-- ************************************************************
-- * BEFORE INSERT or UPDATE on WSlot
-- *	- Check that room exists
-- ************************************************************
create function tg_wslot_biu() returns opaque as '
begin
    if count(*) = 0 from Room where roomno = new.roomno then
        raise exception ''Room % does not exist'', new.roomno;
    end if;
    return new;
end;
' language 'plpgsql';

create trigger tg_wslot_biu before insert or update
    on WSlot for each row execute procedure tg_wslot_biu();


-- ************************************************************
-- * AFTER UPDATE on PField
-- *	- Let PSlots of this field follow
-- ************************************************************
create function tg_pfield_au() returns opaque as '
begin
    if new.name != old.name then
        update PSlot set pfname = new.name where pfname = old.name;
    end if;
    return new;
end;
' language 'plpgsql';

create trigger tg_pfield_au after update
    on PField for each row execute procedure tg_pfield_au();


-- ************************************************************
-- * AFTER DELETE on PField
-- *	- Remove all slots of this patchfield
-- ************************************************************
create function tg_pfield_ad() returns opaque as '
begin
    delete from PSlot where pfname = old.name;
    return old;
end;
' language 'plpgsql';

create trigger tg_pfield_ad after delete
    on PField for each row execute procedure tg_pfield_ad();


-- ************************************************************
-- * BEFORE INSERT or UPDATE on PSlot
-- *	- Ensure that our patchfield does exist
-- ************************************************************
create function tg_pslot_biu() returns opaque as '
declare
    pfrec	record;
    rename new to ps;
begin
    select into pfrec * from PField where name = ps.pfname;
    if not found then
        raise exception ''Patchfield "%" does not exist'', ps.pfname;
    end if;
    return ps;
end;
' language 'plpgsql';

create trigger tg_pslot_biu before insert or update
    on PSlot for each row execute procedure tg_pslot_biu();


-- ************************************************************
-- * AFTER UPDATE on System
-- *	- If system name changes let interfaces follow
-- ************************************************************
create function tg_system_au() returns opaque as '
begin
    if new.name != old.name then
        update IFace set sysname = new.name where sysname = old.name;
    end if;
    return new;
end;
' language 'plpgsql';

create trigger tg_system_au after update
    on System for each row execute procedure tg_system_au();


-- ************************************************************
-- * BEFORE INSERT or UPDATE on IFace
-- *	- set the slotname to IF.sysname.ifname
-- ************************************************************
create function tg_iface_biu() returns opaque as '
declare
    sname	text;
    sysrec	record;
begin
    select into sysrec * from system where name = new.sysname;
    if not found then
        raise exception ''system "%" does not exist'', new.sysname;
    end if;
    sname := ''IF.'' || new.sysname;
    sname := sname || ''.'';
    sname := sname || new.ifname;
    if length(sname) > 20 then
        raise exception ''IFace slotname "%" too long (20 char max)'', sname;
    end if;
    new.slotname := sname;
    return new;
end;
' language 'plpgsql';

create trigger tg_iface_biu before insert or update
    on IFace for each row execute procedure tg_iface_biu();


-- ************************************************************
-- * AFTER INSERT or UPDATE or DELETE on Hub
-- *	- insert/delete/rename slots as required
-- ************************************************************
create function tg_hub_a() returns opaque as '
declare
    hname	text;
    dummy	integer;
begin
    if tg_op = ''INSERT'' then
	dummy := tg_hub_adjustslots(new.name, 0, new.nslots);
	return new;
    end if;
    if tg_op = ''UPDATE'' then
	if new.name != old.name then
	    update HSlot set hubname = new.name where hubname = old.name;
	end if;
	dummy := tg_hub_adjustslots(new.name, old.nslots, new.nslots);
	return new;
    end if;
    if tg_op = ''DELETE'' then
	dummy := tg_hub_adjustslots(old.name, old.nslots, 0);
	return old;
    end if;
end;
' language 'plpgsql';

create trigger tg_hub_a after insert or update or delete
    on Hub for each row execute procedure tg_hub_a();


-- ************************************************************
-- * Support function to add/remove slots of Hub
-- ************************************************************
create function tg_hub_adjustslots(bpchar, integer, integer)
returns integer as '
declare
    hname	alias for $1;
    oldnslots	alias for $2;
    newnslots	alias for $3;
begin
    if newnslots = oldnslots then
        return 0;
    end if;
    if newnslots < oldnslots then
        delete from HSlot where hubname = hname and slotno > newnslots;
	return 0;
    end if;
    for i in oldnslots + 1 .. newnslots loop
        insert into HSlot (slotname, hubname, slotno, slotlink)
		values (''HS.dummy'', hname, i, '''');
    end loop;
    return 0;
end;
' language 'plpgsql';


-- ************************************************************
-- * BEFORE INSERT or UPDATE on HSlot
-- *	- prevent from manual manipulation
-- *	- set the slotname to HS.hubname.slotno
-- ************************************************************
create function tg_hslot_biu() returns opaque as '
declare
    sname	text;
    xname	HSlot.slotname%TYPE;
    hubrec	record;
begin
    select into hubrec * from Hub where name = new.hubname;
    if not found then
        raise exception ''no manual manipulation of HSlot'';
    end if;
    if new.slotno < 1 or new.slotno > hubrec.nslots then
        raise exception ''no manual manipulation of HSlot'';
    end if;
    if tg_op = ''UPDATE'' then
	if new.hubname != old.hubname then
	    if count(*) > 0 from Hub where name = old.hubname then
		raise exception ''no manual manipulation of HSlot'';
	    end if;
	end if;
    end if;
    sname := ''HS.'' || trim(new.hubname);
    sname := sname || ''.'';
    sname := sname || new.slotno::text;
    if length(sname) > 20 then
        raise exception ''HSlot slotname "%" too long (20 char max)'', sname;
    end if;
    new.slotname := sname;
    return new;
end;
' language 'plpgsql';

create trigger tg_hslot_biu before insert or update
    on HSlot for each row execute procedure tg_hslot_biu();


-- ************************************************************
-- * BEFORE DELETE on HSlot
-- *	- prevent from manual manipulation
-- ************************************************************
create function tg_hslot_bd() returns opaque as '
declare
    hubrec	record;
begin
    select into hubrec * from Hub where name = old.hubname;
    if not found then
        return old;
    end if;
    if old.slotno > hubrec.nslots then
        return old;
    end if;
    raise exception ''no manual manipulation of HSlot'';
end;
' language 'plpgsql';

create trigger tg_hslot_bd before delete
    on HSlot for each row execute procedure tg_hslot_bd();


-- ************************************************************
-- * BEFORE INSERT on all slots
-- *	- Check name prefix
-- ************************************************************
create function tg_chkslotname() returns opaque as '
begin
    if substr(new.slotname, 1, 2) != tg_argv[0] then
        raise exception ''slotname must begin with %'', tg_argv[0];
    end if;
    return new;
end;
' language 'plpgsql';

create trigger tg_chkslotname before insert
    on PSlot for each row execute procedure tg_chkslotname('PS');

create trigger tg_chkslotname before insert
    on WSlot for each row execute procedure tg_chkslotname('WS');

create trigger tg_chkslotname before insert
    on PLine for each row execute procedure tg_chkslotname('PL');

create trigger tg_chkslotname before insert
    on IFace for each row execute procedure tg_chkslotname('IF');

create trigger tg_chkslotname before insert
    on PHone for each row execute procedure tg_chkslotname('PH');


-- ************************************************************
-- * BEFORE INSERT or UPDATE on all slots with slotlink
-- *	- Set slotlink to empty string if NULL value given
-- ************************************************************
create function tg_chkslotlink() returns opaque as '
begin
    if new.slotlink isnull then
        new.slotlink := '''';
    end if;
    return new;
end;
' language 'plpgsql';

create trigger tg_chkslotlink before insert or update
    on PSlot for each row execute procedure tg_chkslotlink();

create trigger tg_chkslotlink before insert or update
    on WSlot for each row execute procedure tg_chkslotlink();

create trigger tg_chkslotlink before insert or update
    on IFace for each row execute procedure tg_chkslotlink();

create trigger tg_chkslotlink before insert or update
    on HSlot for each row execute procedure tg_chkslotlink();

create trigger tg_chkslotlink before insert or update
    on PHone for each row execute procedure tg_chkslotlink();


-- ************************************************************
-- * BEFORE INSERT or UPDATE on all slots with backlink
-- *	- Set backlink to empty string if NULL value given
-- ************************************************************
create function tg_chkbacklink() returns opaque as '
begin
    if new.backlink isnull then
        new.backlink := '''';
    end if;
    return new;
end;
' language 'plpgsql';

create trigger tg_chkbacklink before insert or update
    on PSlot for each row execute procedure tg_chkbacklink();

create trigger tg_chkbacklink before insert or update
    on WSlot for each row execute procedure tg_chkbacklink();

create trigger tg_chkbacklink before insert or update
    on PLine for each row execute procedure tg_chkbacklink();


-- ************************************************************
-- * BEFORE UPDATE on PSlot
-- *	- do delete/insert instead of update if name changes
-- ************************************************************
create function tg_pslot_bu() returns opaque as '
begin
    if new.slotname != old.slotname then
        delete from PSlot where slotname = old.slotname;
	insert into PSlot (
		    slotname,
		    pfname,
		    slotlink,
		    backlink
		) values (
		    new.slotname,
		    new.pfname,
		    new.slotlink,
		    new.backlink
		);
        return null;
    end if;
    return new;
end;
' language 'plpgsql';

create trigger tg_pslot_bu before update
    on PSlot for each row execute procedure tg_pslot_bu();


-- ************************************************************
-- * BEFORE UPDATE on WSlot
-- *	- do delete/insert instead of update if name changes
-- ************************************************************
create function tg_wslot_bu() returns opaque as '
begin
    if new.slotname != old.slotname then
        delete from WSlot where slotname = old.slotname;
	insert into WSlot (
		    slotname,
		    roomno,
		    slotlink,
		    backlink
		) values (
		    new.slotname,
		    new.roomno,
		    new.slotlink,
		    new.backlink
		);
        return null;
    end if;
    return new;
end;
' language 'plpgsql';

create trigger tg_wslot_bu before update
    on WSlot for each row execute procedure tg_Wslot_bu();


-- ************************************************************
-- * BEFORE UPDATE on PLine
-- *	- do delete/insert instead of update if name changes
-- ************************************************************
create function tg_pline_bu() returns opaque as '
begin
    if new.slotname != old.slotname then
        delete from PLine where slotname = old.slotname;
	insert into PLine (
		    slotname,
		    phonenumber,
		    comment,
		    backlink
		) values (
		    new.slotname,
		    new.phonenumber,
		    new.comment,
		    new.backlink
		);
        return null;
    end if;
    return new;
end;
' language 'plpgsql';

create trigger tg_pline_bu before update
    on PLine for each row execute procedure tg_pline_bu();


-- ************************************************************
-- * BEFORE UPDATE on IFace
-- *	- do delete/insert instead of update if name changes
-- ************************************************************
create function tg_iface_bu() returns opaque as '
begin
    if new.slotname != old.slotname then
        delete from IFace where slotname = old.slotname;
	insert into IFace (
		    slotname,
		    sysname,
		    ifname,
		    slotlink
		) values (
		    new.slotname,
		    new.sysname,
		    new.ifname,
		    new.slotlink
		);
        return null;
    end if;
    return new;
end;
' language 'plpgsql';

create trigger tg_iface_bu before update
    on IFace for each row execute procedure tg_iface_bu();


-- ************************************************************
-- * BEFORE UPDATE on HSlot
-- *	- do delete/insert instead of update if name changes
-- ************************************************************
create function tg_hslot_bu() returns opaque as '
begin
    if new.slotname != old.slotname or new.hubname != old.hubname then
        delete from HSlot where slotname = old.slotname;
	insert into HSlot (
		    slotname,
		    hubname,
		    slotno,
		    slotlink
		) values (
		    new.slotname,
		    new.hubname,
		    new.slotno,
		    new.slotlink
		);
        return null;
    end if;
    return new;
end;
' language 'plpgsql';

create trigger tg_hslot_bu before update
    on HSlot for each row execute procedure tg_hslot_bu();


-- ************************************************************
-- * BEFORE UPDATE on PHone
-- *	- do delete/insert instead of update if name changes
-- ************************************************************
create function tg_phone_bu() returns opaque as '
begin
    if new.slotname != old.slotname then
        delete from PHone where slotname = old.slotname;
	insert into PHone (
		    slotname,
		    comment,
		    slotlink
		) values (
		    new.slotname,
		    new.comment,
		    new.slotlink
		);
        return null;
    end if;
    return new;
end;
' language 'plpgsql';

create trigger tg_phone_bu before update
    on PHone for each row execute procedure tg_phone_bu();


-- ************************************************************
-- * AFTER INSERT or UPDATE or DELETE on slot with backlink
-- *	- Ensure that the opponent correctly points back to us
-- ************************************************************
create function tg_backlink_a() returns opaque as '
declare
    dummy	integer;
begin
    if tg_op = ''INSERT'' then
        if new.backlink != '''' then
	    dummy := tg_backlink_set(new.backlink, new.slotname);
	end if;
	return new;
    end if;
    if tg_op = ''UPDATE'' then
        if new.backlink != old.backlink then
	    if old.backlink != '''' then
	        dummy := tg_backlink_unset(old.backlink, old.slotname);
	    end if;
	    if new.backlink != '''' then
	        dummy := tg_backlink_set(new.backlink, new.slotname);
	    end if;
	else
	    if new.slotname != old.slotname and new.backlink != '''' then
	        dummy := tg_slotlink_set(new.backlink, new.slotname);
	    end if;
	end if;
	return new;
    end if;
    if tg_op = ''DELETE'' then
        if old.backlink != '''' then
	    dummy := tg_backlink_unset(old.backlink, old.slotname);
	end if;
	return old;
    end if;
end;
' language 'plpgsql';


create trigger tg_backlink_a after insert or update or delete
    on PSlot for each row execute procedure tg_backlink_a('PS');

create trigger tg_backlink_a after insert or update or delete
    on WSlot for each row execute procedure tg_backlink_a('WS');

create trigger tg_backlink_a after insert or update or delete
    on PLine for each row execute procedure tg_backlink_a('PL');


-- ************************************************************
-- * Support function to set the opponents backlink field
-- * if it does not already point to the requested slot
-- ************************************************************
create function tg_backlink_set(bpchar, bpchar)
returns integer as '
declare
    myname	alias for $1;
    blname	alias for $2;
    mytype	char(2);
    link	char(4);
    rec		record;
begin
    mytype := substr(myname, 1, 2);
    link := mytype || substr(blname, 1, 2);
    if link = ''PLPL'' then
        raise exception 
		''backlink between two phone lines does not make sense'';
    end if;
    if link in (''PLWS'', ''WSPL'') then
        raise exception 
		''direct link of phone line to wall slot not permitted'';
    end if;
    if mytype = ''PS'' then
        select into rec * from PSlot where slotname = myname;
	if not found then
	    raise exception ''% does not exist'', myname;
	end if;
	if rec.backlink != blname then
	    update PSlot set backlink = blname where slotname = myname;
	end if;
	return 0;
    end if;
    if mytype = ''WS'' then
        select into rec * from WSlot where slotname = myname;
	if not found then
	    raise exception ''% does not exist'', myname;
	end if;
	if rec.backlink != blname then
	    update WSlot set backlink = blname where slotname = myname;
	end if;
	return 0;
    end if;
    if mytype = ''PL'' then
        select into rec * from PLine where slotname = myname;
	if not found then
	    raise exception ''% does not exist'', myname;
	end if;
	if rec.backlink != blname then
	    update PLine set backlink = blname where slotname = myname;
	end if;
	return 0;
    end if;
    raise exception ''illegal backlink beginning with %'', mytype;
end;
' language 'plpgsql';


-- ************************************************************
-- * Support function to clear out the backlink field if
-- * it still points to specific slot
-- ************************************************************
create function tg_backlink_unset(bpchar, bpchar)
returns integer as '
declare
    myname	alias for $1;
    blname	alias for $2;
    mytype	char(2);
    rec		record;
begin
    mytype := substr(myname, 1, 2);
    if mytype = ''PS'' then
        select into rec * from PSlot where slotname = myname;
	if not found then
	    return 0;
	end if;
	if rec.backlink = blname then
	    update PSlot set backlink = '''' where slotname = myname;
	end if;
	return 0;
    end if;
    if mytype = ''WS'' then
        select into rec * from WSlot where slotname = myname;
	if not found then
	    return 0;
	end if;
	if rec.backlink = blname then
	    update WSlot set backlink = '''' where slotname = myname;
	end if;
	return 0;
    end if;
    if mytype = ''PL'' then
        select into rec * from PLine where slotname = myname;
	if not found then
	    return 0;
	end if;
	if rec.backlink = blname then
	    update PLine set backlink = '''' where slotname = myname;
	end if;
	return 0;
    end if;
end;
' language 'plpgsql';


-- ************************************************************
-- * AFTER INSERT or UPDATE or DELETE on slot with slotlink
-- *	- Ensure that the opponent correctly points back to us
-- ************************************************************
create function tg_slotlink_a() returns opaque as '
declare
    dummy	integer;
begin
    if tg_op = ''INSERT'' then
        if new.slotlink != '''' then
	    dummy := tg_slotlink_set(new.slotlink, new.slotname);
	end if;
	return new;
    end if;
    if tg_op = ''UPDATE'' then
        if new.slotlink != old.slotlink then
	    if old.slotlink != '''' then
	        dummy := tg_slotlink_unset(old.slotlink, old.slotname);
	    end if;
	    if new.slotlink != '''' then
	        dummy := tg_slotlink_set(new.slotlink, new.slotname);
	    end if;
	else
	    if new.slotname != old.slotname and new.slotlink != '''' then
	        dummy := tg_slotlink_set(new.slotlink, new.slotname);
	    end if;
	end if;
	return new;
    end if;
    if tg_op = ''DELETE'' then
        if old.slotlink != '''' then
	    dummy := tg_slotlink_unset(old.slotlink, old.slotname);
	end if;
	return old;
    end if;
end;
' language 'plpgsql';


create trigger tg_slotlink_a after insert or update or delete
    on PSlot for each row execute procedure tg_slotlink_a('PS');

create trigger tg_slotlink_a after insert or update or delete
    on WSlot for each row execute procedure tg_slotlink_a('WS');

create trigger tg_slotlink_a after insert or update or delete
    on IFace for each row execute procedure tg_slotlink_a('IF');

create trigger tg_slotlink_a after insert or update or delete
    on HSlot for each row execute procedure tg_slotlink_a('HS');

create trigger tg_slotlink_a after insert or update or delete
    on PHone for each row execute procedure tg_slotlink_a('PH');


-- ************************************************************
-- * Support function to set the opponents slotlink field
-- * if it does not already point to the requested slot
-- ************************************************************
create function tg_slotlink_set(bpchar, bpchar)
returns integer as '
declare
    myname	alias for $1;
    blname	alias for $2;
    mytype	char(2);
    link	char(4);
    rec		record;
begin
    mytype := substr(myname, 1, 2);
    link := mytype || substr(blname, 1, 2);
    if link = ''PHPH'' then
        raise exception 
		''slotlink between two phones does not make sense'';
    end if;
    if link in (''PHHS'', ''HSPH'') then
        raise exception 
		''link of phone to hub does not make sense'';
    end if;
    if link in (''PHIF'', ''IFPH'') then
        raise exception 
		''link of phone to hub does not make sense'';
    end if;
    if link in (''PSWS'', ''WSPS'') then
        raise exception 
		''slotlink from patchslot to wallslot not permitted'';
    end if;
    if mytype = ''PS'' then
        select into rec * from PSlot where slotname = myname;
	if not found then
	    raise exception ''% does not exist'', myname;
	end if;
	if rec.slotlink != blname then
	    update PSlot set slotlink = blname where slotname = myname;
	end if;
	return 0;
    end if;
    if mytype = ''WS'' then
        select into rec * from WSlot where slotname = myname;
	if not found then
	    raise exception ''% does not exist'', myname;
	end if;
	if rec.slotlink != blname then
	    update WSlot set slotlink = blname where slotname = myname;
	end if;
	return 0;
    end if;
    if mytype = ''IF'' then
        select into rec * from IFace where slotname = myname;
	if not found then
	    raise exception ''% does not exist'', myname;
	end if;
	if rec.slotlink != blname then
	    update IFace set slotlink = blname where slotname = myname;
	end if;
	return 0;
    end if;
    if mytype = ''HS'' then
        select into rec * from HSlot where slotname = myname;
	if not found then
	    raise exception ''% does not exist'', myname;
	end if;
	if rec.slotlink != blname then
	    update HSlot set slotlink = blname where slotname = myname;
	end if;
	return 0;
    end if;
    if mytype = ''PH'' then
        select into rec * from PHone where slotname = myname;
	if not found then
	    raise exception ''% does not exist'', myname;
	end if;
	if rec.slotlink != blname then
	    update PHone set slotlink = blname where slotname = myname;
	end if;
	return 0;
    end if;
    raise exception ''illegal slotlink beginning with %'', mytype;
end;
' language 'plpgsql';


-- ************************************************************
-- * Support function to clear out the slotlink field if
-- * it still points to specific slot
-- ************************************************************
create function tg_slotlink_unset(bpchar, bpchar)
returns integer as '
declare
    myname	alias for $1;
    blname	alias for $2;
    mytype	char(2);
    rec		record;
begin
    mytype := substr(myname, 1, 2);
    if mytype = ''PS'' then
        select into rec * from PSlot where slotname = myname;
	if not found then
	    return 0;
	end if;
	if rec.slotlink = blname then
	    update PSlot set slotlink = '''' where slotname = myname;
	end if;
	return 0;
    end if;
    if mytype = ''WS'' then
        select into rec * from WSlot where slotname = myname;
	if not found then
	    return 0;
	end if;
	if rec.slotlink = blname then
	    update WSlot set slotlink = '''' where slotname = myname;
	end if;
	return 0;
    end if;
    if mytype = ''IF'' then
        select into rec * from IFace where slotname = myname;
	if not found then
	    return 0;
	end if;
	if rec.slotlink = blname then
	    update IFace set slotlink = '''' where slotname = myname;
	end if;
	return 0;
    end if;
    if mytype = ''HS'' then
        select into rec * from HSlot where slotname = myname;
	if not found then
	    return 0;
	end if;
	if rec.slotlink = blname then
	    update HSlot set slotlink = '''' where slotname = myname;
	end if;
	return 0;
    end if;
    if mytype = ''PH'' then
        select into rec * from PHone where slotname = myname;
	if not found then
	    return 0;
	end if;
	if rec.slotlink = blname then
	    update PHone set slotlink = '''' where slotname = myname;
	end if;
	return 0;
    end if;
end;
' language 'plpgsql';


-- ************************************************************
-- * Describe the backside of a patchfield slot
-- ************************************************************
create function pslot_backlink_view(bpchar)
returns text as '
<<outer>>
declare
    rec		record;
    bltype	char(2);
    retval	text;
begin
    select into rec * from PSlot where slotname = $1;
    if not found then
        return '''';
    end if;
    if rec.backlink = '''' then
        return ''-'';
    end if;
    bltype := substr(rec.backlink, 1, 2);
    if bltype = ''PL'' then
        declare
	    rec		record;
	begin
	    select into rec * from PLine where slotname = outer.rec.backlink;
	    retval := ''Phone line '' || trim(rec.phonenumber);
	    if rec.comment != '''' then
	        retval := retval || '' ('';
		retval := retval || rec.comment;
		retval := retval || '')'';
	    end if;
	    return retval;
	end;
    end if;
    if bltype = ''WS'' then
        select into rec * from WSlot where slotname = rec.backlink;
	retval := trim(rec.slotname) || '' in room '';
	retval := retval || trim(rec.roomno);
	retval := retval || '' -> '';
	return retval || wslot_slotlink_view(rec.slotname);
    end if;
    return rec.backlink;
end;
' language 'plpgsql';


-- ************************************************************
-- * Describe the front of a patchfield slot
-- ************************************************************
create function pslot_slotlink_view(bpchar)
returns text as '
declare
    psrec	record;
    sltype	char(2);
    retval	text;
begin
    select into psrec * from PSlot where slotname = $1;
    if not found then
        return '''';
    end if;
    if psrec.slotlink = '''' then
        return ''-'';
    end if;
    sltype := substr(psrec.slotlink, 1, 2);
    if sltype = ''PS'' then
	retval := trim(psrec.slotlink) || '' -> '';
	return retval || pslot_backlink_view(psrec.slotlink);
    end if;
    if sltype = ''HS'' then
        retval := comment from Hub H, HSlot HS
			where HS.slotname = psrec.slotlink
			  and H.name = HS.hubname;
        retval := retval || '' slot '';
	retval := retval || slotno::text from HSlot
			where slotname = psrec.slotlink;
	return retval;
    end if;
    return psrec.slotlink;
end;
' language 'plpgsql';


-- ************************************************************
-- * Describe the front of a wall connector slot
-- ************************************************************
create function wslot_slotlink_view(bpchar)
returns text as '
declare
    rec		record;
    sltype	char(2);
    retval	text;
begin
    select into rec * from WSlot where slotname = $1;
    if not found then
        return '''';
    end if;
    if rec.slotlink = '''' then
        return ''-'';
    end if;
    sltype := substr(rec.slotlink, 1, 2);
    if sltype = ''PH'' then
        select into rec * from PHone where slotname = rec.slotlink;
	retval := ''Phone '' || trim(rec.slotname);
	if rec.comment != '''' then
	    retval := retval || '' ('';
	    retval := retval || rec.comment;
	    retval := retval || '')'';
	end if;
	return retval;
    end if;
    if sltype = ''IF'' then
	declare
	    syrow	System%RowType;
	    ifrow	IFace%ROWTYPE;
        begin
	    select into ifrow * from IFace where slotname = rec.slotlink;
	    select into syrow * from System where name = ifrow.sysname;
	    retval := syrow.name || '' IF '';
	    retval := retval || ifrow.ifname;
	    if syrow.comment != '''' then
	        retval := retval || '' ('';
		retval := retval || syrow.comment;
		retval := retval || '')'';
	    end if;
	    return retval;
	end;
    end if;
    return rec.slotlink;
end;
' language 'plpgsql';



-- ************************************************************
-- * View of a patchfield describing backside and patches
-- ************************************************************
create view Pfield_v1 as select PF.pfname, PF.slotname,
	pslot_backlink_view(PF.slotname) as backside,
	pslot_slotlink_view(PF.slotname) as patch
    from PSlot PF;


--
-- First we build the house - so we create the rooms
--
insert into Room values ('001', 'Entrance');
insert into Room values ('002', 'Office');
insert into Room values ('003', 'Office');
insert into Room values ('004', 'Technical');
insert into Room values ('101', 'Office');
insert into Room values ('102', 'Conference');
insert into Room values ('103', 'Restroom');
insert into Room values ('104', 'Technical');
insert into Room values ('105', 'Office');
insert into Room values ('106', 'Office');

--
-- Second we install the wall connectors
--
insert into WSlot values ('WS.001.1a', '001', '', '');
insert into WSlot values ('WS.001.1b', '001', '', '');
insert into WSlot values ('WS.001.2a', '001', '', '');
insert into WSlot values ('WS.001.2b', '001', '', '');
insert into WSlot values ('WS.001.3a', '001', '', '');
insert into WSlot values ('WS.001.3b', '001', '', '');

insert into WSlot values ('WS.002.1a', '002', '', '');
insert into WSlot values ('WS.002.1b', '002', '', '');
insert into WSlot values ('WS.002.2a', '002', '', '');
insert into WSlot values ('WS.002.2b', '002', '', '');
insert into WSlot values ('WS.002.3a', '002', '', '');
insert into WSlot values ('WS.002.3b', '002', '', '');

insert into WSlot values ('WS.003.1a', '003', '', '');
insert into WSlot values ('WS.003.1b', '003', '', '');
insert into WSlot values ('WS.003.2a', '003', '', '');
insert into WSlot values ('WS.003.2b', '003', '', '');
insert into WSlot values ('WS.003.3a', '003', '', '');
insert into WSlot values ('WS.003.3b', '003', '', '');

insert into WSlot values ('WS.101.1a', '101', '', '');
insert into WSlot values ('WS.101.1b', '101', '', '');
insert into WSlot values ('WS.101.2a', '101', '', '');
insert into WSlot values ('WS.101.2b', '101', '', '');
insert into WSlot values ('WS.101.3a', '101', '', '');
insert into WSlot values ('WS.101.3b', '101', '', '');

insert into WSlot values ('WS.102.1a', '102', '', '');
insert into WSlot values ('WS.102.1b', '102', '', '');
insert into WSlot values ('WS.102.2a', '102', '', '');
insert into WSlot values ('WS.102.2b', '102', '', '');
insert into WSlot values ('WS.102.3a', '102', '', '');
insert into WSlot values ('WS.102.3b', '102', '', '');

insert into WSlot values ('WS.105.1a', '105', '', '');
insert into WSlot values ('WS.105.1b', '105', '', '');
insert into WSlot values ('WS.105.2a', '105', '', '');
insert into WSlot values ('WS.105.2b', '105', '', '');
insert into WSlot values ('WS.105.3a', '105', '', '');
insert into WSlot values ('WS.105.3b', '105', '', '');

insert into WSlot values ('WS.106.1a', '106', '', '');
insert into WSlot values ('WS.106.1b', '106', '', '');
insert into WSlot values ('WS.106.2a', '106', '', '');
insert into WSlot values ('WS.106.2b', '106', '', '');
insert into WSlot values ('WS.106.3a', '106', '', '');
insert into WSlot values ('WS.106.3b', '106', '', '');

--
-- Now create the patch fields and their slots
--
insert into PField values ('PF0_1', 'Wallslots basement');

--
-- The cables for these will be made later, so they are unconnected for now
--
insert into PSlot values ('PS.base.a1', 'PF0_1', '', '');
insert into PSlot values ('PS.base.a2', 'PF0_1', '', '');
insert into PSlot values ('PS.base.a3', 'PF0_1', '', '');
insert into PSlot values ('PS.base.a4', 'PF0_1', '', '');
insert into PSlot values ('PS.base.a5', 'PF0_1', '', '');
insert into PSlot values ('PS.base.a6', 'PF0_1', '', '');

--
-- These are already wired to the wall connectors
--
insert into PSlot values ('PS.base.b1', 'PF0_1', '', 'WS.002.1a');
insert into PSlot values ('PS.base.b2', 'PF0_1', '', 'WS.002.1b');
insert into PSlot values ('PS.base.b3', 'PF0_1', '', 'WS.002.2a');
insert into PSlot values ('PS.base.b4', 'PF0_1', '', 'WS.002.2b');
insert into PSlot values ('PS.base.b5', 'PF0_1', '', 'WS.002.3a');
insert into PSlot values ('PS.base.b6', 'PF0_1', '', 'WS.002.3b');

insert into PSlot values ('PS.base.c1', 'PF0_1', '', 'WS.003.1a');
insert into PSlot values ('PS.base.c2', 'PF0_1', '', 'WS.003.1b');
insert into PSlot values ('PS.base.c3', 'PF0_1', '', 'WS.003.2a');
insert into PSlot values ('PS.base.c4', 'PF0_1', '', 'WS.003.2b');
insert into PSlot values ('PS.base.c5', 'PF0_1', '', 'WS.003.3a');
insert into PSlot values ('PS.base.c6', 'PF0_1', '', 'WS.003.3b');

--
-- This patchfield will be renamed later into PF0_2 - so its
-- slots references in pfname should follow
--
insert into PField values ('PF0_X', 'Phonelines basement');

insert into PSlot values ('PS.base.ta1', 'PF0_X', '', '');
insert into PSlot values ('PS.base.ta2', 'PF0_X', '', '');
insert into PSlot values ('PS.base.ta3', 'PF0_X', '', '');
insert into PSlot values ('PS.base.ta4', 'PF0_X', '', '');
insert into PSlot values ('PS.base.ta5', 'PF0_X', '', '');
insert into PSlot values ('PS.base.ta6', 'PF0_X', '', '');

insert into PSlot values ('PS.base.tb1', 'PF0_X', '', '');
insert into PSlot values ('PS.base.tb2', 'PF0_X', '', '');
insert into PSlot values ('PS.base.tb3', 'PF0_X', '', '');
insert into PSlot values ('PS.base.tb4', 'PF0_X', '', '');
insert into PSlot values ('PS.base.tb5', 'PF0_X', '', '');
insert into PSlot values ('PS.base.tb6', 'PF0_X', '', '');

insert into PField values ('PF1_1', 'Wallslots 1st floor');

insert into PSlot values ('PS.1st.a1', 'PF1_1', '', 'WS.101.1a');
insert into PSlot values ('PS.1st.a2', 'PF1_1', '', 'WS.101.1b');
insert into PSlot values ('PS.1st.a3', 'PF1_1', '', 'WS.101.2a');
insert into PSlot values ('PS.1st.a4', 'PF1_1', '', 'WS.101.2b');
insert into PSlot values ('PS.1st.a5', 'PF1_1', '', 'WS.101.3a');
insert into PSlot values ('PS.1st.a6', 'PF1_1', '', 'WS.101.3b');

insert into PSlot values ('PS.1st.b1', 'PF1_1', '', 'WS.102.1a');
insert into PSlot values ('PS.1st.b2', 'PF1_1', '', 'WS.102.1b');
insert into PSlot values ('PS.1st.b3', 'PF1_1', '', 'WS.102.2a');
insert into PSlot values ('PS.1st.b4', 'PF1_1', '', 'WS.102.2b');
insert into PSlot values ('PS.1st.b5', 'PF1_1', '', 'WS.102.3a');
insert into PSlot values ('PS.1st.b6', 'PF1_1', '', 'WS.102.3b');

insert into PSlot values ('PS.1st.c1', 'PF1_1', '', 'WS.105.1a');
insert into PSlot values ('PS.1st.c2', 'PF1_1', '', 'WS.105.1b');
insert into PSlot values ('PS.1st.c3', 'PF1_1', '', 'WS.105.2a');
insert into PSlot values ('PS.1st.c4', 'PF1_1', '', 'WS.105.2b');
insert into PSlot values ('PS.1st.c5', 'PF1_1', '', 'WS.105.3a');
insert into PSlot values ('PS.1st.c6', 'PF1_1', '', 'WS.105.3b');

insert into PSlot values ('PS.1st.d1', 'PF1_1', '', 'WS.106.1a');
insert into PSlot values ('PS.1st.d2', 'PF1_1', '', 'WS.106.1b');
insert into PSlot values ('PS.1st.d3', 'PF1_1', '', 'WS.106.2a');
insert into PSlot values ('PS.1st.d4', 'PF1_1', '', 'WS.106.2b');
insert into PSlot values ('PS.1st.d5', 'PF1_1', '', 'WS.106.3a');
insert into PSlot values ('PS.1st.d6', 'PF1_1', '', 'WS.106.3b');

--
-- Now we wire the wall connectors 1a-2a in room 001 to the
-- patchfield. In the second update we make an error, and
-- correct it after
--
update PSlot set backlink = 'WS.001.1a' where slotname = 'PS.base.a1';
update PSlot set backlink = 'WS.001.1b' where slotname = 'PS.base.a3';
select * from WSlot where roomno = '001' order by slotname;
select * from PSlot where slotname ~ 'PS.base.a' order by slotname;
update PSlot set backlink = 'WS.001.2a' where slotname = 'PS.base.a3';
select * from WSlot where roomno = '001' order by slotname;
select * from PSlot where slotname ~ 'PS.base.a' order by slotname;
update PSlot set backlink = 'WS.001.1b' where slotname = 'PS.base.a2';
select * from WSlot where roomno = '001' order by slotname;
select * from PSlot where slotname ~ 'PS.base.a' order by slotname;

--
-- Same procedure for 2b-3b but this time updating the WSlot instead
-- of the PSlot. Due to the triggers the result is the same:
-- WSlot and corresponding PSlot point to each other.
--
update WSlot set backlink = 'PS.base.a4' where slotname = 'WS.001.2b';
update WSlot set backlink = 'PS.base.a6' where slotname = 'WS.001.3a';
select * from WSlot where roomno = '001' order by slotname;
select * from PSlot where slotname ~ 'PS.base.a' order by slotname;
update WSlot set backlink = 'PS.base.a6' where slotname = 'WS.001.3b';
select * from WSlot where roomno = '001' order by slotname;
select * from PSlot where slotname ~ 'PS.base.a' order by slotname;
update WSlot set backlink = 'PS.base.a5' where slotname = 'WS.001.3a';
select * from WSlot where roomno = '001' order by slotname;
select * from PSlot where slotname ~ 'PS.base.a' order by slotname;

insert into PField values ('PF1_2', 'Phonelines 1st floor');

insert into PSlot values ('PS.1st.ta1', 'PF1_2', '', '');
insert into PSlot values ('PS.1st.ta2', 'PF1_2', '', '');
insert into PSlot values ('PS.1st.ta3', 'PF1_2', '', '');
insert into PSlot values ('PS.1st.ta4', 'PF1_2', '', '');
insert into PSlot values ('PS.1st.ta5', 'PF1_2', '', '');
insert into PSlot values ('PS.1st.ta6', 'PF1_2', '', '');

insert into PSlot values ('PS.1st.tb1', 'PF1_2', '', '');
insert into PSlot values ('PS.1st.tb2', 'PF1_2', '', '');
insert into PSlot values ('PS.1st.tb3', 'PF1_2', '', '');
insert into PSlot values ('PS.1st.tb4', 'PF1_2', '', '');
insert into PSlot values ('PS.1st.tb5', 'PF1_2', '', '');
insert into PSlot values ('PS.1st.tb6', 'PF1_2', '', '');

--
-- Fix the wrong name for patchfield PF0_2
--
update PField set name = 'PF0_2' where name = 'PF0_X';

select * from PSlot order by slotname;
select * from WSlot order by slotname;

--
-- Install the central phone system and create the phone numbers.
-- They are weired on insert to the patchfields. Again the
-- triggers automatically tell the PSlots to update their
-- backlink field.
--
insert into PLine values ('PL.001', '-0', 'Central call', 'PS.base.ta1');
insert into PLine values ('PL.002', '-101', '', 'PS.base.ta2');
insert into PLine values ('PL.003', '-102', '', 'PS.base.ta3');
insert into PLine values ('PL.004', '-103', '', 'PS.base.ta5');
insert into PLine values ('PL.005', '-104', '', 'PS.base.ta6');
insert into PLine values ('PL.006', '-106', '', 'PS.base.tb2');
insert into PLine values ('PL.007', '-108', '', 'PS.base.tb3');
insert into PLine values ('PL.008', '-109', '', 'PS.base.tb4');
insert into PLine values ('PL.009', '-121', '', 'PS.base.tb5');
insert into PLine values ('PL.010', '-122', '', 'PS.base.tb6');
insert into PLine values ('PL.015', '-134', '', 'PS.1st.ta1');
insert into PLine values ('PL.016', '-137', '', 'PS.1st.ta3');
insert into PLine values ('PL.017', '-139', '', 'PS.1st.ta4');
insert into PLine values ('PL.018', '-362', '', 'PS.1st.tb1');
insert into PLine values ('PL.019', '-363', '', 'PS.1st.tb2');
insert into PLine values ('PL.020', '-364', '', 'PS.1st.tb3');
insert into PLine values ('PL.021', '-365', '', 'PS.1st.tb5');
insert into PLine values ('PL.022', '-367', '', 'PS.1st.tb6');
insert into PLine values ('PL.028', '-501', 'Fax entrance', 'PS.base.ta2');
insert into PLine values ('PL.029', '-502', 'Fax 1st floor', 'PS.1st.ta1');

--
-- Buy some phones, plug them into the wall and patch the
-- phone lines to the corresponding patchfield slots.
--
insert into PHone values ('PH.hc001', 'Hicom standard', 'WS.001.1a');
update PSlot set slotlink = 'PS.base.ta1' where slotname = 'PS.base.a1';
insert into PHone values ('PH.hc002', 'Hicom standard', 'WS.002.1a');
update PSlot set slotlink = 'PS.base.ta5' where slotname = 'PS.base.b1';
insert into PHone values ('PH.hc003', 'Hicom standard', 'WS.002.2a');
update PSlot set slotlink = 'PS.base.tb2' where slotname = 'PS.base.b3';
insert into PHone values ('PH.fax001', 'Canon fax', 'WS.001.2a');
update PSlot set slotlink = 'PS.base.ta2' where slotname = 'PS.base.a3';

--
-- Install a hub at one of the patchfields, plug a computers
-- ethernet interface into the wall and patch it to the hub.
--
insert into Hub values ('base.hub1', 'Patchfield PF0_1 hub', 16);
insert into System values ('orion', 'PC');
insert into IFace values ('IF', 'orion', 'eth0', 'WS.002.1b');
update PSlot set slotlink = 'HS.base.hub1.1' where slotname = 'PS.base.b2';

--
-- Now we take a look at the patchfield
--
select * from PField_v1 where pfname = 'PF0_1' order by slotname;
select * from PField_v1 where pfname = 'PF0_2' order by slotname;

--
-- Finally we want errors
--
insert into PField values ('PF1_1', 'should fail due to unique index');
update PSlot set backlink = 'WS.not.there' where slotname = 'PS.base.a1';
update PSlot set backlink = 'XX.illegal' where slotname = 'PS.base.a1';
update PSlot set slotlink = 'PS.not.there' where slotname = 'PS.base.a1';
update PSlot set slotlink = 'XX.illegal' where slotname = 'PS.base.a1';
insert into HSlot values ('HS', 'base.hub1', 1, '');
insert into HSlot values ('HS', 'base.hub1', 20, '');
delete from HSlot;
insert into IFace values ('IF', 'notthere', 'eth0', '');
insert into IFace values ('IF', 'orion', 'ethernet_interface_name_too_long', '');

