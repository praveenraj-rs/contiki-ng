/* 6TiSCH Minimal Scheduling Function (RFC 9033), see msf.c */
#define BENCH_SCHEDULER_INCLUDE "msf.h"
#define BENCH_SCHEDULER_INIT() msf_init()

#define TSCH_CONF_WITH_SIXTOP 1
/* RFC 9033 Section 2 and 14: slotframes 0, 1 and 2 have SLOTFRAME_LENGTH slots */
#define TSCH_SCHEDULE_CONF_DEFAULT_LENGTH 101
#define TSCH_SCHEDULE_CONF_MAX_LINKS 64
/* A 6P transaction per neighbor, several neighbors may talk to us at once */
#define SIXTOP_CONF_MAX_TRANSACTIONS 4
/* TSCH hooks used by MSF: AutoTxCell on demand, and the NumCells*, NumTx* counters */
#define TSCH_CALLBACK_PACKET_READY msf_callback_packet_ready
#define TSCH_CALLBACK_SLOT_START msf_callback_slot_start
#define TSCH_CALLBACK_TX_DONE msf_callback_tx_done
#define TSCH_CALLBACK_RX_FRAME msf_callback_rx_frame
