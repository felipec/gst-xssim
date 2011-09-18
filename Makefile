CC := $(CROSS_COMPILE)gcc

CFLAGS := -O2 -ggdb -Wall -Wextra -Wno-unused-parameter -Wmissing-prototypes -ansi
LDFLAGS := -Wl,--no-undefined -Wl,--as-needed

override CFLAGS += -std=c99 -DGST_DISABLE_DEPRECATED

GST_CFLAGS := $(shell pkg-config --cflags gstreamer-0.10 gstreamer-base-0.10)
GST_LIBS := $(shell pkg-config --libs gstreamer-0.10 gstreamer-base-0.10)

all:

version := $(shell ./get-version)
prefix := /usr

D = $(DESTDIR)

gst_plugin := libgstxssim.so

$(gst_plugin): xssim.o
$(gst_plugin): override CFLAGS += $(GST_CFLAGS) -fPIC \
	-D VERSION='"$(version)"'
$(gst_plugin): override LIBS += $(GST_LIBS)

targets += $(gst_plugin)

all: $(targets)

# pretty print
ifndef V
QUIET_CC    = @echo '   CC         '$@;
QUIET_LINK  = @echo '   LINK       '$@;
QUIET_CLEAN = @echo '   CLEAN      '$@;
endif

install: $(targets)
	install -m 755 -D libgstxssim.so $(D)$(prefix)/lib/gstreamer-0.10/libgstxssim.so

%.o:: %.c
	$(QUIET_CC)$(CC) $(CFLAGS) -MMD -o $@ -c $<

%.so::
	$(QUIET_LINK)$(CC) $(LDFLAGS) -shared -o $@ $^ $(LIBS)

%.a::
	$(QUIET_LINK)$(AR) rcs $@ $^

clean:
	$(QUIET_CLEAN)$(RM) -v $(targets) *.o *.d

dist: base := gst-xssim-$(version)
dist:
	git archive --format=tar --prefix=$(base)/ HEAD > /tmp/$(base).tar
	mkdir -p $(base)
	echo $(version) > $(base)/.version
	chmod 664 $(base)/.version
	tar --append -f /tmp/$(base).tar --owner root --group root $(base)/.version
	rm -r $(base)
	gzip /tmp/$(base).tar

-include *.d
