/*
 * 6TiSCH Minimal Scheduling Function (MSF), RFC 9033.
 *
 * Implementation for Contiki-NG on top of its 6top/6P stack (RFC 8480). The
 * 6P API usage (sixp_pkt_set_*, sixp_output, response callbacks) follows
 * examples/6tisch/6p-packet/test-sf.c.
 *
 * "RFC 9033 Section x.y" in the comments below refers to the MSF RFC.
 *
 * Coverage of the RFC:
 *  - Section 2   minimal cell (slotframe 0 is created by TSCH), 3 slotframes
 *  - Section 3   autonomous cells (AutoRxCell, on-demand AutoTxCell), SAX hash
 *  - Section 4   node behavior at boot (steps 4-6; the CoJP join of step 3 is
 *                not available in Contiki-NG and therefore not implemented)
 *  - Section 5.1 adapting to traffic (NumCellsElapsed / NumCellsUsed)
 *  - Section 5.2 switching parent
 *  - Section 5.3 handling schedule collisions (NumTx / NumTxAck / RELOCATE)
 *  - Section 6-13 SIGNAL unused, SFID 0, CellList rules, 6P timeout,
 *                cell ordering, metadata unused, 6P error handling
 *  - Section 14/15 constants and statistics (see msf.h)
 *
 * Known limitations (documented where they apply):
 *  - Contiki-NG's TSCH allows a single link per timeslot inside a slotframe;
 *    an AutoTxCell that hashes onto the AutoRxCell slot replaces it until the
 *    AutoTxCell is removed again (Section 3).
 *  - "quarantine" (Section 12) clears the cells and suppresses 6P exchanges
 *    with the neighbor; it does not evict it from the neighbor/routing tables.
 */

#include "contiki.h"
#include "lib/random.h"
#include "sys/critical.h"
#include "net/linkaddr.h"
#include "net/packetbuf.h"
#include "net/mac/mac.h"
#include "net/mac/tsch/tsch.h"
#include "net/mac/tsch/sixtop/sixtop.h"
#include "net/mac/tsch/sixtop/sixp.h"
#include "net/mac/tsch/sixtop/sixp-pkt.h"
#include "net/routing/routing.h"

#include <string.h>

#include "msf.h"

#include "sys/log.h"
#define LOG_MODULE "MSF"
#define LOG_LEVEL LOG_LEVEL_INFO

/*---------------------------------------------------------------------------*/
/* Definitions                                                               */
/*---------------------------------------------------------------------------*/

/* MSF asks for one cell at a time (Section 4.6 and 5.1). */
#define MSF_MAX_CELLS_PER_REQUEST 1
/* Size of one 6P cell: 16-bit slotOffset + 16-bit channelOffset (RFC 8480). */
#define MSF_6P_CELL_SIZE 4
/* Header of an ADD/DELETE/RELOCATE request: Metadata, CellOptions, NumCells. */
#define MSF_6P_REQ_HDR_SIZE 4

#define MSF_TICK_PERIOD           CLOCK_SECOND
#define MSF_LOCAL_RETRY_DELAY     (2 * CLOCK_SECOND)
#define MSF_SWITCH_TIMEOUT        (5 * 60 * CLOCK_SECOND)
#define MSF_CLEAR_MAX_ATTEMPTS    5
#define MSF_MAX_PENDING_RESPONSES 3
#define MSF_MAX_CLEAR_QUEUE       2
#define MSF_MAX_QUARANTINE        2

#define MSF_ELAPSED(now, since) ((clock_time_t)((now) - (since)))

#define MSF_CODE(x) ((sixp_pkt_code_t)(uint8_t)(x))

/* Direction of a negotiated cell, from the point of view of this node. */
enum {
  MSF_DIR_TX = 0,
  MSF_DIR_RX = 1,
  MSF_NUM_DIRS
};
#define MSF_DIR_TO_LINK_OPTION(d) ((d) == MSF_DIR_TX ? LINK_OPTION_TX : LINK_OPTION_RX)
#define MSF_DIR_TO_CELL_OPTION(d) ((d) == MSF_DIR_TX ? \
                                   SIXP_PKT_CELL_OPTION_TX : SIXP_PKT_CELL_OPTION_RX)

/* Who requested the cell: we (towards our parent) or the peer (we are its parent). */
enum {
  MSF_ROLE_INITIATOR,
  MSF_ROLE_RESPONDER
};

/* Outcome of the Section 5.1 evaluation, decided when NumCellsElapsed hits MAX_NUM_CELLS. */
enum {
  MSF_ACTION_NONE,
  MSF_ACTION_ADD,
  MSF_ACTION_DELETE
};

struct msf_cell_id {
  uint16_t slot;
  uint16_t channel;
};

/* A negotiated cell installed in slotframe 2, plus its statistics. */
struct msf_cell {
  uint8_t in_use;
  uint8_t role;
  uint8_t link_options;       /* LINK_OPTION_TX or LINK_OPTION_RX */
  linkaddr_t peer;
  uint16_t slot;
  uint16_t channel;
  /* Section 5.3 / 15: wider than the recommended 1 byte, since MAX_NUMTX is 256. */
  uint16_t num_tx;            /* NumTx */
  uint16_t num_tx_ack;        /* NumTxAck */
  uint8_t stats_valid;        /* NumTx was halved at least once since installation */
  clock_time_t last_active;
};

/* Section 5.1 counters, one pair per direction (Section 15: 1 byte each). */
struct msf_traffic {
  uint8_t elapsed;            /* NumCellsElapsed */
  uint8_t used;               /* NumCellsUsed */
};

/* An AutoTxCell installed on demand (Section 3). */
struct msf_auto_tx {
  uint8_t in_use;
  uint8_t replaced_auto_rx;   /* it took over the AutoRxCell timeslot */
  linkaddr_t peer;
  uint16_t slot;
  uint16_t channel;
};

/* The single 6P transaction we can have open as initiator. */
struct msf_txn {
  uint8_t busy;
  linkaddr_t peer;
  sixp_pkt_cmd_t cmd;
  uint8_t dir;
  struct msf_cell_id cand[MSF_NUM_CANDIDATE_CELLS];
  uint8_t num_cand;
  struct msf_cell_id rel;     /* cell to delete / relocate */
};

/* State of a response we sent, applied once it was transmitted. */
struct msf_pending {
  uint8_t in_use;
  linkaddr_t peer;
  sixp_pkt_cmd_t cmd;
  uint8_t link_options;       /* link options of the cells installed on our side */
  uint8_t num_new;
  struct msf_cell_id new_cells[MSF_MAX_CELLS_PER_REQUEST];
  uint8_t num_old;
  struct msf_cell_id old_cells[MSF_MAX_CELLS_PER_REQUEST];
  clock_time_t created;
};

struct msf_clear_entry {
  uint8_t in_use;
  uint8_t attempts;
  linkaddr_t peer;
};

struct msf_quarantine_entry {
  uint8_t in_use;
  linkaddr_t peer;
  clock_time_t since;
};

/*---------------------------------------------------------------------------*/
/* State                                                                     */
/*---------------------------------------------------------------------------*/

PROCESS(msf_process, "MSF");

/* Shared with the TSCH callbacks, which run in interrupt context. */
static volatile uint8_t msf_ready;          /* slotframes 1 and 2 exist */
static volatile uint8_t have_parent;        /* an MSF session is active */
static linkaddr_t parent;                   /* the "selected parent" (Section 1) */
static volatile uint8_t parent_rx_cells;    /* negotiated Rx cells from the parent */
static struct msf_traffic traffic[MSF_NUM_DIRS];
static volatile uint8_t decision[MSF_NUM_DIRS];
static struct msf_cell cells[MSF_MAX_NEGOTIATED_CELLS];

static uint16_t auto_rx_slot;
static uint16_t auto_rx_channel;
static struct msf_auto_tx auto_tx[MSF_MAX_AUTO_TX_CELLS];

static uint8_t want[MSF_NUM_DIRS];          /* action to perform, from decision[] */
static struct msf_txn txn;
static struct msf_pending pending[MSF_MAX_PENDING_RESPONSES];
static struct msf_clear_entry clear_queue[MSF_MAX_CLEAR_QUEUE];
static struct msf_quarantine_entry quarantine[MSF_MAX_QUARANTINE];

/* Section 5.2: parent switch in progress. */
static uint8_t sw_active;
static linkaddr_t sw_old;
static uint8_t sw_target[MSF_NUM_DIRS];
static clock_time_t sw_start;

/* Section 12 "waitretry": no new 6P request until the hold time elapsed. */
static uint8_t hold_active;
static clock_time_t hold_start_time;
static clock_time_t hold_duration;

/* Section 5.3 */
static clock_time_t last_collision_check;
static uint8_t reloc_pending;
static struct msf_cell_id reloc_cell;

static uint8_t req_buf[MSF_6P_REQ_HDR_SIZE + MSF_6P_CELL_SIZE +
                       MSF_6P_CELL_SIZE * MSF_NUM_CANDIDATE_CELLS];

/*---------------------------------------------------------------------------*/
/* Small helpers                                                             */
/*---------------------------------------------------------------------------*/

static void
put_cell(uint8_t *p, uint16_t slot, uint16_t channel)
{
  p[0] = slot & 0xff;
  p[1] = slot >> 8;
  p[2] = channel & 0xff;
  p[3] = channel >> 8;
}
/*---------------------------------------------------------------------------*/
static void
get_cell(const uint8_t *p, struct msf_cell_id *id)
{
  id->slot = p[0] | (p[1] << 8);
  id->channel = p[2] | (p[3] << 8);
}
/*---------------------------------------------------------------------------*/
/*
 * RFC 9033 Appendix A: SAX hash of an EUI-64 address.
 * h0 = 0, l_bit = 0, r_bit = 1. T is the hashing table length.
 */
