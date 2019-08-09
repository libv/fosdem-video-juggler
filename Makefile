CFLAGS += -Wall -Iinclude -O3
LDFLAGS += -pthread

# add drm
CFLAGS += $(shell pkg-config --cflags libdrm)
DRM_LDFLAGS = $(shell pkg-config --libs libdrm)

LDFLAGS += $(DRM_LDFLAGS)

all: juggler

juggler: kms.o capture.o juggler.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

clean:
	rm -f juggler
	rm -f *.o
	rm -f *.P

%.o : %.c
	$(COMPILE.c) -MD -o $@ $<
	@cp $*.d $*.P; \
	    sed -e 's/#.*//' -e 's/^[^:]*: *//' -e 's/ *\\$$//' \
	        -e '/^$$/ d' -e 's/$$/ :/' < $*.d >> $*.P; \
	    rm -f $*.d

-include $(OBJS:%.o=%.P)
