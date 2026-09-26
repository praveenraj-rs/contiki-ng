/*
 * Mobility- and traffic-adaptive 6P Scheduling Function.
 *
 * This is a lightweight, MSF-inspired scheduling function: it keeps a
 * minimal/autonomous slotframe (handled outside this file, in node.c) for
 * broadcast/EB traffic, and manages a *negotiated* unicast slotframe with
 * the preferred parent (RPL time source) using 6P ADD/DELETE requests.
 *
 * Unlike plain traffic-adaptive schemes (e.g. MSF, Orchestra), cell
 * add/delete decisions here also factor in a per-link "mobility score"
 * derived from RSSI volatility, ETX trend and parent-switch events, so that
 * extra Tx/Rx cells are negotiated proactively for links that are becoming
 * unstable (mobile nodes), instead of reacting only after queue buildup.
 */

#ifndef _SF_MOBILITY_H_
#define _SF_MOBILITY_H_

#include "net/mac/tsch/sixtop/sixtop.h"

/* SFID in the "unmanaged" range (0xf0-0xfe), like SF_SIMPLE_SFID. */
#define SF_MOBILITY_SFID             0xf1

/* Handle of the negotiated (unicast) slotframe managed by this SF.
 * Must differ from the minimal/autonomous slotframe handle used in node.c. */
#define SF_MOBILITY_SLOTFRAME_HANDLE 1

/* Bounds on the number of negotiated Tx cells kept with the parent. */
#define SF_MOBILITY_MIN_CELLS        1
#define SF_MOBILITY_MAX_CELLS        4

/*
 * Ground-truth mobility, written directly by imu-mobility-logger.js (Cooja
 * script) from each mote's simulated Position/velocity - see sf-mobility.c
 * for details. Read-only from the firmware's point of view.
 */
extern uint8_t cooja_imu_is_moving;
extern uint16_t cooja_imu_speed_mm_s;

extern const sixtop_sf_t sf_mobility_driver;

#endif /* !_SF_MOBILITY_H_ */
