/*
 * Mobility- and traffic-adaptive 6P Scheduling Function (sf-mobility).
 *
 * Design summary
 * ---------------
 * - Cell negotiation itself reuses the well-tested 6P ADD/DELETE exchange
 *   pattern from the sf-simple example (single feasible-cell negotiation,
 *   random free-slot proposal).
 * - On top of that, a housekeeping process runs periodically and decides,
 *   autonomously (no application involvement needed), whether the node
 *   should request one more Tx cell to its RPL preferred parent, release
 *   one, or leave the schedule untouched. The decision combines:
 *     1. Traffic pressure: number of packets currently queued for the
 *        parent (tsch_queue_nbr_packet_count()).
 *     2. Link quality trend: ETX and RSSI reported by link-stats.
 *     3. A "mobility score" (0-100) derived from RSSI volatility and ETX
 *        degradation speed, plus an immediate boost on parent switch
 *        (detected by comparing the current time-source neighbor address
 *        against the previous housekeeping round).
 *   A node whose mobility score is high negotiates spare Tx capacity
 *   earlier (before its queue actually backs up) and is reluctant to give
 *   cells back up, trading a little energy for lower queue/frame loss and
 *   faster recovery after a parent switch. A static, stable node instead
 *   converges to the minimum number of cells, saving energy - similar in
 *   spirit to MSF, but with mobility as an explicit extra input to the
 *   "add a cell"/"delete a cell" rules.
 */

#include "contiki.h"
#include "contiki-lib.h"
#include "lib/assert.h"
#include "lib/random.h"

#include "net/link-stats.h"
#include "net/mac/tsch/tsch.h"
#include "net/mac/tsch/tsch-queue.h"
#include "net/mac/tsch/sixtop/sixtop.h"
#include "net/mac/tsch/sixtop/sixp.h"
#include "net/mac/tsch/sixtop/sixp-nbr.h"
#include "net/mac/tsch/sixtop/sixp-pkt.h"
#include "net/mac/tsch/sixtop/sixp-trans.h"

#include "sf-mobility.h"

#define DEBUG DEBUG_PRINT
#include "net/net-debug.h"

/* ------------------------------------------------------------------------ */
/* Tunable parameters                                                      */
/* ------------------------------------------------------------------------ */

/* How often the adaptive housekeeping logic re-evaluates the schedule. */
#ifndef SF_MOBILITY_HOUSEKEEPING_PERIOD
#define SF_MOBILITY_HOUSEKEEPING_PERIOD (10 * CLOCK_SECOND)
#endif

/* Queue occupancy (packets buffered for the parent) that alone justifies
 * requesting one more Tx cell. */
#define SF_MOBILITY_QUEUE_HIGH_THRESHOLD 2

/* Mobility score (0-100) thresholds used to bias the add/delete decision. */
#define SF_MOBILITY_SCORE_HIGH   55
#define SF_MOBILITY_SCORE_LOW    15

/* Number of consecutive idle/stable rounds required before a spare cell is
 * released, to avoid schedule oscillation ("cell churn"). */
#define SF_MOBILITY_IDLE_ROUNDS_TO_SHRINK 3

/* RSSI is considered "unknown" using the same sentinel as link-stats. */
#ifndef LINK_STATS_RSSI_UNKNOWN
#define LINK_STATS_RSSI_UNKNOWN -128
#endif

/*
 * Ground-truth mobility source. Real position/velocity is only known to the
 * simulator (exactly as a real IMU chip's reading is only known to the
 * hardware, not to peer motes), so instead of *inferring* mobility from
 * RSSI/ETX side effects, a Cooja test script (imu-mobility-logger.js) pokes
 * these two globals directly from each mote's simulated Position, the same
 * way a real IMU driver would populate hardware registers for this code to
 * read. Kept non-static so their symbols are visible to Cooja's VarMemory
 * (poke-a-C-global-by-name). On real hardware, replace the assignment with
 * actual accelerometer/gyroscope readings.
 */
