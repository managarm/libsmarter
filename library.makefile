
.PHONY: install
install:
	install --mode=0644 $S/include/smarter.hpp $(DESTDIR)$(prefix)include/

