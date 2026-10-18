/*
 * Mobility-adaptive 6TiSCH demo. MA-MSF owns TSCH schedule setup and
 * negotiation; this application generates periodic UDP traffic to the root.
 */

#include "contiki.h"
#include "net/ipv6/simple-udp.h"
#include "net/routing/routing.h"
#include "lib/random.h"
#include "sys/node-id.h"

#ifdef BENCH_SCHEDULER_INCLUDE
#include BENCH_SCHEDULER_INCLUDE
#endif

#include <inttypes.h>
#include "sys/log.h"
#define LOG_MODULE "App"
#define LOG_LEVEL LOG_LEVEL_INFO

#define COORDINATOR_ID 1

#define UDP_PORT          8765

PROCESS(node_process, "Mobility-adaptive MSF node");
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

  LOG_INFO("scheduler: %s\n", BENCH_SCHEDULER_NAME);
#ifdef BENCH_SCHEDULER_INIT
  BENCH_SCHEDULER_INIT();
#endif

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
