CONTIKI_PROJECT = node
all: $(CONTIKI_PROJECT)

PLATFORMS_EXCLUDE = sky z1 native

PROJECT_SOURCEFILES += sf-mobility.c
CONTIKI=../../..

MAKE_MAC = MAKE_MAC_TSCH
MAKE_ROUTING = MAKE_ROUTING_RPL_LITE

# Energy usage estimation (periodic Energest log lines), used by
# run-analysis.py for the charge/duty-cycle plots.
MODULES += os/services/simple-energest

include $(CONTIKI)/Makefile.dir-variables
MODULES += $(CONTIKI_NG_MAC_DIR)/tsch/sixtop

include $(CONTIKI)/Makefile.include
