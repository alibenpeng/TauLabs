/**
 ******************************************************************************
 * @addtogroup TauLabsModules Tau Labs Modules
 * @{
 * @addtogroup AutotuningModule Autotuning Module
 * @{
 *
 * @file       autotune.c
 * @author     The OpenPilot Team, http://www.openpilot.org Copyright (C) 2012.
 * @author     Tau Labs, http://taulabs.org, Copyright (C) 2013-2014
 * @brief      State machine to run autotuning. Low level work done by @ref
 *             StabilizationModule 
 *
 * @see        The GNU Public License (GPL) Version 3
 *
 ******************************************************************************/
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#include "openpilot.h"
#include "pios.h"
#include "physical_constants.h"
#include "flightstatus.h"
#include "modulesettings.h"
#include "manualcontrolcommand.h"
#include "manualcontrolsettings.h"
#include "gyros.h"
#include "actuatordesired.h"
#include "stabilizationdesired.h"
#include "stabilizationsettings.h"
#include "systemident.h"
#include <pios_board_info.h>
#include "pios_thread.h"

// Private constants
#define STACK_SIZE_BYTES 1504
#define TASK_PRIORITY PIOS_THREAD_PRIO_NORMAL

#define AF_NUMX 9
#define AF_NUMP 35

// Private types
enum AUTOTUNE_STATE {AT_INIT, AT_START, AT_RUN, AT_FINISHED, AT_SET};

// Private variables
static struct pios_thread *taskHandle;
static bool module_enabled;

static float DT_s_sum = 0.0f;
static uint32_t DT_s_count = 0;

// Private functions
static void AutotuneTask(void *parameters);
static void af_predict(float X[AF_NUMX], float P[AF_NUMP], const float u_in[3], const float gyro[3], const float dT_s);
static void af_init(float X[AF_NUMX], float P[AF_NUMP]);

/**
 * Initialise the module, called on startup
 * \returns 0 on success or -1 if initialisation failed
 */
int32_t AutotuneInitialize(void)
{
	// Create a queue, connect to manual control command and flightstatus
#ifdef MODULE_Autotune_BUILTIN
	module_enabled = true;
#else
	uint8_t module_state[MODULESETTINGS_ADMINSTATE_NUMELEM];
	ModuleSettingsAdminStateGet(module_state);
	if (module_state[MODULESETTINGS_ADMINSTATE_AUTOTUNE] == MODULESETTINGS_ADMINSTATE_ENABLED)
		module_enabled = true;
	else
		module_enabled = false;
#endif

	if (module_enabled) {
		SystemIdentInitialize();
	}

	return 0;
}

/**
 * Initialise the module, called on startup
 * \returns 0 on success or -1 if initialisation failed
 */
int32_t AutotuneStart(void)
{
	// Start main task if it is enabled
	if(module_enabled) {
		taskHandle = PIOS_Thread_Create(AutotuneTask, "Autotune", STACK_SIZE_BYTES, NULL, TASK_PRIORITY);

		TaskMonitorAdd(TASKINFO_RUNNING_AUTOTUNE, taskHandle);
		PIOS_WDG_RegisterFlag(PIOS_WDG_AUTOTUNE);
	}
	return 0;
}

MODULE_INITCALL(AutotuneInitialize, AutotuneStart)

static void UpdateSystemIdent(const float *X, const float *noise,
		float dT_s) {
	SystemIdentData relay;
	relay.Beta[SYSTEMIDENT_BETA_ROLL]    = X[4];
	relay.Beta[SYSTEMIDENT_BETA_PITCH]   = X[5];
	relay.Bias[SYSTEMIDENT_BIAS_ROLL]    = X[7];
	relay.Bias[SYSTEMIDENT_BIAS_PITCH]   = X[8];
	relay.Tau                            = X[6];
	if (noise) {
		relay.Noise[SYSTEMIDENT_NOISE_ROLL]  = noise[0];
		relay.Noise[SYSTEMIDENT_NOISE_PITCH] = noise[1];
		relay.Noise[SYSTEMIDENT_NOISE_YAW]   = noise[2];
	}
	relay.Period = dT_s * 1000.0f;
	relay.AverageDt = 1000.0f * DT_s_sum / DT_s_count;

	SystemIdentSet(&relay);
}

