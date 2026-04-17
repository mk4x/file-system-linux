GCC = gcc
SOURCES = pifs.c
OBJS := $(patsubst %.c,%.o,$(SOURCES))
CFLAGS = -O2 -Wall -D_FILE_OFFSET_BITS=64 -DFUSE_USE_VERSION=25

.PHONY: pifs

##
# Libs 
##
LIBS := fuse 
LIBS := $(addprefix -l,$(LIBS))

all: pifs

%.o: %.c
	$(GCC) $(CFLAGS) -c -o $@ $<

pifs: $(OBJS)
	$(GCC) $(OBJS) $(LIBS) $(CFLAGS) -o pifs

clean:
	rm -f $(OBJS) lfs
