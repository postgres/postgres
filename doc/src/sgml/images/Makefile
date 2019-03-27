# doc/src/sgml/images/Makefile
#
# see README in this directory about image handling

ALL_IMAGES = \
	gin.svg \
	pagelayout.svg

DITAA = ditaa
DOT = dot

all: $(ALL_IMAGES)

%.svg: %.dot
	$(DOT) -T svg -o $@ $<

%.svg: %.txt
	$(DITAA) -E -S --svg $< $@