static void UpdateStabilizationDesired(bool doingIdent) {
	StabilizationDesiredData stabDesired;
	StabilizationDesiredGet(&stabDesired);

	uint8_t rollMax, pitchMax;

	float manualRate[STABILIZATIONSETTINGS_MANUALRATE_NUMELEM];

	StabilizationSettingsRollMaxGet(&rollMax);
	StabilizationSettingsPitchMaxGet(&pitchMax);
	StabilizationSettingsManualRateGet(manualRate);

	ManualControlCommandRollGet(&stabDesired.Roll);
	stabDesired.Roll *= rollMax;
	ManualControlCommandPitchGet(&stabDesired.Pitch);
	stabDesired.Pitch *= pitchMax;

	ManualControlCommandYawGet(&stabDesired.Yaw);
	stabDesired.Yaw *= manualRate[STABILIZATIONSETTINGS_MANUALRATE_YAW];

	if (doingIdent) {
		stabDesired.StabilizationMode[STABILIZATIONDESIRED_STABILIZATIONMODE_ROLL]  = STABILIZATIONDESIRED_STABILIZATIONMODE_SYSTEMIDENT;
		stabDesired.StabilizationMode[STABILIZATIONDESIRED_STABILIZATIONMODE_PITCH] = STABILIZATIONDESIRED_STABILIZATIONMODE_SYSTEMIDENT;
		stabDesired.StabilizationMode[STABILIZATIONDESIRED_STABILIZATIONMODE_YAW] = STABILIZATIONDESIRED_STABILIZATIONMODE_SYSTEMIDENT;
	} else {
		stabDesired.StabilizationMode[STABILIZATIONDESIRED_STABILIZATIONMODE_ROLL]  = STABILIZATIONDESIRED_STABILIZATIONMODE_ATTITUDE;
		stabDesired.StabilizationMode[STABILIZATIONDESIRED_STABILIZATIONMODE_PITCH] = STABILIZATIONDESIRED_STABILIZATIONMODE_ATTITUDE;
		stabDesired.StabilizationMode[STABILIZATIONDESIRED_STABILIZATIONMODE_YAW] = STABILIZATIONDESIRED_STABILIZATIONMODE_RATE;
	}

	ManualControlCommandThrottleGet(&stabDesired.Throttle);

	StabilizationDesiredSet(&stabDesired);
}

/**
 * Module thread, should not return.
 */
