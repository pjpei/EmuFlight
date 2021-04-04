/*
 * This file is part of Cleanflight and Betaflight and EmuFlight.
 *
 * Cleanflight and Betaflight and EmuFlight are free software. You can redistribute
 * this software and/or modify this software under the terms of the
 * GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * Cleanflight and Betaflight and EmuFlight are distributed in the hope that they
 * will be useful, but WITHOUT ANY WARRANTY; without even the implied
 * warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this software.
 *
 * If not, see <http://www.gnu.org/licenses/>.
 */

// Inertial Measurement Unit (IMU)

#include <stdbool.h>
#include <stdint.h>
#include <math.h>

#include "platform.h"

#include "build/build_config.h"
#include "build/debug.h"

#include "common/axis.h"

#include "pg/pg.h"
#include "pg/pg_ids.h"

#include "drivers/time.h"

#include "fc/runtime_config.h"
#include "fc/rc.h"

#include "flight/gps_rescue.h"
#include "flight/imu.h"
#include "flight/mixer.h"
#include "flight/pid.h"

#include "io/gps.h"

#include "sensors/acceleration.h"
#include "sensors/barometer.h"
#include "sensors/compass.h"
#include "sensors/gyro.h"
#include "sensors/sensors.h"

#if defined(SIMULATOR_BUILD) && defined(SIMULATOR_MULTITHREAD)
#include <stdio.h>
#include <pthread.h>

static pthread_mutex_t imuUpdateLock;

#if defined(SIMULATOR_IMU_SYNC)
static uint32_t imuDeltaT = 0;
static bool imuUpdated = false;
#endif

#define IMU_LOCK pthread_mutex_lock(&imuUpdateLock)
#define IMU_UNLOCK pthread_mutex_unlock(&imuUpdateLock)

#else

#define IMU_LOCK
#define IMU_UNLOCK

#endif

// the limit (in degrees/second) beyond which we stop integrating
// omega_I. At larger spin rates the DCM PI controller can get 'dizzy'
// which results in false gyro drift. See
// http://gentlenav.googlecode.com/files/fastRotations.pdf

#define SPIN_RATE_LIMIT 20

#define ATTITUDE_RESET_QUIET_TIME 250000   // 250ms - gyro quiet period after disarm before attitude reset
#define ATTITUDE_RESET_GYRO_LIMIT 15       // 15 deg/sec - gyro limit for quiet period
#define ATTITUDE_RESET_KP_GAIN    25.0     // dcmKpGain value to use during attitude reset
#define ATTITUDE_RESET_ACTIVE_TIME 500000  // 500ms - Time to wait for attitude to converge at high gain
#define GPS_COG_MIN_GROUNDSPEED 500        // 500cm/s minimum groundspeed for a gps heading to be considered valid

float accAverage[XYZ_AXIS_COUNT];

bool canUseGPSHeading = true;

bool levelRecoveryActive = false;
int levelRecoveryStrength = 0;

static float throttleAngleScale;
static int throttleAngleValue;
static float fc_acc;
static float smallAngleCosZ = 0;

static imuRuntimeConfig_t imuRuntimeConfig;

float rMat[3][3];

STATIC_UNIT_TESTED bool attitudeIsEstablished = false;

// quaternion of sensor frame relative to earth frame
STATIC_UNIT_TESTED quaternion q = QUATERNION_INITIALIZE;
STATIC_UNIT_TESTED quaternionProducts qP = QUATERNION_PRODUCTS_INITIALIZE;

quaternion qM[6] = {QUATERNION_INITIALIZE, QUATERNION_INITIALIZE, QUATERNION_INITIALIZE, QUATERNION_INITIALIZE, QUATERNION_INITIALIZE, QUATERNION_INITIALIZE};
quaternionProducts qPM[6] = {QUATERNION_PRODUCTS_INITIALIZE, QUATERNION_PRODUCTS_INITIALIZE, QUATERNION_PRODUCTS_INITIALIZE, QUATERNION_PRODUCTS_INITIALIZE, QUATERNION_PRODUCTS_INITIALIZE, QUATERNION_PRODUCTS_INITIALIZE};

quaternion qLM[6] = {QUATERNION_INITIALIZE, QUATERNION_INITIALIZE, QUATERNION_INITIALIZE, QUATERNION_INITIALIZE, QUATERNION_INITIALIZE, QUATERNION_INITIALIZE};
quaternion qTM[6] = {QUATERNION_INITIALIZE, QUATERNION_INITIALIZE, QUATERNION_INITIALIZE, QUATERNION_INITIALIZE, QUATERNION_INITIALIZE, QUATERNION_INITIALIZE};

float thrust[6], pitch[6], roll[6];
float anglePitch, angleRoll;

quaternion qA = QUATERNION_INITIALIZE;
quaternionProducts qPA = QUATERNION_PRODUCTS_INITIALIZE;

// quaternion qThrustTransition = QUATERNION_INITIALIZE;
quaternionProducts qPThrustTranslation = QUATERNION_PRODUCTS_INITIALIZE;

// headfree quaternions
quaternion headfree = QUATERNION_INITIALIZE;
quaternion offset = QUATERNION_INITIALIZE;

// absolute angle inclination in multiple of 0.1 degree    180 deg = 1800
attitudeEulerAngles_t attitude = EULER_INITIALIZE;

PG_REGISTER_WITH_RESET_TEMPLATE(imuConfig_t, imuConfig, PG_IMU_CONFIG, 1);

PG_RESET_TEMPLATE(imuConfig_t, imuConfig,
    .dcm_kp = 2500,                // 1.0 * 10000
    .dcm_ki = 7,                   // 0.003 * 10000
    .small_angle = 180,
    .level_recovery = 1,
    .level_recovery_time = 2500,
    .level_recovery_coef = 5,
    .level_recovery_threshold = 1900,
    .roll = {0, 0, 0, 0, 0, 0},
    .pitch = {0, 0, 0, 0, 0, 0},
    .yaw = {0, 0, 0, 0, 0, 0},
    .debugMotor = 1,
);

