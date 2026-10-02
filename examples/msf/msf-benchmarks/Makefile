CONTIKI_PROJECT = node
all: $(CONTIKI_PROJECT)

PLATFORMS_EXCLUDE = sky native

CONTIKI=../../..

MAKE_MAC = MAKE_MAC_TSCH
MAKE_ROUTING = MAKE_ROUTING_RPL_CLASSIC

# 6TiSCH Minimal Scheduling Function (RFC 9033)
PROJECT_SOURCEFILES += msf.c

# Energy usage estimation
MODULES += os/services/simple-energest

include $(CONTIKI)/Makefile.dir-variables
MODULES += $(CONTIKI_NG_MAC_DIR)/tsch/sixtop
include $(CONTIKI)/Makefile.include