static void AutotuneTask(void *parameters)
{
	enum AUTOTUNE_STATE state = AT_INIT;

	uint32_t lastUpdateTime = PIOS_Thread_Systime();

	float X[AF_NUMX] = {0};
	float P[AF_NUMP] = {0};
	float noise[3] = {0};

	af_init(X,P);

	uint32_t last_time = 0.0f;
	const uint32_t DT_MS = 1;

	while(1) {

		PIOS_WDG_UpdateFlag(PIOS_WDG_AUTOTUNE);
		// TODO:
		// 1. get from queue
		// 2. based on whether it is flightstatus or manualcontrol

		uint32_t diffTime;

		const uint32_t PREPARE_TIME = 2000;
		const uint32_t MEASURE_TIME = 60000;

		bool doingIdent = false;

		FlightStatusData flightStatus;
		FlightStatusGet(&flightStatus);

		// Only allow this module to run when autotuning
		if (flightStatus.FlightMode != FLIGHTSTATUS_FLIGHTMODE_AUTOTUNE) {
			state = AT_INIT;
			PIOS_Thread_Sleep(50);
			continue;
		}

		float throttle;

		ManualControlCommandThrottleGet(&throttle);
				
		switch(state) {
			case AT_INIT:

				lastUpdateTime = PIOS_Thread_Systime();

				// Only start when armed and flying
				if (flightStatus.Armed == FLIGHTSTATUS_ARMED_ARMED && throttle > 0) {

					af_init(X,P);

					UpdateSystemIdent(X, NULL, 0.0f);

					state = AT_START;
				}
				break;

			case AT_START:

				diffTime = PIOS_Thread_Systime() - lastUpdateTime;

				// Spend the first block of time in normal rate mode to get airborne
				if (diffTime > PREPARE_TIME) {
					state = AT_RUN;
					lastUpdateTime = PIOS_Thread_Systime();
				}

				DT_s_sum = 0;
				DT_s_count = 0;


				last_time = PIOS_DELAY_GetRaw();

				break;

			case AT_RUN:

				diffTime = PIOS_Thread_Systime() - lastUpdateTime;

				doingIdent = true;

				// Update the system identification, but only when throttle is applied
				// so bad values don't result when landing
				if (throttle > 0) {
					float y[3];
					GyrosxGet(y+0);
					GyrosyGet(y+1);
					GyroszGet(y+2);

					float u[3];
					ActuatorDesiredRollGet(u+0);
					ActuatorDesiredPitchGet(u+1);
					ActuatorDesiredYawGet(u+2);

					float dT_s = PIOS_DELAY_DiffuS(last_time) * 1.0e-6f;
					DT_s_sum += dT_s;
					DT_s_count++;

					af_predict(X,P,u,y, DT_MS * 0.001f);
					for (uint32_t i = 0; i < 3; i++) {
						const float NOISE_ALPHA = 0.9997f;  // 10 second time constant at 300 Hz
						noise[i] = NOISE_ALPHA * noise[i] + (1-NOISE_ALPHA) * (y[i] - X[i]) * (y[i] - X[i]);
					}

					UpdateSystemIdent(X, noise, dT_s);
				}

				if (diffTime > MEASURE_TIME) { // Move on to next state
					state = AT_FINISHED;
					lastUpdateTime = PIOS_Thread_Systime();
				}

				last_time = PIOS_DELAY_GetRaw();

				break;

			case AT_FINISHED:

				// Wait until disarmed and landed before updating the settings
				if (flightStatus.Armed == FLIGHTSTATUS_ARMED_DISARMED && throttle <= 0)
					state = AT_SET;

				break;

			case AT_SET:
				// If at some point we want to store the settings at the end of
				// autotune, that can be done here. However, that will await further
				// testing.

				// Save the settings locally. Note this is done after disarming.
				UAVObjSave(SystemIdentHandle(), 0);
				state = AT_INIT;
				break;

			default:
				// Set an alarm or some shit like that
				break;
		}

		// Update based on manual controls
		UpdateStabilizationDesired(doingIdent);

		PIOS_Thread_Sleep(DT_MS);
	}
}

/**
 * Prediction step for EKF on control inputs to quad that
 * learns the system properties
 * @param X the current state estimate which is updated in place
 * @param P the current covariance matrix, updated in place
 * @param[in] the current control inputs (roll, pitch, yaw)
 * @param[in] the gyro measurements
 */
/**
 * Prediction step for EKF on control inputs to quad that
 * learns the system properties
 * @param X the current state estimate which is updated in place
 * @param P the current covariance matrix, updated in place
 * @param[in] the current control inputs (roll, pitch, yaw)
 * @param[in] the gyro measurements
 */
