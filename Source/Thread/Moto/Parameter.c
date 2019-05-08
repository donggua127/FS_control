/*
 * Parameter.c
 *
 *  Created on: 2019-4-8
 *      Author: zhtro
 */

#include "Parameter.h"

static parameter_t m_param = {
		.KI = 0,
		.KP = 0,
		.KU = 0,
		.EnableChangeRail = 0,
		.StateRPM = {0},
		.KSP = 0,
		.KSI = 0
};

parameter_t * ParamInstance()
{
	return &m_param;
}