uint8_t cooja_imu_is_moving;     /* 0/1, ground truth, set externally */
uint16_t cooja_imu_speed_mm_s;   /* approx. mote speed in mm/s, set externally */

#ifndef SF_MOBILITY_CONF_WITH_COOJA_IMU
#if CONTIKI_TARGET_COOJA
#define SF_MOBILITY_CONF_WITH_COOJA_IMU 1
#else
#define SF_MOBILITY_CONF_WITH_COOJA_IMU 0
#endif
#endif

/* ------------------------------------------------------------------------ */
/* State                                                                    */
/* ------------------------------------------------------------------------ */

typedef struct {
  uint16_t timeslot_offset;
  uint16_t channel_offset;
} sf_mobility_cell_t;

static const uint16_t slotframe_handle = SF_MOBILITY_SLOTFRAME_HANDLE;
static uint8_t res_storage[4 + SF_MOBILITY_MAX_CELLS * 4];
static uint8_t req_storage[4 + SF_MOBILITY_MAX_CELLS * 4];

/* Bookkeeping for the single link we actively manage: the RPL/TSCH
 * preferred parent (time source). Extending this to every neighbor is a
 * matter of moving this state into a small nbr-table-backed struct; the
 * parent is by far the dominant traffic sink in RPL+TSCH deployments, so
 * this is where mobility-awareness matters most. */
static linkaddr_t current_parent;
static uint8_t have_parent = 0;
static uint8_t num_negotiated_tx_cells = 0;
static uint8_t idle_rounds = 0;

static int16_t prev_rssi = LINK_STATS_RSSI_UNKNOWN;
static uint16_t prev_etx = 0;
static uint8_t mobility_score = 0; /* 0 (static) .. 100 (highly mobile) */

PROCESS(sf_mobility_housekeeping_process, "sf-mobility housekeeping");

/* ------------------------------------------------------------------------ */
/* Cell (de)serialization helpers, mirrored from sf-simple                  */
/* ------------------------------------------------------------------------ */

static void
read_cell(const uint8_t *buf, sf_mobility_cell_t *cell)
{
  cell->timeslot_offset = buf[0] + (buf[1] << 8);
  cell->channel_offset = buf[2] + (buf[3] << 8);
}

static void
print_cell_list(const uint8_t *cell_list, uint16_t cell_list_len)
{
  uint16_t i;
  sf_mobility_cell_t cell;

  for(i = 0; i < cell_list_len; i += sizeof(cell)) {
    read_cell(&cell_list[i], &cell);
    PRINTF("%u ", cell.timeslot_offset);
  }
}

static void
add_links_to_schedule(const linkaddr_t *peer_addr, uint8_t link_option,
                      const uint8_t *cell_list, uint16_t cell_list_len)
{
  /* add only the first valid cell */
  sf_mobility_cell_t cell;
  struct tsch_slotframe *slotframe;
  int i;

  assert(cell_list != NULL);

  slotframe = tsch_schedule_get_slotframe_by_handle(slotframe_handle);
  if(slotframe == NULL) {
    return;
  }

  for(i = 0; i < cell_list_len; i += sizeof(cell)) {
    read_cell(&cell_list[i], &cell);
    if(cell.timeslot_offset == 0xffff) {
      continue;
    }

    PRINTF("sf-mobility: Schedule link %d as %s with node ",
           cell.timeslot_offset,
           link_option == LINK_OPTION_RX ? "RX" : "TX");
    PRINTLLADDR((uip_lladdr_t *)peer_addr);
    PRINTF("\n");
    tsch_schedule_add_link(slotframe,
                           link_option, LINK_TYPE_NORMAL, peer_addr,
                           cell.timeslot_offset, cell.channel_offset, 1);
    if(link_option == LINK_OPTION_TX) {
      num_negotiated_tx_cells++;
    }
    break;
  }
}