__attribute__((always_inline)) static inline void af_predict(float X[AF_NUMX], float P[AF_NUMP], const float u_in[3], const float gyro[3], const float dT_s)
{

	const float Ts = dT_s;
	const float Tsq = Ts * Ts;
	const float Tsq3 = Tsq * Ts;
	const float Tsq4 = Tsq * Tsq;

	// for convenience and clarity code below uses the named versions of
	// the state variables
	float w1 = X[0];           // roll rate estimate
	float w2 = X[1];           // pitch rate estimate
	float u1 = X[2];           // scaled roll torque 
	float u2 = X[3];           // scaled pitch torque
	const float e_b1 = expf(X[4]);   // roll torque scale
	const float b1 = X[4];
	const float e_b2 = expf(X[5]);   // pitch torque scale
	const float b2 = X[5];
	const float e_tau = expf(X[6]); // time response of the motors
	const float tau = X[6];
	const float bias1 = X[7];        // bias in the roll torque
	const float bias2 = X[8];       // bias in the pitch torque

	// inputs to the system (roll, pitch, yaw)
	const float u1_in = u_in[0];
	const float u2_in = u_in[1];

	// measurements from gyro
	const float gyro_x = gyro[0];
	const float gyro_y = gyro[1];

	// update named variables because we want to use predicted
	// values below
	w1 = X[0] = w1 - Ts*bias1*e_b1 + Ts*u1*e_b1;
	w2 = X[1] = w2 - Ts*bias2*e_b2 + Ts*u2*e_b2;
	u1 = X[2] = (Ts*u1_in)/(Ts + e_tau) + (u1*e_tau)/(Ts + e_tau);
	u2 = X[3] = (Ts*u2_in)/(Ts + e_tau) + (u2*e_tau)/(Ts + e_tau);

	/**** filter parameters ****/
	const float q_w = 1e-4f;
	const float q_ud = 1e-4f;
	const float q_B = 1e-5f;
	const float q_tau = 1e-5f;
	const float q_bias = 1e-19f;
	const float s_a = 3000.0f;  // expected gyro noise

	const float Q[AF_NUMX] = {q_w, q_w, q_ud, q_ud, q_B, q_B, q_tau, q_bias, q_bias};

	float D[AF_NUMP];
	for (uint32_t i = 0; i < AF_NUMP; i++)
        D[i] = P[i];

    const float e_tau2    = e_tau * e_tau;
    const float e_tau3    = e_tau * e_tau2;
    const float e_tau4    = e_tau2 * e_tau2;
    const float Ts_e_tau2 = (Ts + e_tau) * (Ts + e_tau);
    const float Ts_e_tau4 = Ts_e_tau2 * Ts_e_tau2;

	// covariance propagation - D is stored copy of covariance	
	P[0] = D[0] + Q[0] + 2*Ts*e_b1*(D[2] - D[22] - D[7]*bias1 + D[7]*u1) + Tsq*(e_b1*e_b1)*(D[4] - 2*D[24] + D[29] - 2*D[9]*bias1 + 2*D[26]*bias1 + 2*D[9]*u1 - 2*D[26]*u1 + D[11]*(bias1*bias1) + D[11]*(u1*u1) - 2*D[11]*bias1*u1);
	P[1] = D[1] + Q[1] + 2*Ts*e_b2*(D[5] - D[30] - D[12]*bias2 + D[12]*u2) + Tsq*(e_b2*e_b2)*(D[6] - 2*D[31] + D[34] - 2*D[13]*bias2 + 2*D[32]*bias2 + 2*D[13]*u2 - 2*D[32]*u2 + D[14]*(bias2*bias2) + D[14]*(u2*u2) - 2*D[14]*bias2*u2);
	P[2] = (D[2]*e_tau2 + D[2]*Ts*e_tau + D[4]*Ts*(e_b1*e_tau2) - D[24]*Ts*(e_b1*e_tau2) + D[4]*Tsq*(e_b1*e_tau) - D[24]*Tsq*(e_b1*e_tau) + D[15]*Ts*u1*e_tau - D[15]*Ts*u1_in*e_tau - D[9]*Ts*bias1*(e_b1*e_tau2) - D[9]*Tsq*bias1*(e_b1*e_tau) + D[9]*Ts*u1*(e_b1*e_tau2) + D[9]*Tsq*u1*(e_b1*e_tau) + D[17]*Tsq*u1*(e_b1*e_tau) - D[28]*Tsq*u1*(e_b1*e_tau) - D[17]*Tsq*u1_in*(e_b1*e_tau) + D[28]*Tsq*u1_in*(e_b1*e_tau) + D[19]*Tsq*(u1*u1)*(e_b1*e_tau) - D[19]*Tsq*bias1*u1*(e_b1*e_tau) + D[19]*Tsq*bias1*u1_in*(e_b1*e_tau) - D[19]*Tsq*u1*u1_in*(e_b1*e_tau))/Ts_e_tau2;
	P[3] = (D[3]*e_tau2 + D[3]*Ts*e_tau + D[16]*Ts*u1*e_tau - D[16]*Ts*u1_in*e_tau + D[18]*Tsq*u1*(e_b2*e_tau) - D[33]*Tsq*u1*(e_b2*e_tau) - D[18]*Tsq*u1_in*(e_b2*e_tau) + D[33]*Tsq*u1_in*(e_b2*e_tau) - D[20]*Tsq*bias2*u1*(e_b2*e_tau) + D[20]*Tsq*bias2*u1_in*(e_b2*e_tau) + D[20]*Tsq*u1*u2*(e_b2*e_tau) - D[20]*Tsq*u2*u1_in*(e_b2*e_tau))/Ts_e_tau2;
	P[4] = (Q[2]*Tsq4 + D[4]*e_tau4 + Q[2]*e_tau4 + 2*D[4]*Ts*e_tau3 + 4*Q[2]*Ts*e_tau3 + 4*Q[2]*Tsq3*e_tau + D[4]*Tsq*e_tau2 + 6*Q[2]*Tsq*e_tau2 + 2*D[17]*Tsq*u1*e_tau2 - 2*D[17]*Tsq*u1_in*e_tau2 + D[21]*Tsq*(u1*u1)*e_tau2 + D[21]*Tsq*(u1_in*u1_in)*e_tau2 + 2*D[17]*Ts*u1*e_tau3 - 2*D[17]*Ts*u1_in*e_tau3 - 2*D[21]*Tsq*u1*u1_in*e_tau2)/Ts_e_tau4;
	P[5] = (D[5]*e_tau2 + D[5]*Ts*e_tau + D[6]*Ts*(e_b2*e_tau2) - D[31]*Ts*(e_b2*e_tau2) + D[6]*Tsq*(e_b2*e_tau) - D[31]*Tsq*(e_b2*e_tau) + D[16]*Ts*u2*e_tau - D[16]*Ts*u2_in*e_tau - D[13]*Ts*bias2*(e_b2*e_tau2) - D[13]*Tsq*bias2*(e_b2*e_tau) + D[13]*Ts*u2*(e_b2*e_tau2) + D[13]*Tsq*u2*(e_b2*e_tau) + D[18]*Tsq*u2*(e_b2*e_tau) - D[33]*Tsq*u2*(e_b2*e_tau) - D[18]*Tsq*u2_in*(e_b2*e_tau) + D[33]*Tsq*u2_in*(e_b2*e_tau) + D[20]*Tsq*(u2*u2)*(e_b2*e_tau) - D[20]*Tsq*bias2*u2*(e_b2*e_tau) + D[20]*Tsq*bias2*u2_in*(e_b2*e_tau) - D[20]*Tsq*u2*u2_in*(e_b2*e_tau))/Ts_e_tau2;
	P[6] = (Q[3]*Tsq4 + D[6]*e_tau4 + Q[3]*e_tau4 + 2*D[6]*Ts*e_tau3 + 4*Q[3]*Ts*e_tau3 + 4*Q[3]*Tsq3*e_tau + D[6]*Tsq*e_tau2 + 6*Q[3]*Tsq*e_tau2 + 2*D[18]*Tsq*u2*e_tau2 - 2*D[18]*Tsq*u2_in*e_tau2 + D[21]*Tsq*(u2*u2)*e_tau2 + D[21]*Tsq*(u2_in*u2_in)*e_tau2 + 2*D[18]*Ts*u2*e_tau3 - 2*D[18]*Ts*u2_in*e_tau3 - 2*D[21]*Tsq*u2*u2_in*e_tau2)/Ts_e_tau4;
	P[7] = D[7] - Ts*(D[26]*e_b1 - D[9]*e_b1 + D[11]*e_b1*(bias1 - u1));
	P[8] = D[8] + D[10]*Ts*e_b2;
	P[9] = (e_tau*(D[9]*Ts + D[9]*e_tau + D[19]*Ts*u1 - D[19]*Ts*u1_in))/Ts_e_tau2;
	P[10] = (e_tau*(D[10]*Ts + D[10]*e_tau + D[19]*Ts*u2 - D[19]*Ts*u2_in))/Ts_e_tau2;
	P[11] = D[11] + Q[4];
	P[12] = D[12] - Ts*(D[32]*e_b2 - D[13]*e_b2 + D[14]*e_b2*(bias2 - u2));
	P[13] = (e_tau*(D[13]*Ts + D[13]*e_tau + D[20]*Ts*u2 - D[20]*Ts*u2_in))/Ts_e_tau2;
	P[14] = D[14] + Q[5];
	P[15] = D[15] - Ts*(D[28]*e_b1 - D[17]*e_b1 + D[19]*e_b1*(bias1 - u1));
	P[16] = D[16] - Ts*(D[33]*e_b2 - D[18]*e_b2 + D[20]*e_b2*(bias2 - u2));
	P[17] = (e_tau*(D[17]*Ts + D[17]*e_tau + D[21]*Ts*u1 - D[21]*Ts*u1_in))/Ts_e_tau2;
	P[18] = (e_tau*(D[18]*Ts + D[18]*e_tau + D[21]*Ts*u2 - D[21]*Ts*u2_in))/Ts_e_tau2;
	P[19] = D[19];
	P[20] = D[20];
	P[21] = D[21] + Q[6];
	P[22] = D[22] - Ts*(D[29]*e_b1 - D[24]*e_b1 + D[26]*e_b1*(bias1 - u1));
	P[23] = (D[25]*e_b2 - D[27]*e_b2*(bias2 - u2))*Ts + D[23];
	P[24] = (e_tau*(D[24]*Ts + D[24]*e_tau + D[28]*Ts*u1 - D[28]*Ts*u1_in))/Ts_e_tau2;
	P[25] = (e_tau*(D[25]*Ts + D[25]*e_tau + D[28]*Ts*u2 - D[28]*Ts*u2_in))/Ts_e_tau2;
	P[26] = D[26];
	P[27] = D[27];
	P[28] = D[28];
	P[29] = D[29] + Q[7];
	P[30] = D[30] - Ts*(D[34]*e_b2 - D[31]*e_b2 + D[32]*e_b2*(bias2 - u2));
	P[31] = (e_tau*(D[31]*Ts + D[31]*e_tau + D[33]*Ts*u2 - D[33]*Ts*u2_in))/Ts_e_tau2;
	P[32] = D[32];
	P[33] = D[33];
	P[34] = D[34] + Q[8];

    
	/********* this is the update part of the equation ***********/

    float S[3] = {P[0] + s_a, P[1] + s_a};

	X[0] = w1 + (P[0]*(gyro_x - w1))/S[0];
	X[1] = w2 + (P[1]*(gyro_y - w2))/S[1];
	X[2] = u1 + (P[2]*(gyro_x - w1))/S[0];
	X[3] = u2 + (P[5]*(gyro_y - w2))/S[1];
	X[4] = b1 + (P[7]*(gyro_x - w1))/S[0];
	X[5] = b2 + (P[12]*(gyro_y - w2))/S[1];
	X[6] = tau + (P[15]*(gyro_x - w1))/S[0] + (P[16]*(gyro_y - w2))/S[1];
	X[7] = bias1 + (P[22]*(gyro_x - w1))/S[0];
	X[8] = bias2 + (P[30]*(gyro_y - w2))/S[1];

	// update the duplicate cache
	for (uint32_t i = 0; i < AF_NUMP; i++)
        D[i] = P[i];
    
	// This is an approximation that removes some cross axis uncertainty but
	// substantially reduces the number of calculations
	P[0] = -D[0]*(D[0]/S[0] - 1);
	P[1] = -D[1]*(D[1]/S[1] - 1);
	P[2] = -D[2]*(D[0]/S[0] - 1);
	P[3] = -D[3]*(D[1]/S[1] - 1);
	P[4] = D[4] - D[2]*D[2]/S[0];
	P[5] = -D[5]*(D[1]/S[1] - 1);
	P[6] = D[6] - D[5]*D[5]/S[1];
	P[7] = -D[7]*(D[0]/S[0] - 1);
	P[8] = -D[8]*(D[1]/S[1] - 1);
	P[9] = D[9] - (D[2]*D[7])/S[0];
	P[10] = D[10] - (D[5]*D[8])/S[1];
	P[11] = D[11] - D[7]*D[7]/S[0];
	P[12] = -D[12]*(D[1]/S[1] - 1);
	P[13] = D[13] - (D[5]*D[12])/S[1];
	P[14] = D[14] - D[12]*D[12]/S[1];
	P[15] = -D[15]*(D[0]/S[0] - 1);
	P[16] = -D[16]*(D[1]/S[1] - 1);
	P[17] = D[17] - (D[2]*D[15])/S[0];
	P[18] = D[18] - (D[5]*D[16])/S[1];
	P[19] = D[19] - (D[7]*D[15])/S[0];
	P[20] = D[20] - (D[12]*D[16])/S[1];
	P[21] = D[21] - D[15]*D[15]/S[0] - D[16]*D[16]/S[1];
	P[22] = -D[22]*(D[0]/S[0] - 1);
	P[23] = -D[23]*(D[1]/S[1] - 1);
	P[24] = D[24] - (D[2]*D[22])/S[0];
	P[25] = D[25] - (D[5]*D[23])/S[1];
	P[26] = D[26] - (D[7]*D[22])/S[0];
	P[27] = D[27] - (D[12]*D[23])/S[1];
	P[28] = D[28] - (D[15]*D[22])/S[0] - (D[16]*D[23])/S[1];
	P[29] = D[29] - D[22]*D[22]/S[0];
	P[30] = -D[30]*(D[1]/S[1] - 1);
	P[31] = D[31] - (D[5]*D[30])/S[1];
	P[32] = D[32] - (D[12]*D[30])/S[1];
	P[33] = D[33] - (D[16]*D[30])/S[1];
	P[34] = D[34] - D[30]*D[30]/S[1];

	// apply limits to some of the state variables
	if (X[6] > -1.5f)
	    X[6] = -1.5f;
	if (X[6] < -5.0f)
	    X[6] = -5.0f;
	if (X[7] > 0.5f)
	    X[7] = 0.5f;
	if (X[7] < -0.5f)
	    X[7] = -0.5f;
	if (X[8] > 0.5f)
	    X[8] = 0.5f;
	if (X[8] < -0.5f)
	    X[8] = -0.5f;
}

