/*
 * Mobility-Aware 6TiSCH Minimal Scheduling Function (MA-MSF).
 *
 * RFC 9033 MSF (see examples/msf/msf-benchmarks/msf.c) extended with a
 * mobility input. The node's own speed and moving flag are read from
 * cooja_imu_speed_mm_s / cooja_imu_is_moving, which the Cooja script
 * (coojalogger.js) writes into the mote memory once per second; on real
 * hardware an IMU driver would fill them. Without the IMU, a score derived
 * from parent switches and RSSI/ETX volatility is used.
 *
 * The mobility score (0 static .. 100 fast) changes, at run time:
 *  - the Section 5.1 evaluation window and ADD/DELETE thresholds
 *  - the number of Tx cells held with the parent
 *  - the waitretry delay (Section 12), the 6P timeout (Section 9) and the
 *    parent switch timeout (Section 5.2)
 *  - whether cells are relocated (Section 5.3)
 *  - what the parent learns about the child (6P Metadata, Section 11)
 *
 * With score 0 the behaviour is the one of plain MSF.
 */

#ifndef MA_MSF_H_
#define MA_MSF_H_

#include "contiki.h"
#include "net/linkaddr.h"
#include "net/mac/tsch/tsch-conf.h"
#include "net/mac/tsch/sixtop/sixtop.h"

struct tsch_link;
struct tsch_neighbor;

/* RFC 9033 Section 7: the SFID of MSF is 0. */
#define MA_MSF_SFID 0

/* RFC 9033 Section 2 and 3: minimal, autonomous and negotiated slotframes. */
#define MA_MSF_SLOTFRAME_MINIMAL      0
#define MA_MSF_SLOTFRAME_AUTONOMOUS   1
#define MA_MSF_SLOTFRAME_NEGOTIATED   2

#define MA_MSF_SLOTFRAME_LENGTH TSCH_SCHEDULE_DEFAULT_LENGTH

/*
 * RFC 9033 Section 14, Table 2. Override with MA_MSF_CONF_<NAME>.
 * These are the values used by a static node.
 */
#ifdef MA_MSF_CONF_NUM_CH_OFFSET
#define MA_MSF_NUM_CH_OFFSET MA_MSF_CONF_NUM_CH_OFFSET
#else
#define MA_MSF_NUM_CH_OFFSET 16
#endif

#ifdef MA_MSF_CONF_MAX_NUM_CELLS
#define MA_MSF_MAX_NUM_CELLS MA_MSF_CONF_MAX_NUM_CELLS
#else
#define MA_MSF_MAX_NUM_CELLS 100
#endif

#ifdef MA_MSF_CONF_LIM_NUMCELLSUSED_HIGH
#define MA_MSF_LIM_NUMCELLSUSED_HIGH MA_MSF_CONF_LIM_NUMCELLSUSED_HIGH
#else
#define MA_MSF_LIM_NUMCELLSUSED_HIGH 75 /* percent of the window */
#endif

#ifdef MA_MSF_CONF_LIM_NUMCELLSUSED_LOW
#define MA_MSF_LIM_NUMCELLSUSED_LOW MA_MSF_CONF_LIM_NUMCELLSUSED_LOW
#else
#define MA_MSF_LIM_NUMCELLSUSED_LOW 25 /* percent of the window */
#endif

#ifdef MA_MSF_CONF_MAX_NUMTX
#define MA_MSF_MAX_NUMTX MA_MSF_CONF_MAX_NUMTX
#else
#define MA_MSF_MAX_NUMTX 256
#endif

#ifdef MA_MSF_CONF_HOUSEKEEPINGCOLLISION_PERIOD
#define MA_MSF_HOUSEKEEPINGCOLLISION_PERIOD MA_MSF_CONF_HOUSEKEEPINGCOLLISION_PERIOD
#else
#define MA_MSF_HOUSEKEEPINGCOLLISION_PERIOD (60 * CLOCK_SECOND)
#endif

#ifdef MA_MSF_CONF_RELOCATE_PDRTHRES
#define MA_MSF_RELOCATE_PDRTHRES MA_MSF_CONF_RELOCATE_PDRTHRES
#else
#define MA_MSF_RELOCATE_PDRTHRES 50 /* percent */
#endif

#ifdef MA_MSF_CONF_QUARANTINE_DURATION
#define MA_MSF_QUARANTINE_DURATION MA_MSF_CONF_QUARANTINE_DURATION
#else
#define MA_MSF_QUARANTINE_DURATION (5 * 60 * CLOCK_SECOND)
#endif