static void
remove_links_to_schedule(const uint8_t *cell_list, uint16_t cell_list_len)
{
  /* remove all the cells */
  sf_mobility_cell_t cell;
  struct tsch_slotframe *slotframe;
  int i;

  assert(cell_list != NULL);

  slotframe = tsch_schedule_get_slotframe_by_handle(slotframe_handle);
  if(slotframe == NULL) {
    return;
  }

  for(i = 0; i < cell_list_len; i += sizeof(cell)) {
    read_cell(&cell_list[i], &cell);
    if(cell.timeslot_offset == 0xffff) {
      continue;
    }
    if(tsch_schedule_get_link_by_offsets(slotframe, cell.timeslot_offset,
                                        cell.channel_offset) != NULL &&
       num_negotiated_tx_cells > 0) {
      num_negotiated_tx_cells--;
    }
    tsch_schedule_remove_link_by_offsets(slotframe,
                                         cell.timeslot_offset,
                                         cell.channel_offset);
  }
}

/* ------------------------------------------------------------------------ */
/* 6P response callbacks                                                    */
/* ------------------------------------------------------------------------ */

static void
add_response_sent_callback(void *arg, uint16_t arg_len,
                           const linkaddr_t *dest_addr,
                           sixp_output_status_t status)
{
  uint8_t *body = (uint8_t *)arg;
  uint16_t body_len = arg_len;
  const uint8_t *cell_list;
  uint16_t cell_list_len;
  sixp_nbr_t *nbr;

  assert(body != NULL && dest_addr != NULL);

  if(status == SIXP_OUTPUT_STATUS_SUCCESS &&
     sixp_pkt_get_cell_list(SIXP_PKT_TYPE_RESPONSE,
                            (sixp_pkt_code_t)(uint8_t)SIXP_PKT_RC_SUCCESS,
                            &cell_list, &cell_list_len,
                            body, body_len) == 0 &&
     (nbr = sixp_nbr_find(dest_addr)) != NULL) {
    add_links_to_schedule(dest_addr, LINK_OPTION_RX, cell_list, cell_list_len);
  }
}

static void
delete_response_sent_callback(void *arg, uint16_t arg_len,
                              const linkaddr_t *dest_addr,
                              sixp_output_status_t status)
{
  uint8_t *body = (uint8_t *)arg;
  uint16_t body_len = arg_len;
  const uint8_t *cell_list;
  uint16_t cell_list_len;
  sixp_nbr_t *nbr;

  assert(body != NULL && dest_addr != NULL);

  if(status == SIXP_OUTPUT_STATUS_SUCCESS &&
     sixp_pkt_get_cell_list(SIXP_PKT_TYPE_RESPONSE,
                            (sixp_pkt_code_t)(uint8_t)SIXP_PKT_RC_SUCCESS,
                            &cell_list, &cell_list_len,
                            body, body_len) == 0 &&
     (nbr = sixp_nbr_find(dest_addr)) != NULL) {
    remove_links_to_schedule(cell_list, cell_list_len);
  }
}

/* ------------------------------------------------------------------------ */
/* 6P request handlers (we are the responder, e.g. we are someone's parent) */
/* ------------------------------------------------------------------------ */