static float invSqrt(float x)
{
    return 1.0f / sqrtf(x);
}

static void imuQuaternionComputeProducts(quaternion *quat, quaternionProducts *quatProd)
{
    quatProd->ww = quat->w * quat->w;
    quatProd->wx = quat->w * quat->x;
    quatProd->wy = quat->w * quat->y;
    quatProd->wz = quat->w * quat->z;
    quatProd->xx = quat->x * quat->x;
    quatProd->xy = quat->x * quat->y;
    quatProd->xz = quat->x * quat->z;
    quatProd->yy = quat->y * quat->y;
    quatProd->yz = quat->y * quat->z;
    quatProd->zz = quat->z * quat->z;
}

static void imuQuaternionComputeProductsProducts(quaternionProducts *quat)
{
    quat->ww = quat->w * quat->w;
    quat->wx = quat->w * quat->x;
    quat->wy = quat->w * quat->y;
    quat->wz = quat->w * quat->z;
    quat->xx = quat->x * quat->x;
    quat->xy = quat->x * quat->y;
    quat->xz = quat->x * quat->z;
    quat->yy = quat->y * quat->y;
    quat->yz = quat->y * quat->z;
    quat->zz = quat->z * quat->z;
}

STATIC_UNIT_TESTED void imuComputeRotationMatrix(void) {
    imuQuaternionComputeProducts(&q, &qP);

    rMat[0][0] = 1.0f - 2.0f * qP.yy - 2.0f * qP.zz;
    rMat[0][1] = 2.0f * (qP.xy + -qP.wz);
    rMat[0][2] = 2.0f * (qP.xz - -qP.wy);

    rMat[1][0] = 2.0f * (qP.xy - -qP.wz);
    rMat[1][1] = 1.0f - 2.0f * qP.xx - 2.0f * qP.zz;
    rMat[1][2] = 2.0f * (qP.yz + -qP.wx);

    rMat[2][0] = 2.0f * (qP.xz + -qP.wy);
    rMat[2][1] = 2.0f * (qP.yz - -qP.wx);
    rMat[2][2] = 1.0f - 2.0f * qP.xx - 2.0f * qP.yy;

#if defined(SIMULATOR_BUILD) && !defined(USE_IMU_CALC) && !defined(SET_IMU_FROM_EULER)
    rMat[1][0] = -2.0f * (qP.xy - -qP.wz);
    rMat[2][0] = -2.0f * (qP.xz + -qP.wy);
#endif
}

/*
* Calculate RC time constant used in the accZ lpf.
*/
static float calculateAccZLowPassFilterRCTimeConstant(float accz_lpf_cutoff)
{
    return 0.5f / (M_PIf * accz_lpf_cutoff);
}

static float calculateThrottleAngleScale(uint16_t throttle_correction_angle)
{
    return (1800.0f / M_PIf) * (900.0f / throttle_correction_angle);
}

static void imuComputeMotorQuatOffset(quaternionProducts *quatProd, int16_t initialRoll, int16_t initialPitch, int16_t initialYaw)
{
    if (initialRoll > 1800) {
        initialRoll -= 3600;
    }

    if (initialPitch > 1800) {
        initialPitch -= 3600;
    }

    if (initialYaw > 1800) {
        initialYaw -= 3600;
    }

    const float cosRoll = cos_approx(DECIDEGREES_TO_RADIANS(initialRoll) * 0.5f);
    const float sinRoll = sin_approx(DECIDEGREES_TO_RADIANS(initialRoll) * 0.5f);

    const float cosPitch = cos_approx(DECIDEGREES_TO_RADIANS(initialPitch) * 0.5f);
    const float sinPitch = sin_approx(DECIDEGREES_TO_RADIANS(initialPitch) * 0.5f);

    const float cosYaw = cos_approx(DECIDEGREES_TO_RADIANS(-initialYaw) * 0.5f);
    const float sinYaw = sin_approx(DECIDEGREES_TO_RADIANS(-initialYaw) * 0.5f);

    float q0 = cosRoll * cosPitch * cosYaw + sinRoll * sinPitch * sinYaw;
    float q1 = sinRoll * cosPitch * cosYaw - cosRoll * sinPitch * sinYaw;
    float q2 = cosRoll * sinPitch * cosYaw + sinRoll * cosPitch * sinYaw;
    float q3 = cosRoll * cosPitch * sinYaw - sinRoll * sinPitch * cosYaw;

    float recipNorm = invSqrt(sq(q0) + sq(q1) + sq(q2) + sq(q3));
    q0 *=recipNorm;
    q1 *=recipNorm;
    q2 *=recipNorm;
    q3 *=recipNorm;

    quatProd->x = q1;
    quatProd->y = q2;
    quatProd->z = q3;
    quatProd->w = q0;

    quatProd->xy = q1 * q2;
    quatProd->xz = q1 * q3;
    quatProd->yz = q2 * q3;

    quatProd->wx = q0 * q1;
    quatProd->wy = q0 * q2;
    quatProd->wz = q0 * q3;
}