#ifdef MA_MSF_CONF_WAIT_DURATION_MIN
#define MA_MSF_WAIT_DURATION_MIN MA_MSF_CONF_WAIT_DURATION_MIN
#else
#define MA_MSF_WAIT_DURATION_MIN (30 * CLOCK_SECOND)
#endif

#ifdef MA_MSF_CONF_WAIT_DURATION_MAX
#define MA_MSF_WAIT_DURATION_MAX MA_MSF_CONF_WAIT_DURATION_MAX
#else
#define MA_MSF_WAIT_DURATION_MAX (60 * CLOCK_SECOND)
#endif

/* RFC 9033 Section 9: ((2^MAXBE) - 1) * MAXRETRIES * SLOTFRAME_LENGTH timeslots of 10 ms. */
#ifdef MA_MSF_CONF_6P_TIMEOUT
#define MA_MSF_6P_TIMEOUT MA_MSF_CONF_6P_TIMEOUT
#else
#define MA_MSF_6P_TIMEOUT \
  ((clock_time_t)((((1UL << TSCH_MAC_MAX_BE) - 1) * TSCH_MAC_MAX_FRAME_RETRIES * \
                   MA_MSF_SLOTFRAME_LENGTH * CLOCK_SECOND) / 100))
#endif

/* RFC 9033 Section 4.6 and 8: at least five candidate cells. */
#ifdef MA_MSF_CONF_NUM_CANDIDATE_CELLS
#define MA_MSF_NUM_CANDIDATE_CELLS MA_MSF_CONF_NUM_CANDIDATE_CELLS
#else
#define MA_MSF_NUM_CANDIDATE_CELLS 5
#endif

/* Implementation limits. */
#ifdef MA_MSF_CONF_MAX_NEGOTIATED_CELLS
#define MA_MSF_MAX_NEGOTIATED_CELLS MA_MSF_CONF_MAX_NEGOTIATED_CELLS
#else
#define MA_MSF_MAX_NEGOTIATED_CELLS 24
#endif

#ifdef MA_MSF_CONF_MAX_CELLS_PER_PARENT
#define MA_MSF_MAX_CELLS_PER_PARENT MA_MSF_CONF_MAX_CELLS_PER_PARENT
#else
#define MA_MSF_MAX_CELLS_PER_PARENT 8
#endif

#ifdef MA_MSF_CONF_MAX_AUTO_TX_CELLS
#define MA_MSF_MAX_AUTO_TX_CELLS MA_MSF_CONF_MAX_AUTO_TX_CELLS
#else
#define MA_MSF_MAX_AUTO_TX_CELLS 4
#endif

/* Idle time after which the cells of a silent child are reclaimed (Section 5.1 cleanup). */
#ifdef MA_MSF_CONF_CELL_IDLE_TIMEOUT
#define MA_MSF_CELL_IDLE_TIMEOUT MA_MSF_CONF_CELL_IDLE_TIMEOUT
#else
#define MA_MSF_CELL_IDLE_TIMEOUT (5 * 60 * CLOCK_SECOND)
#endif

/*
 * Mobility-aware parameters. The first value of each pair is the static
 * value (Table 2 above), the "MOBILE" value is reached at score 100 and
 * values in between are interpolated linearly.
 */

/* Mobility source: 1 = cooja_imu_* globals written by coojalogger.js, 0 = RSSI/ETX/parent-switch proxy. */
#ifndef MA_MSF_CONF_WITH_COOJA_IMU
#if CONTIKI_TARGET_COOJA
#define MA_MSF_CONF_WITH_COOJA_IMU 1
#else
#define MA_MSF_CONF_WITH_COOJA_IMU 0
#endif
#endif

/* Speed that maps to score 100. */
#ifndef MA_MSF_CONF_SPEED_FULL_MM_S
#define MA_MSF_CONF_SPEED_FULL_MM_S 2000
#endif

/* "Mobile" state with hysteresis: entered at HIGH, left at LOW. */
#ifndef MA_MSF_CONF_SCORE_HIGH
#define MA_MSF_CONF_SCORE_HIGH 55
#endif
#ifndef MA_MSF_CONF_SCORE_LOW
#define MA_MSF_CONF_SCORE_LOW 20
#endif

/* Minimum score right after a parent switch. */
#ifndef MA_MSF_CONF_SWITCH_BOOST
#define MA_MSF_CONF_SWITCH_BOOST 60
#endif

/* Section 5.1 window (MAX_NUM_CELLS) at score 100. */
#ifndef MA_MSF_CONF_MOBILE_MAX_NUM_CELLS
#define MA_MSF_CONF_MOBILE_MAX_NUM_CELLS 30
#endif