static uint16_t
sax_hash(const linkaddr_t *addr, uint16_t table_len)
{
  uint32_t h = 0;
  uint8_t i;

  for(i = 0; i < LINKADDR_SIZE; i++) {
    uint32_t sum = (h << 0) + (h >> 1) + addr->u8[i]; /* step 2 */
    h = (sum ^ h) % table_len;                        /* steps 3-5 */
  }
  return (uint16_t)h;
}
/*---------------------------------------------------------------------------*/
/* RFC 9033 Section 3: slotOffset(MAC) = 1 + hash(EUI64, length(Slotframe_1) - 1) */
static uint16_t
auto_slot_of(const linkaddr_t *addr)
{
  return 1 + sax_hash(addr, MSF_SLOTFRAME_LENGTH - 1);
}
/*---------------------------------------------------------------------------*/
/* RFC 9033 Section 3: channelOffset(MAC) = hash(EUI64, NUM_CH_OFFSET) */
static uint16_t
auto_channel_of(const linkaddr_t *addr)
{
  return sax_hash(addr, MSF_NUM_CH_OFFSET);
}
/*---------------------------------------------------------------------------*/
static void
hold_wait_retry(void)
{
  /* RFC 9033 Section 12 "waitretry": wait [WAIT_DURATION_MIN, WAIT_DURATION_MAX]. */
  hold_active = 1;
  hold_start_time = clock_time();
  hold_duration = MSF_WAIT_DURATION_MIN +
    (random_rand() % (MSF_WAIT_DURATION_MAX - MSF_WAIT_DURATION_MIN + 1));
}
/*---------------------------------------------------------------------------*/
static void
hold_short(void)
{
  hold_active = 1;
  hold_start_time = clock_time();
  hold_duration = MSF_LOCAL_RETRY_DELAY;
}
/*---------------------------------------------------------------------------*/
static int
hold_is_active(clock_time_t now)
{
  if(hold_active && MSF_ELAPSED(now, hold_start_time) >= hold_duration) {
    hold_active = 0;
  }
  return hold_active;
}
/*---------------------------------------------------------------------------*/
static struct tsch_slotframe *
get_slotframe(uint16_t handle)
{
  return tsch_schedule_get_slotframe_by_handle(handle);
}
/*---------------------------------------------------------------------------*/
/*
 * RFC 9033 Section 8: a candidate slotOffset must not be slot 0 (minimal cell)
 * and must not collide with any cell already scheduled by this node.
 */
static int
slot_is_free(uint16_t slot)
{
  struct tsch_slotframe *sf1, *sf2;

  if(slot == 0 || slot >= MSF_SLOTFRAME_LENGTH || tsch_is_locked()) {
    return 0;
  }
  sf1 = get_slotframe(MSF_SLOTFRAME_AUTONOMOUS);
  sf2 = get_slotframe(MSF_SLOTFRAME_NEGOTIATED);
  if(sf1 == NULL || sf2 == NULL) {
    return 0;
  }
  return tsch_schedule_get_link_by_timeslot(sf1, slot) == NULL &&
         tsch_schedule_get_link_by_timeslot(sf2, slot) == NULL;
}
/*---------------------------------------------------------------------------*/
static int
cell_id_in_list(const struct msf_cell_id *list, uint8_t len, const struct msf_cell_id *id)
{
  uint8_t i;
  for(i = 0; i < len; i++) {
    if(list[i].slot == id->slot && list[i].channel == id->channel) {
      return 1;
    }
  }
  return 0;
}
/*---------------------------------------------------------------------------*/
/*
 * RFC 9033 Section 8: pick cells with different slotOffsets, randomly and
 * uniformly among the free ones, and random channelOffsets.
 */
static uint8_t
pick_candidate_cells(struct msf_cell_id *out, uint8_t wanted)
{
  uint8_t n = 0;
  uint16_t tries;

  for(tries = 0; tries < 8 * MSF_SLOTFRAME_LENGTH && n < wanted; tries++) {
    struct msf_cell_id id;
    uint8_t j;

    id.slot = 1 + random_rand() % (MSF_SLOTFRAME_LENGTH - 1);
    id.channel = random_rand() % MSF_NUM_CH_OFFSET;
    if(!slot_is_free(id.slot)) {
      continue;
    }
    /* Only the slotOffset must differ between cells of one list. */
    for(j = 0; j < n; j++) {
      if(out[j].slot == id.slot) {
        break;
      }
    }
    if(j < n) {
      continue;
    }
    out[n++] = id;
  }
  return n;
}
/*---------------------------------------------------------------------------*/
/* Section 5.1 counters; the callers hold no lock, the TSCH callbacks are ISRs. */
static void
traffic_reset(void)
{
  int_master_status_t status = critical_enter();
  memset(traffic, 0, sizeof(traffic));
  decision[MSF_DIR_TX] = MSF_ACTION_NONE;
  decision[MSF_DIR_RX] = MSF_ACTION_NONE;
  critical_exit(status);
}

/*---------------------------------------------------------------------------*/
/* Negotiated cell table                                                     */
/*---------------------------------------------------------------------------*/

static struct msf_cell *
cell_lookup(const linkaddr_t *peer, uint8_t link_options, uint16_t slot, uint16_t channel)
{
  uint8_t i;
  for(i = 0; i < MSF_MAX_NEGOTIATED_CELLS; i++) {
    struct msf_cell *c = &cells[i];
    if(c->in_use && c->link_options == link_options && c->slot == slot &&
       c->channel == channel && linkaddr_cmp(&c->peer, peer)) {
      return c;
    }
  }
  return NULL;
}
/*---------------------------------------------------------------------------*/
/* Lookup for the TSCH callbacks: only the position in the schedule is known. */
static struct msf_cell *
cell_lookup_by_position(uint16_t slot, uint16_t channel, uint8_t link_options)
{
  uint8_t i;
  for(i = 0; i < MSF_MAX_NEGOTIATED_CELLS; i++) {
    struct msf_cell *c = &cells[i];
    if(c->in_use && c->slot == slot && c->channel == channel &&
       c->link_options == link_options) {
      return c;
    }
  }
  return NULL;
}
/*---------------------------------------------------------------------------*/
static uint8_t
cells_count(const linkaddr_t *peer, uint8_t link_options, uint8_t role)
{
  uint8_t i, n = 0;
  for(i = 0; i < MSF_MAX_NEGOTIATED_CELLS; i++) {
    struct msf_cell *c = &cells[i];
    if(c->in_use && c->role == role && c->link_options == link_options &&
       linkaddr_cmp(&c->peer, peer)) {
      n++;
    }
  }
  return n;
}
/*---------------------------------------------------------------------------*/
static uint8_t
cells_free(void)
{
  uint8_t i, n = 0;
  for(i = 0; i < MSF_MAX_NEGOTIATED_CELLS; i++) {
    if(!cells[i].in_use) {
      n++;
    }
  }
  return n;
}
/*---------------------------------------------------------------------------*/
static void
refresh_counts(void)
{
  parent_rx_cells = have_parent ?
    cells_count(&parent, LINK_OPTION_RX, MSF_ROLE_INITIATOR) : 0;
}
/*---------------------------------------------------------------------------*/
static struct msf_cell *
cell_install(const linkaddr_t *peer, uint8_t link_options,
             uint16_t slot, uint16_t channel, uint8_t role)
{
  struct tsch_slotframe *sf2;
  struct msf_cell *c = NULL;
  uint8_t i;

  sf2 = get_slotframe(MSF_SLOTFRAME_NEGOTIATED);
  if(sf2 == NULL) {
    return NULL;
  }
  for(i = 0; i < MSF_MAX_NEGOTIATED_CELLS; i++) {
    if(!cells[i].in_use) {
      c = &cells[i];
      break;
    }
  }
  if(c == NULL) {
    return NULL;
  }
  /* Negotiated cells are dedicated (not shared) Tx or Rx cells, Section 5. */
  if(tsch_schedule_add_link(sf2, link_options, LINK_TYPE_NORMAL, peer,
                            slot, channel, 1) == NULL) {
    return NULL;
  }
  memset(c, 0, sizeof(*c));
  c->role = role;
  c->link_options = link_options;
  linkaddr_copy(&c->peer, peer);
  c->slot = slot;
  c->channel = channel;
  c->last_active = clock_time();
  c->in_use = 1; /* last: makes the cell visible to the TSCH callbacks */
  refresh_counts();

  LOG_INFO("negotiated %s cell added: slot %u ch %u peer ",
           link_options == LINK_OPTION_TX ? "Tx" : "Rx", slot, channel);
  LOG_INFO_LLADDR(peer);
  LOG_INFO_(" (%s)\n", role == MSF_ROLE_INITIATOR ? "initiator" : "responder");
  return c;
}
/*---------------------------------------------------------------------------*/
static void
cell_remove(struct msf_cell *c)
{
  struct tsch_slotframe *sf2;

  c->in_use = 0; /* first: the TSCH callbacks must stop touching it */
  sf2 = get_slotframe(MSF_SLOTFRAME_NEGOTIATED);
  if(sf2 != NULL) {
    tsch_schedule_remove_link_by_offsets(sf2, c->slot, c->channel);
  }
  refresh_counts();

  LOG_INFO("negotiated %s cell removed: slot %u ch %u peer ",
           c->link_options == LINK_OPTION_TX ? "Tx" : "Rx", c->slot, c->channel);
  LOG_INFO_LLADDR(&c->peer);
  LOG_INFO_("\n");
}
/*---------------------------------------------------------------------------*/
static void
cells_remove_all(const linkaddr_t *peer)
{
  uint8_t i;
  for(i = 0; i < MSF_MAX_NEGOTIATED_CELLS; i++) {
    if(cells[i].in_use && linkaddr_cmp(&cells[i].peer, peer)) {
      cell_remove(&cells[i]);
    }
  }
}