static void
add_req_input(const uint8_t *body, uint16_t body_len, const linkaddr_t *peer_addr)
{
  uint8_t i;
  sf_mobility_cell_t cell;
  struct tsch_slotframe *slotframe;
  int feasible_link;
  uint8_t num_cells;
  const uint8_t *cell_list;
  uint16_t cell_list_len;
  uint16_t res_len;

  assert(body != NULL && peer_addr != NULL);

  if(sixp_pkt_get_num_cells(SIXP_PKT_TYPE_REQUEST,
                            (sixp_pkt_code_t)(uint8_t)SIXP_PKT_CMD_ADD,
                            &num_cells, body, body_len) != 0 ||
     sixp_pkt_get_cell_list(SIXP_PKT_TYPE_REQUEST,
                            (sixp_pkt_code_t)(uint8_t)SIXP_PKT_CMD_ADD,
                            &cell_list, &cell_list_len,
                            body, body_len) != 0) {
    PRINTF("sf-mobility: Parse error on add request\n");
    return;
  }

  PRINTF("sf-mobility: Received a 6P Add Request for %d links from node ",
         num_cells);
  PRINTLLADDR((uip_lladdr_t *)peer_addr);
  PRINTF(" with LinkList : ");
  print_cell_list(cell_list, cell_list_len);
  PRINTF("\n");

  slotframe = tsch_schedule_get_slotframe_by_handle(slotframe_handle);
  if(slotframe == NULL) {
    return;
  }

  if(num_cells > 0 && cell_list_len > 0) {
    memset(res_storage, 0, sizeof(res_storage));
    res_len = 0;

    for(i = 0, feasible_link = 0;
        i < cell_list_len && feasible_link < num_cells;
        i += sizeof(cell)) {
      read_cell(&cell_list[i], &cell);
      if(tsch_schedule_get_link_by_offsets(slotframe,
                                           cell.timeslot_offset,
                                           cell.channel_offset) == NULL) {
        sixp_pkt_set_cell_list(SIXP_PKT_TYPE_RESPONSE,
                               (sixp_pkt_code_t)(uint8_t)SIXP_PKT_RC_SUCCESS,
                               (uint8_t *)&cell, sizeof(cell),
                               feasible_link,
                               res_storage, sizeof(res_storage));
        res_len += sizeof(cell);
        feasible_link++;
      }
    }

    if(feasible_link == num_cells) {
      PRINTF("sf-mobility: Send a 6P Response to node ");
      PRINTLLADDR((uip_lladdr_t *)peer_addr);
      PRINTF("\n");

      sixp_output(SIXP_PKT_TYPE_RESPONSE,
                  (sixp_pkt_code_t)(uint8_t)SIXP_PKT_RC_SUCCESS,
                  SF_MOBILITY_SFID,
                  res_storage, res_len, peer_addr,
                  add_response_sent_callback, res_storage, res_len);
    }
  }
}

static void
delete_req_input(const uint8_t *body, uint16_t body_len,
                 const linkaddr_t *peer_addr)
{
  uint8_t i;
  sf_mobility_cell_t cell;
  struct tsch_slotframe *slotframe;
  uint8_t num_cells;
  const uint8_t *cell_list;
  uint16_t cell_list_len;
  uint16_t res_len;
  int removed_link;

  assert(body != NULL && peer_addr != NULL);

  if(sixp_pkt_get_num_cells(SIXP_PKT_TYPE_REQUEST,
                            (sixp_pkt_code_t)(uint8_t)SIXP_PKT_CMD_DELETE,
                            &num_cells, body, body_len) != 0 ||
     sixp_pkt_get_cell_list(SIXP_PKT_TYPE_REQUEST,
                            (sixp_pkt_code_t)(uint8_t)SIXP_PKT_CMD_DELETE,
                            &cell_list, &cell_list_len,
                            body, body_len) != 0) {
    PRINTF("sf-mobility: Parse error on delete request\n");
    return;
  }

  PRINTF("sf-mobility: Received a 6P Delete Request for %d links from node ",
         num_cells);
  PRINTLLADDR((uip_lladdr_t *)peer_addr);
  PRINTF(" with LinkList : ");
  print_cell_list(cell_list, cell_list_len);
  PRINTF("\n");

  slotframe = tsch_schedule_get_slotframe_by_handle(slotframe_handle);
  if(slotframe == NULL) {
    return;
  }

  memset(res_storage, 0, sizeof(res_storage));
  res_len = 0;

  if(num_cells > 0 && cell_list_len > 0) {
    for(i = 0, removed_link = 0; i < cell_list_len; i += sizeof(cell)) {
      read_cell(&cell_list[i], &cell);
      if(tsch_schedule_get_link_by_offsets(slotframe,
                                           cell.timeslot_offset,
                                           cell.channel_offset) != NULL) {
        sixp_pkt_set_cell_list(SIXP_PKT_TYPE_RESPONSE,
                               (sixp_pkt_code_t)(uint8_t)SIXP_PKT_RC_SUCCESS,
                               (uint8_t *)&cell, sizeof(cell),
                               removed_link,
                               res_storage, sizeof(res_storage));
        res_len += sizeof(cell);
      }
    }
  }

  PRINTF("sf-mobility: Send a 6P Response to node ");
  PRINTLLADDR((uip_lladdr_t *)peer_addr);
  PRINTF("\n");
  sixp_output(SIXP_PKT_TYPE_RESPONSE,
              (sixp_pkt_code_t)(uint8_t)SIXP_PKT_RC_SUCCESS,
              SF_MOBILITY_SFID,
              res_storage, res_len, peer_addr,
              delete_response_sent_callback, res_storage, res_len);
}