static void imuComputeRemoveYaw(quaternionProducts *quatProd, int16_t initialYaw)
{
    if (initialYaw > 1800) {
        initialYaw -= 3600;
    }

    const float cosRoll = 1;
    const float sinRoll = 0;

    const float cosPitch = 1;
    const float sinPitch = 0;

    const float cosYaw = cos_approx(DECIDEGREES_TO_RADIANS(-initialYaw) * 0.5f);
    const float sinYaw = sin_approx(DECIDEGREES_TO_RADIANS(-initialYaw) * 0.5f);

    float q0 = cosRoll * cosPitch * cosYaw + sinRoll * sinPitch * sinYaw;
    float q1 = sinRoll * cosPitch * cosYaw - cosRoll * sinPitch * sinYaw;
    float q2 = cosRoll * sinPitch * cosYaw + sinRoll * cosPitch * sinYaw;
    float q3 = cosRoll * cosPitch * sinYaw - sinRoll * sinPitch * cosYaw;

    float recipNorm = invSqrt(sq(q0) + sq(q1) + sq(q2) + sq(q3));
    q0 *=recipNorm;
    q1 *=recipNorm;
    q2 *=recipNorm;
    q3 *=recipNorm;

    float A,B,C,D,E,F,G,H;

    A = (quatProd->w + quatProd->x) * (q0 + q1);
    B = (quatProd->z - quatProd->y) * (q2 - q3);
    C = (quatProd->w - quatProd->x) * (q2 + q3);
    D = (quatProd->y + quatProd->z) * (q0 - q1);
    E = (quatProd->x + quatProd->z) * (q1 + q2);
    F = (quatProd->x - quatProd->z) * (q1 - q2);
    G = (quatProd->w + quatProd->y) * (q0 - q3);
    H = (quatProd->w - quatProd->y) * (q0 + q3);

    quatProd->w = B + (- E - F + G + H) / 2.0f;
    quatProd->x = A - (+ E + F + G + H) / 2.0f;
    quatProd->y = C + (+ E - F + G - H) / 2.0f;
    quatProd->z = D + (+ E - F - G + H) / 2.0f;
    // Normalise quaternion
    recipNorm = invSqrt(sq(quatProd->w) + sq(quatProd->x) + sq(quatProd->y) + sq(quatProd->z));
    quatProd->w *= recipNorm;
    quatProd->x *= recipNorm;
    quatProd->y *= recipNorm;
    quatProd->z *= recipNorm;

    quatProd->xy = quatProd->x * quatProd->y;
    quatProd->xz = quatProd->x * quatProd->z;
    quatProd->yz = quatProd->y * quatProd->z;

    quatProd->wx = quatProd->w * quatProd->x;
    quatProd->wy = quatProd->w * quatProd->y;
    quatProd->wz = quatProd->w * quatProd->z;
}


void imuQuaternionMultiplicationProd(quaternion *q1, quaternionProducts *q2, quaternion *result, int order)
{
    float A,B,C,D,E,F,G,H;
  if(order == 1) {
    A = (q1->w + q1->x) * (q2->w + q2->x);
    B = (q1->z - q1->y) * (q2->y - q2->z);
    C = (q1->w - q1->x) * (q2->y + q2->z);
    D = (q1->y + q1->z) * (q2->w - q2->x);
    E = (q1->x + q1->z) * (q2->x + q2->y);
    F = (q1->x - q1->z) * (q2->x - q2->y);
    G = (q1->w + q1->y) * (q2->w - q2->z);
    H = (q1->w - q1->y) * (q2->w + q2->z);
  } else {
    A = (q2->w + q2->x) * (q1->w + q1->x);
    B = (q2->z - q2->y) * (q1->y - q1->z);
    C = (q2->w - q2->x) * (q1->y + q1->z);
    D = (q2->y + q2->z) * (q1->w - q1->x);
    E = (q2->x + q2->z) * (q1->x + q1->y);
    F = (q2->x - q2->z) * (q1->x - q1->y);
    G = (q2->w + q2->y) * (q1->w - q1->z);
    H = (q2->w - q2->y) * (q1->w + q1->z);
  }
    result->w = B + (- E - F + G + H) / 2.0f;
    result->x = A - (+ E + F + G + H) / 2.0f;
    result->y = C + (+ E - F + G - H) / 2.0f;
    result->z = D + (+ E - F - G + H) / 2.0f;
    // Normalise quaternion
    float recipNorm = invSqrt(sq(result->w) + sq(result->x) + sq(result->y) + sq(result->z));
    result->w *= recipNorm;
    result->x *= recipNorm;
    result->y *= recipNorm;
    result->z *= recipNorm;
}

void imuConfigure(uint16_t throttle_correction_angle, uint8_t throttle_correction_value)
{
    imuRuntimeConfig.dcm_kp = imuConfig()->dcm_kp / 10000.0f;
    imuRuntimeConfig.dcm_ki = imuConfig()->dcm_ki / 10000.0f;

    imuRuntimeConfig.level_recovery = imuConfig()->level_recovery;
    imuRuntimeConfig.level_recovery_time = imuConfig()->level_recovery_time;
    imuRuntimeConfig.level_recovery_coef = imuConfig()->level_recovery_coef;
    imuRuntimeConfig.level_recovery_threshold = imuConfig()->level_recovery_threshold;

    smallAngleCosZ = cos_approx(degreesToRadians(imuConfig()->small_angle));

    fc_acc = calculateAccZLowPassFilterRCTimeConstant(5.0f); // Set to fix value
    throttleAngleScale = calculateThrottleAngleScale(throttle_correction_angle);

    throttleAngleValue = throttle_correction_value;

    // initialize the quaternion offset for each motor
    for (int motor = 0; motor < 6; motor++) {
        imuComputeMotorQuatOffset(&qPM[motor], imuConfig()->roll[motor]*10, imuConfig()->pitch[motor]*10, imuConfig()->yaw[motor]*10);
    }
    imuComputeMotorQuatOffset(&qPA, 0, 0, 0);
    imuComputeMotorQuatOffset(&qPThrustTranslation, 0, 0, 0);
}

void imuInit(void)
{
#ifdef USE_GPS
    canUseGPSHeading = true;
#else
    canUseGPSHeading = false;
#endif

    imuComputeRotationMatrix();

#if defined(SIMULATOR_BUILD) && defined(SIMULATOR_MULTITHREAD)
    if (pthread_mutex_init(&imuUpdateLock, NULL) != 0) {
        printf("Create imuUpdateLock error!\n");
    }
#endif
}

#if defined(USE_ACC)

