/*
 * 6TiSCH Minimal Scheduling Function (MSF), RFC 9033.
 *
 * Section references ("RFC 9033 Section x.y") in msf.c/msf.h point to
 * https://www.rfc-editor.org/rfc/rfc9033. 6P command/format details are from
 * RFC 8480; the 6P API usage follows examples/6tisch/6p-packet/test-sf.c.
 */

#ifndef MSF_H_
#define MSF_H_

#include "contiki.h"
#include "net/linkaddr.h"
#include "net/mac/tsch/tsch-conf.h"
#include "net/mac/tsch/sixtop/sixtop.h"

struct tsch_link;
struct tsch_neighbor;

/* RFC 9033 Section 7: the SFID of MSF is 0. */
#define MSF_SFID 0

/*
 * Slotframes (RFC 9033 Section 2 and 3): 0 = minimal 6TiSCH cell (created by
 * TSCH itself), 1 = autonomous cells, 2 = 6P negotiated cells.
 */
#define MSF_SLOTFRAME_MINIMAL      0
#define MSF_SLOTFRAME_AUTONOMOUS   1
#define MSF_SLOTFRAME_NEGOTIATED   2

/* RFC 9033 Section 2: all three slotframes have the same length (RECOMMENDED). */
#define MSF_SLOTFRAME_LENGTH TSCH_SCHEDULE_DEFAULT_LENGTH

/*
 * RFC 9033 Section 14, Table 2: MSF constants and their RECOMMENDED values.
 * Every value can be overridden from project-conf.h with MSF_CONF_<NAME>.
 */
#ifdef MSF_CONF_NUM_CH_OFFSET
#define MSF_NUM_CH_OFFSET MSF_CONF_NUM_CH_OFFSET
#else
#define MSF_NUM_CH_OFFSET 16
#endif

#ifdef MSF_CONF_MAX_NUM_CELLS
#define MSF_MAX_NUM_CELLS MSF_CONF_MAX_NUM_CELLS
#else
#define MSF_MAX_NUM_CELLS 100
#endif

#ifdef MSF_CONF_LIM_NUMCELLSUSED_HIGH
#define MSF_LIM_NUMCELLSUSED_HIGH MSF_CONF_LIM_NUMCELLSUSED_HIGH
#else
#define MSF_LIM_NUMCELLSUSED_HIGH 75
#endif

#ifdef MSF_CONF_LIM_NUMCELLSUSED_LOW
#define MSF_LIM_NUMCELLSUSED_LOW MSF_CONF_LIM_NUMCELLSUSED_LOW
#else
#define MSF_LIM_NUMCELLSUSED_LOW 25
#endif

#ifdef MSF_CONF_MAX_NUMTX
#define MSF_MAX_NUMTX MSF_CONF_MAX_NUMTX
#else
#define MSF_MAX_NUMTX 256
#endif

#ifdef MSF_CONF_HOUSEKEEPINGCOLLISION_PERIOD
#define MSF_HOUSEKEEPINGCOLLISION_PERIOD MSF_CONF_HOUSEKEEPINGCOLLISION_PERIOD
#else
#define MSF_HOUSEKEEPINGCOLLISION_PERIOD (60 * CLOCK_SECOND)
#endif

#ifdef MSF_CONF_RELOCATE_PDRTHRES
#define MSF_RELOCATE_PDRTHRES MSF_CONF_RELOCATE_PDRTHRES
#else
#define MSF_RELOCATE_PDRTHRES 50 /* percent */
#endif

#ifdef MSF_CONF_QUARANTINE_DURATION
#define MSF_QUARANTINE_DURATION MSF_CONF_QUARANTINE_DURATION
#else
#define MSF_QUARANTINE_DURATION (5 * 60 * CLOCK_SECOND)
#endif

#ifdef MSF_CONF_WAIT_DURATION_MIN
#define MSF_WAIT_DURATION_MIN MSF_CONF_WAIT_DURATION_MIN
#else
#define MSF_WAIT_DURATION_MIN (30 * CLOCK_SECOND)
#endif

