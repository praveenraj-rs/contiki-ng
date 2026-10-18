/* Mobility-Aware 6TiSCH Minimal Scheduling Function, see ma-msf.c */
#define BENCH_SCHEDULER_INCLUDE "ma-msf.h"
#define BENCH_SCHEDULER_INIT() ma_msf_init()

#define TSCH_CONF_WITH_SIXTOP 1
/* RFC 9033 Section 2 and 14: slotframes 0, 1 and 2 have SLOTFRAME_LENGTH slots */
#define TSCH_SCHEDULE_CONF_DEFAULT_LENGTH 101
#define TSCH_SCHEDULE_CONF_MAX_LINKS 64
/* A 6P transaction per neighbor, several neighbors may talk to us at once */
#define SIXTOP_CONF_MAX_TRANSACTIONS 4
/* TSCH hooks used by MA-MSF: AutoTxCell on demand, and the NumCells*, NumTx* counters */
#define TSCH_CALLBACK_PACKET_READY ma_msf_callback_packet_ready
#define TSCH_CALLBACK_SLOT_START ma_msf_callback_slot_start
#define TSCH_CALLBACK_TX_DONE ma_msf_callback_tx_done
#define TSCH_CALLBACK_RX_FRAME ma_msf_callback_rx_frame

/* Mobility source: 1 = cooja_imu_* written by coojalogger.js, 0 = RSSI/ETX/parent-switch proxy.
 * Select with: make SCHEDULER=ma-msf DEFINES=MA_MSF_CONF_WITH_COOJA_IMU=0 */
#ifndef MA_MSF_CONF_WITH_COOJA_IMU
#define MA_MSF_CONF_WITH_COOJA_IMU 1
#endif
