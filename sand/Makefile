include ../Makefile.config
include ../Makefile.rules

all install test clean:
	for p in src test ; do cd $$p && $(MAKE) $@ && cd .. ; done

.PHONY: all install test clean
