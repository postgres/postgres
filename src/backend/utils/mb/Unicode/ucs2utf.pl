#
# $Id: ucs2utf.pl,v 1.1 2000/10/30 10:40:30 ishii Exp $
# convert UCS-2 to UTF-8
#
sub ucs2utf {
	local($ucs) = @_;
	local $utf;

	if ($ucs <= 0x007f) {
		$utf = $ucs;
	} elsif ($ucs > 0x007f && $ucs <= 0x07ff) {
		$utf = (($ucs & 0x003f) | 0x80) | ((($ucs >> 6) | 0xc0) << 8);
	} else {
		$utf = ((($ucs >> 12) | 0xe0) << 16) | 
			(((($ucs & 0x0fc0) >> 6) | 0x80) << 8) |
				(($ucs & 0x003f) | 0x80);
	}
	return($utf);
}
1;
