#
# Copyright (c) 2001-2014, PostgreSQL Global Development Group
#
# src/backend/utils/mb/Unicode/ucs2utf.pl
# convert UCS-4 to UTF-8
#
sub ucs2utf
{
	local ($ucs) = @_;
	local $utf;

	if ($ucs <= 0x007f)
	{
		$utf = $ucs;
	}
	elsif ($ucs > 0x007f && $ucs <= 0x07ff)
	{
		$utf = (($ucs & 0x003f) | 0x80) | ((($ucs >> 6) | 0xc0) << 8);
	}
	elsif ($ucs > 0x07ff && $ucs <= 0xffff)
	{
		$utf =
		  ((($ucs >> 12) | 0xe0) << 16) |
		  (((($ucs & 0x0fc0) >> 6) | 0x80) << 8) | (($ucs & 0x003f) | 0x80);
	}
	else
	{
		$utf =
		  ((($ucs >> 18) | 0xf0) << 24) |
		  (((($ucs & 0x3ffff) >> 12) | 0x80) << 16) |
		  (((($ucs & 0x0fc0) >> 6) | 0x80) << 8) | (($ucs & 0x003f) | 0x80);
	}
	return ($utf);
}
1;