static void
request_input(sixp_pkt_cmd_t cmd,
              const uint8_t *body, uint16_t body_len,
              const linkaddr_t *peer_addr)
{
  assert(body != NULL && peer_addr != NULL);

  switch(cmd) {
    case SIXP_PKT_CMD_ADD:
      add_req_input(body, body_len, peer_addr);
      break;
    case SIXP_PKT_CMD_DELETE:
      delete_req_input(body, body_len, peer_addr);
      break;
    default:
      break;
  }
}

static void
response_input(sixp_pkt_rc_t rc,
               const uint8_t *body, uint16_t body_len,
               const linkaddr_t *peer_addr)
{
  const uint8_t *cell_list;
  uint16_t cell_list_len;
  sixp_nbr_t *nbr;
  sixp_trans_t *trans;

  assert(body != NULL && peer_addr != NULL);

  if((nbr = sixp_nbr_find(peer_addr)) == NULL ||
     (trans = sixp_trans_find(peer_addr)) == NULL) {
    return;
  }

  if(rc == SIXP_PKT_RC_SUCCESS) {
    switch(sixp_trans_get_cmd(trans)) {
      case SIXP_PKT_CMD_ADD:
        if(sixp_pkt_get_cell_list(SIXP_PKT_TYPE_RESPONSE,
                                  (sixp_pkt_code_t)(uint8_t)SIXP_PKT_RC_SUCCESS,
                                  &cell_list, &cell_list_len,
                                  body, body_len) != 0) {
          PRINTF("sf-mobility: Parse error on add response\n");
          return;
        }
        PRINTF("sf-mobility: Received a 6P Add Response with LinkList : ");
        print_cell_list(cell_list, cell_list_len);
        PRINTF("\n");
        add_links_to_schedule(peer_addr, LINK_OPTION_TX,
                              cell_list, cell_list_len);
        break;
      case SIXP_PKT_CMD_DELETE:
        if(sixp_pkt_get_cell_list(SIXP_PKT_TYPE_RESPONSE,
                                  (sixp_pkt_code_t)(uint8_t)SIXP_PKT_RC_SUCCESS,
                                  &cell_list, &cell_list_len,
                                  body, body_len) != 0) {
          PRINTF("sf-mobility: Parse error on delete response\n");
          return;
        }
        PRINTF("sf-mobility: Received a 6P Delete Response with LinkList : ");
        print_cell_list(cell_list, cell_list_len);
        PRINTF("\n");
        remove_links_to_schedule(cell_list, cell_list_len);
        break;
      default:
        PRINTF("sf-mobility: unsupported response\n");
    }
  }
}

static void
input(sixp_pkt_type_t type, sixp_pkt_code_t code,
      const uint8_t *body, uint16_t body_len, const linkaddr_t *src_addr)
{
  assert(body != NULL && src_addr != NULL);
  switch(type) {
    case SIXP_PKT_TYPE_REQUEST:
      request_input(code.cmd, body, body_len, src_addr);
      break;
    case SIXP_PKT_TYPE_RESPONSE:
      response_input(code.rc, body, body_len, src_addr);
      break;
    default:
      break;
  }
}

