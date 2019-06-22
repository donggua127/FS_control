/*
 * V2V.h
 *
 *  Created on: 2019-6-4
 *      Author: zhtro
 */

#ifndef V2V_H_
#define V2V_H_

#include "Sensor/RFID/EPCdef.h"

#define V2V_ID_NONE (0x0000)
#define V2V_DISTANCE_INFINITY  (0xFFFFFFFF)

typedef struct
{
	uint16_t frontId;
	uint16_t backId;
}v2v_param_t;

typedef struct
{
	uint8_t epc[12];
	uint32_t distance;
	int32_t deltadistance;
	uint16_t rpm;
	uint8_t status;
}v2v_req_carstatus_t;
/*
 * API
 */
extern void V2VInit();
extern void V2VHandShakeFrontCar();
extern void V2VSetFrontCarId(uint16_t frontid);
extern uint16_t V2VGetFrontCarId();
extern uint32_t V2VGetDistanceToFrontCar();
extern uint16_t V2VGetFrontCarSpeed();
extern epc_t V2VGetFrontCarEpc();
extern void V2VSetDeltaDistance(int32_t delta);

#endif /* V2V_H_ */