static void imuMahonyAHRSupdate(float dt, float gx, float gy, float gz,
                                float useAcc, float ax, float ay, float az,
                                bool useMag,
                                bool useCOG, float courseOverGround, const float dcmKpGain
                              )
{
    static float integralFBx = 0.0f,  integralFBy = 0.0f, integralFBz = 0.0f;    // integral error terms scaled by Ki

    // Calculate general spin rate (rad/s)
    const float spin_rate = sqrtf(sq(gx) + sq(gy) + sq(gz));

    // Use raw heading error (from GPS or whatever else)
    float ex = 0, ey = 0, ez = 0;
    if (useCOG) {
        while (courseOverGround >  M_PIf) {
            courseOverGround -= (2.0f * M_PIf);
        }

        while (courseOverGround < -M_PIf) {
            courseOverGround += (2.0f * M_PIf);
        }

        const float ez_ef = (- sin_approx(courseOverGround) * rMat[0][0] - cos_approx(courseOverGround) * rMat[1][0]);

        ex = rMat[2][0] * ez_ef;
        ey = rMat[2][1] * ez_ef;
        ez = rMat[2][2] * ez_ef;
    }

#ifdef USE_MAG
    // Use measured magnetic field vector
    float mx = mag.magADC[X];
    float my = mag.magADC[Y];
    float mz = mag.magADC[Z];
    float recipMagNorm = sq(mx) + sq(my) + sq(mz);
    if (useMag && recipMagNorm > 0.01f) {
        // Normalise magnetometer measurement
        recipMagNorm = invSqrt(recipMagNorm);
        mx *= recipMagNorm;
        my *= recipMagNorm;
        mz *= recipMagNorm;

        // For magnetometer correction we make an assumption that magnetic field is perpendicular to gravity (ignore Z-component in EF).
        // This way magnetic field will only affect heading and wont mess roll/pitch angles

        // (hx; hy; 0) - measured mag field vector in EF (assuming Z-component is zero)
        // (bx; 0; 0) - reference mag field vector heading due North in EF (assuming Z-component is zero)
        const float hx = rMat[0][0] * mx + rMat[0][1] * my + rMat[0][2] * mz;
        const float hy = rMat[1][0] * mx + rMat[1][1] * my + rMat[1][2] * mz;
        const float bx = sqrtf(hx * hx + hy * hy);

        // magnetometer error is cross product between estimated magnetic north and measured magnetic north (calculated in EF)
        const float ez_ef = -(hy * bx);

        // Rotate mag error vector back to BF and accumulate
        ex += rMat[2][0] * ez_ef;
        ey += rMat[2][1] * ez_ef;
        ez += rMat[2][2] * ez_ef;
    }
#else
    UNUSED(useMag);
#endif

    // Use measured acceleration vector
    float recipAccNorm = sq(ax) + sq(ay) + sq(az);
    if (useAcc && recipAccNorm > 0.01f) {
        // Normalise accelerometer measurement
        recipAccNorm = invSqrt(recipAccNorm);
        ax *= recipAccNorm;
        ay *= recipAccNorm;
        az *= recipAccNorm;

        // Error is sum of cross product between estimated direction and measured direction of gravity
        ex += (ay * rMat[2][2] - az * rMat[2][1]) * useAcc;
        ey += (az * rMat[2][0] - ax * rMat[2][2]) * useAcc;
        ez += (ax * rMat[2][1] - ay * rMat[2][0]) * useAcc;
    }

    // Compute and apply integral feedback if enabled
    if (imuRuntimeConfig.dcm_ki > 0.0f) {
        // Stop integrating if spinning beyond the certain limit
        if (spin_rate < DEGREES_TO_RADIANS(SPIN_RATE_LIMIT)) {
            const float dcmKiGain = imuRuntimeConfig.dcm_ki;
            integralFBx += dcmKiGain * ex * dt * useAcc;    // integral error scaled by Ki
            integralFBy += dcmKiGain * ey * dt * useAcc;
            integralFBz += dcmKiGain * ez * dt * useAcc;
        }
    } else {
        integralFBx = 0.0f;    // prevent integral windup
        integralFBy = 0.0f;
        integralFBz = 0.0f;
    }

    // Apply proportional and integral feedback
    gx += dcmKpGain * ex * useAcc + integralFBx;
    gy += dcmKpGain * ey * useAcc + integralFBy;
    gz += dcmKpGain * ez * useAcc + integralFBz;

    // Integrate rate of change of quaternion
    gx *= (0.5f * dt);
    gy *= (0.5f * dt);
    gz *= (0.5f * dt);

    quaternion buffer;
    buffer.w = q.w;
    buffer.x = q.x;
    buffer.y = q.y;
    buffer.z = q.z;

    q.w += (-buffer.x * gx - buffer.y * gy - buffer.z * gz);
    q.x += (+buffer.w * gx + buffer.y * gz - buffer.z * gy);
    q.y += (+buffer.w * gy - buffer.x * gz + buffer.z * gx);
    q.z += (+buffer.w * gz + buffer.x * gy - buffer.y * gx);

    // Normalise quaternion
    float recipNorm = invSqrt(sq(q.w) + sq(q.x) + sq(q.y) + sq(q.z));
    q.w *= recipNorm;
    q.x *= recipNorm;
    q.y *= recipNorm;
    q.z *= recipNorm;

    // Pre-compute rotation matrix from quaternion
    imuComputeRotationMatrix();

    attitudeIsEstablished = true;
}

void setNewLevel(void) {
   static int inAngleMode = 0;
   if ((FLIGHT_MODE(ANGLE_MODE) && inAngleMode == 0) || (FLIGHT_MODE(ANGLE_MODE) && FLIGHT_MODE(SET_LYNCH_MODE) && (getRcDeflectionAbs(ROLL) > 0.1f || getRcDeflectionAbs(PITCH) > 0.1f))) {
    inAngleMode = 1;
    qPA.w = q.w;
    qPA.x = -q.x;
    qPA.y = -q.y;
    qPA.z = -q.z;
    imuQuaternionComputeProductsProducts(&qPA);
    imuComputeRemoveYaw(&qPA, attitude.values.yaw);
    //imuComputeMotorQuatOffset(&qPA, attitude.values.roll, attitude.values.pitch, attitude.values.yaw);
    //imuComputeMotorQuatOffset(&qPA, -attitude.values.roll, -attitude.values.pitch, 0);
  } else if (!FLIGHT_MODE(ANGLE_MODE)) {
    inAngleMode = 0;
  }
}
float translationThrustFix = 1;

