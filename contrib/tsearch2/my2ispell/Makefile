ZIPFILE=nb_NO
LANGUAGE=norsk


UNZIP=unzip -o


all: $(LANGUAGE).dict $(LANGUAGE).aff

$(ZIPFILE).aff: $(ZIPFILE).zip
	$(UNZIP) $? $@
	touch $@ 


# 1 Cleanup dictionary
# 2 remove " symbol
# 3 add compoundwords controlled flag to word which hasn't it, but
#   has compound only suffixes

$(LANGUAGE).dict: $(ZIPFILE).zip
	$(UNZIP) $? $(ZIPFILE).dic
	grep -v -E '^[[:digit:]]+$$' < $(ZIPFILE).dic \
	 | grep -v '\.' \
	 | sed -e 's/"//g' \
	 | perl -pi -e 's|/(\S+)| $$q=$$1; ( $$q=~/[\\_`]/ && $$q!~/z/ ) ? "/$${q}z" : "/$${q}"|e' \
	 | sort \
	> $@

#just convert affix file

$(LANGUAGE).aff: $(ZIPFILE).aff 
	grep -v -i zyzyzy $(ZIPFILE).aff \
	 | grep -v -i zyzyzy \
	 | perl -pi \
		-e 's/^COMPOUNDFLAG\s+(\S+)/compoundwords controlled $$1/;' \
	        -e 's/^COMPOUNDMIN\s+(\d+)/compoundmin $$1/;' \
	        -e 's/^PFX\s+(\S+)\s+([YN])\s+\d+.*$$/ if ( !$$wasprf ) { $$wasprf=1; "prefixes\n\nflag $$1:" } else { "flag $$1:" } /e;' \
	        -e 's/^PFX\s+\S+\s+(\S+)\s+(\S+)\s+(\S+)/ uc("   $$3    > $$2")/e;' \
	        -e 's/^(.*)SFX\s+(\S+)\s+([YN])\s+\d+.*$$/ $$flg=($$3 eq "Y") ? "*" : ""; $$flg="~$$flg" if length $$1; $$q=$$2; $$q="\\$$q" if $$q!~m#[a-zA-Z]#; if ( !$$wassfx ) { $$wassfx=1; "suffixes\n\nflag $$flg$$q:" } else { "flag $$flg$$q:" } /e;' \
	        -e 's/^.*SFX\s+\S+\s+(\S+)\s+(\S+)\s+(\S+)/ uc("   $$3    > ".( ($$1 eq "0") ? "" : "-$$1,").( ($$2 eq "0") ? "" : "$$2") )/e;' \
		-e 's/^(SET|TRY)/#$$1/' \
	> $@ 

clean:
	rm -rf $(ZIPFILE).aff $(ZIPFILE).dic $(LANGUAGE).dict $(LANGUAGE).aff 


