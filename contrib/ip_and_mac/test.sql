--
--	A quick test of the IP address code
--
--	$Id: test.sql,v 1.1 1998/02/14 17:58:09 scrappy Exp $
--

-- temporary table:
create table addresses (address ipaddr);

-- sample data from two subnets:
insert into addresses values ('158.37.96.15');
insert into addresses values ('158.37.96.16');
insert into addresses values ('158.37.96.17');
insert into addresses values ('158.37.97.15');
insert into addresses values ('158.37.97.16');
insert into addresses values ('158.37.97.17');
insert into addresses values ('158.37.98.15');
insert into addresses values ('158.37.98.16');
insert into addresses values ('158.37.98.17');
insert into addresses values ('158.37.96.150');
insert into addresses values ('158.37.96.160');
insert into addresses values ('158.37.96.170');
insert into addresses values ('158.37.97.150');
insert into addresses values ('158.37.97.160');
insert into addresses values ('158.37.97.170');
insert into addresses values ('158.37.98.150');
insert into addresses values ('158.37.98.160');
insert into addresses values ('158.37.98.170');

-- show them all:
select * from addresses;

-- select the ones in subnet 96:
select * from addresses where ipaddr_in_net(address, '158.37.96.0/24');

-- select the ones not in subnet 96:
select * from addresses where not ipaddr_in_net(address, '158.37.96.0/24');

-- select the ones in subnet 97:
select * from addresses where ipaddr_in_net(address, '158.37.97.0/24');

-- select the ones not in subnet 97:
select * from addresses where not ipaddr_in_net(address, '158.37.97.0/24');

-- select the ones in subnet 96 or 97, sorted:
select * from addresses where ipaddr_in_net(address, '158.37.96.0/23')
	order by address;

-- now some networks:
create table networks (network ipaddr);

-- now the subnets mentioned above:
insert into networks values ('158.37.96.0/24');
insert into networks values ('158.37.97.0/24');
insert into networks values ('158.37.98.0/24');

-- select the netmasks of the net containing each:
select address, ipaddr_mask(network) from addresses, networks
	where ipaddr_in_net(address, network);

-- select the broadcast address of the net containing each:
select address, ipaddr_bcast(network) from addresses, networks
	where ipaddr_in_net(address, network);

-- tidy up:
drop table addresses;
drop table networks;

--
--	eof
--