/* ------------------------------------------------------------------------ */
/* 6P transaction initiators                                               */
/* ------------------------------------------------------------------------ */

static int
sf_mobility_add_link(const linkaddr_t *peer_addr)
{
  uint8_t index = 0;
  struct tsch_slotframe *sf =
    tsch_schedule_get_slotframe_by_handle(slotframe_handle);
  uint8_t req_len;
  sf_mobility_cell_t cell_list[1];
  uint16_t random_slot;
  uint16_t attempts;

  assert(peer_addr != NULL && sf != NULL);

  /* Propose a single free slot; the peer will confirm feasibility. */
  for(attempts = 0; attempts < TSCH_SCHEDULE_DEFAULT_LENGTH * 2; attempts++) {
    random_slot = ((random_rand() & 0xFF)) % TSCH_SCHEDULE_DEFAULT_LENGTH;
    if(tsch_schedule_get_link_by_offsets(sf, random_slot, 0) == NULL) {
      cell_list[0].timeslot_offset = random_slot;
      cell_list[0].channel_offset = 0;
      index = 1;
      break;
    }
  }

  if(index == 0) {
    PRINTF("sf-mobility: no free slot found to propose\n");
    return -1;
  }

  memset(req_storage, 0, sizeof(req_storage));
  if(sixp_pkt_set_cell_options(SIXP_PKT_TYPE_REQUEST,
                               (sixp_pkt_code_t)(uint8_t)SIXP_PKT_CMD_ADD,
                               SIXP_PKT_CELL_OPTION_TX,
                               req_storage, sizeof(req_storage)) != 0 ||
     sixp_pkt_set_num_cells(SIXP_PKT_TYPE_REQUEST,
                            (sixp_pkt_code_t)(uint8_t)SIXP_PKT_CMD_ADD,
                            index, req_storage, sizeof(req_storage)) != 0 ||
     sixp_pkt_set_cell_list(SIXP_PKT_TYPE_REQUEST,
                            (sixp_pkt_code_t)(uint8_t)SIXP_PKT_CMD_ADD,
                            (const uint8_t *)cell_list,
                            index * sizeof(sf_mobility_cell_t), 0,
                            req_storage, sizeof(req_storage)) != 0) {
    PRINTF("sf-mobility: Build error on add request\n");
    return -1;
  }

  req_len = 4 + index * sizeof(sf_mobility_cell_t);
  sixp_output(SIXP_PKT_TYPE_REQUEST, (sixp_pkt_code_t)(uint8_t)SIXP_PKT_CMD_ADD,
              SF_MOBILITY_SFID, req_storage, req_len, peer_addr,
              NULL, NULL, 0);

  PRINTF("sf-mobility: Send a 6P Add Request (mobility_score=%u) to node ",
         mobility_score);
  PRINTLLADDR((uip_lladdr_t *)peer_addr);
  PRINTF("\n");
  return 0;
}