void applyThrustTransition(void) {
    if(FLIGHT_MODE(LYNCH_TRANSLATE)) {
        float rollTranslation = getRcDeflection(ROLL)*450;
        float pitchTranslation = getRcDeflection(PITCH)*450;
        if (getCosTiltAngle() > 0.0f) { // right side up treat yaw inputs in the normal direction
        imuComputeMotorQuatOffset(&qPThrustTranslation, -rollTranslation, -pitchTranslation, 0);
      } else {
        imuComputeMotorQuatOffset(&qPThrustTranslation, -rollTranslation, pitchTranslation, 0);
      }
        translationThrustFix = cos_approx(DEGREES_TO_RADIANS(rollTranslation/10)) * cos_approx(DEGREES_TO_RADIANS(pitchTranslation/10));
        translationThrustFix = 1 / translationThrustFix;
    } else {
        imuComputeMotorQuatOffset(&qPThrustTranslation, 0, 0, 0);
        translationThrustFix = 1;
    }
}

STATIC_UNIT_TESTED void imuUpdateEulerAngles(void)
{
    quaternionProducts buffer;
    static int changedToAngle = 0;
    static int motorsSetup = 0;
    if (FLIGHT_MODE(HEADFREE_MODE)) {
       imuQuaternionComputeProducts(&headfree, &buffer);

       attitude.values.roll = lrintf(atan2_approx((+2.0f * (buffer.wx + buffer.yz)), (+1.0f - 2.0f * (buffer.xx + buffer.yy))) * (1800.0f / M_PIf));
       attitude.values.pitch = lrintf(((0.5f * M_PIf) - acos_approx(+2.0f * (buffer.wy - buffer.xz))) * (1800.0f / M_PIf));
       attitude.values.yaw = lrintf((-atan2_approx((+2.0f * (buffer.wz + buffer.xy)), (+1.0f - 2.0f * (buffer.yy + buffer.zz))) * (1800.0f / M_PIf)));
    } else {
       attitude.values.roll = lrintf(((0.5f * M_PIf) - acos_approx(rMat[2][1])) * (1800.0f / M_PIf));
       attitude.values.pitch = lrintf(((0.5f * M_PIf) - acos_approx(-rMat[2][0])) * (1800.0f / M_PIf));
       attitude.values.yaw = lrintf((-atan2_approx(rMat[1][0], rMat[0][0]) * (1800.0f / M_PIf)));
    }

    applyThrustTransition();
    for (int motor = 0; motor < 6; motor++) {

    if (FLIGHT_MODE(SET_LYNCH_MODE) || (FLIGHT_MODE(ANGLE_MODE) && changedToAngle == 0) || motorsSetup == 0) {
    imuQuaternionMultiplicationProd(&q, &qPM[motor], &qM[motor], 1);
    qLM[motor] = qM[motor];
    }
    imuQuaternionMultiplicationProd(&qLM[motor], &qPThrustTranslation, &qTM[motor], 1);

    float temporaryThrust = 1.0f - 2.0f * qTM[motor].x*qTM[motor].x - 2.0f * qTM[motor].y*qTM[motor].y;
    float temporaryPitch = lrintf(((0.5f * M_PIf) - acos_approx(-(2.0f * (qTM[motor].x*qTM[motor].z - qTM[motor].w*qTM[motor].y)))) * (1800.0f / M_PIf));
    float temporaryRoll = lrintf(((0.5f * M_PIf) - acos_approx((2.0f * (qTM[motor].y*qTM[motor].z + qTM[motor].w*qTM[motor].x)))) * (1800.0f / M_PIf));

      if (motor == imuConfig()->debugMotor - 1) {
          DEBUG_SET(DEBUG_LYNCH, 0, lrintf(attitude.values.roll));
          DEBUG_SET(DEBUG_LYNCH, 1, lrintf(temporaryRoll));
          DEBUG_SET(DEBUG_LYNCH, 2, lrintf(temporaryPitch));
          DEBUG_SET(DEBUG_LYNCH, 3, lrintf(temporaryThrust*1000));
      }

// recalculate the thrust of the motors. do this when entering angle mode or while in set lynch mode
      if (FLIGHT_MODE(SET_LYNCH_MODE) || (FLIGHT_MODE(ANGLE_MODE) && changedToAngle == 0) || (FLIGHT_MODE(ANGLE_MODE) ||
       FLIGHT_MODE(LYNCH_TRANSLATE))) {
        thrust[motor] = temporaryThrust;
        pitch[motor] = temporaryPitch;
        roll[motor] = temporaryRoll;
      }
    }

    if (!FLIGHT_MODE(ANGLE_MODE)) {
      changedToAngle = 0;
    } else {
      changedToAngle = 1;
    }
    motorsSetup = 1;

    attitude.values.roll = lrintf(atan2_approx(rMat[2][1], rMat[2][2]) * (1800.0f / M_PIf));

    setNewLevel();

    attitude.values.roll = lrintf(((0.5f * M_PIf) - acos_approx(rMat[2][1])) * (1800.0f / M_PIf));

    imuQuaternionMultiplicationProd(&q, &qPA, &qA, 1);

    anglePitch = lrintf(((0.5f * M_PIf) - acos_approx(-(2.0f * (qA.x*qA.z - qA.w*qA.y)))) * (1800.0f / M_PIf));
    angleRoll = lrintf(((0.5f * M_PIf) - acos_approx((2.0f * (qA.y*qA.z + qA.w*qA.x)))) * (1800.0f / M_PIf));

    DEBUG_SET(DEBUG_LYNCH_ANGLE, 0, lrintf(attitude.values.roll));
    DEBUG_SET(DEBUG_LYNCH_ANGLE, 1, lrintf(attitude.values.pitch));
    DEBUG_SET(DEBUG_LYNCH_ANGLE, 2, lrintf(angleRoll));
    DEBUG_SET(DEBUG_LYNCH_ANGLE, 3, lrintf(anglePitch));


    DEBUG_SET(DEBUG_QUAT, 0 , lrintf(q.w*1000));
    DEBUG_SET(DEBUG_QUAT, 1 , lrintf(q.x*1000));
    DEBUG_SET(DEBUG_QUAT, 2 , lrintf(q.y*1000));
    DEBUG_SET(DEBUG_QUAT, 3 , lrintf(q.z*1000));

    if (attitude.values.yaw < 0) {
        attitude.values.yaw += 3600;
    }
}

