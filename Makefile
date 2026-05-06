GCC = gcc
PIFS_SOURCES = pifs.c
PIFS_OBJS := $(patsubst %.c,%.o,$(PIFS_SOURCES))
FORMAT_SOURCES = format_pifs.c
FORMAT_OBJS := $(patsubst %.c,%.o,$(FORMAT_SOURCES))
CFLAGS = -O2 -Wall -D_FILE_OFFSET_BITS=64 -DFUSE_USE_VERSION=25

.PHONY: all clean

##
# Libs 
##
LIBS := fuse 
LIBS := $(addprefix -l,$(LIBS))

all: pifs format_pifs

%.o: %.c
	$(GCC) $(CFLAGS) -c -o $@ $<

pifs: $(PIFS_OBJS)
	$(GCC) $(PIFS_OBJS) $(LIBS) $(CFLAGS) -o pifs

format_pifs: $(FORMAT_OBJS)
	$(GCC) $(FORMAT_OBJS) $(CFLAGS) -o format_pifs

clean:
	rm -f $(PIFS_OBJS) $(FORMAT_OBJS) pifs format_pifs
