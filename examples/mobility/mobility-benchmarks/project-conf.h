#ifndef PROJECT_CONF_H_
#define PROJECT_CONF_H_

/* TSCH_CONF_WITH_SIXTOP is scheduler-dependent (only sf-mobility needs
 * 6top/6P) - set by the Makefile's SCHEDULER branch, not here. */

/* IEEE802.15.4 PANID */
#define IEEE802154_CONF_PANID 0xabcd

/* Do not start TSCH at init, wait for NETSTACK_MAC.on() in node.c */
#define TSCH_CONF_AUTOSTART 0

/* 6TiSCH schedule length - the primary X-axis for the slotframe-length
 * sweep; override with DEFINES=TSCH_SCHEDULE_CONF_DEFAULT_LENGTH=<n>. */
#ifndef TSCH_SCHEDULE_CONF_DEFAULT_LENGTH
#define TSCH_SCHEDULE_CONF_DEFAULT_LENGTH 11
#endif

/* Make link-stats react a bit faster to help the mobility score, while
 * still smoothing out single-packet noise. */
#ifndef LINK_STATS_CONF_ETX_FROM_PACKET_COUNT
#define LINK_STATS_CONF_ETX_FROM_PACKET_COUNT 1
#endif

/* Cooja motes moving around need to (re)join quickly after a parent
 * switch: keep RPL probing interval reasonably low for this demo. */
#define RPL_CONF_DIO_INTERVAL_MIN 10

/* Logging + benchmarking: same settings as
 * examples/benchmarks/result-visualization, so its run-analysis.py can be
 * reused unmodified against this example's COOJA.testlog. */
#define LOG_CONF_LEVEL_RPL     LOG_LEVEL_INFO
#define LOG_CONF_LEVEL_MAC     LOG_LEVEL_INFO
#define LOG_CONF_LEVEL_TCPIP   LOG_LEVEL_WARN
#define LOG_CONF_LEVEL_IPV6    LOG_LEVEL_WARN
#define LOG_CONF_LEVEL_6LOWPAN LOG_LEVEL_WARN
#define TSCH_LOG_CONF_PER_SLOT 0

/* Enable "num packets: tx=... ack=... rx=... queue_drops=... to=..." logs,
 * used by run-analysis.py for the PAR/queue-drop metrics. */
#define LINK_STATS_CONF_PACKET_COUNTERS 1

/* Mobility-score source for sf-mobility (see update_mobility_score() in
 * sf-mobility.c):
 *   1 = ground-truth Cooja-IMU injection, poked by imu-mobility-logger.js
 *   0 = RSSI/ETX heuristic fallback (no JS script/mote-memory access needed)
 * Left overridable (not a bare #define) so a single command-line switch
 * selects the test case without editing this file, e.g.:
 *   make TARGET=cooja node.cooja DEFINES=SF_MOBILITY_CONF_WITH_COOJA_IMU=0
 */
#ifndef SF_MOBILITY_CONF_WITH_COOJA_IMU
#define SF_MOBILITY_CONF_WITH_COOJA_IMU 1
#endif

/* Application settings - the primary X-axis for the traffic-load sweep;
 * override with DEFINES=APP_SEND_INTERVAL_SEC=<n>. */
#ifndef APP_SEND_INTERVAL_SEC
#define APP_SEND_INTERVAL_SEC 10
#endif
#ifndef APP_WARM_UP_PERIOD_SEC
#define APP_WARM_UP_PERIOD_SEC 120
#endif

#endif /* PROJECT_CONF_H_ */