/*---------------------------------------------------------------------------*/
/* Autonomous cells: RFC 9033 Section 3                                      */
/*---------------------------------------------------------------------------*/

static void
auto_rx_install(void)
{
  struct tsch_slotframe *sf1 = get_slotframe(MSF_SLOTFRAME_AUTONOMOUS);

  /* AutoRxCell: TX=0, RX=1, SHARED=0. It MUST stay scheduled once synchronized. */
  if(sf1 != NULL) {
    tsch_schedule_add_link(sf1, LINK_OPTION_RX, LINK_TYPE_NORMAL, NULL,
                           auto_rx_slot, auto_rx_channel, 1);
  }
}
/*---------------------------------------------------------------------------*/
static struct msf_auto_tx *
auto_tx_find(const linkaddr_t *peer)
{
  uint8_t i;
  for(i = 0; i < MSF_MAX_AUTO_TX_CELLS; i++) {
    if(auto_tx[i].in_use && linkaddr_cmp(&auto_tx[i].peer, peer)) {
      return &auto_tx[i];
    }
  }
  return NULL;
}
/*---------------------------------------------------------------------------*/
/*
 * Add an AutoTxCell towards `peer`: TX=1, RX=0, SHARED=1 at the slot/channel
 * derived from the peer's address. Called when there is a frame to send and no
 * negotiated Tx cell to that peer.
 */
static void
auto_tx_add(const linkaddr_t *peer)
{
  struct tsch_slotframe *sf1;
  struct tsch_link *existing;
  struct msf_auto_tx *e = NULL;
  uint16_t slot, channel;
  uint8_t options = LINK_OPTION_TX | LINK_OPTION_SHARED;
  uint8_t replaced = 0;
  uint8_t i;

  if(auto_tx_find(peer) != NULL || tsch_is_locked()) {
    return;
  }
  if(cells_count(peer, LINK_OPTION_TX, MSF_ROLE_INITIATOR) > 0 ||
     cells_count(peer, LINK_OPTION_TX, MSF_ROLE_RESPONDER) > 0) {
    return;
  }
  sf1 = get_slotframe(MSF_SLOTFRAME_AUTONOMOUS);
  if(sf1 == NULL) {
    return;
  }
  for(i = 0; i < MSF_MAX_AUTO_TX_CELLS; i++) {
    if(!auto_tx[i].in_use) {
      e = &auto_tx[i];
      break;
    }
  }
  if(e == NULL) {
    return; /* the frame goes out on the minimal cell */
  }

  slot = auto_slot_of(peer);
  channel = auto_channel_of(peer);
  existing = tsch_schedule_get_link_by_timeslot(sf1, slot);
  if(existing != NULL) {
    if(slot != auto_rx_slot) {
      return; /* taken by another AutoTxCell: use the minimal cell */
    }
    /*
     * Hash collision with the AutoRxCell: the AutoTxCell takes precedence, but
     * when there is nothing to send (back-off) the slot is used for reception.
     * With the same channelOffset one Tx|Rx link covers both; otherwise the
     * AutoRxCell is suspended until the AutoTxCell is removed.
     */
    if(channel == auto_rx_channel) {
      options |= LINK_OPTION_RX;
    }
    replaced = 1;
  }

  if(tsch_schedule_add_link(sf1, options, LINK_TYPE_NORMAL, peer, slot, channel, 1) == NULL) {
    return;
  }
  e->replaced_auto_rx = replaced;
  linkaddr_copy(&e->peer, peer);
  e->slot = slot;
  e->channel = channel;
  e->in_use = 1;
}
/*---------------------------------------------------------------------------*/
static void
auto_tx_remove(struct msf_auto_tx *e)
{
  struct tsch_slotframe *sf1 = get_slotframe(MSF_SLOTFRAME_AUTONOMOUS);

  if(sf1 != NULL) {
    tsch_schedule_remove_link_by_offsets(sf1, e->slot, e->channel);
    if(e->replaced_auto_rx) {
      auto_rx_install();
    }
  }
  e->in_use = 0;
}
/*---------------------------------------------------------------------------*/
/*
 * Remove an AutoTxCell when there is no frame left for it, or when a
 * negotiated Tx cell to that neighbor exists.
 */
static void
auto_tx_cleanup(void)
{
  uint8_t i;
  for(i = 0; i < MSF_MAX_AUTO_TX_CELLS; i++) {
    struct msf_auto_tx *e = &auto_tx[i];
    struct tsch_neighbor *n;
    if(!e->in_use) {
      continue;
    }
    n = tsch_queue_get_nbr(&e->peer);
    if(n == NULL || tsch_queue_is_empty(n) ||
       cells_count(&e->peer, LINK_OPTION_TX, MSF_ROLE_INITIATOR) > 0 ||
       cells_count(&e->peer, LINK_OPTION_TX, MSF_ROLE_RESPONDER) > 0) {
      auto_tx_remove(e);
    }
  }
}

/*---------------------------------------------------------------------------*/
/* TSCH callbacks (interrupt context, keep them short): RFC 9033 Section 5   */
/*---------------------------------------------------------------------------*/

/*
 * Section 5.1: increment NumCellsElapsed; when it reaches MAX_NUM_CELLS take
 * the add/delete decision and restart both counters.
 */
static void
count_elapsed(uint8_t dir)
{
  struct msf_traffic *t = &traffic[dir];

  t->elapsed++;
  if(t->elapsed >= MSF_MAX_NUM_CELLS) {
    if(t->used > MSF_LIM_NUMCELLSUSED_HIGH) {
      decision[dir] = MSF_ACTION_ADD;
    } else if(t->used < MSF_LIM_NUMCELLSUSED_LOW) {
      decision[dir] = MSF_ACTION_DELETE;
    }
    t->elapsed = 0;
    t->used = 0;
    process_poll(&msf_process);
  }
}
/*---------------------------------------------------------------------------*/
static void
count_used(uint8_t dir)
{
  if(traffic[dir].used < 255) {
    traffic[dir].used++;
  }
}
/*---------------------------------------------------------------------------*/
/*
 * Section 5.1, NumCellsElapsed: incremented when the current cell is a
 * negotiated cell to the selected parent (whether or not it is used). For
 * downstream traffic the AutoRxCell counts too while no negotiated Rx cell
 * exists.
 */
void
msf_callback_slot_start(struct tsch_link *link)
{
  if(link == NULL || !have_parent) {
    return;
  }
  if(link->slotframe_handle == MSF_SLOTFRAME_NEGOTIATED) {
    if(linkaddr_cmp(&link->addr, &parent)) {
      count_elapsed((link->link_options & LINK_OPTION_TX) ? MSF_DIR_TX : MSF_DIR_RX);
    }
  } else if(link->slotframe_handle == MSF_SLOTFRAME_AUTONOMOUS &&
            link->timeslot == auto_rx_slot &&
            (link->link_options & LINK_OPTION_RX) && parent_rx_cells == 0) {
    count_elapsed(MSF_DIR_RX);
  }
}
/*---------------------------------------------------------------------------*/
/*
 * Section 5.1, NumCellsUsed (Tx): a frame was sent to the parent on a
 * negotiated cell, whether or not it was acknowledged.
 * Section 5.3: per-cell NumTx / NumTxAck; both are halved when NumTx reaches
 * MAX_NUMTX, which keeps the PDR and lets the counters keep growing.
 */
void
msf_callback_tx_done(struct tsch_link *link, struct tsch_neighbor *n, uint8_t mac_tx_status)
{
  struct msf_cell *c;

  if(link == NULL || link->slotframe_handle != MSF_SLOTFRAME_NEGOTIATED ||
     !(link->link_options & LINK_OPTION_TX)) {
    return;
  }
  c = cell_lookup_by_position(link->timeslot, link->channel_offset, LINK_OPTION_TX);
  if(c != NULL) {
    c->last_active = clock_time();
    c->num_tx++;
    if(mac_tx_status == MAC_TX_OK) {
      c->num_tx_ack++;
    }
    if(c->num_tx >= MSF_MAX_NUMTX) {
      c->num_tx /= 2;
      c->num_tx_ack /= 2;
      c->stats_valid = 1;
    }
  }
  if(have_parent && linkaddr_cmp(&link->addr, &parent)) {
    count_used(MSF_DIR_TX);
  }
}
/*---------------------------------------------------------------------------*/
/*
 * Section 5.1, NumCellsUsed (Rx): a valid frame was received from the parent
 * on a negotiated Rx cell (or on the AutoRxCell while there is none).
 */
void
msf_callback_rx_frame(struct tsch_link *link, const linkaddr_t *src)
{
  struct msf_cell *c;

  if(link == NULL) {
    return;
  }
  if(link->slotframe_handle == MSF_SLOTFRAME_NEGOTIATED) {
    if(!(link->link_options & LINK_OPTION_RX)) {
      return;
    }
    c = cell_lookup_by_position(link->timeslot, link->channel_offset, LINK_OPTION_RX);
    if(c != NULL) {
      c->last_active = clock_time();
    }
    if(have_parent && linkaddr_cmp(&link->addr, &parent) && linkaddr_cmp(src, &parent)) {
      count_used(MSF_DIR_RX);
    }
  } else if(link->slotframe_handle == MSF_SLOTFRAME_AUTONOMOUS &&
            link->timeslot == auto_rx_slot &&
            (link->link_options & LINK_OPTION_RX) && parent_rx_cells == 0 &&
            have_parent && linkaddr_cmp(src, &parent)) {
    count_used(MSF_DIR_RX);
  }
}
/*---------------------------------------------------------------------------*/
/*
 * Section 3: called by TSCH for each frame handed to its queues; installs an
 * AutoTxCell towards the unicast destination if it has no negotiated Tx cell.
 */