static int
sf_mobility_remove_link(const linkaddr_t *peer_addr)
{
  uint8_t i, index = 0;
  struct tsch_slotframe *sf =
    tsch_schedule_get_slotframe_by_handle(slotframe_handle);
  struct tsch_link *l;
  uint16_t req_len;
  sf_mobility_cell_t cell;

  assert(peer_addr != NULL && sf != NULL);

  for(i = 0; i < TSCH_SCHEDULE_DEFAULT_LENGTH; i++) {
    l = tsch_schedule_get_link_by_offsets(sf, i, 0);
    if(l && linkaddr_cmp(&l->addr, peer_addr) &&
       (l->link_options & LINK_OPTION_TX)) {
      cell.timeslot_offset = i;
      cell.channel_offset = l->channel_offset;
      index = 1;
      break;
    }
  }

  if(index == 0) {
    return -1;
  }

  memset(req_storage, 0, sizeof(req_storage));
  if(sixp_pkt_set_num_cells(SIXP_PKT_TYPE_REQUEST,
                            (sixp_pkt_code_t)(uint8_t)SIXP_PKT_CMD_DELETE,
                            1, req_storage, sizeof(req_storage)) != 0 ||
     sixp_pkt_set_cell_list(SIXP_PKT_TYPE_REQUEST,
                            (sixp_pkt_code_t)(uint8_t)SIXP_PKT_CMD_DELETE,
                            (const uint8_t *)&cell, sizeof(cell), 0,
                            req_storage, sizeof(req_storage)) != 0) {
    PRINTF("sf-mobility: Build error on delete request\n");
    return -1;
  }

  req_len = 4 + sizeof(sf_mobility_cell_t);
  sixp_output(SIXP_PKT_TYPE_REQUEST,
              (sixp_pkt_code_t)(uint8_t)SIXP_PKT_CMD_DELETE,
              SF_MOBILITY_SFID, req_storage, req_len, peer_addr,
              NULL, NULL, 0);

  PRINTF("sf-mobility: Send a 6P Delete Request to node ");
  PRINTLLADDR((uip_lladdr_t *)peer_addr);
  PRINTF("\n");
  return 0;
}

/* ------------------------------------------------------------------------ */
/* Mobility-aware adaptive housekeeping                                     */
/* ------------------------------------------------------------------------ */

/*
 * Update mobility_score (0-100).
 *
 * With SF_MOBILITY_CONF_WITH_COOJA_IMU (default on the cooja target): use
 * the ground-truth speed/motion flag written directly into
 * cooja_imu_speed_mm_s / cooja_imu_is_moving by imu-mobility-logger.js, one
 * exponential moving average away from raw ground truth. This is simpler
 * and more accurate in simulation than inferring motion indirectly.
 *
 * Otherwise (real hardware without an IMU, or Cooja builds that want the
 * portable fallback): derive a proxy score from RSSI volatility and ETX
 * trend of the current parent link, since a large RSSI swing or a rising
 * ETX between two consecutive housekeeping rounds usually indicates
 * relative motion between the node and its parent.
 */
static void
update_mobility_score(const struct link_stats *stats)
{
#if SF_MOBILITY_CONF_WITH_COOJA_IMU
  uint16_t sample_score;

  sample_score = cooja_imu_speed_mm_s >= 2000 ? 100 : cooja_imu_speed_mm_s / 20;
  if(cooja_imu_is_moving && sample_score < 25) {
    /* Motion flagged even though the speed sample was small/stale (e.g.
     * right after a direction change): still treat it as significant. */
    sample_score = 25;
  }

  /* Exponential moving average: smooth out single-sample noise. */
  mobility_score = (uint8_t)((mobility_score * 3 + sample_score) / 4);
#else /* SF_MOBILITY_CONF_WITH_COOJA_IMU */
  int16_t rssi_delta;
  int16_t etx_delta;
  uint16_t sample_score = 0;

  if(stats == NULL || !link_stats_is_fresh(stats)) {
    /* No fresh data yet: neither confirm nor deny mobility, just decay. */
    mobility_score = mobility_score > 5 ? mobility_score - 5 : 0;
    return;
  }

  if(prev_rssi != LINK_STATS_RSSI_UNKNOWN && stats->rssi != LINK_STATS_RSSI_UNKNOWN) {
    rssi_delta = stats->rssi - prev_rssi;
    if(rssi_delta < 0) {
      rssi_delta = -rssi_delta;
    }
    /* ~1dB of jitter is normal; scale beyond that. */
    sample_score += (uint16_t)(rssi_delta * 4);
  }

  if(prev_etx != 0) {
    etx_delta = (int16_t)stats->etx - (int16_t)prev_etx;
    if(etx_delta > 0) {
      /* Rising ETX (link getting worse) weighs more than falling ETX. */
      sample_score += (uint16_t)(etx_delta / 4);
    }
  }

  if(sample_score > 100) {
    sample_score = 100;
  }

  /* Exponential moving average: smooth out single-sample noise. */
  mobility_score = (uint8_t)((mobility_score * 3 + sample_score) / 4);

  prev_rssi = stats->rssi;
  prev_etx = stats->etx;
#endif /* SF_MOBILITY_CONF_WITH_COOJA_IMU */
}