static float imuIsAccelerometerHealthy(float *accAverage)
{
    float accMagnitudeSq = 0;
    for (int axis = 0; axis < 3; axis++) {
        const float a = accAverage[axis];
        accMagnitudeSq += a * a;
    }

    accMagnitudeSq = accMagnitudeSq * sq(acc.dev.acc_1G_rec);

    float accStrength = 0.0f;
    // Accept accel readings only in range 0.9g - 1.1g
    if ((0.5f < accMagnitudeSq) && (accMagnitudeSq < 1.69f)) {
        if (accMagnitudeSq > 1.0f) {
            accStrength = scaleRangef(accMagnitudeSq, 0.5, 1.0f, 0.0f, 1.0f);
        } else {
            accStrength = scaleRangef(accMagnitudeSq, 1.0f, 1.69f, 1.0f, 0.0f);
        }
    }
    return accStrength;
}

// Calculate the dcmKpGain to use. When armed, the gain is imuRuntimeConfig.dcm_kp * 1.0 scaling.
// When disarmed after initial boot, the scaling is set to 10.0 for the first 20 seconds to speed up initial convergence.
// After disarming we want to quickly reestablish convergence to deal with the attitude estimation being incorrect due to a crash.
//   - wait for a 250ms period of low gyro activity to ensure the craft is not moving
//   - use a large dcmKpGain value for 500ms to allow the attitude estimate to quickly converge
//   - reset the gain back to the standard setting
static float imuCalcKpGain(timeUs_t currentTimeUs, float useAcc, float *gyroAverage)
{
    static bool lastArmState = false;
    static timeUs_t gyroQuietPeriodTimeEnd = 0;
    static timeUs_t attitudeResetTimeEnd = 0;
    static bool attitudeResetCompleted = false;
    float ret;
    bool attitudeResetActive = false;

    const bool armState = ARMING_FLAG(ARMED);

    if (!armState) {
        if (lastArmState) {   // Just disarmed; start the gyro quiet period
            gyroQuietPeriodTimeEnd = currentTimeUs + ATTITUDE_RESET_QUIET_TIME;
            attitudeResetTimeEnd = 0;
            attitudeResetCompleted = false;
        }

        // If gyro activity exceeds the threshold then restart the quiet period.
        // Also, if the attitude reset has been complete and there is subsequent gyro activity then
        // start the reset cycle again. This addresses the case where the pilot rights the craft after a crash.
        if ((attitudeResetTimeEnd > 0) || (gyroQuietPeriodTimeEnd > 0) || attitudeResetCompleted) {
            if ((fabsf(gyroAverage[X]) > ATTITUDE_RESET_GYRO_LIMIT)
                || (fabsf(gyroAverage[Y]) > ATTITUDE_RESET_GYRO_LIMIT)
                || (fabsf(gyroAverage[Z]) > ATTITUDE_RESET_GYRO_LIMIT)
                || (!useAcc)) {

                gyroQuietPeriodTimeEnd = currentTimeUs + ATTITUDE_RESET_QUIET_TIME;
                attitudeResetTimeEnd = 0;
            }
        }
        if (attitudeResetTimeEnd > 0) {        // Resetting the attitude estimation
            if (currentTimeUs >= attitudeResetTimeEnd) {
                gyroQuietPeriodTimeEnd = 0;
                attitudeResetTimeEnd = 0;
                attitudeResetCompleted = true;
            } else {
                attitudeResetActive = true;
            }
        } else if ((gyroQuietPeriodTimeEnd > 0) && (currentTimeUs >= gyroQuietPeriodTimeEnd)) {
            // Start the high gain period to bring the estimation into convergence
            attitudeResetTimeEnd = currentTimeUs + ATTITUDE_RESET_ACTIVE_TIME;
            gyroQuietPeriodTimeEnd = 0;
        }
    }
    lastArmState = armState;

    if (attitudeResetActive) {
        ret = ATTITUDE_RESET_KP_GAIN;
    } else {
       ret = imuRuntimeConfig.dcm_kp;
       if (!armState) {
          ret = ret * 10.0f; // Scale the kP to generally converge faster when disarmed.
       }
    }

    if (levelRecoveryActive) {
        ret = imuRuntimeConfig.dcm_kp * (1.0f + imuRuntimeConfig.level_recovery_coef * levelRecoveryStrength / 1000);
    }

    return ret;
}

static void imuHandleLevelRecovery(timeUs_t currentTimeUs)
{
    static timeUs_t previousCrashTime = 0;

    for (int i = 0; i < XYZ_AXIS_COUNT; i++) {
        if (ABS(gyro.gyroADCf[i]) > imuRuntimeConfig.level_recovery_threshold) {
            previousCrashTime = currentTimeUs;
        }
    }

    timeUs_t elapsedSinceCrash = (currentTimeUs - previousCrashTime);
    if (elapsedSinceCrash < imuRuntimeConfig.level_recovery_time * 1000) {
        levelRecoveryActive = true;
        // 0 min, 1000 max
        // First half - full, second half - decaying
        levelRecoveryStrength = (imuRuntimeConfig.level_recovery_time * 1000 - elapsedSinceCrash) / imuRuntimeConfig.level_recovery_time;
        levelRecoveryStrength *= 2;
        if (levelRecoveryStrength > 1000)
            levelRecoveryStrength = 1000;
    } else {
        levelRecoveryActive = false;
        levelRecoveryStrength = 0;
    }

    if (!ARMING_FLAG(ARMED)) {
        levelRecoveryActive = false;
        levelRecoveryStrength = 0;
    }
}