#ifdef MSF_CONF_WAIT_DURATION_MAX
#define MSF_WAIT_DURATION_MAX MSF_CONF_WAIT_DURATION_MAX
#else
#define MSF_WAIT_DURATION_MAX (60 * CLOCK_SECOND)
#endif

/*
 * RFC 9033 Section 9: 6P timeout = ((2^MAXBE) - 1) * MAXRETRIES * SLOTFRAME_LENGTH
 * (in timeslots; 10 ms each with the default TSCH timing).
 */
#ifdef MSF_CONF_6P_TIMEOUT
#define MSF_6P_TIMEOUT MSF_CONF_6P_TIMEOUT
#else
#define MSF_6P_TIMEOUT \
  ((clock_time_t)((((1UL << TSCH_MAC_MAX_BE) - 1) * TSCH_MAC_MAX_FRAME_RETRIES * \
                   MSF_SLOTFRAME_LENGTH * CLOCK_SECOND) / 100))
#endif

/* RFC 9033 Section 8 / 4.6: a CellList SHOULD have five or more cells. */
#ifdef MSF_CONF_NUM_CANDIDATE_CELLS
#define MSF_NUM_CANDIDATE_CELLS MSF_CONF_NUM_CANDIDATE_CELLS
#else
#define MSF_NUM_CANDIDATE_CELLS 5
#endif

/* Implementation-specific limits. */
#ifdef MSF_CONF_MAX_NEGOTIATED_CELLS
#define MSF_MAX_NEGOTIATED_CELLS MSF_CONF_MAX_NEGOTIATED_CELLS
#else
#define MSF_MAX_NEGOTIATED_CELLS 24 /* cells in the negotiated slotframe (all peers) */
#endif

#ifdef MSF_CONF_MAX_CELLS_PER_PARENT
#define MSF_MAX_CELLS_PER_PARENT MSF_CONF_MAX_CELLS_PER_PARENT
#else
#define MSF_MAX_CELLS_PER_PARENT 8 /* per direction (Tx / Rx) towards the parent */
#endif

#ifdef MSF_CONF_MAX_AUTO_TX_CELLS
#define MSF_MAX_AUTO_TX_CELLS MSF_CONF_MAX_AUTO_TX_CELLS
#else
#define MSF_MAX_AUTO_TX_CELLS 4 /* simultaneous AutoTxCells (RFC 9033 Section 3) */
#endif

/* RFC 9033 Section 5.1: implementation-specific cleanup of cells of vanished neighbors. */
#ifdef MSF_CONF_CELL_IDLE_TIMEOUT
#define MSF_CELL_IDLE_TIMEOUT MSF_CONF_CELL_IDLE_TIMEOUT
#else
#define MSF_CELL_IDLE_TIMEOUT (30 * 60 * CLOCK_SECOND)
#endif

#if MSF_MAX_NUM_CELLS > 255
#error "MSF_MAX_NUM_CELLS must fit the 1-byte NumCellsElapsed counter (RFC 9033 Table 3)"
#endif

/**
 * Initializes MSF and registers it with 6top (RFC 9033 Section 7).
 * To be called once from the application before it starts using the network.
 */
void msf_init(void);

/** The MSF scheduling function driver, as expected by sixtop_add_sf(). */
extern const sixtop_sf_t msf_driver;

/*
 * TSCH callbacks; enable them in project-conf.h:
 *   #define TSCH_CALLBACK_PACKET_READY msf_callback_packet_ready
 *   #define TSCH_CALLBACK_SLOT_START   msf_callback_slot_start
 *   #define TSCH_CALLBACK_TX_DONE      msf_callback_tx_done
 *   #define TSCH_CALLBACK_RX_FRAME     msf_callback_rx_frame
 */
int msf_callback_packet_ready(void);
void msf_callback_slot_start(struct tsch_link *link);
void msf_callback_tx_done(struct tsch_link *link, struct tsch_neighbor *n,
                          uint8_t mac_tx_status);
void msf_callback_rx_frame(struct tsch_link *link, const linkaddr_t *src);

#endif /* MSF_H_ */
