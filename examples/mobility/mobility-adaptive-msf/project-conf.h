#ifndef PROJECT_CONF_H_
#define PROJECT_CONF_H_

/*
 * Keep protocol, logging, and traffic settings aligned with
 * examples/benchmarks/benchmark-tests/project-conf.h. Mobility-specific
 * sensing is configured by the selected MA-MSF scheduler.
 */

/* Logging */
#define LOG_CONF_LEVEL_RPL                         LOG_LEVEL_INFO
#define LOG_CONF_LEVEL_TCPIP                       LOG_LEVEL_WARN
#define LOG_CONF_LEVEL_IPV6                        LOG_LEVEL_WARN
#define LOG_CONF_LEVEL_6LOWPAN                     LOG_LEVEL_WARN
#define LOG_CONF_LEVEL_MAC                         LOG_LEVEL_INFO
#define TSCH_LOG_CONF_PER_SLOT                     0

/* Enable printing of packet counters */
#define LINK_STATS_CONF_PACKET_COUNTERS          1

/* Application settings */
#define APP_SEND_INTERVAL_SEC 10
#define APP_WARM_UP_PERIOD_SEC 120

#define SICSLOWPAN_CONF_FRAG 0
#define UIP_CONF_BUFFER_SIZE 200

/* Scheduler-specific settings, chosen with SCHEDULER=<name>. */
#ifdef BENCH_SCHEDULER_CONF
#include BENCH_SCHEDULER_CONF
#endif

#endif /* PROJECT_CONF_H_ */
