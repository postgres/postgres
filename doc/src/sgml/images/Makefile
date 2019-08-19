# doc/src/sgml/images/Makefile
#
# see README in this directory about image handling

ALL_IMAGES = \
	genetic-algorithm.svg \
	gin.svg \
	pagelayout.svg

DITAA = ditaa
DOT = dot
XSLTPROC = xsltproc

all: $(ALL_IMAGES)

%.svg.tmp: %.gv
	$(DOT) -T svg -o $@ $<

%.svg.tmp: %.txt
	$(DITAA) -E -S --svg $< $@

# Post-processing for SVG files coming from other tools
#
# Use --novalid to avoid loading SVG DTD if a file specifies it, since
# it might not be available locally, and we don't need it.
%.svg: %.svg.tmp fixup-svg.xsl
	$(XSLTPROC) --novalid -o $@ $(word 2,$^) $<