int
msf_callback_packet_ready(void)
{
  const linkaddr_t *dest = packetbuf_addr(PACKETBUF_ADDR_RECEIVER);

  if(msf_ready && dest != NULL &&
     !linkaddr_cmp(dest, &linkaddr_null) &&
     !linkaddr_cmp(dest, &tsch_broadcast_address) &&
     !linkaddr_cmp(dest, &linkaddr_node_addr)) {
    auto_tx_add(dest);
  }
  return 0;
}

/*---------------------------------------------------------------------------*/
/* Quarantine and CLEAR queue: RFC 9033 Section 12                           */
/*---------------------------------------------------------------------------*/

static int
is_quarantined(const linkaddr_t *peer)
{
  uint8_t i;
  clock_time_t now = clock_time();

  for(i = 0; i < MSF_MAX_QUARANTINE; i++) {
    struct msf_quarantine_entry *q = &quarantine[i];
    if(q->in_use && linkaddr_cmp(&q->peer, peer)) {
      if(MSF_ELAPSED(now, q->since) < MSF_QUARANTINE_DURATION) {
        return 1;
      }
      q->in_use = 0;
    }
  }
  return 0;
}
/*---------------------------------------------------------------------------*/
static void
quarantine_add(const linkaddr_t *peer)
{
  uint8_t i;
  struct msf_quarantine_entry *slot = &quarantine[0];

  for(i = 0; i < MSF_MAX_QUARANTINE; i++) {
    if(!quarantine[i].in_use || linkaddr_cmp(&quarantine[i].peer, peer)) {
      slot = &quarantine[i];
      break;
    }
  }
  slot->in_use = 1;
  linkaddr_copy(&slot->peer, peer);
  slot->since = clock_time();
}
/*---------------------------------------------------------------------------*/
/*
 * "clear": remove all cells scheduled with the neighbor from the local
 * schedule and issue a 6P CLEAR to it (the command may fail at link layer).
 */
static void
queue_clear(const linkaddr_t *peer)
{
  uint8_t i;

  cells_remove_all(peer);
  for(i = 0; i < MSF_MAX_CLEAR_QUEUE; i++) {
    if(clear_queue[i].in_use && linkaddr_cmp(&clear_queue[i].peer, peer)) {
      return;
    }
  }
  for(i = 0; i < MSF_MAX_CLEAR_QUEUE; i++) {
    if(!clear_queue[i].in_use) {
      clear_queue[i].in_use = 1;
      clear_queue[i].attempts = 0;
      linkaddr_copy(&clear_queue[i].peer, peer);
      process_poll(&msf_process);
      return;
    }
  }
}
/*---------------------------------------------------------------------------*/
/* "quarantine": same as "clear", and no 6P exchange with the neighbor for QUARANTINE_DURATION. */
static void
quarantine_peer(const linkaddr_t *peer)
{
  LOG_WARN("quarantine ");
  LOG_WARN_LLADDR(peer);
  LOG_WARN_("\n");
  quarantine_add(peer);
  queue_clear(peer);
}

/*---------------------------------------------------------------------------*/
/* 6P requests we initiate: RFC 9033 Section 4.6, 5.1, 5.2, 5.3 and 8        */
/*---------------------------------------------------------------------------*/

static void
request_sent_callback(void *arg, uint16_t arg_len, const linkaddr_t *dest_addr,
                      sixp_output_status_t status)
{
  if(status != SIXP_OUTPUT_STATUS_SUCCESS && txn.busy &&
     linkaddr_cmp(dest_addr, &txn.peer)) {
    LOG_WARN("6P request was not delivered\n");
    txn.busy = 0;
    hold_wait_retry();
  }
}
/*---------------------------------------------------------------------------*/
static int
txn_send(const linkaddr_t *peer, sixp_pkt_cmd_t cmd, uint16_t len)
{
  linkaddr_copy(&txn.peer, peer);
  txn.cmd = cmd;
  txn.busy = 1;
  if(sixp_output(SIXP_PKT_TYPE_REQUEST, MSF_CODE(cmd), MSF_SFID,
                 req_buf, len, &txn.peer, request_sent_callback, NULL, 0) != 0) {
    txn.busy = 0;
    hold_short();
    return -1;
  }
  return 0;
}
/*---------------------------------------------------------------------------*/
/* Fill Metadata (unused by MSF, Section 11), CellOptions and NumCells. */
static int
build_request_header(sixp_pkt_cmd_t cmd, uint8_t dir)
{
  memset(req_buf, 0, sizeof(req_buf));
  return sixp_pkt_set_metadata(SIXP_PKT_TYPE_REQUEST, MSF_CODE(cmd), 0,
                               req_buf, sizeof(req_buf)) == 0 &&
         sixp_pkt_set_cell_options(SIXP_PKT_TYPE_REQUEST, MSF_CODE(cmd),
                                   MSF_DIR_TO_CELL_OPTION(dir),
                                   req_buf, sizeof(req_buf)) == 0 &&
         sixp_pkt_set_num_cells(SIXP_PKT_TYPE_REQUEST, MSF_CODE(cmd), 1,
                                req_buf, sizeof(req_buf)) == 0 ? 0 : -1;
}
/*---------------------------------------------------------------------------*/
/* Fill the CellList with the candidate cells picked in txn.cand. */
static int
set_candidates(sixp_pkt_cmd_t cmd, uint8_t as_cand_cell_list)
{
  uint8_t list[MSF_6P_CELL_SIZE * MSF_NUM_CANDIDATE_CELLS];
  uint8_t i;

  for(i = 0; i < txn.num_cand; i++) {
    put_cell(&list[i * MSF_6P_CELL_SIZE], txn.cand[i].slot, txn.cand[i].channel);
  }
  if(as_cand_cell_list) {
    return sixp_pkt_set_cand_cell_list(SIXP_PKT_TYPE_REQUEST, MSF_CODE(cmd),
                                       list, txn.num_cand * MSF_6P_CELL_SIZE, 0,
                                       req_buf, sizeof(req_buf));
  }
  return sixp_pkt_set_cell_list(SIXP_PKT_TYPE_REQUEST, MSF_CODE(cmd),
                                list, txn.num_cand * MSF_6P_CELL_SIZE, 0,
                                req_buf, sizeof(req_buf));
}
/*---------------------------------------------------------------------------*/
/*
 * 6P ADD with the parent. Section 4.6 (first Tx cell, and cells added while
 * switching parent) and Section 5.1: NumCells = 1, CellList with at least
 * five candidate cells picked as described in Section 8.
 */
static int
send_add(uint8_t dir)
{
  memset(&txn, 0, sizeof(txn));
  txn.dir = dir;
  txn.num_cand = pick_candidate_cells(txn.cand, MSF_NUM_CANDIDATE_CELLS);
  if(txn.num_cand == 0) {
    return -1;
  }
  if(build_request_header(SIXP_PKT_CMD_ADD, dir) != 0 ||
     set_candidates(SIXP_PKT_CMD_ADD, 0) != 0) {
    return -1;
  }
  LOG_INFO("send 6P ADD (%s, %u candidate cells) to parent ",
           dir == MSF_DIR_TX ? "Tx" : "Rx", txn.num_cand);
  LOG_INFO_LLADDR(&parent);
  LOG_INFO_("\n");
  return txn_send(&parent, SIXP_PKT_CMD_ADD,
                  MSF_6P_REQ_HDR_SIZE + txn.num_cand * MSF_6P_CELL_SIZE);
}
/*---------------------------------------------------------------------------*/
/* 6P DELETE of a single cell, Section 5.1. */
static int
send_delete(const struct msf_cell *c, uint8_t dir)
{
  uint8_t list[MSF_6P_CELL_SIZE];

  memset(&txn, 0, sizeof(txn));
  txn.dir = dir;
  txn.rel.slot = c->slot;
  txn.rel.channel = c->channel;
  if(build_request_header(SIXP_PKT_CMD_DELETE, dir) != 0) {
    return -1;
  }
  put_cell(list, c->slot, c->channel);
  if(sixp_pkt_set_cell_list(SIXP_PKT_TYPE_REQUEST, MSF_CODE(SIXP_PKT_CMD_DELETE),
                            list, sizeof(list), 0, req_buf, sizeof(req_buf)) != 0) {
    return -1;
  }
  LOG_INFO("send 6P DELETE (%s cell slot %u ch %u) to parent ",
           dir == MSF_DIR_TX ? "Tx" : "Rx", c->slot, c->channel);
  LOG_INFO_LLADDR(&parent);
  LOG_INFO_("\n");
  return txn_send(&parent, SIXP_PKT_CMD_DELETE, MSF_6P_REQ_HDR_SIZE + sizeof(list));
}
/*---------------------------------------------------------------------------*/
/* 6P RELOCATE of a negotiated Tx cell, Section 5.3 (Rx relocation is not supported). */
static int
send_relocate(const struct msf_cell *c)
{
  uint8_t rel[MSF_6P_CELL_SIZE];

  memset(&txn, 0, sizeof(txn));
  txn.dir = MSF_DIR_TX;
  txn.rel.slot = c->slot;
  txn.rel.channel = c->channel;
  txn.num_cand = pick_candidate_cells(txn.cand, MSF_NUM_CANDIDATE_CELLS);
  if(txn.num_cand == 0) {
    return -1;
  }
  if(build_request_header(SIXP_PKT_CMD_RELOCATE, MSF_DIR_TX) != 0) {
    return -1;
  }
  put_cell(rel, c->slot, c->channel);
  if(sixp_pkt_set_rel_cell_list(SIXP_PKT_TYPE_REQUEST, MSF_CODE(SIXP_PKT_CMD_RELOCATE),
                                rel, sizeof(rel), 0, req_buf, sizeof(req_buf)) != 0 ||
     set_candidates(SIXP_PKT_CMD_RELOCATE, 1) != 0) {
    return -1;
  }
  LOG_INFO("send 6P RELOCATE (Tx cell slot %u ch %u) to parent ", c->slot, c->channel);
  LOG_INFO_LLADDR(&parent);
  LOG_INFO_("\n");
  return txn_send(&parent, SIXP_PKT_CMD_RELOCATE,
                  MSF_6P_REQ_HDR_SIZE + sizeof(rel) + txn.num_cand * MSF_6P_CELL_SIZE);
}
/*---------------------------------------------------------------------------*/
/* 6P CLEAR: Metadata only. */
static int
send_clear(const linkaddr_t *peer)
{
  memset(&txn, 0, sizeof(txn));
  memset(req_buf, 0, sizeof(req_buf));
  if(sixp_pkt_set_metadata(SIXP_PKT_TYPE_REQUEST, MSF_CODE(SIXP_PKT_CMD_CLEAR), 0,
                           req_buf, sizeof(req_buf)) != 0) {
    return -1;
  }
  LOG_INFO("send 6P CLEAR to ");
  LOG_INFO_LLADDR(peer);
  LOG_INFO_("\n");
  return txn_send(peer, SIXP_PKT_CMD_CLEAR, sizeof(sixp_pkt_metadata_t));
}