bool isLevelRecoveryActive(void)
{
  return levelRecoveryActive;
}

#if defined(USE_GPS)
static void imuComputeQuaternionFromRPY(quaternionProducts *quatProd, int16_t initialRoll, int16_t initialPitch, int16_t initialYaw)
{
    if (initialRoll > 1800) {
        initialRoll -= 3600;
    }

    if (initialPitch > 1800) {
        initialPitch -= 3600;
    }

    if (initialYaw > 1800) {
        initialYaw -= 3600;
    }

    const float cosRoll = cos_approx(DECIDEGREES_TO_RADIANS(initialRoll) * 0.5f);
    const float sinRoll = sin_approx(DECIDEGREES_TO_RADIANS(initialRoll) * 0.5f);

    const float cosPitch = cos_approx(DECIDEGREES_TO_RADIANS(initialPitch) * 0.5f);
    const float sinPitch = sin_approx(DECIDEGREES_TO_RADIANS(initialPitch) * 0.5f);

    const float cosYaw = cos_approx(DECIDEGREES_TO_RADIANS(-initialYaw) * 0.5f);
    const float sinYaw = sin_approx(DECIDEGREES_TO_RADIANS(-initialYaw) * 0.5f);

    const float q0 = cosRoll * cosPitch * cosYaw + sinRoll * sinPitch * sinYaw;
    const float q1 = sinRoll * cosPitch * cosYaw - cosRoll * sinPitch * sinYaw;
    const float q2 = cosRoll * sinPitch * cosYaw + sinRoll * cosPitch * sinYaw;
    const float q3 = cosRoll * cosPitch * sinYaw - sinRoll * sinPitch * cosYaw;

    quatProd->xx = sq(q1);
    quatProd->yy = sq(q2);
    quatProd->zz = sq(q3);

    quatProd->xy = q1 * q2;
    quatProd->xz = q1 * q3;
    quatProd->yz = q2 * q3;

    quatProd->wx = q0 * q1;
    quatProd->wy = q0 * q2;
    quatProd->wz = q0 * q3;

    imuComputeRotationMatrix();

    attitudeIsEstablished = true;
}
#endif

void imuQuaternionMultiplication(quaternion *q1, quaternion *q2, quaternion *result)
{
    const float A = (q1->w + q1->x) * (q2->w + q2->x);
    const float B = (q1->z - q1->y) * (q2->y - q2->z);
    const float C = (q1->w - q1->x) * (q2->y + q2->z);
    const float D = (q1->y + q1->z) * (q2->w - q2->x);
    const float E = (q1->x + q1->z) * (q2->x + q2->y);
    const float F = (q1->x - q1->z) * (q2->x - q2->y);
    const float G = (q1->w + q1->y) * (q2->w - q2->z);
    const float H = (q1->w - q1->y) * (q2->w + q2->z);

    result->w = B + (- E - F + G + H) / 2.0f;
    result->x = A - (+ E + F + G + H) / 2.0f;
    result->y = C + (+ E - F + G - H) / 2.0f;
    result->z = D + (+ E - F - G + H) / 2.0f;
}

static void imuCalculateEstimatedAttitude(timeUs_t currentTimeUs)
{
    static timeUs_t previousIMUUpdateTime;
    float useAcc = 0;
    bool useMag = false;
    bool useCOG = false; // Whether or not correct yaw via imuMahonyAHRSupdate from our ground course
    float courseOverGround = 0; // To be used when useCOG is true.  Stored in Radians

    const timeDelta_t deltaT = currentTimeUs - previousIMUUpdateTime;
    previousIMUUpdateTime = currentTimeUs;

#ifdef USE_MAG
    if (sensors(SENSOR_MAG) && compassIsHealthy()
#ifdef USE_GPS_RESCUE
        && !gpsRescueDisableMag()
#endif
        ) {
        useMag = true;
    }
#endif
#if defined(USE_GPS)
    if (!useMag && sensors(SENSOR_GPS) && STATE(GPS_FIX) && gpsSol.numSat >= 5 && gpsSol.groundSpeed >= GPS_COG_MIN_GROUNDSPEED) {
        // Use GPS course over ground to correct attitude.values.yaw
        if (isFixedWing()) {
            courseOverGround = DECIDEGREES_TO_RADIANS(gpsSol.groundCourse);
            useCOG = true;
        } else {
            courseOverGround = DECIDEGREES_TO_RADIANS(gpsSol.groundCourse);

            useCOG = true;
        }

        if (useCOG && shouldInitializeGPSHeading()) {
            // Reset our reference and reinitialize quaternion.  This will likely ideally happen more than once per flight, but for now,
            // shouldInitializeGPSHeading() returns true only once.
            imuComputeQuaternionFromRPY(&qP, attitude.values.roll, attitude.values.pitch, gpsSol.groundCourse);

            useCOG = false; // Don't use the COG when we first reinitialize.  Next time around though, yes.
        }
    }
#endif

#if defined(SIMULATOR_BUILD) && !defined(USE_IMU_CALC)
    UNUSED(imuMahonyAHRSupdate);
    UNUSED(imuIsAccelerometerHealthy);
    UNUSED(useAcc);
    UNUSED(useMag);
    UNUSED(useCOG);
    UNUSED(canUseGPSHeading);
    UNUSED(courseOverGround);
    UNUSED(deltaT);
    UNUSED(imuCalcKpGain);
#else

#if defined(SIMULATOR_BUILD) && defined(SIMULATOR_IMU_SYNC)
//  printf("[imu]deltaT = %u, imuDeltaT = %u, currentTimeUs = %u, micros64_real = %lu\n", deltaT, imuDeltaT, currentTimeUs, micros64_real());
    deltaT = imuDeltaT;
#endif
    float gyroAverage[XYZ_AXIS_COUNT];
    gyroGetAccumulationAverage(gyroAverage);

    if (accGetAccumulationAverage(accAverage)) {
        useAcc = imuIsAccelerometerHealthy(accAverage);
    }
    if (imuRuntimeConfig.level_recovery) {
        imuHandleLevelRecovery(currentTimeUs);
    }

    imuMahonyAHRSupdate(deltaT * 1e-6f,
                        DEGREES_TO_RADIANS(gyroAverage[X]), DEGREES_TO_RADIANS(gyroAverage[Y]), DEGREES_TO_RADIANS(gyroAverage[Z]),
                        useAcc, accAverage[X], accAverage[Y], accAverage[Z],
                        useMag,
                        useCOG, courseOverGround,  imuCalcKpGain(currentTimeUs, useAcc, gyroAverage));

    imuUpdateEulerAngles();
#endif
}

