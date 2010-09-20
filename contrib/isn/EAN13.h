/*
 * EAN13.h
 *	  PostgreSQL type definitions for ISNs (ISBN, ISMN, ISSN, EAN13, UPC)
 *
 * Information recompiled by Kronuz on August 23, 2006
 * http://www.gs1.org/productssolutions/idkeys/support/prefix_list.html
 *
 * IDENTIFICATION
 *	  contrib/isn/EAN13.h
 *
 */

/* where the digit set begins, and how many of them are in the table */
const unsigned EAN13_index[10][2] = {
	{0, 6},
	{6, 1},
	{7, 1},
	{8, 5},
	{13, 20},
	{33, 15},
	{48, 19},
	{67, 23},
	{90, 17},
	{107, 12},
};
const char *EAN13_range[][2] = {
	{"000", "019"},				/* GS1 US */
	{"020", "029"},				/* Restricted distribution (MO defined) */
	{"030", "039"},				/* GS1 US */
	{"040", "049"},				/* Restricted distribution (MO defined) */
	{"050", "059"},				/* Coupons */
	{"060", "099"},				/* GS1 US */
	{"100", "139"},				/* GS1 US */
	{"200", "299"},				/* Restricted distribution (MO defined) */
	{"300", "379"},				/* GS1 France */
	{"380", "380"},				/* GS1 Bulgaria */
	{"383", "383"},				/* GS1 Slovenija */
	{"385", "385"},				/* GS1 Croatia */
	{"387", "387"},				/* GS1 BIH (Bosnia-Herzegovina) */
	{"400", "440"},				/* GS1 Germany */
	{"450", "459"},				/* GS1 Japan */
	{"460", "469"},				/* GS1 Russia */
	{"470", "470"},				/* GS1 Kyrgyzstan */
	{"471", "471"},				/* GS1 Taiwan */
	{"474", "474"},				/* GS1 Estonia */
	{"475", "475"},				/* GS1 Latvia */
	{"476", "476"},				/* GS1 Azerbaijan */
	{"477", "477"},				/* GS1 Lithuania */
	{"478", "478"},				/* GS1 Uzbekistan */
	{"479", "479"},				/* GS1 Sri Lanka */
	{"480", "480"},				/* GS1 Philippines */
	{"481", "481"},				/* GS1 Belarus */
	{"482", "482"},				/* GS1 Ukraine */
	{"484", "484"},				/* GS1 Moldova */
	{"485", "485"},				/* GS1 Armenia */
	{"486", "486"},				/* GS1 Georgia */
	{"487", "487"},				/* GS1 Kazakstan */
	{"489", "489"},				/* GS1 Hong Kong */
	{"490", "499"},				/* GS1 Japan */
	{"500", "509"},				/* GS1 UK */
	{"520", "520"},				/* GS1 Greece */
	{"528", "528"},				/* GS1 Lebanon */
	{"529", "529"},				/* GS1 Cyprus */
	{"530", "530"},				/* GS1 Albania */
	{"531", "531"},				/* GS1 MAC (FYR Macedonia) */
	{"535", "535"},				/* GS1 Malta */
	{"539", "539"},				/* GS1 Ireland */
	{"540", "549"},				/* GS1 Belgium & Luxembourg */
	{"560", "560"},				/* GS1 Portugal */
	{"569", "569"},				/* GS1 Iceland */
	{"570", "579"},				/* GS1 Denmark */
	{"590", "590"},				/* GS1 Poland */
	{"594", "594"},				/* GS1 Romania */
	{"599", "599"},				/* GS1 Hungary */
	{"600", "601"},				/* GS1 South Africa */
	{"603", "603"},				/* GS1 Ghana */
	{"608", "608"},				/* GS1 Bahrain */
	{"609", "609"},				/* GS1 Mauritius */
	{"611", "611"},				/* GS1 Morocco */
	{"613", "613"},				/* GS1 Algeria */
	{"616", "616"},				/* GS1 Kenya */
	{"618", "618"},				/* GS1 Ivory Coast */
	{"619", "619"},				/* GS1 Tunisia */
	{"621", "621"},				/* GS1 Syria */
	{"622", "622"},				/* GS1 Egypt */
	{"624", "624"},				/* GS1 Libya */
	{"625", "625"},				/* GS1 Jordan */
	{"626", "626"},				/* GS1 Iran */
	{"627", "627"},				/* GS1 Kuwait */
	{"628", "628"},				/* GS1 Saudi Arabia */
	{"629", "629"},				/* GS1 Emirates */
	{"640", "649"},				/* GS1 Finland */
	{"690", "695"},				/* GS1 China */
	{"700", "709"},				/* GS1 Norway */
	{"729", "729"},				/* GS1 Israel */
	{"730", "739"},				/* GS1 Sweden */
	{"740", "740"},				/* GS1 Guatemala */
	{"741", "741"},				/* GS1 El Salvador */
	{"742", "742"},				/* GS1 Honduras */
	{"743", "743"},				/* GS1 Nicaragua */
	{"744", "744"},				/* GS1 Costa Rica */
	{"745", "745"},				/* GS1 Panama */
	{"746", "746"},				/* GS1 Republica Dominicana */
	{"750", "750"},				/* GS1 Mexico */
	{"754", "755"},				/* GS1 Canada */
	{"759", "759"},				/* GS1 Venezuela */
	{"760", "769"},				/* GS1 Schweiz, Suisse, Svizzera */
	{"770", "770"},				/* GS1 Colombia */
	{"773", "773"},				/* GS1 Uruguay */
	{"775", "775"},				/* GS1 Peru */
	{"777", "777"},				/* GS1 Bolivia */
	{"779", "779"},				/* GS1 Argentina */
	{"780", "780"},				/* GS1 Chile */
	{"784", "784"},				/* GS1 Paraguay */
	{"786", "786"},				/* GS1 Ecuador */
	{"789", "790"},				/* GS1 Brasil */
	{"800", "839"},				/* GS1 Italy */
	{"840", "849"},				/* GS1 Spain */
	{"850", "850"},				/* GS1 Cuba */
	{"858", "858"},				/* GS1 Slovakia */
	{"859", "859"},				/* GS1 Czech */
	{"860", "860"},				/* GS1 YU (Serbia & Montenegro) */
	{"865", "865"},				/* GS1 Mongolia */
	{"867", "867"},				/* GS1 North Korea */
	{"869", "869"},				/* GS1 Turkey */
	{"870", "879"},				/* GS1 Netherlands */
	{"880", "880"},				/* GS1 South Korea */
	{"884", "884"},				/* GS1 Cambodia */
	{"885", "885"},				/* GS1 Thailand */
	{"888", "888"},				/* GS1 Singapore */
	{"890", "890"},				/* GS1 India */
	{"893", "893"},				/* GS1 Vietnam */
	{"899", "899"},				/* GS1 Indonesia */
	{"900", "919"},				/* GS1 Austria */
	{"930", "939"},				/* GS1 Australia */
	{"940", "949"},				/* GS1 New Zealand */
	{"950", "950"},				/* GS1 Head Office */
	{"955", "955"},				/* GS1 Malaysia */
	{"958", "958"},				/* GS1 Macau */
	{"977", "977"},				/* Serial publications (ISSN) */
	{"978", "978"},				/* Bookland (ISBN) */
	{"979", "979"},				/* International Standard Music Number (ISMN)
								 * and ISBN contingent */
	{"980", "980"},				/* Refund receipts */
	{"981", "982"},				/* Common Currency Coupons */
	{"990", "999"},				/* Coupons */
	{NULL, NULL}
};