/*---------------------------------------------------------------------------*/
/* 6P responses to our requests                                              */
/*---------------------------------------------------------------------------*/

static void
complete_add(const uint8_t *body, uint16_t body_len)
{
  const uint8_t *list;
  sixp_pkt_offset_t list_len;
  sixp_pkt_offset_t i;
  int installed = 0;

  if(sixp_pkt_get_cell_list(SIXP_PKT_TYPE_RESPONSE, MSF_CODE(SIXP_PKT_RC_SUCCESS),
                            &list, &list_len, body, body_len) != 0) {
    LOG_WARN("malformed 6P ADD response\n");
    return;
  }
  for(i = 0; i + MSF_6P_CELL_SIZE <= list_len && !installed; i += MSF_6P_CELL_SIZE) {
    struct msf_cell_id id;
    get_cell(&list[i], &id);
    /* The parent must choose among the cells we offered. */
    if(cell_id_in_list(txn.cand, txn.num_cand, &id) && slot_is_free(id.slot) &&
       cell_install(&txn.peer, MSF_DIR_TO_LINK_OPTION(txn.dir), id.slot, id.channel,
                    MSF_ROLE_INITIATOR) != NULL) {
      installed = 1;
    }
  }
  if(!installed) {
    LOG_WARN("6P ADD response without a usable cell\n");
  }
}
/*---------------------------------------------------------------------------*/
static void
complete_delete(const uint8_t *body, uint16_t body_len)
{
  const uint8_t *list;
  sixp_pkt_offset_t list_len;
  sixp_pkt_offset_t i;

  if(sixp_pkt_get_cell_list(SIXP_PKT_TYPE_RESPONSE, MSF_CODE(SIXP_PKT_RC_SUCCESS),
                            &list, &list_len, body, body_len) != 0) {
    return;
  }
  for(i = 0; i + MSF_6P_CELL_SIZE <= list_len; i += MSF_6P_CELL_SIZE) {
    struct msf_cell_id id;
    struct msf_cell *c;
    get_cell(&list[i], &id);
    c = cell_lookup(&txn.peer, MSF_DIR_TO_LINK_OPTION(txn.dir), id.slot, id.channel);
    if(c != NULL) {
      cell_remove(c);
    }
  }
}
/*---------------------------------------------------------------------------*/
static void
complete_relocate(const uint8_t *body, uint16_t body_len)
{
  const uint8_t *list;
  sixp_pkt_offset_t list_len;
  sixp_pkt_offset_t i;
  struct msf_cell *old;

  if(sixp_pkt_get_cell_list(SIXP_PKT_TYPE_RESPONSE, MSF_CODE(SIXP_PKT_RC_SUCCESS),
                            &list, &list_len, body, body_len) != 0) {
    return;
  }
  old = cell_lookup(&txn.peer, LINK_OPTION_TX, txn.rel.slot, txn.rel.channel);
  for(i = 0; i + MSF_6P_CELL_SIZE <= list_len; i += MSF_6P_CELL_SIZE) {
    struct msf_cell_id id;
    get_cell(&list[i], &id);
    if(old != NULL && cell_id_in_list(txn.cand, txn.num_cand, &id) && slot_is_free(id.slot)) {
      cell_remove(old);
      cell_install(&txn.peer, LINK_OPTION_TX, id.slot, id.channel, MSF_ROLE_INITIATOR);
      return;
    }
  }
  /* An empty CellList means the parent found no free cell: keep the old cell. */
}
/*---------------------------------------------------------------------------*/
static void
response_input(sixp_pkt_rc_t rc, const uint8_t *body, uint16_t body_len,
               const linkaddr_t *src)
{
  if(!txn.busy || !linkaddr_cmp(src, &txn.peer)) {
    return;
  }
  txn.busy = 0;
  if(txn.cmd == SIXP_PKT_CMD_CLEAR) {
    return; /* best effort: the local cells are already gone */
  }

  switch(rc) {
  case SIXP_PKT_RC_SUCCESS:
  case SIXP_PKT_RC_EOL:
    switch(txn.cmd) {
    case SIXP_PKT_CMD_ADD:
      complete_add(body, body_len);
      break;
    case SIXP_PKT_CMD_DELETE:
      complete_delete(body, body_len);
      break;
    case SIXP_PKT_CMD_RELOCATE:
      complete_relocate(body, body_len);
      break;
    default:
      break;
    }
    break;
  /* RFC 9033 Section 12, Table 1 */
  case SIXP_PKT_RC_ERR:
  case SIXP_PKT_RC_RESET:
  case SIXP_PKT_RC_ERR_VERSION:
  case SIXP_PKT_RC_ERR_SFID:
    quarantine_peer(src);
    break;
  case SIXP_PKT_RC_ERR_SEQNUM:
  case SIXP_PKT_RC_ERR_CELLLIST:
    queue_clear(src);
    hold_wait_retry();
    break;
  case SIXP_PKT_RC_ERR_BUSY:
  case SIXP_PKT_RC_ERR_LOCKED:
    hold_wait_retry();
    break;
  default:
    hold_wait_retry();
    break;
  }
}

/*---------------------------------------------------------------------------*/
/* 6P requests we respond to (we are the parent of the requester)            */
/*---------------------------------------------------------------------------*/

static void
respond_error(const linkaddr_t *dest, sixp_pkt_rc_t rc)
{
  sixp_output(SIXP_PKT_TYPE_RESPONSE, MSF_CODE(rc), MSF_SFID,
              NULL, 0, dest, NULL, NULL, 0);
}
/*---------------------------------------------------------------------------*/
static struct msf_pending *
pending_alloc(const linkaddr_t *peer, sixp_pkt_cmd_t cmd)
{
  uint8_t i;
  for(i = 0; i < MSF_MAX_PENDING_RESPONSES; i++) {
    if(!pending[i].in_use) {
      memset(&pending[i], 0, sizeof(pending[i]));
      pending[i].in_use = 1;
      pending[i].cmd = cmd;
      pending[i].created = clock_time();
      linkaddr_copy(&pending[i].peer, peer);
      return &pending[i];
    }
  }
  return NULL;
}
/*---------------------------------------------------------------------------*/
/* Apply the schedule change once the response has been transmitted. */
static void
pending_apply(struct msf_pending *op)
{
  uint8_t i;

  switch(op->cmd) {
  case SIXP_PKT_CMD_ADD:
    for(i = 0; i < op->num_new; i++) {
      if(slot_is_free(op->new_cells[i].slot)) {
        cell_install(&op->peer, op->link_options, op->new_cells[i].slot,
                     op->new_cells[i].channel, MSF_ROLE_RESPONDER);
      }
    }
    break;
  case SIXP_PKT_CMD_DELETE:
    for(i = 0; i < op->num_old; i++) {
      struct msf_cell *c = cell_lookup(&op->peer, op->link_options,
                                       op->old_cells[i].slot, op->old_cells[i].channel);
      if(c != NULL) {
        cell_remove(c);
      }
    }
    break;
  case SIXP_PKT_CMD_RELOCATE:
    if(op->num_new > 0 && op->num_old > 0) {
      struct msf_cell *c = cell_lookup(&op->peer, op->link_options,
                                       op->old_cells[0].slot, op->old_cells[0].channel);
      if(c != NULL && slot_is_free(op->new_cells[0].slot)) {
        cell_remove(c);
        cell_install(&op->peer, op->link_options, op->new_cells[0].slot,
                     op->new_cells[0].channel, MSF_ROLE_RESPONDER);
      }
    }
    break;
  default:
    break;
  }
}
/*---------------------------------------------------------------------------*/
static void
response_sent_callback(void *arg, uint16_t arg_len, const linkaddr_t *dest_addr,
                       sixp_output_status_t status)
{
  struct msf_pending *op = (struct msf_pending *)arg;

  if(op == NULL) {
    return;
  }
  if(status == SIXP_OUTPUT_STATUS_SUCCESS) {
    pending_apply(op);
  }
  op->in_use = 0;
}
/*---------------------------------------------------------------------------*/
static void
send_response(struct msf_pending *op, const uint8_t *cell_list, uint8_t num_cells)
{
  if(sixp_output(SIXP_PKT_TYPE_RESPONSE, MSF_CODE(SIXP_PKT_RC_SUCCESS), MSF_SFID,
                 cell_list, num_cells * MSF_6P_CELL_SIZE, &op->peer,
                 response_sent_callback, op, sizeof(*op)) != 0) {
    op->in_use = 0;
  }
}
/*---------------------------------------------------------------------------*/
/* Section 8 rules for a cell offered by a peer: usable slot, no duplicate. */
static int
offered_cell_is_usable(const struct msf_cell_id *id, const struct msf_cell_id *chosen,
                       uint8_t num_chosen)
{
  uint8_t i;
  if(!slot_is_free(id->slot)) {
    return 0;
  }
  for(i = 0; i < num_chosen; i++) {
    if(chosen[i].slot == id->slot) {
      return 0;
    }
  }
  return 1;
}
/*---------------------------------------------------------------------------*/
/*
 * 6P ADD received: choose up to NumCells cells from the CellList. Cell options
 * are those of the requester: TX from the requester means we install an Rx cell.
 * No usable cell results in an empty CellList with RC_SUCCESS (RFC 8480).
 */
