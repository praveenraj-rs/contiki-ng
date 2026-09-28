CONTIKI_PROJECT = node
all: $(CONTIKI_PROJECT)

PLATFORMS_EXCLUDE = sky z1 native

CONTIKI=../../..

MAKE_MAC = MAKE_MAC_TSCH
MAKE_ROUTING = MAKE_ROUTING_RPL_LITE

# Energy usage estimation (periodic Energest log lines), used by
# run-analysis.py/extract_metrics.py for the charge/duty-cycle metrics.
MODULES += os/services/simple-energest

include $(CONTIKI)/Makefile.dir-variables

# Scheduler under test. One of:
#   minimal        - no 6P/Orchestra, just TSCH's built-in minimal schedule
#   orchestra-sb   - Orchestra, sender-based unicast cell placement
#   orchestra-rb   - Orchestra, receiver-based unicast cell placement
#   sf-mobility    - sf-mobility.c (mobility-adaptive 6P scheduling)
SCHEDULER ?= sf-mobility

ifeq ($(SCHEDULER),sf-mobility)
  PROJECT_SOURCEFILES += sf-mobility.c
  CFLAGS += -DSCHED_SF_MOBILITY=1 -DTSCH_CONF_WITH_SIXTOP=1
  MODULES += $(CONTIKI_NG_MAC_DIR)/tsch/sixtop
else ifeq ($(SCHEDULER),orchestra-sb)
  CFLAGS += -DSCHED_ORCHESTRA=1 \
            -DORCHESTRA_CONF_RULES="{&eb_per_time_source,&unicast_per_neighbor_rpl_ns,&default_common}" \
            -DORCHESTRA_CONF_UNICAST_SENDER_BASED=1
  MODULES += $(CONTIKI_NG_SERVICES_DIR)/orchestra
else ifeq ($(SCHEDULER),orchestra-rb)
  CFLAGS += -DSCHED_ORCHESTRA=1 \
            -DORCHESTRA_CONF_RULES="{&eb_per_time_source,&unicast_per_neighbor_rpl_ns,&default_common}" \
            -DORCHESTRA_CONF_UNICAST_SENDER_BASED=0
  MODULES += $(CONTIKI_NG_SERVICES_DIR)/orchestra
else ifeq ($(SCHEDULER),minimal)
  CFLAGS += -DSCHED_MINIMAL=1
else
  $(error Unknown SCHEDULER '$(SCHEDULER)': use minimal, orchestra-sb, orchestra-rb or sf-mobility)
endif

include $(CONTIKI)/Makefile.include
