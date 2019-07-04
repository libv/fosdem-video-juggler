CFLAGS += -Wall -Iinclude -O3

# add drm
CFLAGS += $(shell pkg-config --cflags libdrm)
DRM_LDFLAGS = $(shell pkg-config --libs libdrm)

all: hdmi_output hdmi_capture

hdmi_output: output.o
	$(CC) $(CFLAGS) $(DRM_LDFLAGS) -o $@ $^

hdmi_capture: capture.o
	$(CC) $(CFLAGS) $(V4L2_LDFLAGS) -o $@ $^

clean:
	rm -f hdmi_output
	rm -f hdmi_capture
	rm -f *.o
	rm -f *.P

%.o : %.c
	$(COMPILE.c) -MD -o $@ $<
	@cp $*.d $*.P; \
	    sed -e 's/#.*//' -e 's/^[^:]*: *//' -e 's/ *\\$$//' \
	        -e '/^$$/ d' -e 's/$$/ :/' < $*.d >> $*.P; \
	    rm -f $*.d

-include $(OBJS:%.o=%.P)