static void
handle_add_request(const uint8_t *body, uint16_t body_len, const linkaddr_t *src)
{
  sixp_pkt_cell_options_t options;
  sixp_pkt_num_cells_t num_cells;
  const uint8_t *list;
  sixp_pkt_offset_t list_len;
  sixp_pkt_offset_t i;
  struct msf_pending *op;
  uint8_t resp[MSF_6P_CELL_SIZE * MSF_MAX_CELLS_PER_REQUEST];
  sixp_pkt_code_t code = MSF_CODE(SIXP_PKT_CMD_ADD);

  if(sixp_pkt_get_cell_options(SIXP_PKT_TYPE_REQUEST, code, &options, body, body_len) != 0 ||
     sixp_pkt_get_num_cells(SIXP_PKT_TYPE_REQUEST, code, &num_cells, body, body_len) != 0 ||
     sixp_pkt_get_cell_list(SIXP_PKT_TYPE_REQUEST, code, &list, &list_len, body, body_len) != 0) {
    respond_error(src, SIXP_PKT_RC_ERR);
    return;
  }
  /* Exactly one of TX / RX, and never SHARED (Section 5.1 and 8). */
  if((options & SIXP_PKT_CELL_OPTION_SHARED) || num_cells == 0 ||
     !!(options & SIXP_PKT_CELL_OPTION_TX) == !!(options & SIXP_PKT_CELL_OPTION_RX)) {
    respond_error(src, SIXP_PKT_RC_ERR_CELLLIST);
    return;
  }
  op = pending_alloc(src, SIXP_PKT_CMD_ADD);
  if(op == NULL) {
    respond_error(src, SIXP_PKT_RC_ERR_BUSY);
    return;
  }
  op->link_options = (options & SIXP_PKT_CELL_OPTION_TX) ? LINK_OPTION_RX : LINK_OPTION_TX;

  for(i = 0; i + MSF_6P_CELL_SIZE <= list_len && op->num_new < num_cells &&
      op->num_new < MSF_MAX_CELLS_PER_REQUEST && cells_free() > op->num_new;
      i += MSF_6P_CELL_SIZE) {
    struct msf_cell_id id;
    get_cell(&list[i], &id);
    if(offered_cell_is_usable(&id, op->new_cells, op->num_new)) {
      op->new_cells[op->num_new] = id;
      sixp_pkt_set_cell_list(SIXP_PKT_TYPE_RESPONSE, MSF_CODE(SIXP_PKT_RC_SUCCESS),
                             &list[i], MSF_6P_CELL_SIZE,
                             op->num_new * MSF_6P_CELL_SIZE, resp, sizeof(resp));
      op->num_new++;
    }
  }
  LOG_INFO("6P ADD from ");
  LOG_INFO_LLADDR(src);
  LOG_INFO_(": %u cell(s) granted\n", op->num_new);
  send_response(op, resp, op->num_new);
}
/*---------------------------------------------------------------------------*/
/* 6P DELETE received: the response lists the cells that were found and are removed. */
static void
handle_delete_request(const uint8_t *body, uint16_t body_len, const linkaddr_t *src)
{
  sixp_pkt_cell_options_t options;
  sixp_pkt_num_cells_t num_cells;
  const uint8_t *list;
  sixp_pkt_offset_t list_len;
  sixp_pkt_offset_t i;
  struct msf_pending *op;
  uint8_t resp[MSF_6P_CELL_SIZE * MSF_MAX_CELLS_PER_REQUEST];
  sixp_pkt_code_t code = MSF_CODE(SIXP_PKT_CMD_DELETE);

  if(sixp_pkt_get_cell_options(SIXP_PKT_TYPE_REQUEST, code, &options, body, body_len) != 0 ||
     sixp_pkt_get_num_cells(SIXP_PKT_TYPE_REQUEST, code, &num_cells, body, body_len) != 0 ||
     sixp_pkt_get_cell_list(SIXP_PKT_TYPE_REQUEST, code, &list, &list_len, body, body_len) != 0) {
    respond_error(src, SIXP_PKT_RC_ERR);
    return;
  }
  op = pending_alloc(src, SIXP_PKT_CMD_DELETE);
  if(op == NULL) {
    respond_error(src, SIXP_PKT_RC_ERR_BUSY);
    return;
  }
  op->link_options = (options & SIXP_PKT_CELL_OPTION_TX) ? LINK_OPTION_RX : LINK_OPTION_TX;

  for(i = 0; i + MSF_6P_CELL_SIZE <= list_len && op->num_old < num_cells &&
      op->num_old < MSF_MAX_CELLS_PER_REQUEST; i += MSF_6P_CELL_SIZE) {
    struct msf_cell_id id;
    struct msf_cell *c;
    get_cell(&list[i], &id);
    c = cell_lookup(src, op->link_options, id.slot, id.channel);
    if(c != NULL && c->role == MSF_ROLE_RESPONDER) {
      op->old_cells[op->num_old] = id;
      sixp_pkt_set_cell_list(SIXP_PKT_TYPE_RESPONSE, MSF_CODE(SIXP_PKT_RC_SUCCESS),
                             &list[i], MSF_6P_CELL_SIZE,
                             op->num_old * MSF_6P_CELL_SIZE, resp, sizeof(resp));
      op->num_old++;
    }
  }
  if(op->num_old == 0) {
    op->in_use = 0;
    respond_error(src, SIXP_PKT_RC_ERR_CELLLIST);
    return;
  }
  send_response(op, resp, op->num_old);
}
/*---------------------------------------------------------------------------*/
/*
 * 6P RELOCATE received (Section 5.3): move one of the requester's Tx cells,
 * i.e. our Rx cell, to the first usable cell of the candidate list.
 */
static void
handle_relocate_request(const uint8_t *body, uint16_t body_len, const linkaddr_t *src)
{
  sixp_pkt_cell_options_t options;
  sixp_pkt_num_cells_t num_cells;
  const uint8_t *rel_list, *cand_list;
  sixp_pkt_offset_t rel_len, cand_len, i;
  struct msf_cell_id rel;
  struct msf_pending *op;
  struct msf_cell *old;
  uint8_t resp[MSF_6P_CELL_SIZE];
  sixp_pkt_code_t code = MSF_CODE(SIXP_PKT_CMD_RELOCATE);

  if(sixp_pkt_get_cell_options(SIXP_PKT_TYPE_REQUEST, code, &options, body, body_len) != 0 ||
     sixp_pkt_get_num_cells(SIXP_PKT_TYPE_REQUEST, code, &num_cells, body, body_len) != 0 ||
     sixp_pkt_get_rel_cell_list(SIXP_PKT_TYPE_REQUEST, code, &rel_list, &rel_len, body, body_len) != 0 ||
     sixp_pkt_get_cand_cell_list(SIXP_PKT_TYPE_REQUEST, code, &cand_list, &cand_len, body, body_len) != 0) {
    respond_error(src, SIXP_PKT_RC_ERR);
    return;
  }
  /* "The RELOCATION for negotiated Rx cells is not supported by MSF." */
  if(options != SIXP_PKT_CELL_OPTION_TX || rel_len < MSF_6P_CELL_SIZE) {
    respond_error(src, SIXP_PKT_RC_ERR_CELLLIST);
    return;
  }
  get_cell(rel_list, &rel);
  old = cell_lookup(src, LINK_OPTION_RX, rel.slot, rel.channel);
  if(old == NULL || old->role != MSF_ROLE_RESPONDER) {
    respond_error(src, SIXP_PKT_RC_ERR_CELLLIST);
    return;
  }
  op = pending_alloc(src, SIXP_PKT_CMD_RELOCATE);
  if(op == NULL) {
    respond_error(src, SIXP_PKT_RC_ERR_BUSY);
    return;
  }
  op->link_options = LINK_OPTION_RX;
  op->old_cells[0] = rel;
  op->num_old = 1;
  for(i = 0; i + MSF_6P_CELL_SIZE <= cand_len && op->num_new == 0; i += MSF_6P_CELL_SIZE) {
    struct msf_cell_id id;
    get_cell(&cand_list[i], &id);
    if(offered_cell_is_usable(&id, op->new_cells, 0)) {
      op->new_cells[0] = id;
      sixp_pkt_set_cell_list(SIXP_PKT_TYPE_RESPONSE, MSF_CODE(SIXP_PKT_RC_SUCCESS),
                             &cand_list[i], MSF_6P_CELL_SIZE, 0, resp, sizeof(resp));
      op->num_new = 1;
    }
  }
  send_response(op, resp, op->num_new);
}
/*---------------------------------------------------------------------------*/
/* 6P CLEAR received: erase every negotiated cell with the requester, never the autonomous cells. */
static void
handle_clear_request(const linkaddr_t *src)
{
  LOG_INFO("6P CLEAR from ");
  LOG_INFO_LLADDR(src);
  LOG_INFO_("\n");
  cells_remove_all(src);
  if(have_parent && linkaddr_cmp(src, &parent)) {
    traffic_reset();
  }
  sixp_output(SIXP_PKT_TYPE_RESPONSE, MSF_CODE(SIXP_PKT_RC_SUCCESS), MSF_SFID,
              NULL, 0, src, NULL, NULL, 0);
}

