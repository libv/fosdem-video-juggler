CFLAGS += -Wall -Iinclude -O3 -g
LDFLAGS += -pthread

# add drm
CFLAGS += $(shell pkg-config --cflags libdrm)
LDFLAGS += $(shell pkg-config --libs libdrm)

CFLAGS += $(shell pkg-config --cflags libpng)
LDFLAGS += $(shell pkg-config --libs libpng)

all: juggler

juggler_objects = \
	kms.o \
	status.o \
	projector.o \
	capture.o \
	juggler.o

juggler: $(juggler_objects)
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