static void
housekeeping(void)
{
  struct tsch_neighbor *time_source;
  const linkaddr_t *parent_addr;
  const struct link_stats *stats;
  int queue_len;

  time_source = tsch_queue_get_time_source();
  if(time_source == NULL) {
    return;
  }

  parent_addr = tsch_queue_get_nbr_address(time_source);
  if(parent_addr == NULL) {
    return;
  }

  if(!have_parent || !linkaddr_cmp(&current_parent, parent_addr)) {
    /* Parent switch (or first parent acquisition): this is itself a strong
     * mobility signal, and the new link has no history yet, so react
     * immediately instead of waiting for statistics to accumulate. */
    PRINTF("sf-mobility: parent changed, treating as a mobility event\n");
    linkaddr_copy(&current_parent, parent_addr);
    have_parent = 1;
    prev_rssi = LINK_STATS_RSSI_UNKNOWN;
    prev_etx = 0;
    mobility_score = 100;
    idle_rounds = 0;
    num_negotiated_tx_cells = 0;
    if(sf_mobility_add_link(&current_parent) == 0) {
      num_negotiated_tx_cells = 1;
    }
    return;
  }

  stats = link_stats_from_lladdr(&current_parent);
  update_mobility_score(stats);

  queue_len = tsch_queue_nbr_packet_count(time_source);

  PRINTF("sf-mobility: housekeeping queue_len=%d mobility_score=%u cells=%u\n",
         queue_len, mobility_score, num_negotiated_tx_cells);

  if((queue_len >= SF_MOBILITY_QUEUE_HIGH_THRESHOLD ||
      mobility_score >= SF_MOBILITY_SCORE_HIGH) &&
     num_negotiated_tx_cells < SF_MOBILITY_MAX_CELLS) {
    /* Traffic pressure and/or high mobility: negotiate one more Tx cell
     * ahead of time so frames are less likely to be queue-dropped or lost
     * while the link is unstable. */
    if(sf_mobility_add_link(&current_parent) == 0) {
      idle_rounds = 0;
    }
  } else if(queue_len == 0 && mobility_score <= SF_MOBILITY_SCORE_LOW &&
            num_negotiated_tx_cells > SF_MOBILITY_MIN_CELLS) {
    /* Link is calm/static and the extra capacity is unused: reclaim it
     * after a few stable rounds to save energy (fewer radio wake-ups). */
    idle_rounds++;
    if(idle_rounds >= SF_MOBILITY_IDLE_ROUNDS_TO_SHRINK) {
      if(sf_mobility_remove_link(&current_parent) == 0) {
        idle_rounds = 0;
      }
    }
  } else {
    idle_rounds = 0;
  }
}

PROCESS_THREAD(sf_mobility_housekeeping_process, ev, data)
{
  static struct etimer et;

  PROCESS_BEGIN();

  etimer_set(&et, SF_MOBILITY_HOUSEKEEPING_PERIOD);
  while(1) {
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&et));
    etimer_reset(&et);
    housekeeping();
  }

  PROCESS_END();
}

static void
init(void)
{
  have_parent = 0;
  num_negotiated_tx_cells = 0;
  idle_rounds = 0;
  mobility_score = 0;
  prev_rssi = LINK_STATS_RSSI_UNKNOWN;
  prev_etx = 0;
  cooja_imu_is_moving = 0;
  cooja_imu_speed_mm_s = 0;
  process_start(&sf_mobility_housekeeping_process, NULL);
}

const sixtop_sf_t sf_mobility_driver = {
  SF_MOBILITY_SFID,
  CLOCK_SECOND,
  init,
  input,
  NULL,
  NULL
};