/*---------------------------------------------------------------------------*/
/* Scheduling function driver: RFC 9033 Section 7 and 9                      */
/*---------------------------------------------------------------------------*/

static void
sf_input(sixp_pkt_type_t type, sixp_pkt_code_t code,
         const uint8_t *body, uint16_t body_len, const linkaddr_t *src)
{
  if(!msf_ready) {
    if(type == SIXP_PKT_TYPE_REQUEST) {
      respond_error(src, SIXP_PKT_RC_ERR_BUSY);
    }
    return;
  }
  if(is_quarantined(src)) {
    return; /* Section 12: drop everything from a quarantined neighbor */
  }

  switch(type) {
  case SIXP_PKT_TYPE_REQUEST:
    switch(code.cmd) {
    case SIXP_PKT_CMD_ADD:
      handle_add_request(body, body_len, src);
      break;
    case SIXP_PKT_CMD_DELETE:
      handle_delete_request(body, body_len, src);
      break;
    case SIXP_PKT_CMD_RELOCATE:
      handle_relocate_request(body, body_len, src);
      break;
    case SIXP_PKT_CMD_CLEAR:
      handle_clear_request(src);
      break;
    default:
      /* Section 6: SIGNAL is not used by MSF; COUNT/LIST are not needed either. */
      respond_error(src, SIXP_PKT_RC_ERR);
      break;
    }
    break;
  case SIXP_PKT_TYPE_RESPONSE:
    response_input(code.rc, body, body_len, src);
    break;
  default:
    break;
  }
}
/*---------------------------------------------------------------------------*/
/* The transaction timed out: retry after a random wait (Section 12 "waitretry"). */
static void
sf_timeout(sixp_pkt_cmd_t cmd, const linkaddr_t *peer_addr)
{
  if(txn.busy && linkaddr_cmp(peer_addr, &txn.peer)) {
    LOG_WARN("6P transaction timed out\n");
    txn.busy = 0;
    if(txn.cmd != SIXP_PKT_CMD_CLEAR) {
      hold_wait_retry();
    }
  }
}
/*---------------------------------------------------------------------------*/
/* Section 13: schedule inconsistency is handled like RC_ERR_SEQNUM ("clear"). */
static void
sf_error(sixp_error_t err, sixp_pkt_cmd_t cmd, uint8_t seqno,
         const linkaddr_t *peer_addr)
{
  if(err == SIXP_ERROR_SCHEDULE_INCONSISTENCY) {
    LOG_WARN("schedule inconsistency with ");
    LOG_WARN_LLADDR(peer_addr);
    LOG_WARN_("\n");
    queue_clear(peer_addr);
  }
}

/*---------------------------------------------------------------------------*/
/* Housekeeping                                                              */
/*---------------------------------------------------------------------------*/

static void
state_reset(void)
{
  struct tsch_slotframe *sf;

  msf_ready = 0;
  have_parent = 0;
  parent_rx_cells = 0;
  memset(cells, 0, sizeof(cells));
  memset(auto_tx, 0, sizeof(auto_tx));
  memset(&txn, 0, sizeof(txn));
  memset(pending, 0, sizeof(pending));
  memset(clear_queue, 0, sizeof(clear_queue));
  memset(quarantine, 0, sizeof(quarantine));
  memset(want, 0, sizeof(want));
  traffic_reset();
  sw_active = 0;
  hold_active = 0;
  reloc_pending = 0;
  last_collision_check = clock_time();

  auto_rx_slot = auto_slot_of(&linkaddr_node_addr);
  auto_rx_channel = auto_channel_of(&linkaddr_node_addr);

  if(!tsch_is_locked()) {
    if((sf = get_slotframe(MSF_SLOTFRAME_NEGOTIATED)) != NULL) {
      tsch_schedule_remove_slotframe(sf);
    }
    if((sf = get_slotframe(MSF_SLOTFRAME_AUTONOMOUS)) != NULL) {
      tsch_schedule_remove_slotframe(sf);
    }
  }
}
/*---------------------------------------------------------------------------*/
/*
 * Create slotframes 1 (autonomous) and 2 (negotiated), same length as the
 * minimal slotframe 0 (Section 2), and the AutoRxCell (Section 3). TSCH
 * rebuilds its schedule every time it (re)associates, so this is redone
 * whenever the slotframes are found missing.
 */
static int
schedule_setup(void)
{
  struct tsch_slotframe *sf1, *sf2;

  if(tsch_is_locked()) {
    return msf_ready;
  }
  sf1 = get_slotframe(MSF_SLOTFRAME_AUTONOMOUS);
  sf2 = get_slotframe(MSF_SLOTFRAME_NEGOTIATED);
  if(sf1 != NULL && sf2 != NULL &&
     tsch_schedule_get_link_by_timeslot(sf1, auto_rx_slot) != NULL) {
    return 1;
  }
  if(msf_ready) {
    LOG_WARN("TSCH schedule was rebuilt, restarting MSF\n");
    state_reset();
    sf1 = sf2 = NULL;
  }
  if(sf1 == NULL) {
    sf1 = tsch_schedule_add_slotframe(MSF_SLOTFRAME_AUTONOMOUS, MSF_SLOTFRAME_LENGTH);
  }
  if(sf2 == NULL) {
    sf2 = tsch_schedule_add_slotframe(MSF_SLOTFRAME_NEGOTIATED, MSF_SLOTFRAME_LENGTH);
  }
  if(sf1 == NULL || sf2 == NULL) {
    return 0;
  }
  auto_rx_install();
  msf_ready = 1;
  LOG_INFO("schedule ready: AutoRxCell slot %u ch %u, slotframe length %u\n",
           auto_rx_slot, auto_rx_channel, MSF_SLOTFRAME_LENGTH);
  return 1;
}
/*---------------------------------------------------------------------------*/
static void
start_session(const linkaddr_t *new_parent)
{
  have_parent = 0;
  linkaddr_copy(&parent, new_parent);
  traffic_reset();
  memset(want, 0, sizeof(want));
  hold_active = 0;
  have_parent = 1;
  refresh_counts();
  LOG_INFO("MSF session with parent ");
  LOG_INFO_LLADDR(&parent);
  LOG_INFO_("\n");
}
/*---------------------------------------------------------------------------*/
/*
 * RFC 9033 Section 5.2: count the negotiated cells with the old parent,
 * schedule as many with the new parent, then CLEAR the old parent (done in
 * check_switch_done()). NumCells* counters restart for the new parent.
 */
static void
begin_switch(const linkaddr_t *new_parent)
{
  uint8_t tx = cells_count(&parent, LINK_OPTION_TX, MSF_ROLE_INITIATOR);
  uint8_t rx = cells_count(&parent, LINK_OPTION_RX, MSF_ROLE_INITIATOR);

  if(sw_active) {
    /* Another switch while one is still open: drop the parent it was leaving. */
    queue_clear(&sw_old);
    tx = tx < sw_target[MSF_DIR_TX] ? sw_target[MSF_DIR_TX] : tx;
    rx = rx < sw_target[MSF_DIR_RX] ? sw_target[MSF_DIR_RX] : rx;
  }
  linkaddr_copy(&sw_old, &parent);
  sw_target[MSF_DIR_TX] = tx > 0 ? tx : 1;
  sw_target[MSF_DIR_RX] = rx;
  sw_start = clock_time();
  sw_active = 1;

  LOG_INFO("parent switch ");
  LOG_INFO_LLADDR(&parent);
  LOG_INFO_(" -> ");
  LOG_INFO_LLADDR(new_parent);
  LOG_INFO_(", moving %u Tx and %u Rx cells\n", sw_target[MSF_DIR_TX], sw_target[MSF_DIR_RX]);

  start_session(new_parent);
}
/*---------------------------------------------------------------------------*/
static void
check_switch_done(clock_time_t now)
{
  if(!sw_active) {
    return;
  }
  if((cells_count(&parent, LINK_OPTION_TX, MSF_ROLE_INITIATOR) >= sw_target[MSF_DIR_TX] &&
      cells_count(&parent, LINK_OPTION_RX, MSF_ROLE_INITIATOR) >= sw_target[MSF_DIR_RX]) ||
     MSF_ELAPSED(now, sw_start) >= MSF_SWITCH_TIMEOUT) {
    queue_clear(&sw_old);
    sw_active = 0;
  }
}
/*---------------------------------------------------------------------------*/
/*
 * The selected parent is the RPL preferred parent, which tsch-rpl makes the
 * TSCH time source. The session only starts once RPL has a route (Section 4.5).
 */
