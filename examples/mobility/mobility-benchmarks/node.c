/*
 * Scheduler-comparison benchmark app for Cooja: the SAME traffic pattern
 * and log format run under three schedulers, selected purely by the
 * Makefile's SCHEDULER variable (minimal | orchestra-sb | orchestra-rb |
 * sf-mobility), so results are comparable apples-to-apples:
 *
 *  - minimal:      no 6P/Orchestra at all; relies on TSCH's built-in
 *                  6TiSCH minimal schedule (one shared cell).
 *  - orchestra-sb: Orchestra, sender-based unicast cell placement.
 *  - orchestra-rb: Orchestra, receiver-based unicast cell placement.
 *  - sf-mobility:  sf-mobility.c, mobility-adaptive 6P scheduling.
 *
 * Traffic pattern models an industrial AGV/mobile robot:
 *  - periodic small telemetry (position/battery stand-in) every
 *    APP_SEND_INTERVAL_SEC.
 *  - a bursty "pick/drop" event every AGV_EVENT_INTERVAL_SEC: several
 *    packets sent back-to-back, as at a station stop.
 *  - periodic ETX-to-parent logging, for the ETX comparison metric.
 *
 * Log lines are kept in the exact format expected by
 * examples/benchmarks/result-visualization/run-analysis.py (and this
 * folder's extract_metrics.py, which extends it with latency/ETX).
 */

#include "contiki.h"
#include "net/ipv6/simple-udp.h"
#include "net/ipv6/uip-ds6.h"
#include "net/mac/tsch/tsch.h"
#include "net/link-stats.h"
#include "net/routing/routing.h"
#include "lib/random.h"
#include "sys/node-id.h"
#include "services/simple-energest/simple-energest.h"

#if SCHED_SF_MOBILITY
#include "sf-mobility.h"
#endif

#include <inttypes.h>
#include "sys/log.h"
#define LOG_MODULE "App"
#define LOG_LEVEL LOG_LEVEL_INFO

#define COORDINATOR_ID 1

#define UDP_PORT 8765

/* AGV "pick/drop" burst event, on top of the periodic telemetry. */
#ifndef AGV_EVENT_INTERVAL_SEC
#define AGV_EVENT_INTERVAL_SEC 60
#endif
#ifndef AGV_EVENT_BURST_COUNT
#define AGV_EVENT_BURST_COUNT 3
#endif

PROCESS(node_process, "Scheduler-benchmark node");
AUTOSTART_PROCESSES(&node_process);

static struct simple_udp_connection udp_conn;

static void
rx_packet(struct simple_udp_connection *c,
          const uip_ipaddr_t *sender_addr,
          uint16_t sender_port,
          const uip_ipaddr_t *receiver_addr,
          uint16_t receiver_port,
          const uint8_t *data,
          uint16_t datalen)
{
  uint32_t seqnum;

  if(datalen >= sizeof(seqnum)) {
    memcpy(&seqnum, data, sizeof(seqnum));
    /* Same format as examples/benchmarks/result-visualization, so both its
     * run-analysis.py and this folder's extract_metrics.py work unmodified. */
    LOG_INFO("app receive packet seqnum=%" PRIu32 " from=", seqnum);
    LOG_INFO_6ADDR(sender_addr);
    LOG_INFO_("\n");
  }
}

static void
send_packet(uip_ipaddr_t *dst, uint32_t seqnum)
{
  LOG_INFO("app generate packet seqnum=%" PRIu32 " node_id=%u\n",
           seqnum, node_id);
  simple_udp_sendto(&udp_conn, &seqnum, sizeof(seqnum), dst);
}

static void
log_etx_to_parent(const uip_ipaddr_t *dst)
{
  const uip_lladdr_t *lladdr = uip_ds6_nbr_lladdr_from_ipaddr(dst);
  const struct link_stats *stats =
    lladdr != NULL ? link_stats_from_lladdr((const linkaddr_t *)lladdr) : NULL;
  if(stats != NULL) {
    LOG_INFO("etx to parent=%u\n", stats->etx);
  }
}

PROCESS_THREAD(node_process, ev, data)
{
  static struct etimer periodic_timer;
  static struct etimer event_timer;
  static uint32_t seqnum;
  static uint8_t burst_remaining;
  uip_ipaddr_t dst;

  PROCESS_BEGIN();

#if SCHED_SF_MOBILITY
  /* Only sf-mobility needs 6top; minimal/Orchestra configure themselves
   * automatically (TSCH's built-in minimal schedule, resp. BUILD_WITH_ORCHESTRA). */
  sixtop_add_sf(&sf_mobility_driver);
#endif
  NETSTACK_MAC.on();
  simple_energest_init();

  simple_udp_register(&udp_conn, UDP_PORT, NULL, UDP_PORT, rx_packet);

  if(node_id == COORDINATOR_ID) {
    LOG_INFO("set as root\n");
    NETSTACK_ROUTING.root_start();
  } else {
    etimer_set(&periodic_timer, APP_WARM_UP_PERIOD_SEC * CLOCK_SECOND
               + random_rand() % (APP_SEND_INTERVAL_SEC * CLOCK_SECOND));
    etimer_set(&event_timer, APP_WARM_UP_PERIOD_SEC * CLOCK_SECOND
               + AGV_EVENT_INTERVAL_SEC * CLOCK_SECOND);
    burst_remaining = 0;

    while(1) {
      PROCESS_WAIT_EVENT();

      if(etimer_expired(&periodic_timer)) {
        if(NETSTACK_ROUTING.node_is_reachable() &&
           NETSTACK_ROUTING.get_root_ipaddr(&dst)) {
          seqnum++;
          send_packet(&dst, seqnum);
          log_etx_to_parent(&dst);
        }
        etimer_set(&periodic_timer, APP_SEND_INTERVAL_SEC * CLOCK_SECOND);
      }

      if(etimer_expired(&event_timer)) {
        /* Station stop: pick/drop burst instead of a single telemetry packet. */
        burst_remaining = AGV_EVENT_BURST_COUNT;
        etimer_set(&event_timer, AGV_EVENT_INTERVAL_SEC * CLOCK_SECOND);
      }

      if(burst_remaining > 0 &&
         NETSTACK_ROUTING.node_is_reachable() &&
         NETSTACK_ROUTING.get_root_ipaddr(&dst)) {
        seqnum++;
        send_packet(&dst, seqnum);
        burst_remaining--;
      }
    }
  }

  PROCESS_END();
}