static int calculateThrottleAngleCorrection(void)
{
    /*
    * Use 0 as the throttle angle correction if we are inverted, vertical or with a
    * small angle < 0.86 deg
    * TODO: Define this small angle in config.
    */
    if (getCosTiltAngle() <= 0.015f) {
        return 0;
    }
    int angle = lrintf(acos_approx(getCosTiltAngle()) * throttleAngleScale);
    if (angle > 900)
        angle = 900;
    return lrintf(throttleAngleValue * sin_approx(angle / (900.0f * M_PIf / 2.0f)));
}

void imuUpdateAttitude(timeUs_t currentTimeUs)
{
    if (sensors(SENSOR_ACC) && acc.isAccelUpdatedAtLeastOnce) {
        IMU_LOCK;
#if defined(SIMULATOR_BUILD) && defined(SIMULATOR_IMU_SYNC)
        if (imuUpdated == false) {
            IMU_UNLOCK;
            return;
        }
        imuUpdated = false;
#endif
        imuCalculateEstimatedAttitude(currentTimeUs);
        IMU_UNLOCK;

        // Update the throttle correction for angle and supply it to the mixer
        int throttleAngleCorrection = 0;
        if (throttleAngleValue && (FLIGHT_MODE(ANGLE_MODE) || FLIGHT_MODE(HORIZON_MODE)) && ARMING_FLAG(ARMED)) {
            throttleAngleCorrection = calculateThrottleAngleCorrection();
        }
        mixerSetThrottleAngleCorrection(throttleAngleCorrection);

    } else {
        acc.accADC[X] = 0;
        acc.accADC[Y] = 0;
        acc.accADC[Z] = 0;
    }
}
#endif // USE_ACC

bool shouldInitializeGPSHeading()
{
    static bool initialized = false;

    if (!initialized) {
        initialized = true;

        return true;
    }

    return false;
}

float getCosTiltAngle(void)
{
    return rMat[2][2];
}

float getMotorThrust(int motor)
{
    return thrust[motor];
}

float getMotorPitch(int motor)
{
    return pitch[motor];
}

float getMotorRoll(int motor)
{
    return roll[motor];
}

float getTranslationThrustFix(void) {
    return translationThrustFix;
}

float getAngleAngle(int axis)
{
    if (axis == ROLL) {
        return angleRoll;
    } else if (axis == PITCH) {
        return anglePitch;
    }

    return 0;
}

void getQuaternion(quaternion *quat)
{
   quat->w = q.w;
   quat->x = q.x;
   quat->y = q.y;
   quat->z = q.z;
}

#ifdef SIMULATOR_BUILD
void imuSetAttitudeRPY(float roll, float pitch, float yaw)
{
    IMU_LOCK;

    attitude.values.roll = roll * 10;
    attitude.values.pitch = pitch * 10;
    attitude.values.yaw = yaw * 10;

    IMU_UNLOCK;
}

void imuSetAttitudeQuat(float w, float x, float y, float z)
{
    IMU_LOCK;

    q.w = w;
    q.x = x;
    q.y = y;
    q.z = z;

    imuComputeRotationMatrix();

    attitudeIsEstablished = true;

    imuUpdateEulerAngles();

    IMU_UNLOCK;
}
#endif
#if defined(SIMULATOR_BUILD) && defined(SIMULATOR_IMU_SYNC)
void imuSetHasNewData(uint32_t dt)
{
    IMU_LOCK;

    imuUpdated = true;
    imuDeltaT = dt;

    IMU_UNLOCK;
}
#endif

bool imuQuaternionHeadfreeOffsetSet(void)
{
    if ((ABS(attitude.values.roll) < 450)  && (ABS(attitude.values.pitch) < 450)) {
        const float yaw = -atan2_approx((+2.0f * (qP.wz + qP.xy)), (+1.0f - 2.0f * (qP.yy + qP.zz)));

        offset.w = cos_approx(yaw/2);
        offset.x = 0;
        offset.y = 0;
        offset.z = sin_approx(yaw/2);

        return true;
    } else {
        return false;
    }
}


void imuQuaternionHeadfreeTransformVectorEarthToBody(t_fp_vector_def *v)
{
    quaternionProducts buffer;

    imuQuaternionMultiplication(&offset, &q, &headfree);
    imuQuaternionComputeProducts(&headfree, &buffer);

    const float x = (buffer.ww + buffer.xx - buffer.yy - buffer.zz) * v->X + 2.0f * (buffer.xy + buffer.wz) * v->Y + 2.0f * (buffer.xz - buffer.wy) * v->Z;
    const float y = 2.0f * (buffer.xy - buffer.wz) * v->X + (buffer.ww - buffer.xx + buffer.yy - buffer.zz) * v->Y + 2.0f * (buffer.yz + buffer.wx) * v->Z;
    const float z = 2.0f * (buffer.xz + buffer.wy) * v->X + 2.0f * (buffer.yz - buffer.wx) * v->Y + (buffer.ww - buffer.xx - buffer.yy + buffer.zz) * v->Z;

    v->X = x;
    v->Y = y;
    v->Z = z;
}

bool isUpright(void)
{
#ifdef USE_ACC
    return !sensors(SENSOR_ACC) || (attitudeIsEstablished && getCosTiltAngle() > smallAngleCosZ);
#else
    return true;
#endif
}

bool updateAngles() {
  return false;
}