static void
update_parent(void)
{
  struct tsch_neighbor *time_source;
  const linkaddr_t *addr;

  if(tsch_is_coordinator) {
    return; /* the DODAG root has no parent */
  }
  time_source = tsch_queue_get_time_source();
  addr = time_source != NULL ? tsch_queue_get_nbr_address(time_source) : NULL;
  if(addr == NULL || linkaddr_cmp(addr, &linkaddr_null)) {
    return;
  }
  if(!have_parent) {
    if(NETSTACK_ROUTING.node_is_reachable()) {
      start_session(addr);
    }
  } else if(!linkaddr_cmp(addr, &parent)) {
    begin_switch(addr);
  }
}
/*---------------------------------------------------------------------------*/
/* Section 5.1: cells of vanished neighbors are reclaimed (implementation-specific cleanup). */
static void
cleanup_idle_cells(clock_time_t now)
{
  uint8_t i;
  for(i = 0; i < MSF_MAX_NEGOTIATED_CELLS; i++) {
    struct msf_cell *c = &cells[i];
    if(c->in_use && c->role == MSF_ROLE_RESPONDER &&
       MSF_ELAPSED(now, c->last_active) >= MSF_CELL_IDLE_TIMEOUT) {
      LOG_INFO("idle cell cleanup\n");
      cell_remove(c);
    }
  }
}
/*---------------------------------------------------------------------------*/
static void
reap_pending_responses(clock_time_t now)
{
  uint8_t i;
  for(i = 0; i < MSF_MAX_PENDING_RESPONSES; i++) {
    if(pending[i].in_use && MSF_ELAPSED(now, pending[i].created) >= 2 * MSF_6P_TIMEOUT) {
      pending[i].in_use = 0;
    }
  }
}
/*---------------------------------------------------------------------------*/
/* Take the decisions made by the TSCH callbacks (Section 5.1). */
static void
collect_decisions(void)
{
  uint8_t dir;
  for(dir = 0; dir < MSF_NUM_DIRS; dir++) {
    uint8_t d;
    int_master_status_t status = critical_enter();
    d = decision[dir];
    decision[dir] = MSF_ACTION_NONE;
    critical_exit(status);
    if(d != MSF_ACTION_NONE) {
      want[dir] = d;
    }
  }
}
/*---------------------------------------------------------------------------*/
/* Section 5.3: relocate a Tx cell whose PDR is well below the best one. */
static void
check_collisions(clock_time_t now)
{
  uint8_t i, valid = 0;
  uint32_t best = 0, worst = 100;
  struct msf_cell *worst_cell = NULL;

  if(MSF_ELAPSED(now, last_collision_check) < MSF_HOUSEKEEPINGCOLLISION_PERIOD) {
    return;
  }
  last_collision_check = now;
  if(!have_parent || sw_active || reloc_pending) {
    return;
  }
  for(i = 0; i < MSF_MAX_NEGOTIATED_CELLS; i++) {
    struct msf_cell *c = &cells[i];
    uint32_t pdr;
    /* Cells whose NumTx was not halved yet are not statistically significant. */
    if(!c->in_use || c->role != MSF_ROLE_INITIATOR || c->link_options != LINK_OPTION_TX ||
       !linkaddr_cmp(&c->peer, &parent) || !c->stats_valid || c->num_tx == 0) {
      continue;
    }
    pdr = (uint32_t)c->num_tx_ack * 100 / c->num_tx;
    valid++;
    if(pdr > best) {
      best = pdr;
    }
    if(pdr < worst) {
      worst = pdr;
      worst_cell = c;
    }
  }
  if(valid >= 2 && worst_cell != NULL && best - worst > MSF_RELOCATE_PDRTHRES) {
    LOG_INFO("collision suspected on Tx cell slot %u ch %u (PDR %u%% vs best %u%%)\n",
             worst_cell->slot, worst_cell->channel, (unsigned)worst, (unsigned)best);
    reloc_cell.slot = worst_cell->slot;
    reloc_cell.channel = worst_cell->channel;
    reloc_pending = 1;
  }
}
/*---------------------------------------------------------------------------*/
static int
run_clear_queue(void)
{
  uint8_t i;
  for(i = 0; i < MSF_MAX_CLEAR_QUEUE; i++) {
    struct msf_clear_entry *e = &clear_queue[i];
    if(!e->in_use) {
      continue;
    }
    if(send_clear(&e->peer) == 0 || ++e->attempts >= MSF_CLEAR_MAX_ATTEMPTS) {
      e->in_use = 0; /* best effort, never retried once sent */
    }
    return 1;
  }
  return 0;
}
/*---------------------------------------------------------------------------*/
/* Pick the negotiated cell of a direction that a DELETE should remove. */
static struct msf_cell *
cell_to_delete(uint8_t dir)
{
  uint8_t i;
  struct msf_cell *victim = NULL;

  for(i = 0; i < MSF_MAX_NEGOTIATED_CELLS; i++) {
    struct msf_cell *c = &cells[i];
    if(c->in_use && c->role == MSF_ROLE_INITIATOR &&
       c->link_options == MSF_DIR_TO_LINK_OPTION(dir) && linkaddr_cmp(&c->peer, &parent)) {
      victim = c; /* the most recently added one */
    }
  }
  return victim;
}
/*---------------------------------------------------------------------------*/
/* Start at most one 6P transaction per call, in order of importance. */
static void
run_initiator(clock_time_t now)
{
  uint8_t tx, rx;
  struct msf_cell *c;

  if(txn.busy || !msf_ready) {
    return;
  }
  if(run_clear_queue()) {
    return;
  }
  if(!have_parent || hold_is_active(now) || is_quarantined(&parent)) {
    return;
  }
  tx = cells_count(&parent, LINK_OPTION_TX, MSF_ROLE_INITIATOR);
  rx = cells_count(&parent, LINK_OPTION_RX, MSF_ROLE_INITIATOR);

  /* Section 4.6: the first negotiated Tx cell; repeated until it is installed. */
  if(tx == 0 || (sw_active && tx < sw_target[MSF_DIR_TX])) {
    if(send_add(MSF_DIR_TX) != 0) {
      hold_short();
    }
    return;
  }
  /* Section 5.2: same number of Rx cells with the new parent. */
  if(sw_active) {
    if(rx < sw_target[MSF_DIR_RX] && send_add(MSF_DIR_RX) != 0) {
      hold_short();
    }
    memset(want, 0, sizeof(want));
    return;
  }
  /* Section 5.3 */
  if(reloc_pending) {
    reloc_pending = 0;
    c = cell_lookup(&parent, LINK_OPTION_TX, reloc_cell.slot, reloc_cell.channel);
    if(c != NULL && send_relocate(c) != 0) {
      hold_short();
    }
    return;
  }
  /* Section 5.1 */
  if(want[MSF_DIR_TX] == MSF_ACTION_ADD) {
    want[MSF_DIR_TX] = MSF_ACTION_NONE;
    if(tx < MSF_MAX_CELLS_PER_PARENT) {
      if(send_add(MSF_DIR_TX) != 0) {
        hold_short();
      }
      return;
    }
  }
  if(want[MSF_DIR_TX] == MSF_ACTION_DELETE) {
    want[MSF_DIR_TX] = MSF_ACTION_NONE;
    /* Section 4.8: at least one negotiated Tx cell stays with the parent. */
    if(tx > 1 && (c = cell_to_delete(MSF_DIR_TX)) != NULL) {
      if(send_delete(c, MSF_DIR_TX) != 0) {
        hold_short();
      }
      return;
    }
  }
  if(want[MSF_DIR_RX] == MSF_ACTION_ADD) {
    want[MSF_DIR_RX] = MSF_ACTION_NONE;
    if(rx < MSF_MAX_CELLS_PER_PARENT) {
      if(send_add(MSF_DIR_RX) != 0) {
        hold_short();
      }
      return;
    }
  }
  if(want[MSF_DIR_RX] == MSF_ACTION_DELETE) {
    want[MSF_DIR_RX] = MSF_ACTION_NONE;
    if(rx > 0 && (c = cell_to_delete(MSF_DIR_RX)) != NULL) {
      if(send_delete(c, MSF_DIR_RX) != 0) {
        hold_short();
      }
      return;
    }
  }
}
/*---------------------------------------------------------------------------*/
static void
housekeeping(void)
{
  clock_time_t now = clock_time();

  if(!tsch_is_associated) {
    if(msf_ready) {
      state_reset();
    }
    return;
  }
  if(!schedule_setup()) {
    return;
  }
  auto_tx_cleanup();
  update_parent();
  check_switch_done(now);
  cleanup_idle_cells(now);
  reap_pending_responses(now);
  collect_decisions();
  check_collisions(now);
  run_initiator(now);
}
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(msf_process, ev, data)
{
  static struct etimer tick;

  PROCESS_BEGIN();

  etimer_set(&tick, MSF_TICK_PERIOD);
  while(1) {
    PROCESS_YIELD();
    if(etimer_expired(&tick)) {
      etimer_reset(&tick);
    }
    housekeeping();
  }

  PROCESS_END();
}
/*---------------------------------------------------------------------------*/
static void
sf_init(void)
{
  state_reset();
  process_start(&msf_process, NULL);
}
/*---------------------------------------------------------------------------*/
const sixtop_sf_t msf_driver = {
  MSF_SFID,
  MSF_6P_TIMEOUT,
  sf_init,
  sf_input,
  sf_timeout,
  sf_error
};
/*---------------------------------------------------------------------------*/
void
msf_init(void)
{
  sixtop_add_sf(&msf_driver);
}
/*---------------------------------------------------------------------------*/
