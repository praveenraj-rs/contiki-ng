CONTIKI_PROJECT = node
all: $(CONTIKI_PROJECT)

PLATFORMS_EXCLUDE = sky z1 native

CONTIKI=../../..

MAKE_MAC = MAKE_MAC_TSCH
MAKE_ROUTING = MAKE_ROUTING_RPL_CLASSIC

# Energy usage estimation
MODULES += os/services/simple-energest

include $(CONTIKI)/Makefile.dir-variables

# Scheduler under test, one folder per scheduler in schedulers/ with:
#   scheduler.mk       extra modules / sources
#   scheduler-conf.h   extra configuration, included from project-conf.h
# Select with: make SCHEDULER=<name> ... (or the SCHEDULER environment variable)
SCHEDULER ?= minimal
SCHEDULER_DIR = schedulers/$(SCHEDULER)
ifeq ($(wildcard $(SCHEDULER_DIR)/scheduler.mk),)
$(error Unknown SCHEDULER "$(SCHEDULER)": no $(SCHEDULER_DIR)/scheduler.mk)
endif
PROJECTDIRS += $(SCHEDULER_DIR)
CFLAGS += -DBENCH_SCHEDULER_NAME=\"$(SCHEDULER)\"
CFLAGS += -DBENCH_SCHEDULER_CONF=\"scheduler-conf.h\"
include $(SCHEDULER_DIR)/scheduler.mk

include $(CONTIKI)/Makefile.include
