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