/**
 * Initialize the state variable and covariance matrix
 * for the system identification EKF
 */
static void af_init(float X[AF_NUMX], float P[AF_NUMP])
{
	const float q_init[AF_NUMX] = {
		1.0f, 1.0f,
		1.0f, 1.0f,
		0.05f, 0.05f,
		0.05f,
		0.05f, 0.05f
	};

	X[0] = X[1] = 0.0f;    // assume no rotation
	X[2] = X[3] = 0.0f;    // and no net torque
	X[4] = X[5]        = 10.0f;   // medium amount of strength
	X[6] = -4.0f;                 // and 50 ms time scale
	X[7] = X[8] = 0.0f; // zero bias

	// P initialization
	P[0] = q_init[0];
	P[1] = q_init[1];
	P[2] = 0.0f;
	P[3] = 0.0f;
	P[4] = q_init[2];
	P[5] = 0.0f;
	P[6] = q_init[3];
	P[7] = 0.0f;
	P[8] = 0.0f;
	P[9] = 0.0f;
	P[10] = 0.0f;
	P[11] = q_init[4];
	P[12] = 0.0f;
	P[13] = 0.0f;
	P[14] = q_init[5];
	P[15] = 0.0f;
	P[16] = 0.0f;
	P[17] = 0.0f;
	P[18] = 0.0f;
	P[19] = 0.0f;
	P[20] = 0.0f;
	P[21] = q_init[6];
	P[22] = 0.0f;
	P[23] = 0.0f;
	P[24] = 0.0f;
	P[25] = 0.0f;
	P[26] = 0.0f;
	P[27] = 0.0f;
	P[28] = 0.0f;
	P[29] = q_init[7];
	P[30] = 0.0f;
	P[31] = 0.0f;
	P[32] = 0.0f;
	P[33] = 0.0f;
	P[34] = q_init[8];
}

/**
 * @}
 * @}
 */
