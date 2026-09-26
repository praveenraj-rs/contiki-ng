/*
 * Mobility-adaptive MSF-like TSCH scheduling demo for Cooja.
 *
 * Sets up two slotframes:
 *  - handle 0: minimal/autonomous slotframe used for EB and shared
 *    broadcast/keep-alive traffic (like the 6TiSCH minimal schedule).
 *  - handle 1: negotiated unicast slotframe, entirely managed at runtime
 *    by sf-mobility.c through 6P ADD/DELETE with the RPL preferred parent.
 *
 * The application just generates periodic UDP traffic towards the DAG
 * root; all scheduling decisions (including reacting to mobility/parent
 * switches) are handled autonomously by sf-mobility.
 */

#include "contiki.h"
#include "net/ipv6/simple-udp.h"
#include "net/mac/tsch/tsch.h"
#include "net/routing/routing.h"
#include "lib/random.h"
#include "sys/node-id.h"
#include "services/simple-energest/simple-energest.h"

#include "sf-mobility.h"

#include <inttypes.h>
#include "sys/log.h"
#define LOG_MODULE "App"
#define LOG_LEVEL LOG_LEVEL_INFO

#define COORDINATOR_ID 1

#define UDP_PORT          8765

/* Autonomous/minimal slotframe: EB + shared broadcast at slot 0. */
#define MINIMAL_SLOTFRAME_HANDLE 0
#define MINIMAL_SLOTFRAME_SIZE   TSCH_SCHEDULE_CONF_DEFAULT_LENGTH

PROCESS(node_process, "Mobility-adaptive MSF node");
AUTOSTART_PROCESSES(&node_process);

static struct simple_udp_connection udp_conn;

static void
setup_minimal_schedule(void)
{
  struct tsch_slotframe *sf_min =
    tsch_schedule_add_slotframe(MINIMAL_SLOTFRAME_HANDLE,
                                MINIMAL_SLOTFRAME_SIZE);

  /* Shared cell at (0, 0) for EB / broadcast / new-neighbor discovery,
   * as in the 6TiSCH minimal schedule. */
  tsch_schedule_add_link(sf_min,
                         LINK_OPTION_RX | LINK_OPTION_TX | LINK_OPTION_SHARED,
                         LINK_TYPE_ADVERTISING, &tsch_broadcast_address,
                         0, 0, 1);

  /* Negotiated slotframe: starts empty, sf-mobility populates it via 6P. */
  tsch_schedule_add_slotframe(SF_MOBILITY_SLOTFRAME_HANDLE,
                              MINIMAL_SLOTFRAME_SIZE);
}

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
    /* Kept in the same format as examples/benchmarks/result-visualization,
     * so run-analysis.py can be reused unmodified. */
    LOG_INFO("app receive packet seqnum=%" PRIu32 " from=", seqnum);
    LOG_INFO_6ADDR(sender_addr);
    LOG_INFO_("\n");
  }
}

PROCESS_THREAD(node_process, ev, data)
{
  static struct etimer periodic_timer;
  static uint32_t seqnum;
  uip_ipaddr_t dst;

  PROCESS_BEGIN();

  setup_minimal_schedule();
  sixtop_add_sf(&sf_mobility_driver);
  NETSTACK_MAC.on();
  simple_energest_init();

  simple_udp_register(&udp_conn, UDP_PORT, NULL, UDP_PORT, rx_packet);

  if(node_id == COORDINATOR_ID) {
    LOG_INFO("set as root\n");
    NETSTACK_ROUTING.root_start();
  } else {
    etimer_set(&periodic_timer, APP_WARM_UP_PERIOD_SEC * CLOCK_SECOND
               + random_rand() % (APP_SEND_INTERVAL_SEC * CLOCK_SECOND));

    while(1) {
      PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&periodic_timer));
      if(NETSTACK_ROUTING.node_is_reachable() &&
         NETSTACK_ROUTING.get_root_ipaddr(&dst)) {
        seqnum++;
        /* Kept in the same format as examples/benchmarks/result-visualization,
         * so run-analysis.py can be reused unmodified. */
        LOG_INFO("app generate packet seqnum=%" PRIu32 " node_id=%u\n",
                 seqnum, node_id);
        simple_udp_sendto(&udp_conn, &seqnum, sizeof(seqnum), &dst);
      }
      etimer_set(&periodic_timer, APP_SEND_INTERVAL_SEC * CLOCK_SECOND);
    }
  }

  PROCESS_END();
}
