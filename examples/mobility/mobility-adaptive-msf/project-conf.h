#ifndef PROJECT_CONF_H_
#define PROJECT_CONF_H_

/* Enable Sixtop and let sf-mobility manage the negotiated slotframe */
#define TSCH_CONF_WITH_SIXTOP 1

/* IEEE802.15.4 PANID */
#define IEEE802154_CONF_PANID 0xabcd

/* Do not start TSCH at init, wait for NETSTACK_MAC.on() in node.c */
#define TSCH_CONF_AUTOSTART 0

/* 6TiSCH schedule length: slotframe 0 (minimal/autonomous) and slotframe 1
 * (negotiated, managed by sf-mobility) share the same length here for
 * simplicity. */
#define TSCH_SCHEDULE_CONF_DEFAULT_LENGTH 11

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

/* Application settings */
#define APP_SEND_INTERVAL_SEC 10
#define APP_WARM_UP_PERIOD_SEC 120

#endif /* PROJECT_CONF_H_ */
