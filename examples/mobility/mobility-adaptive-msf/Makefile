CONTIKI_PROJECT = node
all: $(CONTIKI_PROJECT)

PLATFORMS_EXCLUDE = sky z1 native

CONTIKI=../../..

MAKE_MAC = MAKE_MAC_TSCH
MAKE_ROUTING = MAKE_ROUTING_RPL_CLASSIC

# Energy usage estimation (periodic Energest log lines), used by
# run-analysis.py for the charge/duty-cycle plots.
MODULES += os/services/simple-energest

include $(CONTIKI)/Makefile.dir-variables

# Select the scheduling function in the same way as benchmark-tests. Cooja
# invokes make from the simulation file, so keep MA-MSF as the default.
SCHEDULER ?= ma-msf
SCHEDULER_DIR = scheduler/$(SCHEDULER)
ifeq ($(wildcard $(SCHEDULER_DIR)/scheduler.mk),)
$(error Unknown SCHEDULER "$(SCHEDULER)": no $(SCHEDULER_DIR)/scheduler.mk)
endif
PROJECTDIRS += $(SCHEDULER_DIR)
CFLAGS += -DBENCH_SCHEDULER_NAME=\"$(SCHEDULER)\"
CFLAGS += -DBENCH_SCHEDULER_CONF=\"scheduler-conf.h\"
include $(SCHEDULER_DIR)/scheduler.mk

include $(CONTIKI)/Makefile.include
