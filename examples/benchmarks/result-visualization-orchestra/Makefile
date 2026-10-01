CONTIKI_PROJECT = node
all: $(CONTIKI_PROJECT)

PLATFORMS_EXCLUDE = sky native

CONTIKI=../../..

MAKE_MAC = MAKE_MAC_TSCH
MAKE_ROUTING = MAKE_ROUTING_RPL_CLASSIC

include $(CONTIKI)/Makefile.dir-variables

# Orchestra autonomous scheduler (sender/receiver-based is set in project-conf.h)
MODULES += $(CONTIKI_NG_SERVICES_DIR)/orchestra

# Energy usage estimation
MODULES += os/services/simple-energest

include $(CONTIKI)/Makefile.include
