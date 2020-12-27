CFLAGS += -Wall -Iinclude -O0 -g
LDFLAGS += -pthread

# add drm
CFLAGS += $(shell pkg-config --cflags libdrm)
LDFLAGS += $(shell pkg-config --libs libdrm)

CFLAGS += $(shell pkg-config --cflags libpng)
LDFLAGS += $(shell pkg-config --libs libpng)

all: juggler test_output demp_test

juggler_objects = \
	kms.o \
	status.o \
	projector.o \
	capture.o \
	juggler.o

juggler: $(juggler_objects)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

test_output_objects = \
	kms.o \
	test.o

test_output: $(test_output_objects)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

demp_test_objects = \
	kms.o \
	demp.o

demp_test: $(demp_test_objects)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

clean:
	rm -f juggler
	rm -f test_output
	rm -f demp_test
	rm -f *.o
	rm -f *.P

%.o : %.c
	$(COMPILE.c) -MD -o $@ $<
	@cp $*.d $*.P; \
	    sed -e 's/#.*//' -e 's/^[^:]*: *//' -e 's/ *\\$$//' \
	        -e '/^$$/ d' -e 's/$$/ :/' < $*.d >> $*.P; \
	    rm -f $*.d

-include $(juggler_objects:%.o=%.P)
-include $(test_output_objects:%.o=%.P)
-include $(demp_test_objects:%.o=%.P)