/* Section 5.1 thresholds at score 100, percent of the window. */
#ifndef MA_MSF_CONF_MOBILE_HIGH_PCT
#define MA_MSF_CONF_MOBILE_HIGH_PCT 40
#endif
#ifndef MA_MSF_CONF_MOBILE_LOW_PCT
#define MA_MSF_CONF_MOBILE_LOW_PCT 0 /* never release cells while moving fast */
#endif

/* Tx cells held with the parent while mobile: 1 + EXTRA_CELLS. */
#ifndef MA_MSF_CONF_EXTRA_CELLS
#define MA_MSF_CONF_EXTRA_CELLS 2
#endif

/* While mobile, this many packets queued for the parent trigger an ADD. */
#ifndef MA_MSF_CONF_QUEUE_HIGH
#define MA_MSF_CONF_QUEUE_HIGH 2
#endif

/* Section 12 waitretry at score 100. */
#ifndef MA_MSF_CONF_MOBILE_WAIT_MIN
#define MA_MSF_CONF_MOBILE_WAIT_MIN (3 * CLOCK_SECOND)
#endif
#ifndef MA_MSF_CONF_MOBILE_WAIT_MAX
#define MA_MSF_CONF_MOBILE_WAIT_MAX (8 * CLOCK_SECOND)
#endif

/* Section 9 6P timeout at score 100. */
#ifndef MA_MSF_CONF_MOBILE_6P_TIMEOUT
#define MA_MSF_CONF_MOBILE_6P_TIMEOUT (20 * CLOCK_SECOND)
#endif

/* Section 5.2 switch timeout: static / at score 100. */
#ifndef MA_MSF_CONF_SWITCH_TIMEOUT
#define MA_MSF_CONF_SWITCH_TIMEOUT (5 * 60 * CLOCK_SECOND)
#endif
#ifndef MA_MSF_CONF_MOBILE_SWITCH_TIMEOUT
#define MA_MSF_CONF_MOBILE_SWITCH_TIMEOUT (30 * CLOCK_SECOND)
#endif

/* Idle timeout for the cells of a child that announced it is moving. */
#ifndef MA_MSF_CONF_MOBILE_CELL_IDLE_TIMEOUT
#define MA_MSF_CONF_MOBILE_CELL_IDLE_TIMEOUT (60 * CLOCK_SECOND)
#endif

/* Announce the mobility state to the parent in the 6P Metadata field of requests. */
#ifndef MA_MSF_CONF_SIGNAL_MOBILITY
#define MA_MSF_CONF_SIGNAL_MOBILITY 1
#endif

/*
 * A moving node becomes an RPL leaf (RPL Classic only): it stops sending DIOs
 * and tells its children to find another parent, so that neighbors do not
 * route through a node that is about to leave. It still sends EBs, so others
 * can join through it. It follows the node's own motion (entered at
 * SCORE_HIGH, left at SCORE_LOW), not the parent switch boost.
 */
#ifndef MA_MSF_CONF_RPL_LEAF_WHEN_MOBILE
#define MA_MSF_CONF_RPL_LEAF_WHEN_MOBILE ROUTING_CONF_RPL_CLASSIC
#endif

#if MA_MSF_MAX_NUM_CELLS > 255
#error "MA_MSF_MAX_NUM_CELLS must fit the 1-byte NumCellsElapsed counter"
#endif

/*
 * Own mobility, written from outside (coojalogger.js through Cooja's
 * VarMemory, or an IMU driver). Non-static so the symbols are visible to Cooja.
 */
extern uint8_t cooja_imu_is_moving;
extern uint16_t cooja_imu_speed_mm_s;

/** Initializes MA-MSF and registers it with 6top. */
void ma_msf_init(void);

/** Current mobility score, 0 (static) .. 100 (fast). */
uint8_t ma_msf_get_mobility_score(void);

/** The 6top scheduling function driver; the 6P timeout is updated at run time. */
extern sixtop_sf_t ma_msf_driver;

/*
 * TSCH callbacks; enable them in scheduler-conf.h:
 *   #define TSCH_CALLBACK_PACKET_READY ma_msf_callback_packet_ready
 *   #define TSCH_CALLBACK_SLOT_START   ma_msf_callback_slot_start
 *   #define TSCH_CALLBACK_TX_DONE      ma_msf_callback_tx_done
 *   #define TSCH_CALLBACK_RX_FRAME     ma_msf_callback_rx_frame
 */
int ma_msf_callback_packet_ready(void);
void ma_msf_callback_slot_start(struct tsch_link *link);
void ma_msf_callback_tx_done(struct tsch_link *link, struct tsch_neighbor *n,
                             uint8_t mac_tx_status);
void ma_msf_callback_rx_frame(struct tsch_link *link, const linkaddr_t *src);

#endif /* MA_MSF_H_ */
