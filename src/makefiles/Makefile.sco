%.so: %.o
	$(LD) -G -Bdynamic -o $@ $<
%.so: %.o
	$(LD) -G -Bdynamic -o $@ $<
