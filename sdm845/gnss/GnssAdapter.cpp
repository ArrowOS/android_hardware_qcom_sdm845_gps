/* Copyright (c) 2017-2018, The Linux Foundation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name of The Linux Foundation, nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */
#define LOG_NDEBUG 0
#define LOG_TAG "LocSvc_GnssAdapter"

#include <inttypes.h>
#include <sys/stat.h>
#include <errno.h>
#include <ctype.h>
#include <cutils/properties.h>
#include <math.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>
#include <GnssAdapter.h>
#include <string>
#include <loc_log.h>
#include <loc_nmea.h>
#include <Agps.h>
#include <SystemStatus.h>

#include <vector>

#define RAD2DEG    (180.0 / M_PI)

using namespace loc_core;

/* Method to fetch status cb from loc_net_iface library */
typedef AgpsCbInfo& (*LocAgpsGetAgpsCbInfo)(LocAgpsOpenResultCb openResultCb,
        LocAgpsCloseResultCb closeResultCb, void* userDataPtr);

static void agpsOpenResultCb (bool isSuccess, AGpsExtType agpsType, const char* apn,
        AGpsBearerType bearerType, void* userDataPtr);
static void agpsCloseResultCb (bool isSuccess, AGpsExtType agpsType, void* userDataPtr);

GnssAdapter::GnssAdapter() :
    LocAdapterBase(0,
                   LocDualContext::getLocFgContext(NULL,
                                                   NULL,
                                                   LocDualContext::mLocationHalName,
                                                   false), true, nullptr),
    mUlpProxy(new UlpProxyBase()),
    mUlpPositionMode(),
    mGnssSvIdUsedInPosition(),
    mGnssSvIdUsedInPosAvail(false),
    mControlCallbacks(),
    mPowerVoteId(0),
    mNmeaMask(0),
    mGnssSvIdConfig(),
    mGnssSvTypeConfig(),
    mGnssSvTypeConfigCb(nullptr),
    mNiData(),
    mAgpsManager(),
    mAgpsCbInfo(),
    mOdcpiRequestCb(nullptr),
    mOdcpiRequestActive(false),
    mOdcpiTimer(this),
    mOdcpiRequest(),
    mSystemStatus(SystemStatus::getInstance(mMsgTask)),
    mServerUrl(":"),
    mXtraObserver(mSystemStatus->getOsObserver(), mMsgTask)
{
    LOC_LOGD("%s]: Constructor %p", __func__, this);
    mUlpPositionMode.mode = LOC_POSITION_MODE_INVALID;

    pthread_condattr_t condAttr;
    pthread_condattr_init(&condAttr);
    pthread_condattr_setclock(&condAttr, CLOCK_MONOTONIC);
    pthread_cond_init(&mNiData.session.tCond, &condAttr);
    pthread_cond_init(&mNiData.sessionEs.tCond, &condAttr);
    pthread_condattr_destroy(&condAttr);

    /* Set ATL open/close callbacks */
    AgpsAtlOpenStatusCb atlOpenStatusCb =
            [this](int handle, int isSuccess, char* apn,
                    AGpsBearerType bearerType, AGpsExtType agpsType, LocApnTypeMask mask) {

                mLocApi->atlOpenStatus(
                        handle, isSuccess, apn, bearerType, agpsType, mask);
            };
    AgpsAtlCloseStatusCb atlCloseStatusCb =
            [this](int handle, int isSuccess) {

                mLocApi->atlCloseStatus(handle, isSuccess);
            };

    /* Register DS Client APIs */
    AgpsDSClientInitFn dsClientInitFn =
            [this](bool isDueToSSR) {

                return mLocApi->initDataServiceClient(isDueToSSR);
            };

    AgpsDSClientOpenAndStartDataCallFn dsClientOpenAndStartDataCallFn =
            [this] {

                return mLocApi->openAndStartDataCall();
            };

    AgpsDSClientStopDataCallFn dsClientStopDataCallFn =
            [this] {

                mLocApi->stopDataCall();
            };

    AgpsDSClientCloseDataCallFn dsClientCloseDataCallFn =
            [this] {

                mLocApi->closeDataCall();
            };

    AgpsDSClientReleaseFn dsClientReleaseFn =
            [this] {

                mLocApi->releaseDataServiceClient();
            };

    /* Send Msg function */
    SendMsgToAdapterMsgQueueFn sendMsgFn =
            [this](LocMsg* msg) {

                sendMsg(msg);
            };
    mAgpsManager.registerATLCallbacks(atlOpenStatusCb, atlCloseStatusCb,
            dsClientInitFn, dsClientOpenAndStartDataCallFn, dsClientStopDataCallFn,
            dsClientCloseDataCallFn, dsClientReleaseFn, sendMsgFn);

    readConfigCommand();
    setConfigCommand();
    initDefaultAgpsCommand();
}

void
GnssAdapter::setControlCallbacksCommand(LocationControlCallbacks& controlCallbacks)
{
    struct MsgSetControlCallbacks : public LocMsg {
        GnssAdapter& mAdapter;
        const LocationControlCallbacks mControlCallbacks;
        inline MsgSetControlCallbacks(GnssAdapter& adapter,
                                      LocationControlCallbacks& controlCallbacks) :
            LocMsg(),
            mAdapter(adapter),
            mControlCallbacks(controlCallbacks) {}
        inline virtual void proc() const {
            mAdapter.setControlCallbacks(mControlCallbacks);
        }
    };

    sendMsg(new MsgSetControlCallbacks(*this, controlCallbacks));
}

void
GnssAdapter::convertOptions(LocPosMode& out, const TrackingOptions& trackingOptions)
{
    switch (trackingOptions.mode) {
    case GNSS_SUPL_MODE_MSB:
        out.mode = LOC_POSITION_MODE_MS_BASED;
        break;
    case GNSS_SUPL_MODE_MSA:
        out.mode = LOC_POSITION_MODE_MS_ASSISTED;
        break;
    default:
        out.mode = LOC_POSITION_MODE_STANDALONE;
        break;
    }
    out.share_position = true;
    out.min_interval = trackingOptions.minInterval;
    out.powerMode = trackingOptions.powerMode;
    out.timeBetweenMeasurements = trackingOptions.tbm;
}

void
GnssAdapter::convertLocation(Location& out, const UlpLocation& ulpLocation,
                             const GpsLocationExtended& locationExtended,
                             const LocPosTechMask techMask)
{
    memset(&out, 0, sizeof(Location));
    out.size = sizeof(Location);
    if (LOC_GPS_LOCATION_HAS_LAT_LONG & ulpLocation.gpsLocation.flags) {
        out.flags |= LOCATION_HAS_LAT_LONG_BIT;
        out.latitude = ulpLocation.gpsLocation.latitude;
        out.longitude = ulpLocation.gpsLocation.longitude;
    }
    if (LOC_GPS_LOCATION_HAS_ALTITUDE & ulpLocation.gpsLocation.flags) {
        out.flags |= LOCATION_HAS_ALTITUDE_BIT;
        out.altitude = ulpLocation.gpsLocation.altitude;
    }
    if (LOC_GPS_LOCATION_HAS_SPEED & ulpLocation.gpsLocation.flags) {
        out.flags |= LOCATION_HAS_SPEED_BIT;
        out.speed = ulpLocation.gpsLocation.speed;
    }
    if (LOC_GPS_LOCATION_HAS_BEARING & ulpLocation.gpsLocation.flags) {
        out.flags |= LOCATION_HAS_BEARING_BIT;
        out.bearing = ulpLocation.gpsLocation.bearing;
    }
    if (LOC_GPS_LOCATION_HAS_ACCURACY & ulpLocation.gpsLocation.flags) {
        out.flags |= LOCATION_HAS_ACCURACY_BIT;
        out.accuracy = ulpLocation.gpsLocation.accuracy;
    }
    if (GPS_LOCATION_EXTENDED_HAS_VERT_UNC & locationExtended.flags) {
        out.flags |= LOCATION_HAS_VERTICAL_ACCURACY_BIT;
        out.verticalAccuracy = locationExtended.vert_unc;
    }
    if (GPS_LOCATION_EXTENDED_HAS_SPEED_UNC & locationExtended.flags) {
        out.flags |= LOCATION_HAS_SPEED_ACCURACY_BIT;
        out.speedAccuracy = locationExtended.speed_unc;
    }
    if (GPS_LOCATION_EXTENDED_HAS_BEARING_UNC & locationExtended.flags) {
        out.flags |= LOCATION_HAS_BEARING_ACCURACY_BIT;
        out.bearingAccuracy = locationExtended.bearing_unc;
    }
    out.timestamp = ulpLocation.gpsLocation.timestamp;
    if (LOC_POS_TECH_MASK_SATELLITE & techMask) {
        out.techMask |= LOCATION_TECHNOLOGY_GNSS_BIT;
    }
    if (LOC_POS_TECH_MASK_CELLID & techMask) {
        out.techMask |= LOCATION_TECHNOLOGY_CELL_BIT;
    }
    if (LOC_POS_TECH_MASK_WIFI & techMask) {
        out.techMask |= LOCATION_TECHNOLOGY_WIFI_BIT;
    }
    if (LOC_POS_TECH_MASK_SENSORS & techMask) {
        out.techMask |= LOCATION_TECHNOLOGY_SENSORS_BIT;
    }
}

void
GnssAdapter::convertLocationInfo(GnssLocationInfoNotification& out,
                                 const GpsLocationExtended& locationExtended)
{
    out.size = sizeof(GnssLocationInfoNotification);
    if (GPS_LOCATION_EXTENDED_HAS_ALTITUDE_MEAN_SEA_LEVEL & locationExtended.flags) {
        out.flags |= GNSS_LOCATION_INFO_ALTITUDE_MEAN_SEA_LEVEL_BIT;
        out.altitudeMeanSeaLevel = locationExtended.altitudeMeanSeaLevel;
    }
    if (GPS_LOCATION_EXTENDED_HAS_DOP & locationExtended.flags) {
        out.flags |= GNSS_LOCATION_INFO_DOP_BIT;
        out.pdop = locationExtended.pdop;
        out.hdop = locationExtended.hdop;
        out.vdop = locationExtended.vdop;
    }
    if (GPS_LOCATION_EXTENDED_HAS_EXT_DOP & locationExtended.flags) {
        out.flags |= GNSS_LOCATION_INFO_EXT_DOP_BIT;
        out.gdop = locationExtended.extDOP.GDOP;
        out.tdop = locationExtended.extDOP.TDOP;
    }
    if (GPS_LOCATION_EXTENDED_HAS_MAG_DEV & locationExtended.flags) {
        out.flags |= GNSS_LOCATION_INFO_MAGNETIC_DEVIATION_BIT;
        out.magneticDeviation = locationExtended.magneticDeviation;
    }
    if (GPS_LOCATION_EXTENDED_HAS_HOR_RELIABILITY & locationExtended.flags) {
        out.flags |= GNSS_LOCATION_INFO_HOR_RELIABILITY_BIT;
        switch (locationExtended.horizontal_reliability) {
            case LOC_RELIABILITY_VERY_LOW:
                out.horReliability = LOCATION_RELIABILITY_VERY_LOW;
                break;
            case LOC_RELIABILITY_LOW:
                out.horReliability = LOCATION_RELIABILITY_LOW;
                break;
            case LOC_RELIABILITY_MEDIUM:
                out.horReliability = LOCATION_RELIABILITY_MEDIUM;
                break;
            case LOC_RELIABILITY_HIGH:
                out.horReliability = LOCATION_RELIABILITY_HIGH;
                break;
            default:
                out.horReliability = LOCATION_RELIABILITY_NOT_SET;
                break;
        }
    }
    if (GPS_LOCATION_EXTENDED_HAS_VERT_RELIABILITY & locationExtended.flags) {
        out.flags |= GNSS_LOCATION_INFO_VER_RELIABILITY_BIT;
        switch (locationExtended.vertical_reliability) {
            case LOC_RELIABILITY_VERY_LOW:
                out.verReliability = LOCATION_RELIABILITY_VERY_LOW;
                break;
            case LOC_RELIABILITY_LOW:
                out.verReliability = LOCATION_RELIABILITY_LOW;
                break;
            case LOC_RELIABILITY_MEDIUM:
                out.verReliability = LOCATION_RELIABILITY_MEDIUM;
                break;
            case LOC_RELIABILITY_HIGH:
                out.verReliability = LOCATION_RELIABILITY_HIGH;
                break;
            default:
                out.verReliability = LOCATION_RELIABILITY_NOT_SET;
                break;
        }
    }
    if (GPS_LOCATION_EXTENDED_HAS_HOR_ELIP_UNC_MAJOR & locationExtended.flags) {
        out.flags |= GNSS_LOCATION_INFO_HOR_ACCURACY_ELIP_SEMI_MAJOR_BIT;
        out.horUncEllipseSemiMajor = locationExtended.horUncEllipseSemiMajor;
    }
    if (GPS_LOCATION_EXTENDED_HAS_HOR_ELIP_UNC_MINOR & locationExtended.flags) {
        out.flags |= GNSS_LOCATION_INFO_HOR_ACCURACY_ELIP_SEMI_MINOR_BIT;
        out.horUncEllipseSemiMinor = locationExtended.horUncEllipseSemiMinor;
    }
    if (GPS_LOCATION_EXTENDED_HAS_HOR_ELIP_UNC_AZIMUTH & locationExtended.flags) {
        out.flags |= GNSS_LOCATION_INFO_HOR_ACCURACY_ELIP_AZIMUTH_BIT;
        out.horUncEllipseOrientAzimuth = locationExtended.horUncEllipseOrientAzimuth;
    }
    if (GPS_LOCATION_EXTENDED_HAS_GNSS_SV_USED_DATA & locationExtended.flags) {
        out.flags |= GNSS_LOCATION_INFO_GNSS_SV_USED_DATA_BIT;
        out.svUsedInPosition.gpsSvUsedIdsMask =
                locationExtended.gnss_sv_used_ids.gps_sv_used_ids_mask;
        out.svUsedInPosition.gloSvUsedIdsMask =
                locationExtended.gnss_sv_used_ids.glo_sv_used_ids_mask;
        out.svUsedInPosition.galSvUsedIdsMask =
                locationExtended.gnss_sv_used_ids.gal_sv_used_ids_mask;
        out.svUsedInPosition.bdsSvUsedIdsMask =
                locationExtended.gnss_sv_used_ids.bds_sv_used_ids_mask;
        out.svUsedInPosition.qzssSvUsedIdsMask =
                locationExtended.gnss_sv_used_ids.qzss_sv_used_ids_mask;
    }
    if (GPS_LOCATION_EXTENDED_HAS_NAV_SOLUTION_MASK & locationExtended.flags) {
        out.flags |= GNSS_LOCATION_INFO_NAV_SOLUTION_MASK_BIT;
        out.navSolutionMask = locationExtended.navSolutionMask;
    }
    if (GPS_LOCATION_EXTENDED_HAS_POS_TECH_MASK & locationExtended.flags) {
        out.flags |= GPS_LOCATION_EXTENDED_HAS_POS_TECH_MASK;
        out.posTechMask = locationExtended.tech_mask;
    }
    if (GPS_LOCATION_EXTENDED_HAS_POS_DYNAMICS_DATA & locationExtended.flags) {
        out.flags |= GPS_LOCATION_EXTENDED_HAS_POS_DYNAMICS_DATA;
        if (locationExtended.bodyFrameData.bodyFrameDatamask &
                LOCATION_NAV_DATA_HAS_LONG_ACCEL_BIT) {
            out.bodyFrameData.bodyFrameDataMask |= LOCATION_NAV_DATA_HAS_LONG_ACCEL_BIT;
        }
        if (locationExtended.bodyFrameData.bodyFrameDatamask &
                LOCATION_NAV_DATA_HAS_LAT_ACCEL_BIT) {
            out.bodyFrameData.bodyFrameDataMask |= LOCATION_NAV_DATA_HAS_LAT_ACCEL_BIT;
        }
        if (locationExtended.bodyFrameData.bodyFrameDatamask &
                LOCATION_NAV_DATA_HAS_VERT_ACCEL_BIT) {
            out.bodyFrameData.bodyFrameDataMask |= LOCATION_NAV_DATA_HAS_VERT_ACCEL_BIT;
        }
        if (locationExtended.bodyFrameData.bodyFrameDatamask & LOCATION_NAV_DATA_HAS_YAW_RATE_BIT) {
            out.bodyFrameData.bodyFrameDataMask |= LOCATION_NAV_DATA_HAS_YAW_RATE_BIT;
        }
        if (locationExtended.bodyFrameData.bodyFrameDatamask & LOCATION_NAV_DATA_HAS_PITCH_BIT) {
            out.bodyFrameData.bodyFrameDataMask |= LOCATION_NAV_DATA_HAS_PITCH_BIT;
        }
        out.bodyFrameData.longAccel = locationExtended.bodyFrameData.longAccel;
        out.bodyFrameData.latAccel = locationExtended.bodyFrameData.latAccel;
        out.bodyFrameData.vertAccel = locationExtended.bodyFrameData.vertAccel;
        out.bodyFrameData.yawRate = locationExtended.bodyFrameData.yawRate;
        out.bodyFrameData.pitch = locationExtended.bodyFrameData.pitch;
    }
}

inline uint32_t
GnssAdapter::convertGpsLock(const GnssConfigGpsLock gpsLock)
{
    switch (gpsLock) {
        case GNSS_CONFIG_GPS_LOCK_MO:
            return 1;
        case GNSS_CONFIG_GPS_LOCK_NI:
            return 2;
        case GNSS_CONFIG_GPS_LOCK_MO_AND_NI:
            return 3;
        case GNSS_CONFIG_GPS_LOCK_NONE:
        default:
            return 0;
    }
}

inline GnssConfigGpsLock
GnssAdapter::convertGpsLock(const uint32_t gpsLock)
{
    switch (gpsLock) {
        case 1:
            return GNSS_CONFIG_GPS_LOCK_MO;
        case 2:
            return GNSS_CONFIG_GPS_LOCK_NI;
        case 3:
            return GNSS_CONFIG_GPS_LOCK_MO_AND_NI;
        case 0:
        default:
            return GNSS_CONFIG_GPS_LOCK_NONE;
    }
}

inline uint32_t
GnssAdapter::convertSuplVersion(const GnssConfigSuplVersion suplVersion)
{
    switch (suplVersion) {
        case GNSS_CONFIG_SUPL_VERSION_2_0_0:
            return 0x00020000;
        case GNSS_CONFIG_SUPL_VERSION_2_0_2:
            return 0x00020002;
        case GNSS_CONFIG_SUPL_VERSION_1_0_0:
        default:
            return 0x00010000;
    }
}

inline GnssConfigSuplVersion
GnssAdapter::convertSuplVersion(const uint32_t suplVersion)
{
    switch (suplVersion) {
        case 0x00020000:
            return GNSS_CONFIG_SUPL_VERSION_2_0_0;
        case 0x00020002:
            return GNSS_CONFIG_SUPL_VERSION_2_0_2;
        case 0x00010000:
        default:
            return GNSS_CONFIG_SUPL_VERSION_1_0_0;
    }
}

inline uint32_t
GnssAdapter::convertLppProfile(const GnssConfigLppProfile lppProfile)
{
    switch (lppProfile) {
        case GNSS_CONFIG_LPP_PROFILE_USER_PLANE:
            return 1;
        case GNSS_CONFIG_LPP_PROFILE_CONTROL_PLANE:
            return 2;
        case GNSS_CONFIG_LPP_PROFILE_USER_PLANE_AND_CONTROL_PLANE:
            return 3;
        case GNSS_CONFIG_LPP_PROFILE_RRLP_ON_LTE:
        default:
            return 0;
    }
}

inline GnssConfigLppProfile
GnssAdapter::convertLppProfile(const uint32_t lppProfile)
{
    switch (lppProfile) {
        case 1:
            return GNSS_CONFIG_LPP_PROFILE_USER_PLANE;
        case 2:
            return GNSS_CONFIG_LPP_PROFILE_CONTROL_PLANE;
        case 3:
            return GNSS_CONFIG_LPP_PROFILE_USER_PLANE_AND_CONTROL_PLANE;
        case 0:
        default:
            return GNSS_CONFIG_LPP_PROFILE_RRLP_ON_LTE;
    }
}

uint32_t
GnssAdapter::convertLppeCp(const GnssConfigLppeControlPlaneMask lppeControlPlaneMask)
{
    uint32_t mask = 0;
    if (GNSS_CONFIG_LPPE_CONTROL_PLANE_DBH_BIT & lppeControlPlaneMask) {
        mask |= (1<<0);
    }
    if (GNSS_CONFIG_LPPE_CONTROL_PLANE_WLAN_AP_MEASUREMENTS_BIT & lppeControlPlaneMask) {
        mask |= (1<<1);
    }
    if (GNSS_CONFIG_LPPE_CONTROL_PLANE_SRN_AP_MEASUREMENTS_BIT & lppeControlPlaneMask) {
        mask |= (1<<2);
    }
    if (GNSS_CONFIG_LPPE_CONTROL_PLANE_SENSOR_BARO_MEASUREMENTS_BIT & lppeControlPlaneMask) {
        mask |= (1<<3);
    }
    return mask;
}

GnssConfigLppeControlPlaneMask
GnssAdapter::convertLppeCp(const uint32_t lppeControlPlaneMask)
{
    GnssConfigLppeControlPlaneMask mask = 0;
    if ((1<<0) & lppeControlPlaneMask) {
        mask |= GNSS_CONFIG_LPPE_CONTROL_PLANE_DBH_BIT;
    }
    if ((1<<1) & lppeControlPlaneMask) {
        mask |= GNSS_CONFIG_LPPE_CONTROL_PLANE_WLAN_AP_MEASUREMENTS_BIT;
    }
    if ((1<<2) & lppeControlPlaneMask) {
        mask |= GNSS_CONFIG_LPPE_CONTROL_PLANE_SRN_AP_MEASUREMENTS_BIT;
    }
    if ((1<<3) & lppeControlPlaneMask) {
        mask |= GNSS_CONFIG_LPPE_CONTROL_PLANE_SENSOR_BARO_MEASUREMENTS_BIT;
    }
    return mask;
}


uint32_t
GnssAdapter::convertLppeUp(const GnssConfigLppeUserPlaneMask lppeUserPlaneMask)
{
    uint32_t mask = 0;
    if (GNSS_CONFIG_LPPE_USER_PLANE_DBH_BIT & lppeUserPlaneMask) {
        mask |= (1<<0);
    }
    if (GNSS_CONFIG_LPPE_USER_PLANE_WLAN_AP_MEASUREMENTS_BIT & lppeUserPlaneMask) {
        mask |= (1<<1);
    }
    if (GNSS_CONFIG_LPPE_USER_PLANE_SRN_AP_MEASUREMENTS_BIT & lppeUserPlaneMask) {
        mask |= (1<<2);
    }
    if (GNSS_CONFIG_LPPE_USER_PLANE_SENSOR_BARO_MEASUREMENTS_BIT & lppeUserPlaneMask) {
        mask |= (1<<3);
    }
    return mask;
}

GnssConfigLppeUserPlaneMask
GnssAdapter::convertLppeUp(const uint32_t lppeUserPlaneMask)
{
    GnssConfigLppeUserPlaneMask mask = 0;
    if ((1<<0) & lppeUserPlaneMask) {
        mask |= GNSS_CONFIG_LPPE_USER_PLANE_DBH_BIT;
    }
    if ((1<<1) & lppeUserPlaneMask) {
        mask |= GNSS_CONFIG_LPPE_USER_PLANE_WLAN_AP_MEASUREMENTS_BIT;
    }
    if ((1<<2) & lppeUserPlaneMask) {
        mask |= GNSS_CONFIG_LPPE_USER_PLANE_SRN_AP_MEASUREMENTS_BIT;
    }
    if ((1<<3) & lppeUserPlaneMask) {
        mask |= GNSS_CONFIG_LPPE_USER_PLANE_SENSOR_BARO_MEASUREMENTS_BIT;
    }
    return mask;
}

uint32_t
GnssAdapter::convertAGloProt(const GnssConfigAGlonassPositionProtocolMask aGloPositionProtocolMask)
{
    uint32_t mask = 0;
    if (GNSS_CONFIG_RRC_CONTROL_PLANE_BIT & aGloPositionProtocolMask) {
        mask |= (1<<0);
    }
    if (GNSS_CONFIG_RRLP_USER_PLANE_BIT & aGloPositionProtocolMask) {
        mask |= (1<<1);
    }
    if (GNSS_CONFIG_LLP_USER_PLANE_BIT & aGloPositionProtocolMask) {
        mask |= (1<<2);
    }
    if (GNSS_CONFIG_LLP_CONTROL_PLANE_BIT & aGloPositionProtocolMask) {
        mask |= (1<<3);
    }
    return mask;
}

uint32_t
GnssAdapter::convertEP4ES(const GnssConfigEmergencyPdnForEmergencySupl emergencyPdnForEmergencySupl)
{
    switch (emergencyPdnForEmergencySupl) {
       case GNSS_CONFIG_EMERGENCY_PDN_FOR_EMERGENCY_SUPL_YES:
           return 1;
       case GNSS_CONFIG_EMERGENCY_PDN_FOR_EMERGENCY_SUPL_NO:
       default:
           return 0;
    }
}

uint32_t
GnssAdapter::convertSuplEs(const GnssConfigSuplEmergencyServices suplEmergencyServices)
{
    switch (suplEmergencyServices) {
       case GNSS_CONFIG_SUPL_EMERGENCY_SERVICES_YES:
           return 1;
       case GNSS_CONFIG_SUPL_EMERGENCY_SERVICES_NO:
       default:
           return 0;
    }
}

uint32_t
GnssAdapter::convertSuplMode(const GnssConfigSuplModeMask suplModeMask)
{
    uint32_t mask = 0;
    if (GNSS_CONFIG_SUPL_MODE_MSB_BIT & suplModeMask) {
        mask |= (1<<0);
    }
    if (GNSS_CONFIG_SUPL_MODE_MSA_BIT & suplModeMask) {
        mask |= (1<<1);
    }
    return mask;
}

bool
GnssAdapter::resolveInAddress(const char* hostAddress, struct in_addr* inAddress)
{
    bool ret = true;

    struct hostent* hp;
    hp = gethostbyname(hostAddress);
    if (hp != NULL) { /* DNS OK */
        memcpy(inAddress, hp->h_addr_list[0], hp->h_length);
    } else {
        /* Try IP representation */
        if (inet_aton(hostAddress, inAddress) == 0) {
            /* IP not valid */
            LOC_LOGE("%s]: DNS query on '%s' failed", __func__, hostAddress);
            ret = false;
        }
    }

    return ret;
}

void
GnssAdapter::readConfigCommand()
{
    LOC_LOGD("%s]: ", __func__);

    struct MsgReadConfig : public LocMsg {
        GnssAdapter* mAdapter;
        ContextBase& mContext;
        inline MsgReadConfig(GnssAdapter* adapter,
                             ContextBase& context) :
            LocMsg(),
            mAdapter(adapter),
            mContext(context) {}
        inline virtual void proc() const {
            // reads config into mContext->mGps_conf
            mContext.readConfig();
            mContext.requestUlp((LocAdapterBase*)mAdapter, mContext.getCarrierCapabilities());
        }
    };

    if (mContext != NULL) {
        sendMsg(new MsgReadConfig(this, *mContext));
    }
}

LocationError
GnssAdapter::setSuplHostServer(const char* server, int port)
{
    LocationError locErr = LOCATION_ERROR_SUCCESS;
    if (ContextBase::mGps_conf.AGPS_CONFIG_INJECT) {
        char serverUrl[MAX_URL_LEN] = {};
        int32_t length = -1;
        const char noHost[] = "NONE";

        locErr = LOCATION_ERROR_INVALID_PARAMETER;

        if ((NULL == server) || (server[0] == 0) ||
                (strncasecmp(noHost, server, sizeof(noHost)) == 0)) {
            serverUrl[0] = '\0';
            length = 0;
        } else if (port > 0) {
            length = snprintf(serverUrl, sizeof(serverUrl), "%s:%u", server, port);
        }

        if (length >= 0 && strncasecmp(getServerUrl().c_str(),
                                       serverUrl, sizeof(serverUrl)) != 0) {
            setServerUrl(serverUrl);
            locErr = mLocApi->setServer(serverUrl, length);
            if (locErr != LOCATION_ERROR_SUCCESS) {
                LOC_LOGE("%s]:Error while setting SUPL_HOST server:%s",
                         __func__, serverUrl);
            }
        }
    }
    return locErr;
}

void
GnssAdapter::setConfigCommand()
{
    LOC_LOGD("%s]: ", __func__);

    struct MsgSetConfig : public LocMsg {
        GnssAdapter& mAdapter;
        LocApiBase& mApi;
        inline MsgSetConfig(GnssAdapter& adapter,
                            LocApiBase& api) :
            LocMsg(),
            mAdapter(adapter),
            mApi(api) {}
        inline virtual void proc() const {
            if (ContextBase::mGps_conf.AGPS_CONFIG_INJECT) {
                mApi.setSUPLVersion(mAdapter.convertSuplVersion(ContextBase::mGps_conf.SUPL_VER));
                // mApi.setLPPConfig(mAdapter.convertLppProfile(ContextBase::mGps_conf.LPP_PROFILE));
                mApi.setAGLONASSProtocol(ContextBase::mGps_conf.A_GLONASS_POS_PROTOCOL_SELECT);
            }
            mAdapter.setSuplHostServer(ContextBase::mGps_conf.SUPL_HOST,
                                       ContextBase::mGps_conf.SUPL_PORT);
            mApi.setSensorControlConfig(ContextBase::mSap_conf.SENSOR_USAGE,
                                        ContextBase::mSap_conf.SENSOR_PROVIDER);

            /* Mark these LPPe functions, they are MBN configured.
            mApi.setLPPeProtocolCp(
                    mAdapter.convertLppeCp(ContextBase::mGps_conf.LPPE_CP_TECHNOLOGY));
            mApi.setLPPeProtocolUp(
                    mAdapter.convertLppeUp(ContextBase::mGps_conf.LPPE_UP_TECHNOLOGY));
            */

            // set nmea mask type
            uint32_t mask = 0;
            if (NMEA_PROVIDER_MP == ContextBase::mGps_conf.NMEA_PROVIDER) {
                mask |= LOC_NMEA_ALL_GENERAL_SUPPORTED_MASK;
            }
            if (mApi.isFeatureSupported(LOC_SUPPORTED_FEATURE_DEBUG_NMEA_V02)) {
                mask |= LOC_NMEA_MASK_DEBUG_V02;
            }
            if (mask != 0) {
                mApi.setNMEATypes(mask);
            }
            mAdapter.mNmeaMask= mask;

            mApi.setXtraVersionCheck(ContextBase::mGps_conf.XTRA_VERSION_CHECK);
            if (ContextBase::mSap_conf.GYRO_BIAS_RANDOM_WALK_VALID ||
                ContextBase::mSap_conf.ACCEL_RANDOM_WALK_SPECTRAL_DENSITY_VALID ||
                ContextBase::mSap_conf.ANGLE_RANDOM_WALK_SPECTRAL_DENSITY_VALID ||
                ContextBase::mSap_conf.RATE_RANDOM_WALK_SPECTRAL_DENSITY_VALID ||
                ContextBase::mSap_conf.VELOCITY_RANDOM_WALK_SPECTRAL_DENSITY_VALID ) {
                mApi.setSensorProperties(
                    ContextBase::mSap_conf.GYRO_BIAS_RANDOM_WALK_VALID,
                    ContextBase::mSap_conf.GYRO_BIAS_RANDOM_WALK,
                    ContextBase::mSap_conf.ACCEL_RANDOM_WALK_SPECTRAL_DENSITY_VALID,
                    ContextBase::mSap_conf.ACCEL_RANDOM_WALK_SPECTRAL_DENSITY,
                    ContextBase::mSap_conf.ANGLE_RANDOM_WALK_SPECTRAL_DENSITY_VALID,
                    ContextBase::mSap_conf.ANGLE_RANDOM_WALK_SPECTRAL_DENSITY,
                    ContextBase::mSap_conf.RATE_RANDOM_WALK_SPECTRAL_DENSITY_VALID,
                    ContextBase::mSap_conf.RATE_RANDOM_WALK_SPECTRAL_DENSITY,
                    ContextBase::mSap_conf.VELOCITY_RANDOM_WALK_SPECTRAL_DENSITY_VALID,
                    ContextBase::mSap_conf.VELOCITY_RANDOM_WALK_SPECTRAL_DENSITY);
            }
            mApi.setSensorPerfControlConfig(
                ContextBase::mSap_conf.SENSOR_CONTROL_MODE,
                   ContextBase::mSap_conf.SENSOR_ACCEL_SAMPLES_PER_BATCH,
                   ContextBase::mSap_conf.SENSOR_ACCEL_BATCHES_PER_SEC,
                   ContextBase::mSap_conf.SENSOR_GYRO_SAMPLES_PER_BATCH,
                   ContextBase::mSap_conf.SENSOR_GYRO_BATCHES_PER_SEC,
                   ContextBase::mSap_conf.SENSOR_ACCEL_SAMPLES_PER_BATCH_HIGH,
                   ContextBase::mSap_conf.SENSOR_ACCEL_BATCHES_PER_SEC_HIGH,
                   ContextBase::mSap_conf.SENSOR_GYRO_SAMPLES_PER_BATCH_HIGH,
                   ContextBase::mSap_conf.SENSOR_GYRO_BATCHES_PER_SEC_HIGH,
                   ContextBase::mSap_conf.SENSOR_ALGORITHM_CONFIG_MASK);
        }
    };

    sendMsg(new MsgSetConfig(*this, *mLocApi));
}

uint32_t*
GnssAdapter::gnssUpdateConfigCommand(GnssConfig config)
{
    // count the number of bits set
    GnssConfigFlagsMask flagsCopy = config.flags;
    size_t count = 0;
    while (flagsCopy > 0) {
        if (flagsCopy & 1) {
            count++;
        }
        flagsCopy >>= 1;
    }
    std::string idsString = "[";
    uint32_t* ids = NULL;
    if (count > 0) {
        ids = new uint32_t[count];
        if (ids == nullptr) {
            LOC_LOGE("%s] new allocation failed, fatal error.", __func__);
            return nullptr;
        }
        for (size_t i=0; i < count; ++i) {
            ids[i] = generateSessionId();
            IF_LOC_LOGD {
                idsString += std::to_string(ids[i]) + " ";
            }
        }
    }
    idsString += "]";

    LOC_LOGD("%s]: ids %s flags 0x%X", __func__, idsString.c_str(), config.flags);

    struct MsgGnssUpdateConfig : public LocMsg {
        GnssAdapter& mAdapter;
        LocApiBase& mApi;
        GnssConfig mConfig;
        uint32_t* mIds;
        size_t mCount;
        inline MsgGnssUpdateConfig(GnssAdapter& adapter,
                                   LocApiBase& api,
                                   GnssConfig config,
                                   uint32_t* ids,
                                   size_t count) :
            LocMsg(),
            mAdapter(adapter),
            mApi(api),
            mConfig(config),
            mIds(ids),
            mCount(count) {}
        inline virtual ~MsgGnssUpdateConfig()
        {
            delete[] mIds;
        }
        inline virtual void proc() const {
            LocationError* errs = new LocationError[mCount];
            LocationError err = LOCATION_ERROR_SUCCESS;
            uint32_t index = 0;

            if (errs == nullptr) {
                LOC_LOGE("%s] new allocation failed, fatal error.", __func__);
                return;
            }

            if (mConfig.flags & GNSS_CONFIG_FLAGS_GPS_LOCK_VALID_BIT) {
                uint32_t newGpsLock = mAdapter.convertGpsLock(mConfig.gpsLock);
                ContextBase::mGps_conf.GPS_LOCK = newGpsLock;
                if (0 == ContextBase::mGps_conf.GPS_LOCK) {
                    // we should minimally lock MO
                    ContextBase::mGps_conf.GPS_LOCK = 1;
                }
                if (0 == mAdapter.getPowerVoteId()) {
                    err = mApi.setGpsLock(mConfig.gpsLock);
                }
                if (index < mCount) {
                    errs[index++] = err;
                }
            }
            if (mConfig.flags & GNSS_CONFIG_FLAGS_SUPL_VERSION_VALID_BIT) {
                uint32_t newSuplVersion = mAdapter.convertSuplVersion(mConfig.suplVersion);
                if (newSuplVersion != ContextBase::mGps_conf.SUPL_VER &&
                    ContextBase::mGps_conf.AGPS_CONFIG_INJECT) {
                    ContextBase::mGps_conf.SUPL_VER = newSuplVersion;
                    err = mApi.setSUPLVersion(mConfig.suplVersion);
                } else {
                    err = LOCATION_ERROR_SUCCESS;
                }
                if (index < mCount) {
                    errs[index++] = err;
                }
            }
            if (mConfig.flags & GNSS_CONFIG_FLAGS_SET_ASSISTANCE_DATA_VALID_BIT) {
                if (GNSS_ASSISTANCE_TYPE_SUPL == mConfig.assistanceServer.type) {
                    err = mAdapter.setSuplHostServer(mConfig.assistanceServer.hostName,
                                                     mConfig.assistanceServer.port);
                } else if (GNSS_ASSISTANCE_TYPE_C2K == mConfig.assistanceServer.type) {
                    if (ContextBase::mGps_conf.AGPS_CONFIG_INJECT) {
                        struct in_addr addr;
                        if (!mAdapter.resolveInAddress(mConfig.assistanceServer.hostName,
                                                       &addr)) {
                            LOC_LOGE("%s]: hostName %s cannot be resolved",
                                     __func__, mConfig.assistanceServer.hostName);
                            err = LOCATION_ERROR_INVALID_PARAMETER;
                        } else {
                            unsigned int ip = htonl(addr.s_addr);
                            err = mApi.setServer(ip, mConfig.assistanceServer.port,
                                                    LOC_AGPS_CDMA_PDE_SERVER);
                        }
                    } else {
                        err = LOCATION_ERROR_SUCCESS;
                    }
                } else {
                    LOC_LOGE("%s]: Not a valid gnss assistance type %u",
                             __func__, mConfig.assistanceServer.type);
                    err = LOCATION_ERROR_INVALID_PARAMETER;
                }
                if (index < mCount) {
                    errs[index++] = err;
                }
            }

            /* Comment out LPP injection as it's configured by MBN.
            if (mConfig.flags & GNSS_CONFIG_FLAGS_LPP_PROFILE_VALID_BIT) {
                uint32_t newLppProfile = mAdapter.convertLppProfile(mConfig.lppProfile);
                if (newLppProfile != ContextBase::mGps_conf.LPP_PROFILE &&
                    ContextBase::mGps_conf.AGPS_CONFIG_INJECT) {
                    ContextBase::mGps_conf.LPP_PROFILE = newLppProfile;
                    err = mApi.setLPPConfig(mConfig.lppProfile);
                } else {
                    err = LOCATION_ERROR_SUCCESS;
                }
                if (index < mCount) {
                    errs[index++] = err;
                }
            }
            */

            if (mConfig.flags & GNSS_CONFIG_FLAGS_LPPE_CONTROL_PLANE_VALID_BIT) {
                uint32_t newLppeControlPlaneMask =
                    mAdapter.convertLppeCp(mConfig.lppeControlPlaneMask);
                if (newLppeControlPlaneMask != ContextBase::mGps_conf.LPPE_CP_TECHNOLOGY) {
                    ContextBase::mGps_conf.LPPE_CP_TECHNOLOGY = newLppeControlPlaneMask;
                    err = mApi.setLPPeProtocolCp(mConfig.lppeControlPlaneMask);
                } else {
                    err = LOCATION_ERROR_SUCCESS;
                }
                if (index < mCount) {
                    errs[index++] = err;
                }
            }
            if (mConfig.flags & GNSS_CONFIG_FLAGS_LPPE_USER_PLANE_VALID_BIT) {
                uint32_t newLppeUserPlaneMask =
                    mAdapter.convertLppeUp(mConfig.lppeUserPlaneMask);
                if (newLppeUserPlaneMask != ContextBase::mGps_conf.LPPE_UP_TECHNOLOGY) {
                    ContextBase::mGps_conf.LPPE_UP_TECHNOLOGY = newLppeUserPlaneMask;
                    err = mApi.setLPPeProtocolUp(mConfig.lppeUserPlaneMask);
                } else {
                    err = LOCATION_ERROR_SUCCESS;
                }
                if (index < mCount) {
                    errs[index++] = err;
                }
            }
            if (mConfig.flags & GNSS_CONFIG_FLAGS_AGLONASS_POSITION_PROTOCOL_VALID_BIT) {
                uint32_t newAGloProtMask =
                    mAdapter.convertAGloProt(mConfig.aGlonassPositionProtocolMask);
                if (newAGloProtMask != ContextBase::mGps_conf.A_GLONASS_POS_PROTOCOL_SELECT &&
                    ContextBase::mGps_conf.AGPS_CONFIG_INJECT) {
                    ContextBase::mGps_conf.A_GLONASS_POS_PROTOCOL_SELECT = newAGloProtMask;
                    err = mApi.setAGLONASSProtocol(mConfig.aGlonassPositionProtocolMask);
                } else {
                    err = LOCATION_ERROR_SUCCESS;
                }
                if (index < mCount) {
                    errs[index++] = err;
                }
            }
            if (mConfig.flags & GNSS_CONFIG_FLAGS_EM_PDN_FOR_EM_SUPL_VALID_BIT) {
                uint32_t newEP4ES = mAdapter.convertEP4ES(mConfig.emergencyPdnForEmergencySupl);
                if (newEP4ES != ContextBase::mGps_conf.USE_EMERGENCY_PDN_FOR_EMERGENCY_SUPL) {
                    ContextBase::mGps_conf.USE_EMERGENCY_PDN_FOR_EMERGENCY_SUPL = newEP4ES;
                }
                err = LOCATION_ERROR_SUCCESS;
                if (index < mCount) {
                    errs[index++] = err;
                }
            }
            if (mConfig.flags & GNSS_CONFIG_FLAGS_SUPL_EM_SERVICES_BIT) {
                uint32_t newSuplEs = mAdapter.convertSuplEs(mConfig.suplEmergencyServices);
                if (newSuplEs != ContextBase::mGps_conf.SUPL_ES) {
                    ContextBase::mGps_conf.SUPL_ES = newSuplEs;
                }
                err = LOCATION_ERROR_SUCCESS;
                if (index < mCount) {
                    errs[index++] = err;
                }
            }
            if (mConfig.flags & GNSS_CONFIG_FLAGS_SUPL_MODE_BIT) {
                uint32_t newSuplMode = mAdapter.convertSuplMode(mConfig.suplModeMask);
                if (newSuplMode != ContextBase::mGps_conf.SUPL_MODE) {
                    ContextBase::mGps_conf.SUPL_MODE = newSuplMode;
                    mAdapter.getUlpProxy()->setCapabilities(
                        ContextBase::getCarrierCapabilities());
                    mAdapter.broadcastCapabilities(mAdapter.getCapabilities());
                }
                err = LOCATION_ERROR_SUCCESS;
                if (index < mCount) {
                    errs[index++] = err;
                }
            }
            if (mConfig.flags & GNSS_CONFIG_FLAGS_BLACKLISTED_SV_IDS_BIT) {
                // Check if feature is supported
                if (!mApi.isFeatureSupported(
                        LOC_SUPPORTED_FEATURE_CONSTELLATION_ENABLEMENT_V02)) {
                    LOC_LOGe("Feature constellation enablement not supported.");
                    err = LOCATION_ERROR_NOT_SUPPORTED;
                } else {
                    // Send the SV ID Config to Modem
                    err = mAdapter.gnssSvIdConfigUpdate(mConfig.blacklistedSvIds);
                    if (LOCATION_ERROR_SUCCESS != err) {
                        LOC_LOGe("Failed to send config to modem, err %d", err);
                    }
                }
                if (index < mCount) {
                    errs[index++] = err;
                }
            }

            mAdapter.reportResponse(index, errs, mIds);
            delete[] errs;
        }
    };

    if (NULL != ids) {
        sendMsg(new MsgGnssUpdateConfig(*this, *mLocApi, config, ids, count));
    } else {
        LOC_LOGE("%s]: No GNSS config items to update", __func__);
    }

    return ids;
}

LocationError
GnssAdapter::gnssSvIdConfigUpdate(const std::vector<GnssSvIdSource>& blacklistedSvIds)
{
    // Clear the existing config
    memset(&mGnssSvIdConfig, 0, sizeof(GnssSvIdConfig));

    // Convert the sv id lists to masks
    bool convertSuccess = convertToGnssSvIdConfig(blacklistedSvIds, mGnssSvIdConfig);

    // Now send to Modem if conversion successful
    if (convertSuccess) {
        return gnssSvIdConfigUpdate();
    } else {
        LOC_LOGe("convertToGnssSvIdConfig failed");
        return LOCATION_ERROR_INVALID_PARAMETER;
    }
}

LocationError
GnssAdapter::gnssSvIdConfigUpdate()
{
    LOC_LOGd("blacklist bds 0x%" PRIx64 ", glo 0x%" PRIx64
            ", qzss 0x%" PRIx64 ", gal 0x%" PRIx64,
            mGnssSvIdConfig.bdsBlacklistSvMask, mGnssSvIdConfig.gloBlacklistSvMask,
            mGnssSvIdConfig.qzssBlacklistSvMask, mGnssSvIdConfig.galBlacklistSvMask);

    // Now set required blacklisted SVs
    return mLocApi->setBlacklistSv(mGnssSvIdConfig);
}

uint32_t*
GnssAdapter::gnssGetConfigCommand(GnssConfigFlagsMask configMask) {

    // count the number of bits set
    GnssConfigFlagsMask flagsCopy = configMask;
    size_t count = 0;
    while (flagsCopy > 0) {
        if (flagsCopy & 1) {
            count++;
        }
        flagsCopy >>= 1;
    }
    std::string idsString = "[";
    uint32_t* ids = NULL;
    if (count > 0) {
        ids = new uint32_t[count];
        if (nullptr == ids) {
            LOC_LOGe("new allocation failed, fatal error.");
            return nullptr;
        }
        for (size_t i=0; i < count; ++i) {
            ids[i] = generateSessionId();
            IF_LOC_LOGD {
                idsString += std::to_string(ids[i]) + " ";
            }
        }
    }
    idsString += "]";

    LOC_LOGd("ids %s flags 0x%X", idsString.c_str(), configMask);

    struct MsgGnssGetConfig : public LocMsg {
        GnssAdapter& mAdapter;
        LocApiBase& mApi;
        GnssConfigFlagsMask mConfigMask;
        uint32_t* mIds;
        size_t mCount;
        inline MsgGnssGetConfig(GnssAdapter& adapter,
                                LocApiBase& api,
                                GnssConfigFlagsMask configMask,
                                uint32_t* ids,
                                size_t count) :
            LocMsg(),
            mAdapter(adapter),
            mApi(api),
            mConfigMask(configMask),
            mIds(ids),
            mCount(count) {}
        inline virtual ~MsgGnssGetConfig()
        {
            delete[] mIds;
        }
        inline virtual void proc() const {

            LocationError* errs = new LocationError[mCount];
            LocationError err = LOCATION_ERROR_SUCCESS;
            uint32_t index = 0;

            if (nullptr == errs) {
                LOC_LOGE("%s] new allocation failed, fatal error.", __func__);
                return;
            }

            if (mConfigMask & GNSS_CONFIG_FLAGS_GPS_LOCK_VALID_BIT) {
                if (index < mCount) {
                    errs[index++] = LOCATION_ERROR_NOT_SUPPORTED;
                }
            }
            if (mConfigMask & GNSS_CONFIG_FLAGS_SUPL_VERSION_VALID_BIT) {
                if (index < mCount) {
                    errs[index++] = LOCATION_ERROR_NOT_SUPPORTED;
                }
            }
            if (mConfigMask & GNSS_CONFIG_FLAGS_SET_ASSISTANCE_DATA_VALID_BIT) {
                if (index < mCount) {
                    errs[index++] = LOCATION_ERROR_NOT_SUPPORTED;
                }
            }
            if (mConfigMask & GNSS_CONFIG_FLAGS_LPP_PROFILE_VALID_BIT) {
                if (index < mCount) {
                    errs[index++] = LOCATION_ERROR_NOT_SUPPORTED;
                }
            }
            if (mConfigMask & GNSS_CONFIG_FLAGS_LPPE_CONTROL_PLANE_VALID_BIT) {
                if (index < mCount) {
                    errs[index++] = LOCATION_ERROR_NOT_SUPPORTED;
                }
            }
            if (mConfigMask & GNSS_CONFIG_FLAGS_LPPE_USER_PLANE_VALID_BIT) {
                if (index < mCount) {
                    errs[index++] = LOCATION_ERROR_NOT_SUPPORTED;
                }
            }
            if (mConfigMask & GNSS_CONFIG_FLAGS_AGLONASS_POSITION_PROTOCOL_VALID_BIT) {
                if (index < mCount) {
                    errs[index++] = LOCATION_ERROR_NOT_SUPPORTED;
                }
            }
            if (mConfigMask & GNSS_CONFIG_FLAGS_EM_PDN_FOR_EM_SUPL_VALID_BIT) {
                if (index < mCount) {
                    errs[index++] = LOCATION_ERROR_NOT_SUPPORTED;
                }
            }
            if (mConfigMask & GNSS_CONFIG_FLAGS_SUPL_EM_SERVICES_BIT) {
                if (index < mCount) {
                    errs[index++] = LOCATION_ERROR_NOT_SUPPORTED;
                }
            }
            if (mConfigMask & GNSS_CONFIG_FLAGS_SUPL_MODE_BIT) {
                err = LOCATION_ERROR_NOT_SUPPORTED;
                if (index < mCount) {
                    errs[index++] = LOCATION_ERROR_NOT_SUPPORTED;
                }
            }
            if (mConfigMask & GNSS_CONFIG_FLAGS_BLACKLISTED_SV_IDS_BIT) {
                // Check if feature is supported
                if (!mApi.isFeatureSupported(
                        LOC_SUPPORTED_FEATURE_CONSTELLATION_ENABLEMENT_V02)) {
                    LOC_LOGe("Feature not supported.");
                    err = LOCATION_ERROR_NOT_SUPPORTED;
                } else {
                    // Send request to Modem to fetch the config
                    err = mApi.getBlacklistSv();
                    if (LOCATION_ERROR_SUCCESS != err) {
                        LOC_LOGe("getConfig request to modem failed, err %d", err);
                    }
                }
                if (index < mCount) {
                    errs[index++] = err;
                }
            }

            mAdapter.reportResponse(index, errs, mIds);
            delete[] errs;
        }
    };

    if (NULL != ids) {
        sendMsg(new MsgGnssGetConfig(*this, *mLocApi, configMask, ids, count));
    } else {
        LOC_LOGe("No GNSS config items to Get");
    }

    return ids;
}

bool
GnssAdapter::convertToGnssSvIdConfig(
        const std::vector<GnssSvIdSource>& blacklistedSvIds, GnssSvIdConfig& config)
{
    bool retVal = true;
    config.size = sizeof(GnssSvIdConfig);

    // Empty vector => Clear any previous blacklisted SVs
    if (0 == blacklistedSvIds.size()) {
        config.gloBlacklistSvMask = 0;
        config.bdsBlacklistSvMask = 0;
        config.qzssBlacklistSvMask = 0;
        config.galBlacklistSvMask = 0;
    } else {
        // Parse the vector and convert SV IDs to mask values
        for (GnssSvIdSource source : blacklistedSvIds) {
            uint64_t* svMaskPtr = NULL;
            GnssSvId initialSvId = 0;
            GnssSvId lastSvId = 0;
            switch(source.constellation) {
            case GNSS_SV_TYPE_GLONASS:
                svMaskPtr = &config.gloBlacklistSvMask;
                initialSvId = GNSS_SV_CONFIG_GLO_INITIAL_SV_ID;
                lastSvId = GNSS_SV_CONFIG_GLO_LAST_SV_ID;
                break;
            case GNSS_SV_TYPE_BEIDOU:
                svMaskPtr = &config.bdsBlacklistSvMask;
                initialSvId = GNSS_SV_CONFIG_BDS_INITIAL_SV_ID;
                lastSvId = GNSS_SV_CONFIG_BDS_LAST_SV_ID;
                break;
            case GNSS_SV_TYPE_QZSS:
                svMaskPtr = &config.qzssBlacklistSvMask;
                initialSvId = GNSS_SV_CONFIG_QZSS_INITIAL_SV_ID;
                lastSvId = GNSS_SV_CONFIG_QZSS_LAST_SV_ID;
                break;
            case GNSS_SV_TYPE_GALILEO:
                svMaskPtr = &config.galBlacklistSvMask;
                initialSvId = GNSS_SV_CONFIG_GAL_INITIAL_SV_ID;
                lastSvId = GNSS_SV_CONFIG_GAL_LAST_SV_ID;
                break;
            default:
                break;
            }

            if (NULL == svMaskPtr) {
                LOC_LOGe("Invalid constellation %d", source.constellation);
            } else {
                // SV ID 0 = All SV IDs
                if (0 == source.svId) {
                    *svMaskPtr = GNSS_SV_CONFIG_ALL_BITS_ENABLED_MASK;
                } else if (source.svId < initialSvId || source.svId > lastSvId) {
                    LOC_LOGe("Invalid sv id %d for sv type %d allowed range [%d, %d]",
                            source.svId, source.constellation, initialSvId, lastSvId);
                } else {
                    *svMaskPtr |= ((uint64_t)1 << (source.svId - initialSvId));
                }
            }
        }
    }

    return retVal;
}

void GnssAdapter::convertFromGnssSvIdConfig(
        const GnssSvIdConfig& svConfig, GnssConfig& config)
{
    // Convert blacklisted SV mask values to vectors
    if (svConfig.bdsBlacklistSvMask) {
        convertGnssSvIdMaskToList(
                svConfig.bdsBlacklistSvMask, config.blacklistedSvIds,
                GNSS_SV_CONFIG_BDS_INITIAL_SV_ID, GNSS_SV_TYPE_BEIDOU);
        config.flags |= GNSS_CONFIG_FLAGS_BLACKLISTED_SV_IDS_BIT;
    }
    if (svConfig.galBlacklistSvMask) {
        convertGnssSvIdMaskToList(
                svConfig.galBlacklistSvMask, config.blacklistedSvIds,
                GNSS_SV_CONFIG_GAL_INITIAL_SV_ID, GNSS_SV_TYPE_GALILEO);
        config.flags |= GNSS_CONFIG_FLAGS_BLACKLISTED_SV_IDS_BIT;
    }
    if (svConfig.gloBlacklistSvMask) {
        convertGnssSvIdMaskToList(
                svConfig.gloBlacklistSvMask, config.blacklistedSvIds,
                GNSS_SV_CONFIG_GLO_INITIAL_SV_ID, GNSS_SV_TYPE_GLONASS);
        config.flags |= GNSS_CONFIG_FLAGS_BLACKLISTED_SV_IDS_BIT;
    }
    if (svConfig.qzssBlacklistSvMask) {
        convertGnssSvIdMaskToList(
                svConfig.qzssBlacklistSvMask, config.blacklistedSvIds,
                GNSS_SV_CONFIG_QZSS_INITIAL_SV_ID, GNSS_SV_TYPE_QZSS);
        config.flags |= GNSS_CONFIG_FLAGS_BLACKLISTED_SV_IDS_BIT;
    }
}

void GnssAdapter::convertGnssSvIdMaskToList(
        uint64_t svIdMask, std::vector<GnssSvIdSource>& svIds,
        GnssSvId initialSvId, GnssSvType svType)
{
    GnssSvIdSource source = {};
    source.size = sizeof(GnssSvIdSource);
    source.constellation = svType;

    // SV ID 0 => All SV IDs in mask
    if (GNSS_SV_CONFIG_ALL_BITS_ENABLED_MASK == svIdMask) {
        source.svId = 0;
        svIds.push_back(source);
        return;
    }

    // Convert each bit in svIdMask to vector entry
    uint32_t bitNumber = 0;
    while (svIdMask > 0) {
        if (svIdMask & 0x1) {
            source.svId = bitNumber + initialSvId;
            svIds.push_back(source);
        }
        bitNumber++;
        svIdMask >>= 1;
    }
}

void GnssAdapter::reportGnssSvIdConfigEvent(const GnssSvIdConfig& config)
{
    struct MsgReportGnssSvIdConfig : public LocMsg {
        GnssAdapter& mAdapter;
        const GnssSvIdConfig mConfig;
        inline MsgReportGnssSvIdConfig(GnssAdapter& adapter,
                                 const GnssSvIdConfig& config) :
            LocMsg(),
            mAdapter(adapter),
            mConfig(config) {}
        inline virtual void proc() const {
            mAdapter.reportGnssSvIdConfig(mConfig);
        }
    };

    sendMsg(new MsgReportGnssSvIdConfig(*this, config));
}

void GnssAdapter::reportGnssSvIdConfig(const GnssSvIdConfig& svIdConfig)
{
    GnssConfig config = {};
    config.size = sizeof(GnssConfig);

    // Invoke control clients config callback
    if (nullptr != mControlCallbacks.gnssConfigCb &&
            svIdConfig.size == sizeof(GnssSvIdConfig)) {
        convertFromGnssSvIdConfig(svIdConfig, config);
        LOC_LOGd("blacklist bds 0x%" PRIx64 ", glo 0x%" PRIx64
                ", qzss 0x%" PRIx64 ", gal 0x%" PRIx64,
                svIdConfig.bdsBlacklistSvMask, svIdConfig.gloBlacklistSvMask,
                svIdConfig.qzssBlacklistSvMask, svIdConfig.galBlacklistSvMask);
        mControlCallbacks.gnssConfigCb(config);
    } else {
        LOC_LOGe("Failed to report, size %d", (uint32_t)config.size);
    }
}

void
GnssAdapter::gnssUpdateSvTypeConfigCommand(GnssSvTypeConfig config)
{
    struct MsgGnssUpdateSvTypeConfig : public LocMsg {
        GnssAdapter* mAdapter;
        LocApiBase* mApi;
        GnssSvTypeConfig mConfig;
        inline MsgGnssUpdateSvTypeConfig(
                GnssAdapter* adapter,
                LocApiBase* api,
                GnssSvTypeConfig& config) :
            LocMsg(),
            mAdapter(adapter),
            mApi(api),
            mConfig(config) {}
        inline virtual void proc() const {
            // Check if feature is supported
            if (!mApi->isFeatureSupported(
                    LOC_SUPPORTED_FEATURE_CONSTELLATION_ENABLEMENT_V02)) {
                LOC_LOGe("Feature not supported.");
            } else {
                // Send update request to modem
                LocationError err = mAdapter->gnssSvTypeConfigUpdate(mConfig);
                if (err != LOCATION_ERROR_SUCCESS) {
                    LOC_LOGe("Set constellation request failed, err %d", err);
                }
            }
        }
    };

    sendMsg(new MsgGnssUpdateSvTypeConfig(this, mLocApi, config));
}

LocationError
GnssAdapter::gnssSvTypeConfigUpdate(const GnssSvTypeConfig& config)
{
    gnssSetSvTypeConfig(config);
    return gnssSvTypeConfigUpdate();
}

LocationError
GnssAdapter::gnssSvTypeConfigUpdate()
{
    LocationError err = LOCATION_ERROR_GENERAL_FAILURE;
    LOC_LOGd("constellations blacklisted 0x%" PRIx64 ", enabled 0x%" PRIx64,
             mGnssSvTypeConfig.blacklistedSvTypesMask, mGnssSvTypeConfig.enabledSvTypesMask);

    if (mGnssSvTypeConfig.size == sizeof(mGnssSvTypeConfig)) {
        GnssSvIdConfig blacklistConfig = {};
        // Revert to previously blacklisted SVs for each enabled constellation
        blacklistConfig = mGnssSvIdConfig;
        // Blacklist all SVs for each disabled constellation
        if (mGnssSvTypeConfig.blacklistedSvTypesMask) {
            if (mGnssSvTypeConfig.blacklistedSvTypesMask & GNSS_SV_TYPES_MASK_GLO_BIT) {
                blacklistConfig.gloBlacklistSvMask = GNSS_SV_CONFIG_ALL_BITS_ENABLED_MASK;
            }
            if (mGnssSvTypeConfig.blacklistedSvTypesMask & GNSS_SV_TYPES_MASK_BDS_BIT) {
                blacklistConfig.bdsBlacklistSvMask = GNSS_SV_CONFIG_ALL_BITS_ENABLED_MASK;
            }
            if (mGnssSvTypeConfig.blacklistedSvTypesMask & GNSS_SV_TYPES_MASK_QZSS_BIT) {
                blacklistConfig.qzssBlacklistSvMask = GNSS_SV_CONFIG_ALL_BITS_ENABLED_MASK;
            }
            if (mGnssSvTypeConfig.blacklistedSvTypesMask & GNSS_SV_TYPES_MASK_GAL_BIT) {
                blacklistConfig.galBlacklistSvMask = GNSS_SV_CONFIG_ALL_BITS_ENABLED_MASK;
            }
        }

        // Send blacklist info
        err = mLocApi->setBlacklistSv(blacklistConfig);
        if (LOCATION_ERROR_SUCCESS != err) {
            LOC_LOGE("Failed to send Set Blacklist");
        }
        // Send only enabled constellation config
        if (mGnssSvTypeConfig.enabledSvTypesMask) {
            GnssSvTypeConfig svTypeConfig = {sizeof(GnssSvTypeConfig), 0, 0};
            svTypeConfig.enabledSvTypesMask = mGnssSvTypeConfig.enabledSvTypesMask;
            err = mLocApi->setConstellationControl(svTypeConfig);
            if (LOCATION_ERROR_SUCCESS != err) {
                LOC_LOGE("Failed to send Set Constellation");
            }
        }
    } else {
        LOC_LOGE("Invalid GnssSvTypeConfig size");
        err = LOCATION_ERROR_SUCCESS;
    }

    return err;
}

void
GnssAdapter::gnssGetSvTypeConfigCommand(GnssSvTypeConfigCallback callback)
{
    struct MsgGnssGetSvTypeConfig : public LocMsg {
        GnssAdapter* mAdapter;
        LocApiBase* mApi;
        GnssSvTypeConfigCallback mCallback;
        inline MsgGnssGetSvTypeConfig(
                GnssAdapter* adapter,
                LocApiBase* api,
                GnssSvTypeConfigCallback callback) :
            LocMsg(),
            mAdapter(adapter),
            mApi(api),
            mCallback(callback) {}
        inline virtual void proc() const {
            if (!mApi->isFeatureSupported(
                    LOC_SUPPORTED_FEATURE_CONSTELLATION_ENABLEMENT_V02)) {
                LOC_LOGe("Feature not supported.");
            } else {
                // Save the callback
                mAdapter->gnssSetSvTypeConfigCallback(mCallback);
                // Send GET request to modem
                LocationError err = mApi->getConstellationControl();
                if (err != LOCATION_ERROR_SUCCESS) {
                    LOC_LOGe("Get constellation request failed, err %d", err);
                }
            }
        }
    };

    sendMsg(new MsgGnssGetSvTypeConfig(this, mLocApi, callback));
}

void
GnssAdapter::gnssResetSvTypeConfigCommand()
{
    struct MsgGnssResetSvTypeConfig : public LocMsg {
        GnssAdapter* mAdapter;
        LocApiBase* mApi;
        inline MsgGnssResetSvTypeConfig(
                GnssAdapter* adapter,
                LocApiBase* api) :
            LocMsg(),
            mAdapter(adapter),
            mApi(api) {}
        inline virtual void proc() const {
            if (!mApi->isFeatureSupported(
                    LOC_SUPPORTED_FEATURE_CONSTELLATION_ENABLEMENT_V02)) {
                LOC_LOGe("Feature not supported.");
            } else {
                // Reset constellation config
                mAdapter->gnssSetSvTypeConfig({sizeof(GnssSvTypeConfig), 0, 0});
                // Re-enforce SV blacklist config
                LocationError err = mAdapter->gnssSvIdConfigUpdate();
                if (err != LOCATION_ERROR_SUCCESS) {
                    LOC_LOGe("SV Config request failed, err %d", err);
                }
                // Send reset request to modem
                err = mApi->resetConstellationControl();
                if (err != LOCATION_ERROR_SUCCESS) {
                    LOC_LOGe("Reset constellation request failed, err %d", err);
                }
            }
        }
    };

    sendMsg(new MsgGnssResetSvTypeConfig(this, mLocApi));
}

void GnssAdapter::reportGnssSvTypeConfigEvent(const GnssSvTypeConfig& config)
{
    struct MsgReportGnssSvTypeConfig : public LocMsg {
        GnssAdapter& mAdapter;
        const GnssSvTypeConfig mConfig;
        inline MsgReportGnssSvTypeConfig(GnssAdapter& adapter,
                                 const GnssSvTypeConfig& config) :
            LocMsg(),
            mAdapter(adapter),
            mConfig(config) {}
        inline virtual void proc() const {
            mAdapter.reportGnssSvTypeConfig(mConfig);
        }
    };

    sendMsg(new MsgReportGnssSvTypeConfig(*this, config));
}

void GnssAdapter::reportGnssSvTypeConfig(const GnssSvTypeConfig& config)
{
    // Invoke Get SV Type Callback
    if (NULL != mGnssSvTypeConfigCb &&
            config.size == sizeof(GnssSvTypeConfig)) {
        LOC_LOGd("constellations blacklisted 0x%" PRIx64 ", enabled 0x%" PRIx64,
                 config.blacklistedSvTypesMask, config.enabledSvTypesMask);
        mGnssSvTypeConfigCb(config);
    } else {
        LOC_LOGe("Failed to report, size %d", (uint32_t)config.size);
    }
}

uint32_t
GnssAdapter::gnssDeleteAidingDataCommand(GnssAidingData& data)
{
    uint32_t sessionId = generateSessionId();
    LOC_LOGD("%s]: id %u", __func__, sessionId);

    struct MsgDeleteAidingData : public LocMsg {
        GnssAdapter& mAdapter;
        LocApiBase& mApi;
        uint32_t mSessionId;
        GnssAidingData mData;
        inline MsgDeleteAidingData(GnssAdapter& adapter,
                                   LocApiBase& api,
                                   uint32_t sessionId,
                                   GnssAidingData& data) :
            LocMsg(),
            mAdapter(adapter),
            mApi(api),
            mSessionId(sessionId),
            mData(data) {}
        inline virtual void proc() const {
            LocationError err = LOCATION_ERROR_SUCCESS;
            err = mApi.deleteAidingData(mData);
            mAdapter.reportResponse(err, mSessionId);
            SystemStatus* s = mAdapter.getSystemStatus();
            if ((nullptr != s) && (mData.deleteAll)) {
                s->setDefaultGnssEngineStates();
            }
        }
    };

    sendMsg(new MsgDeleteAidingData(*this, *mLocApi, sessionId, data));
    return sessionId;
}

void
GnssAdapter::gnssUpdateXtraThrottleCommand(const bool enabled)
{
    LOC_LOGD("%s] enabled:%d", __func__, enabled);

    struct UpdateXtraThrottleMsg : public LocMsg {
        GnssAdapter& mAdapter;
        const bool mEnabled;
        inline UpdateXtraThrottleMsg(GnssAdapter& adapter, const bool enabled) :
            LocMsg(),
            mAdapter(adapter),
            mEnabled(enabled) {}
        inline virtual void proc() const {
                mAdapter.mXtraObserver.updateXtraThrottle(mEnabled);
        }
    };

    sendMsg(new UpdateXtraThrottleMsg(*this, enabled));
}

void
GnssAdapter::injectLocationCommand(double latitude, double longitude, float accuracy)
{
    LOC_LOGD("%s]: latitude %8.4f longitude %8.4f accuracy %8.4f",
             __func__, latitude, longitude, accuracy);

    struct MsgInjectLocation : public LocMsg {
        LocApiBase& mApi;
        ContextBase& mContext;
        double mLatitude;
        double mLongitude;
        float mAccuracy;
        inline MsgInjectLocation(LocApiBase& api,
                                 ContextBase& context,
                                 double latitude,
                                 double longitude,
                                 float accuracy) :
            LocMsg(),
            mApi(api),
            mContext(context),
            mLatitude(latitude),
            mLongitude(longitude),
            mAccuracy(accuracy) {}
        inline virtual void proc() const {
            if (!mContext.hasCPIExtendedCapabilities()) {
                mApi.injectPosition(mLatitude, mLongitude, mAccuracy);
            }
        }
    };

    sendMsg(new MsgInjectLocation(*mLocApi, *mContext, latitude, longitude, accuracy));
}

void
GnssAdapter::injectTimeCommand(int64_t time, int64_t timeReference, int32_t uncertainty)
{
    LOC_LOGD("%s]: time %lld timeReference %lld uncertainty %d",
             __func__, (long long)time, (long long)timeReference, uncertainty);

    struct MsgInjectTime : public LocMsg {
        LocApiBase& mApi;
        ContextBase& mContext;
        int64_t mTime;
        int64_t mTimeReference;
        int32_t mUncertainty;
        inline MsgInjectTime(LocApiBase& api,
                             ContextBase& context,
                             int64_t time,
                             int64_t timeReference,
                             int32_t uncertainty) :
            LocMsg(),
            mApi(api),
            mContext(context),
            mTime(time),
            mTimeReference(timeReference),
            mUncertainty(uncertainty) {}
        inline virtual void proc() const {
            mApi.setTime(mTime, mTimeReference, mUncertainty);
        }
    };

    sendMsg(new MsgInjectTime(*mLocApi, *mContext, time, timeReference, uncertainty));
}

void
GnssAdapter::setUlpProxyCommand(UlpProxyBase* ulp)
{
    LOC_LOGD("%s]: ", __func__);

    struct MsgSetUlpProxy : public LocMsg {
        GnssAdapter& mAdapter;
        UlpProxyBase* mUlp;
        inline MsgSetUlpProxy(GnssAdapter& adapter,
                              UlpProxyBase* ulp) :
            LocMsg(),
            mAdapter(adapter),
            mUlp(ulp) {}
        inline virtual void proc() const {
            mAdapter.setUlpProxy(mUlp);
            if (mUlp) {
                mUlp->setCapabilities(ContextBase::getCarrierCapabilities());
            }
        }
    };

    sendMsg(new MsgSetUlpProxy(*this, ulp));
}

void
GnssAdapter::setUlpProxy(UlpProxyBase* ulp)
{
    if (ulp == mUlpProxy) {
        //This takes care of the case when double initalization happens
        //and we get the same object back for UlpProxyBase . Do nothing
        return;
    }

    LOC_LOGV("%s]: %p", __func__, ulp);
    if (NULL == ulp) {
        LOC_LOGE("%s]: ulp pointer is NULL", __func__);
        ulp = new UlpProxyBase();
    }

    if (LOC_POSITION_MODE_INVALID != mUlpProxy->mPosMode.mode) {
        // need to send this mode and start msg to ULP
        ulp->sendFixMode(mUlpProxy->mPosMode);
    }

    if (mUlpProxy->mFixSet) {
        ulp->sendStartFix();
    }

    delete mUlpProxy;
    mUlpProxy = ulp;
}

void
GnssAdapter::addClientCommand(LocationAPI* client, const LocationCallbacks& callbacks)
{
    LOC_LOGD("%s]: client %p", __func__, client);

    struct MsgAddClient : public LocMsg {
        GnssAdapter& mAdapter;
        LocationAPI* mClient;
        const LocationCallbacks mCallbacks;
        inline MsgAddClient(GnssAdapter& adapter,
                            LocationAPI* client,
                            const LocationCallbacks& callbacks) :
            LocMsg(),
            mAdapter(adapter),
            mClient(client),
            mCallbacks(callbacks) {}
        inline virtual void proc() const {
            mAdapter.saveClient(mClient, mCallbacks);
        }
    };

    sendMsg(new MsgAddClient(*this, client, callbacks));
}

void
GnssAdapter::removeClientCommand(LocationAPI* client)
{
    LOC_LOGD("%s]: client %p", __func__, client);

    struct MsgRemoveClient : public LocMsg {
        GnssAdapter& mAdapter;
        LocationAPI* mClient;
        inline MsgRemoveClient(GnssAdapter& adapter,
                               LocationAPI* client) :
            LocMsg(),
            mAdapter(adapter),
            mClient(client) {}
        inline virtual void proc() const {
            mAdapter.stopClientSessions(mClient);
            mAdapter.eraseClient(mClient);
        }
    };

    sendMsg(new MsgRemoveClient(*this, client));
}

void
GnssAdapter::stopClientSessions(LocationAPI* client)
{
    LOC_LOGD("%s]: client %p", __func__, client);
    for (auto it = mTrackingSessions.begin(); it != mTrackingSessions.end();) {
        if (client == it->first.client) {
            LocationError err = stopTrackingMultiplex(it->first.client, it->first.id);
            if (LOCATION_ERROR_SUCCESS == err) {
                it = mTrackingSessions.erase(it);
                continue;
            }
        }
        ++it; // increment only when not erasing an iterator
    }
}

void
GnssAdapter::updateClientsEventMask()
{
    LOC_API_ADAPTER_EVENT_MASK_T mask = 0;
    for (auto it=mClientData.begin(); it != mClientData.end(); ++it) {
        if (it->second.trackingCb != nullptr || it->second.gnssLocationInfoCb != nullptr) {
            mask |= LOC_API_ADAPTER_BIT_PARSED_POSITION_REPORT;
        }
        if (it->second.gnssNiCb != nullptr) {
            mask |= LOC_API_ADAPTER_BIT_NI_NOTIFY_VERIFY_REQUEST;
        }
        if (it->second.gnssSvCb != nullptr) {
            mask |= LOC_API_ADAPTER_BIT_SATELLITE_REPORT;
        }
        if ((it->second.gnssNmeaCb != nullptr) && (mNmeaMask)) {
            mask |= LOC_API_ADAPTER_BIT_NMEA_1HZ_REPORT;
        }
        if (it->second.gnssMeasurementsCb != nullptr) {
            mask |= LOC_API_ADAPTER_BIT_GNSS_MEASUREMENT;
        }
    }

    /*
    ** For Automotive use cases we need to enable MEASUREMENT and POLY
    ** when QDR is enabled
    */
    if (1 == ContextBase::mGps_conf.EXTERNAL_DR_ENABLED) {
        mask |= LOC_API_ADAPTER_BIT_GNSS_MEASUREMENT;
        mask |= LOC_API_ADAPTER_BIT_GNSS_SV_POLYNOMIAL_REPORT;

        LOC_LOGD("%s]: Auto usecase, Enable MEAS/POLY - mask 0x%x", __func__, mask);
    }

    if (mAgpsCbInfo.statusV4Cb != NULL) {
        mask |= LOC_API_ADAPTER_BIT_LOCATION_SERVER_REQUEST;
    }

    // Add ODCPI handling
    if (nullptr != mOdcpiRequestCb) {
        mask |= LOC_API_ADAPTER_BIT_REQUEST_WIFI;
    }

    updateEvtMask(mask, LOC_REGISTRATION_MASK_SET);
}

void
GnssAdapter::handleEngineUpEvent()
{
    struct MsgRestartSessions : public LocMsg {
        GnssAdapter& mAdapter;
        inline MsgRestartSessions(GnssAdapter& adapter) :
            LocMsg(),
            mAdapter(adapter) {}
        virtual void proc() const {
            mAdapter.restartSessions();
            mAdapter.gnssSvIdConfigUpdate();
            mAdapter.gnssSvTypeConfigUpdate();
        }
    };

    setConfigCommand();
    sendMsg(new MsgRestartSessions(*this));
}

void
GnssAdapter::restartSessions()
{
    LOC_LOGD("%s]: ", __func__);

    // odcpi session is no longer active after restart
    mOdcpiRequestActive = false;

    if (mTrackingSessions.empty()) {
        return;
    }

    // get the LocationOptions that has the smallest interval, which should be the active one
    TrackingOptions smallestIntervalOptions = {}; // size is zero until set for the first time
    TrackingOptions highestPowerTrackingOptions = {};
    for (auto it = mTrackingSessions.begin(); it != mTrackingSessions.end(); ++it) {
        // size of zero means we havent set it yet
        if (0 == smallestIntervalOptions.size ||
            it->second.minInterval < smallestIntervalOptions.minInterval) {
             smallestIntervalOptions = it->second;
        }
        GnssPowerMode powerMode = it->second.powerMode;
        // Size of zero means we havent set it yet
        if (0 == highestPowerTrackingOptions.size ||
            (GNSS_POWER_MODE_INVALID != powerMode &&
                    powerMode < highestPowerTrackingOptions.powerMode)) {
             highestPowerTrackingOptions = it->second;
        }
    }

    LocPosMode locPosMode = {};
    highestPowerTrackingOptions.setLocationOptions(smallestIntervalOptions);
    convertOptions(locPosMode, highestPowerTrackingOptions);
    mLocApi->startFix(locPosMode);
}

void
GnssAdapter::requestCapabilitiesCommand(LocationAPI* client)
{
    LOC_LOGD("%s]: ", __func__);

    struct MsgRequestCapabilities : public LocMsg {
        GnssAdapter& mAdapter;
        LocationAPI* mClient;
        inline MsgRequestCapabilities(GnssAdapter& adapter,
                                      LocationAPI* client) :
            LocMsg(),
            mAdapter(adapter),
            mClient(client) {}
        inline virtual void proc() const {
            LocationCallbacks callbacks = mAdapter.getClientCallbacks(mClient);
            if (callbacks.capabilitiesCb == nullptr) {
                LOC_LOGE("%s]: capabilitiesCb is NULL", __func__);
                return;
            }

            LocationCapabilitiesMask mask = mAdapter.getCapabilities();
            callbacks.capabilitiesCb(mask);
        }
    };

    sendMsg(new MsgRequestCapabilities(*this, client));
}

LocationCapabilitiesMask
GnssAdapter::getCapabilities()
{
    LocationCapabilitiesMask mask = 0;
    uint32_t carrierCapabilities = ContextBase::getCarrierCapabilities();
    // time based tracking always supported
    mask |= LOCATION_CAPABILITIES_TIME_BASED_TRACKING_BIT;
    // geofence always supported
    mask |= LOCATION_CAPABILITIES_GEOFENCE_BIT;
    if (carrierCapabilities & LOC_GPS_CAPABILITY_MSB) {
        mask |= LOCATION_CAPABILITIES_GNSS_MSB_BIT;
    }
    if (LOC_GPS_CAPABILITY_MSA & carrierCapabilities) {
        mask |= LOCATION_CAPABILITIES_GNSS_MSA_BIT;
    }
    if (mLocApi == nullptr)
        return mask;
    if (mLocApi->isMessageSupported(LOC_API_ADAPTER_MESSAGE_DISTANCE_BASE_LOCATION_BATCHING)) {
        mask |= LOCATION_CAPABILITIES_TIME_BASED_BATCHING_BIT |
                LOCATION_CAPABILITIES_DISTANCE_BASED_BATCHING_BIT;
    }
    if (mLocApi->isMessageSupported(LOC_API_ADAPTER_MESSAGE_DISTANCE_BASE_TRACKING)) {
        mask |= LOCATION_CAPABILITIES_DISTANCE_BASED_TRACKING_BIT;
    }
    if (mLocApi->isMessageSupported(LOC_API_ADAPTER_MESSAGE_OUTDOOR_TRIP_BATCHING)) {
        mask |= LOCATION_CAPABILITIES_OUTDOOR_TRIP_BATCHING_BIT;
    }
    if (mLocApi->gnssConstellationConfig()) {
        mask |= LOCATION_CAPABILITIES_GNSS_MEASUREMENTS_BIT;
    }
    if (mLocApi->isFeatureSupported(LOC_SUPPORTED_FEATURE_DEBUG_NMEA_V02)) {
        mask |= LOCATION_CAPABILITIES_DEBUG_NMEA_BIT;
    }
    if (mLocApi->isFeatureSupported(LOC_SUPPORTED_FEATURE_CONSTELLATION_ENABLEMENT_V02)) {
        mask |= LOCATION_CAPABILITIES_CONSTELLATION_ENABLEMENT_BIT;
    }
    if (mLocApi->isFeatureSupported(LOC_SUPPORTED_FEATURE_AGPM_V02)) {
        mask |= LOCATION_CAPABILITIES_AGPM_BIT;
    }
    return mask;
}

void
GnssAdapter::broadcastCapabilities(LocationCapabilitiesMask mask)
{
    for (auto it = mClientData.begin(); it != mClientData.end(); ++it) {
        if (nullptr != it->second.capabilitiesCb) {
            it->second.capabilitiesCb(mask);
        }
    }
}

LocationCallbacks
GnssAdapter::getClientCallbacks(LocationAPI* client)
{
    LocationCallbacks callbacks = {};
    auto it = mClientData.find(client);
    if (it != mClientData.end()) {
        callbacks = it->second;
    }
    return callbacks;
}

void
GnssAdapter::saveClient(LocationAPI* client, const LocationCallbacks& callbacks)
{
    mClientData[client] = callbacks;
    updateClientsEventMask();
}

void
GnssAdapter::eraseClient(LocationAPI* client)
{
    auto it = mClientData.find(client);
    if (it != mClientData.end()) {
        mClientData.erase(it);
    }
    updateClientsEventMask();
}

bool
GnssAdapter::hasTrackingCallback(LocationAPI* client)
{
    auto it = mClientData.find(client);
    return (it != mClientData.end() && (it->second.trackingCb || it->second.gnssLocationInfoCb));
}

bool
GnssAdapter::hasMeasurementsCallback(LocationAPI* client)
{
    auto it = mClientData.find(client);
    return (it != mClientData.end() && it->second.gnssMeasurementsCb);
}

bool
GnssAdapter::isTrackingSession(LocationAPI* client, uint32_t sessionId)
{
    LocationSessionKey key(client, sessionId);
    return (mTrackingSessions.find(key) != mTrackingSessions.end());
}

void
GnssAdapter::saveTrackingSession(LocationAPI* client, uint32_t sessionId,
                                 const TrackingOptions& trackingOptions)
{
    LocationSessionKey key(client, sessionId);
    mTrackingSessions[key] = trackingOptions;
}

void
GnssAdapter::eraseTrackingSession(LocationAPI* client, uint32_t sessionId)
{
    LocationSessionKey key(client, sessionId);
    auto itr = mTrackingSessions.find(key);
    if (itr != mTrackingSessions.end()) {
        mTrackingSessions.erase(itr);
    }
}

bool GnssAdapter::setUlpPositionMode(const LocPosMode& mode) {
    if (!mUlpPositionMode.equals(mode)) {
        mUlpPositionMode = mode;
        return true;
    } else {
        return false;
    }
}

void
GnssAdapter::reportResponse(LocationAPI* client, LocationError err, uint32_t sessionId)
{
    LOC_LOGD("%s]: client %p id %u err %u", __func__, client, sessionId, err);

    auto it = mClientData.find(client);
    if (it != mClientData.end() &&
        it->second.responseCb != nullptr) {
        it->second.responseCb(err, sessionId);
    } else {
        LOC_LOGW("%s]: client %p id %u not found in data", __func__, client, sessionId);
    }
}

void
GnssAdapter::reportResponse(LocationError err, uint32_t sessionId)
{
    LOC_LOGD("%s]: id %u err %u", __func__, sessionId, err);

    if (mControlCallbacks.size > 0 && mControlCallbacks.responseCb != nullptr) {
        mControlCallbacks.responseCb(err, sessionId);
    } else {
        LOC_LOGW("%s]: control client response callback not found", __func__);
    }
}

void
GnssAdapter::reportResponse(size_t count, LocationError* errs, uint32_t* ids)
{
    IF_LOC_LOGD {
        std::string idsString = "[";
        std::string errsString = "[";
        if (NULL != ids && NULL != errs) {
            for (size_t i=0; i < count; ++i) {
                idsString += std::to_string(ids[i]) + " ";
                errsString += std::to_string(errs[i]) + " ";
            }
        }
        idsString += "]";
        errsString += "]";

        LOC_LOGD("%s]: ids %s errs %s",
                 __func__, idsString.c_str(), errsString.c_str());
    }

    if (mControlCallbacks.size > 0 && mControlCallbacks.collectiveResponseCb != nullptr) {
        mControlCallbacks.collectiveResponseCb(count, errs, ids);
    } else {
        LOC_LOGW("%s]: control client callback not found", __func__);
    }
}

uint32_t
GnssAdapter::startTrackingCommand(LocationAPI* client, TrackingOptions& options)
{
    uint32_t sessionId = generateSessionId();
    LOC_LOGD("%s]: client %p id %u minInterval %u minDistance %u mode %u powermode %u tbm %u",
             __func__, client, sessionId, options.minInterval, options.minDistance, options.mode,
             options.powerMode, options.tbm);

    struct MsgStartTracking : public LocMsg {
        GnssAdapter& mAdapter;
        LocApiBase& mApi;
        LocationAPI* mClient;
        uint32_t mSessionId;
        mutable TrackingOptions mTrackingOptions;
        inline MsgStartTracking(GnssAdapter& adapter,
                               LocApiBase& api,
                               LocationAPI* client,
                               uint32_t sessionId,
                               TrackingOptions trackingOptions) :
            LocMsg(),
            mAdapter(adapter),
            mApi(api),
            mClient(client),
            mSessionId(sessionId),
            mTrackingOptions(trackingOptions) {}
        inline virtual void proc() const {
            LocationError err = LOCATION_ERROR_SUCCESS;
            if (!mAdapter.hasTrackingCallback(mClient) &&
                !mAdapter.hasMeasurementsCallback(mClient)) {
                err = LOCATION_ERROR_CALLBACK_MISSING;
            } else if (0 == mTrackingOptions.size) {
                err = LOCATION_ERROR_INVALID_PARAMETER;
            } else {
                if (GNSS_POWER_MODE_INVALID != mTrackingOptions.powerMode &&
                        !mApi.isFeatureSupported(LOC_SUPPORTED_FEATURE_AGPM_V02)) {
                    LOC_LOGv("Ignoring power mode, feature not supported.");
                    mTrackingOptions.powerMode = GNSS_POWER_MODE_INVALID;
                }
                if (mApi.isFeatureSupported(LOC_SUPPORTED_FEATURE_AGPM_V02) &&
                        GNSS_POWER_MODE_M4 == mTrackingOptions.powerMode &&
                        mTrackingOptions.tbm > TRACKING_TBM_THRESHOLD_MILLIS) {
                    LOC_LOGd("TBM (%d) > %d Falling back to M2 power mode",
                            mTrackingOptions.tbm, TRACKING_TBM_THRESHOLD_MILLIS);
                    mTrackingOptions.powerMode = GNSS_POWER_MODE_M2;
                }
                if (mTrackingOptions.minInterval < MIN_TRACKING_INTERVAL) {
                    mTrackingOptions.minInterval = MIN_TRACKING_INTERVAL;
                }
                // Api doesn't support multiple clients for time based tracking, so mutiplex
                err = mAdapter.startTrackingMultiplex(mTrackingOptions);
                if (LOCATION_ERROR_SUCCESS == err) {
                    mAdapter.saveTrackingSession(
                            mClient, mSessionId, mTrackingOptions);
                }
            }
            mAdapter.reportResponse(mClient, err, mSessionId);
        }
    };

    sendMsg(new MsgStartTracking(
            *this, *mLocApi, client, sessionId, options));
    return sessionId;
}

LocationError
GnssAdapter::startTrackingMultiplex(const TrackingOptions& options)
{
    LocationError err = LOCATION_ERROR_SUCCESS;

    if (mTrackingSessions.empty()) {
        err = startTracking(options);
    } else {
        // find the smallest interval and powerMode
        TrackingOptions multiplexedOptions = {}; // size is 0 until set for the first time
        GnssPowerMode multiplexedPowerMode = GNSS_POWER_MODE_INVALID;
        for (auto it = mTrackingSessions.begin(); it != mTrackingSessions.end(); ++it) {
            // if not set or there is a new smallest interval, then set the new interval
            if (0 == multiplexedOptions.size ||
                it->second.minInterval < multiplexedOptions.minInterval) {
                multiplexedOptions = it->second;
            }
            // if session is not the one we are updating and either powerMode
            // is not set or there is a new smallest powerMode, then set the new powerMode
            if (GNSS_POWER_MODE_INVALID == multiplexedPowerMode ||
                it->second.powerMode < multiplexedPowerMode) {
                multiplexedPowerMode = it->second.powerMode;
            }
        }
        bool updateOptions = false;
        // if session we are starting has smaller interval then next smallest
        if (options.minInterval < multiplexedOptions.minInterval) {
            multiplexedOptions.minInterval = options.minInterval;
            updateOptions = true;
        }
        // if session we are starting has smaller powerMode then next smallest
        if (options.powerMode < multiplexedPowerMode) {
            multiplexedOptions.powerMode = options.powerMode;
            updateOptions = true;
        }
        if (updateOptions) {
            // restart time based tracking with the newly updated options
            err = startTracking(multiplexedOptions);
        }
    }

    return err;
}

LocationError
GnssAdapter::startTracking(const TrackingOptions& trackingOptions)
{
    LOC_LOGd("minInterval %u minDistance %u mode %u powermode %u tbm %u",
         trackingOptions.minInterval, trackingOptions.minDistance,
         trackingOptions.mode, trackingOptions.powerMode, trackingOptions.tbm);

    LocationError err = LOCATION_ERROR_SUCCESS;
    LocPosMode locPosMode = {};
    convertOptions(locPosMode, trackingOptions);
    if (!mUlpProxy->sendFixMode(locPosMode)) {
        // do nothing
    }
    if (!mUlpProxy->sendStartFix()) {
        loc_api_adapter_err apiErr = mLocApi->startFix(locPosMode);
        if (LOC_API_ADAPTER_ERR_SUCCESS == apiErr) {
            err = LOCATION_ERROR_SUCCESS;
        } else {
            err = LOCATION_ERROR_GENERAL_FAILURE;
        }
    }

    return err;
}

void
GnssAdapter::setPositionModeCommand(LocPosMode& locPosMode)
{
    LOC_LOGD("%s]: min_interval %u mode %u",
             __func__, locPosMode.min_interval, locPosMode.mode);

    struct MsgSetPositionMode : public LocMsg {
        GnssAdapter& mAdapter;
        LocApiBase& mApi;
        LocPosMode mLocPosMode;
        inline MsgSetPositionMode(GnssAdapter& adapter,
                                  LocApiBase& api,
                                  LocPosMode& locPosMode) :
            LocMsg(),
            mAdapter(adapter),
            mApi(api),
            mLocPosMode(locPosMode) {}
        inline virtual void proc() const {
             // saves the mode in adapter to be used when startTrackingCommand is called from ULP
            if (mAdapter.setUlpPositionMode(mLocPosMode)) {
                mApi.setPositionMode(mLocPosMode);
            }
        }
    };

    sendMsg(new MsgSetPositionMode(*this, *mLocApi, locPosMode));
}

void
GnssAdapter::startTrackingCommand()
{
    LOC_LOGD("%s]: ", __func__);

    struct MsgStartTracking : public LocMsg {
        GnssAdapter& mAdapter;
        LocApiBase& mApi;
        inline MsgStartTracking(GnssAdapter& adapter,
                                LocApiBase& api) :
            LocMsg(),
            mAdapter(adapter),
            mApi(api) {}
        inline virtual void proc() const {
            // we get this call from ULP, so just call LocApi without multiplexing because
            // ulp would be doing the multiplexing for us if it is present
            if (!mAdapter.isInSession()) {
                LocPosMode& ulpPositionMode = mAdapter.getUlpPositionMode();
                mApi.startFix(ulpPositionMode);
            }
        }
    };

    sendMsg(new MsgStartTracking(*this, *mLocApi));
}

void
GnssAdapter::updateTrackingOptionsCommand(LocationAPI* client, uint32_t id,
                                          TrackingOptions& options)
{
    LOC_LOGD("%s]: client %p id %u minInterval %u mode %u",
             __func__, client, id, options.minInterval, options.mode);

    struct MsgUpdateTracking : public LocMsg {
        GnssAdapter& mAdapter;
        LocApiBase& mApi;
        LocationAPI* mClient;
        uint32_t mSessionId;
        mutable TrackingOptions mTrackingOptions;
        inline MsgUpdateTracking(GnssAdapter& adapter,
                                LocApiBase& api,
                                LocationAPI* client,
                                uint32_t sessionId,
                                TrackingOptions trackingOptions) :
            LocMsg(),
            mAdapter(adapter),
            mApi(api),
            mClient(client),
            mSessionId(sessionId),
            mTrackingOptions(trackingOptions) {}
        inline virtual void proc() const {
            if (mAdapter.isTrackingSession(mClient, mSessionId)) {
                LocationError err = LOCATION_ERROR_SUCCESS;
                if (0 == mTrackingOptions.size) {
                    err = LOCATION_ERROR_INVALID_PARAMETER;
                } else {
                    if (GNSS_POWER_MODE_INVALID != mTrackingOptions.powerMode &&
                            !mApi.isFeatureSupported(LOC_SUPPORTED_FEATURE_AGPM_V02)) {
                        LOC_LOGv("Ignoring power mode, feature not supported.");
                        mTrackingOptions.powerMode = GNSS_POWER_MODE_INVALID;
                    }
                    if (mApi.isFeatureSupported(LOC_SUPPORTED_FEATURE_AGPM_V02) &&
                            GNSS_POWER_MODE_M4 == mTrackingOptions.powerMode &&
                            mTrackingOptions.tbm > TRACKING_TBM_THRESHOLD_MILLIS) {
                        LOC_LOGd("TBM (%d) > %d Falling back to M2 power mode",
                                mTrackingOptions.tbm, TRACKING_TBM_THRESHOLD_MILLIS);
                        mTrackingOptions.powerMode = GNSS_POWER_MODE_M2;
                    }
                    if (mTrackingOptions.minInterval < MIN_TRACKING_INTERVAL) {
                        mTrackingOptions.minInterval = MIN_TRACKING_INTERVAL;
                    }
                    // Api doesn't support multiple clients for time based tracking, so mutiplex
                    err = mAdapter.updateTrackingMultiplex(
                            mClient, mSessionId, mTrackingOptions);
                    if (LOCATION_ERROR_SUCCESS == err) {
                        mAdapter.saveTrackingSession(
                                mClient, mSessionId, mTrackingOptions);
                    }
                }
                mAdapter.reportResponse(mClient, err, mSessionId);
            }
            // we do not reportResponse for the case where there is no existing tracking session
            // for the client and id being used, since updateTrackingCommand can be sent to both
            // GnssAdapter & FlpAdapter by LocationAPI and we want to avoid incorrect error response
        }
    };

    sendMsg(new MsgUpdateTracking(*this, *mLocApi, client, id, options));
}

LocationError
GnssAdapter::updateTrackingMultiplex(LocationAPI* client, uint32_t id,
                                     const TrackingOptions& trackingOptions)
{
    LocationError err = LOCATION_ERROR_SUCCESS;

    LocationSessionKey key(client, id);

    // get the session we are updating
    auto it = mTrackingSessions.find(key);
    // if session we are updating exists and the minInterval or powerMode has changed
    if (it != mTrackingSessions.end() && (it->second.minInterval != trackingOptions.minInterval ||
        it->second.powerMode != trackingOptions.powerMode)) {
        // find the smallest interval and powerMode, other than the session we are updating
        TrackingOptions multiplexedOptions = {}; // size is 0 until set for the first time
        GnssPowerMode multiplexedPowerMode = GNSS_POWER_MODE_INVALID;
        for (auto it2 = mTrackingSessions.begin(); it2 != mTrackingSessions.end(); ++it2) {
            // if session is not the one we are updating and either interval
            // is not set or there is a new smallest interval, then set the new interval
            if (it2->first != key && (0 == multiplexedOptions.size ||
                it2->second.minInterval < multiplexedOptions.minInterval)) {
                 multiplexedOptions = it2->second;
            }
            // if session is not the one we are updating and either powerMode
            // is not set or there is a new smallest powerMode, then set the new powerMode
            if (it2->first != key && (GNSS_POWER_MODE_INVALID == multiplexedPowerMode ||
                it2->second.powerMode < multiplexedPowerMode)) {
                multiplexedPowerMode = it2->second.powerMode;
            }
        }
        bool updateOptions = false;
        // if session we are updating has smaller interval then next smallest
        if (trackingOptions.minInterval < multiplexedOptions.minInterval) {
            multiplexedOptions.minInterval = trackingOptions.minInterval;
            updateOptions = true;
        }
        // if session we are updating has smaller powerMode then next smallest
        if (trackingOptions.powerMode < multiplexedPowerMode) {
            multiplexedOptions.powerMode = trackingOptions.powerMode;
            updateOptions = true;
        }
        // if only one session exists, then tracking should be updated with it
        if (1 == mTrackingSessions.size()) {
            multiplexedOptions = trackingOptions;
            updateOptions = true;
        }
        if (updateOptions) {
            // restart time based tracking with the newly updated options
            err = startTracking(multiplexedOptions);
        }
    }

    return err;
}

void
GnssAdapter::stopTrackingCommand(LocationAPI* client, uint32_t id)
{
    LOC_LOGD("%s]: client %p id %u", __func__, client, id);

    struct MsgStopTracking : public LocMsg {
        GnssAdapter& mAdapter;
        LocApiBase& mApi;
        LocationAPI* mClient;
        uint32_t mSessionId;
        inline MsgStopTracking(GnssAdapter& adapter,
                               LocApiBase& api,
                               LocationAPI* client,
                               uint32_t sessionId) :
            LocMsg(),
            mAdapter(adapter),
            mApi(api),
            mClient(client),
            mSessionId(sessionId) {}
        inline virtual void proc() const {
            if (mAdapter.isTrackingSession(mClient, mSessionId)) {
                LocationError err = LOCATION_ERROR_SUCCESS;
                // Api doesn't support multiple clients for time based tracking, so mutiplex
                err = mAdapter.stopTrackingMultiplex(mClient, mSessionId);
                if (LOCATION_ERROR_SUCCESS == err) {
                    mAdapter.eraseTrackingSession(mClient, mSessionId);
                }
                mAdapter.reportResponse(mClient, err, mSessionId);
            }
            // we do not reportResponse for the case where there is no existing tracking session
            // for the client and id being used, since stopTrackingCommand can be sent to both
            // GnssAdapter & FlpAdapter by LocationAPI and we want to avoid incorrect error response

        }
    };

    sendMsg(new MsgStopTracking(*this, *mLocApi, client, id));
}

LocationError
GnssAdapter::stopTrackingMultiplex(LocationAPI* client, uint32_t id)
{
    LocationError err = LOCATION_ERROR_SUCCESS;

    if (1 == mTrackingSessions.size()) {
        err = stopTracking();
    } else {
        LocationSessionKey key(client, id);

        // get the session we are stopping
        auto it = mTrackingSessions.find(key);
        if (it != mTrackingSessions.end()) {
            // find the smallest interval and powerMode, other than the session we are stopping
            TrackingOptions multiplexedOptions = {}; // size is 0 until set for the first time
            GnssPowerMode multiplexedPowerMode = GNSS_POWER_MODE_INVALID;
            for (auto it2 = mTrackingSessions.begin(); it2 != mTrackingSessions.end(); ++it2) {
                // if session is not the one we are stopping and either interval
                // is not set or there is a new smallest interval, then set the new interval
                if (it2->first != key && (0 == multiplexedOptions.size ||
                    it2->second.minInterval < multiplexedOptions.minInterval)) {
                     multiplexedOptions = it2->second;
                }
                // if session is not the one we are stopping and either powerMode
                // is not set or there is a new smallest powerMode, then set the new powerMode
                if (it2->first != key && (GNSS_POWER_MODE_INVALID == multiplexedPowerMode ||
                    it2->second.powerMode < multiplexedPowerMode)) {
                    multiplexedPowerMode = it2->second.powerMode;
                }
            }
            // if session we are stopping has smaller interval then next smallest or
            // if session we are stopping has smaller powerMode then next smallest
            if (it->second.minInterval < multiplexedOptions.minInterval ||
                it->second.powerMode < multiplexedPowerMode) {
                multiplexedOptions.powerMode = multiplexedPowerMode;
                // restart time based tracking with the newly updated options
                err = startTracking(multiplexedOptions);
            }
        }
    }

    return err;
}

LocationError
GnssAdapter::stopTracking()
{
    LocationError err = LOCATION_ERROR_SUCCESS;
    if (!mUlpProxy->sendStopFix()) {
        loc_api_adapter_err apiErr = mLocApi->stopFix();
        if (LOC_API_ADAPTER_ERR_SUCCESS == apiErr) {
            err = LOCATION_ERROR_SUCCESS;
        } else {
            err = LOCATION_ERROR_GENERAL_FAILURE;
        }
    }

    return err;
}

void
GnssAdapter::stopTrackingCommand()
{
    LOC_LOGD("%s]: ", __func__);

    struct MsgStopTracking : public LocMsg {
        GnssAdapter& mAdapter;
        LocApiBase& mApi;
        inline MsgStopTracking(GnssAdapter& adapter,
                               LocApiBase& api) :
            LocMsg(),
            mAdapter(adapter),
            mApi(api) {}
        inline virtual void proc() const {
            // clear the position mode
            LocPosMode mLocPosMode = {};
            mLocPosMode.mode = LOC_POSITION_MODE_INVALID;
            mAdapter.setUlpPositionMode(mLocPosMode);
            // don't need to multiplex because ULP will do that for us if it is present
            mApi.stopFix();
        }
    };

    sendMsg(new MsgStopTracking(*this, *mLocApi));
}

void
GnssAdapter::getZppCommand()
{
    LOC_LOGD("%s]: ", __func__);

    struct MsgGetZpp : public LocMsg {
        GnssAdapter& mAdapter;
        LocApiBase& mApi;
        inline MsgGetZpp(GnssAdapter& adapter,
                         LocApiBase& api) :
            LocMsg(),
            mAdapter(adapter),
            mApi(api) {}
        inline virtual void proc() const {
            UlpLocation location = {};
            LocPosTechMask techMask = LOC_POS_TECH_MASK_DEFAULT;
            GpsLocationExtended locationExtended = {};
            locationExtended.size = sizeof(locationExtended);

            mApi.getBestAvailableZppFix(location.gpsLocation, locationExtended,
                    techMask);
            //Mark the location source as from ZPP
            location.gpsLocation.flags |= LOCATION_HAS_SOURCE_INFO;
            location.position_source = ULP_LOCATION_IS_FROM_ZPP;

            mAdapter.getUlpProxy()->reportPosition(location,
                                                   locationExtended,
                                                   LOC_SESS_SUCCESS,
                                                   techMask);
        }
    };

    sendMsg(new MsgGetZpp(*this, *mLocApi));
}

bool
GnssAdapter::hasNiNotifyCallback(LocationAPI* client)
{
    auto it = mClientData.find(client);
    return (it != mClientData.end() && it->second.gnssNiCb);
}

void
GnssAdapter::gnssNiResponseCommand(LocationAPI* client,
                                   uint32_t id,
                                   GnssNiResponse response)
{
    LOC_LOGD("%s]: client %p id %u response %u", __func__, client, id, response);

    struct MsgGnssNiResponse : public LocMsg {
        GnssAdapter& mAdapter;
        LocationAPI* mClient;
        uint32_t mSessionId;
        GnssNiResponse mResponse;
        inline MsgGnssNiResponse(GnssAdapter& adapter,
                                 LocationAPI* client,
                                 uint32_t sessionId,
                                 GnssNiResponse response) :
            LocMsg(),
            mAdapter(adapter),
            mClient(client),
            mSessionId(sessionId),
            mResponse(response) {}
        inline virtual void proc() const {
            NiData& niData = mAdapter.getNiData();
            LocationError err = LOCATION_ERROR_SUCCESS;
            if (!mAdapter.hasNiNotifyCallback(mClient)) {
                err = LOCATION_ERROR_ID_UNKNOWN;
            } else {
                NiSession* pSession = NULL;
                if (mSessionId == niData.sessionEs.reqID &&
                    NULL != niData.sessionEs.rawRequest) {
                    pSession = &niData.sessionEs;
                    // ignore any SUPL NI non-Es session if a SUPL NI ES is accepted
                    if (mResponse == GNSS_NI_RESPONSE_ACCEPT &&
                        NULL != niData.session.rawRequest) {
                            pthread_mutex_lock(&niData.session.tLock);
                            niData.session.resp = GNSS_NI_RESPONSE_IGNORE;
                            niData.session.respRecvd = true;
                            pthread_cond_signal(&niData.session.tCond);
                            pthread_mutex_unlock(&niData.session.tLock);
                    }
                } else if (mSessionId == niData.session.reqID &&
                    NULL != niData.session.rawRequest) {
                    pSession = &niData.session;
                }

                if (pSession) {
                    LOC_LOGI("%s]: gnssNiResponseCommand: send user mResponse %u for id %u",
                             __func__, mResponse, mSessionId);
                    pthread_mutex_lock(&pSession->tLock);
                    pSession->resp = mResponse;
                    pSession->respRecvd = true;
                    pthread_cond_signal(&pSession->tCond);
                    pthread_mutex_unlock(&pSession->tLock);
                } else {
                    err = LOCATION_ERROR_ID_UNKNOWN;
                    LOC_LOGE("%s]: gnssNiResponseCommand: id %u not an active session",
                             __func__, mSessionId);
                }
            }
            mAdapter.reportResponse(mClient, err, mSessionId);
        }
    };

    sendMsg(new MsgGnssNiResponse(*this, client, id, response));

}

void
GnssAdapter::gnssNiResponseCommand(GnssNiResponse response, void* rawRequest)
{
    LOC_LOGD("%s]: response %u", __func__, response);

    struct MsgGnssNiResponse : public LocMsg {
        LocApiBase& mApi;
        const GnssNiResponse mResponse;
        const void* mPayload;
        inline MsgGnssNiResponse(LocApiBase& api,
                                 const GnssNiResponse response,
                                 const void* rawRequest) :
            LocMsg(),
            mApi(api),
            mResponse(response),
            mPayload(rawRequest) {}
        inline virtual ~MsgGnssNiResponse() {
            // this is a bit weird since mPayload is not
            // allocated by this class.  But there is no better way.
            // mPayload actually won't be NULL here.
            free((void*)mPayload);
        }
        inline virtual void proc() const {
            mApi.informNiResponse(mResponse, mPayload);
        }
    };

    sendMsg(new MsgGnssNiResponse(*mLocApi, response, rawRequest));

}

uint32_t
GnssAdapter::enableCommand(LocationTechnologyType techType)
{
    uint32_t sessionId = generateSessionId();
    LOC_LOGD("%s]: id %u techType %u", __func__, sessionId, techType);

    struct MsgEnableGnss : public LocMsg {
        GnssAdapter& mAdapter;
        LocApiBase& mApi;
        ContextBase& mContext;
        uint32_t mSessionId;
        LocationTechnologyType mTechType;
        inline MsgEnableGnss(GnssAdapter& adapter,
                             LocApiBase& api,
                             ContextBase& context,
                             uint32_t sessionId,
                             LocationTechnologyType techType) :
            LocMsg(),
            mAdapter(adapter),
            mApi(api),
            mContext(context),
            mSessionId(sessionId),
            mTechType(techType) {}
        inline virtual void proc() const {
            LocationError err = LOCATION_ERROR_SUCCESS;
            uint32_t powerVoteId = mAdapter.getPowerVoteId();
            if (mTechType != LOCATION_TECHNOLOGY_TYPE_GNSS) {
                err = LOCATION_ERROR_INVALID_PARAMETER;
            } else if (powerVoteId > 0) {
                err = LOCATION_ERROR_ALREADY_STARTED;
            } else {
                mContext.modemPowerVote(true);
                mAdapter.setPowerVoteId(mSessionId);
                mApi.setGpsLock(GNSS_CONFIG_GPS_LOCK_NONE);
                mAdapter.mXtraObserver.updateLockStatus(
                        mAdapter.convertGpsLock(GNSS_CONFIG_GPS_LOCK_NONE));
            }
            mAdapter.reportResponse(err, mSessionId);
        }
    };

    if (mContext != NULL) {
        sendMsg(new MsgEnableGnss(*this, *mLocApi, *mContext, sessionId, techType));
    } else {
        LOC_LOGE("%s]: Context is NULL", __func__);
    }

    return sessionId;
}

void
GnssAdapter::disableCommand(uint32_t id)
{
    LOC_LOGD("%s]: id %u", __func__, id);

    struct MsgDisableGnss : public LocMsg {
        GnssAdapter& mAdapter;
        LocApiBase& mApi;
        ContextBase& mContext;
        uint32_t mSessionId;
        inline MsgDisableGnss(GnssAdapter& adapter,
                             LocApiBase& api,
                             ContextBase& context,
                             uint32_t sessionId) :
            LocMsg(),
            mAdapter(adapter),
            mApi(api),
            mContext(context),
            mSessionId(sessionId) {}
        inline virtual void proc() const {
            LocationError err = LOCATION_ERROR_SUCCESS;
            uint32_t powerVoteId = mAdapter.getPowerVoteId();
            if (powerVoteId != mSessionId) {
                err = LOCATION_ERROR_ID_UNKNOWN;
            } else {
                mContext.modemPowerVote(false);
                mAdapter.setPowerVoteId(0);
                mApi.setGpsLock(mAdapter.convertGpsLock(ContextBase::mGps_conf.GPS_LOCK));
                mAdapter.mXtraObserver.updateLockStatus(
                        mAdapter.convertGpsLock(ContextBase::mGps_conf.GPS_LOCK));
            }
            mAdapter.reportResponse(err, mSessionId);
        }
    };

    if (mContext != NULL) {
        sendMsg(new MsgDisableGnss(*this, *mLocApi, *mContext, id));
    }

}

void
GnssAdapter::reportPositionEvent(const UlpLocation& ulpLocation,
                                 const GpsLocationExtended& locationExtended,
                                 enum loc_sess_status status,
                                 LocPosTechMask techMask,
                                 bool fromUlp)
{
    LOC_LOGD("%s]: fromUlp %u status %u", __func__, fromUlp, status);

    // if this event is not called from ULP, then try to call into ULP and return if successfull
    if (!fromUlp) {
        if (mUlpProxy->reportPosition(ulpLocation, locationExtended,
                                 status, techMask)) {
            return;
        }
    }

    struct MsgReportPosition : public LocMsg {
        GnssAdapter& mAdapter;
        const UlpLocation mUlpLocation;
        const GpsLocationExtended mLocationExtended;
        loc_sess_status mStatus;
        LocPosTechMask mTechMask;
        inline MsgReportPosition(GnssAdapter& adapter,
                                 const UlpLocation& ulpLocation,
                                 const GpsLocationExtended& locationExtended,
                                 loc_sess_status status,
                                 LocPosTechMask techMask) :
            LocMsg(),
            mAdapter(adapter),
            mUlpLocation(ulpLocation),
            mLocationExtended(locationExtended),
            mStatus(status),
            mTechMask(techMask) {}
        inline virtual void proc() const {
            // extract bug report info - this returns true if consumed by systemstatus
            SystemStatus* s = mAdapter.getSystemStatus();
            if ((nullptr != s) &&
                    ((LOC_SESS_SUCCESS == mStatus) || (LOC_SESS_INTERMEDIATE == mStatus))){
                s->eventPosition(mUlpLocation, mLocationExtended);
            }
            mAdapter.reportPosition(mUlpLocation, mLocationExtended, mStatus, mTechMask);
        }
    };

    sendMsg(new MsgReportPosition(*this, ulpLocation, locationExtended, status, techMask));
}

bool
GnssAdapter::needReport(const UlpLocation& ulpLocation,
                        enum loc_sess_status status,
                        LocPosTechMask techMask) {
    bool reported = false;

    reported = LocApiBase::needReport(ulpLocation, status, techMask);
    return reported;
}

void
GnssAdapter::reportPosition(const UlpLocation& ulpLocation,
                            const GpsLocationExtended& locationExtended,
                            enum loc_sess_status status,
                            LocPosTechMask techMask)
{
    bool reported = needReport(ulpLocation, status, techMask);
    mGnssSvIdUsedInPosAvail = false;
    if (reported) {
        if (locationExtended.flags & GPS_LOCATION_EXTENDED_HAS_GNSS_SV_USED_DATA) {
            mGnssSvIdUsedInPosAvail = true;
            mGnssSvIdUsedInPosition = locationExtended.gnss_sv_used_ids;
        }

        for (auto it=mClientData.begin(); it != mClientData.end(); ++it) {
            if (nullptr != it->second.gnssLocationInfoCb) {
                GnssLocationInfoNotification locationInfo = {};
                convertLocationInfo(locationInfo, locationExtended);
                convertLocation(locationInfo.location, ulpLocation, locationExtended, techMask);
                it->second.gnssLocationInfoCb(locationInfo);
            } else if (nullptr != it->second.trackingCb) {
                Location location = {};
                convertLocation(location, ulpLocation, locationExtended, techMask);
                it->second.trackingCb(location);
            }
        }
    }

    if (NMEA_PROVIDER_AP == ContextBase::mGps_conf.NMEA_PROVIDER && !mTrackingSessions.empty()) {
        /*Only BlankNMEA sentence needs to be processed and sent, if both lat, long is 0 &
          horReliability is not set. */
        bool blank_fix = ((0 == ulpLocation.gpsLocation.latitude) &&
                          (0 == ulpLocation.gpsLocation.longitude) &&
                          (LOC_RELIABILITY_NOT_SET == locationExtended.horizontal_reliability));
        uint8_t generate_nmea = (reported && status != LOC_SESS_FAILURE && !blank_fix);
        std::vector<std::string> nmeaArraystr;
        loc_nmea_generate_pos(ulpLocation, locationExtended, generate_nmea, nmeaArraystr);
        for (auto sentence : nmeaArraystr) {
            reportNmea(sentence.c_str(), sentence.length());
        }
    }
}

void
GnssAdapter::reportSvEvent(const GnssSvNotification& svNotify,
                           bool fromUlp)
{
    LOC_LOGD("%s]: fromUlp %u", __func__, fromUlp);

    // if this event is not called from ULP, then try to call into ULP and return if successfull
    if (!fromUlp) {
        if (mUlpProxy->reportSv(svNotify)) {
            return;
        }
    }

    struct MsgReportSv : public LocMsg {
        GnssAdapter& mAdapter;
        const GnssSvNotification mSvNotify;
        inline MsgReportSv(GnssAdapter& adapter,
                           const GnssSvNotification& svNotify) :
            LocMsg(),
            mAdapter(adapter),
            mSvNotify(svNotify) {}
        inline virtual void proc() const {
            mAdapter.reportSv((GnssSvNotification&)mSvNotify);
        }
    };

    sendMsg(new MsgReportSv(*this, svNotify));
}

void
GnssAdapter::reportSv(GnssSvNotification& svNotify)
{
    int numSv = svNotify.count;
    int16_t gnssSvId = 0;
    uint64_t svUsedIdMask = 0;
    for (int i=0; i < numSv; i++) {
        svUsedIdMask = 0;
        gnssSvId = svNotify.gnssSvs[i].svId;
        switch (svNotify.gnssSvs[i].type) {
            case GNSS_SV_TYPE_GPS:
                if (mGnssSvIdUsedInPosAvail) {
                    svUsedIdMask = mGnssSvIdUsedInPosition.gps_sv_used_ids_mask;
                }
                break;
            case GNSS_SV_TYPE_GLONASS:
                if (mGnssSvIdUsedInPosAvail) {
                    svUsedIdMask = mGnssSvIdUsedInPosition.glo_sv_used_ids_mask;
                }
                break;
            case GNSS_SV_TYPE_BEIDOU:
                if (mGnssSvIdUsedInPosAvail) {
                    svUsedIdMask = mGnssSvIdUsedInPosition.bds_sv_used_ids_mask;
                }
                break;
            case GNSS_SV_TYPE_GALILEO:
                if (mGnssSvIdUsedInPosAvail) {
                    svUsedIdMask = mGnssSvIdUsedInPosition.gal_sv_used_ids_mask;
                }
                break;
            case GNSS_SV_TYPE_QZSS:
                if (mGnssSvIdUsedInPosAvail) {
                    svUsedIdMask = mGnssSvIdUsedInPosition.qzss_sv_used_ids_mask;
                }
                // QZSS SV id's need to reported as it is to framework, since
                // framework expects it as it is. See GnssStatus.java.
                // SV id passed to here by LocApi is 1-based.
                svNotify.gnssSvs[i].svId += (QZSS_SV_PRN_MIN - 1);
                break;
            default:
                svUsedIdMask = 0;
                break;
        }

        // If SV ID was used in previous position fix, then set USED_IN_FIX
        // flag, else clear the USED_IN_FIX flag.
        if (svUsedIdMask & (1 << (gnssSvId - 1))) {
            svNotify.gnssSvs[i].gnssSvOptionsMask |= GNSS_SV_OPTIONS_USED_IN_FIX_BIT;
        }
    }

    for (auto it=mClientData.begin(); it != mClientData.end(); ++it) {
        if (nullptr != it->second.gnssSvCb) {
            it->second.gnssSvCb(svNotify);
        }
    }

    if (NMEA_PROVIDER_AP == ContextBase::mGps_conf.NMEA_PROVIDER && !mTrackingSessions.empty()) {
        std::vector<std::string> nmeaArraystr;
        loc_nmea_generate_sv(svNotify, nmeaArraystr);
        for (auto sentence : nmeaArraystr) {
            reportNmea(sentence.c_str(), sentence.length());
        }
    }

    mGnssSvIdUsedInPosAvail = false;
}

void
GnssAdapter::reportNmeaEvent(const char* nmea, size_t length, bool fromUlp)
{
    // if this event is not called from ULP, then try to call into ULP and return if successfull
    if (!fromUlp && !loc_nmea_is_debug(nmea, length)) {
        if (mUlpProxy->reportNmea(nmea, length)) {
            return;
        }
    }

    struct MsgReportNmea : public LocMsg {
        GnssAdapter& mAdapter;
        const char* mNmea;
        size_t mLength;
        inline MsgReportNmea(GnssAdapter& adapter,
                             const char* nmea,
                             size_t length) :
            LocMsg(),
            mAdapter(adapter),
            mNmea(new char[length+1]),
            mLength(length) {
                if (mNmea == nullptr) {
                    LOC_LOGE("%s] new allocation failed, fatal error.", __func__);
                    return;
                }
                strlcpy((char*)mNmea, nmea, length+1);
            }
        inline virtual ~MsgReportNmea()
        {
            delete[] mNmea;
        }
        inline virtual void proc() const {
            // extract bug report info - this returns true if consumed by systemstatus
            bool ret = false;
            SystemStatus* s = mAdapter.getSystemStatus();
            if (nullptr != s) {
                ret = s->setNmeaString(mNmea, mLength);
            }
            if (false == ret) {
                // forward NMEA message to upper layer
                mAdapter.reportNmea(mNmea, mLength);
            }
        }
    };

    sendMsg(new MsgReportNmea(*this, nmea, length));
}

void
GnssAdapter::reportNmea(const char* nmea, size_t length)
{
    GnssNmeaNotification nmeaNotification = {};
    nmeaNotification.size = sizeof(GnssNmeaNotification);

    struct timeval tv;
    gettimeofday(&tv, (struct timezone *) NULL);
    int64_t now = tv.tv_sec * 1000LL + tv.tv_usec / 1000;
    nmeaNotification.timestamp = now;
    nmeaNotification.nmea = nmea;
    nmeaNotification.length = length;

    for (auto it=mClientData.begin(); it != mClientData.end(); ++it) {
        if (nullptr != it->second.gnssNmeaCb) {
            it->second.gnssNmeaCb(nmeaNotification);
        }
    }
}

bool
GnssAdapter::requestNiNotifyEvent(const GnssNiNotification &notify, const void* data)
{
    LOC_LOGI("%s]: notif_type: %d, timeout: %d, default_resp: %d"
             "requestor_id: %s (encoding: %d) text: %s text (encoding: %d) extras: %s",
             __func__, notify.type, notify.timeout, notify.timeoutResponse,
             notify.requestor, notify.requestorEncoding,
             notify.message, notify.messageEncoding, notify.extras);

    struct MsgReportNiNotify : public LocMsg {
        GnssAdapter& mAdapter;
        const GnssNiNotification mNotify;
        const void* mData;
        inline MsgReportNiNotify(GnssAdapter& adapter,
                                 const GnssNiNotification& notify,
                                 const void* data) :
            LocMsg(),
            mAdapter(adapter),
            mNotify(notify),
            mData(data) {}
        inline virtual void proc() const {
            mAdapter.requestNiNotify(mNotify, mData);
        }
    };

    sendMsg(new MsgReportNiNotify(*this, notify, data));

    return true;
}

static void* niThreadProc(void *args)
{
    NiSession* pSession = (NiSession*)args;
    int rc = 0;          /* return code from pthread calls */

    struct timespec present_time;
    struct timespec expire_time;

    pthread_mutex_lock(&pSession->tLock);
    /* Calculate absolute expire time */
    clock_gettime(CLOCK_MONOTONIC, &present_time);
    expire_time.tv_sec  = present_time.tv_sec + pSession->respTimeLeft;
    expire_time.tv_nsec = present_time.tv_nsec;
    LOC_LOGD("%s]: time out set for abs time %ld with delay %d sec",
             __func__, (long)expire_time.tv_sec, pSession->respTimeLeft);

    while (!pSession->respRecvd) {
        rc = pthread_cond_timedwait(&pSession->tCond,
                                    &pSession->tLock,
                                    &expire_time);
        if (rc == ETIMEDOUT) {
            pSession->resp = GNSS_NI_RESPONSE_NO_RESPONSE;
            LOC_LOGD("%s]: time out after valting for specified time. Ret Val %d",
                     __func__, rc);
            break;
        }
    }
    LOC_LOGD("%s]: Java layer has sent us a user response and return value from "
             "pthread_cond_timedwait = %d pSession->resp is %u", __func__, rc, pSession->resp);
    pSession->respRecvd = false; /* Reset the user response flag for the next session*/

    // adding this check to support modem restart, in which case, we need the thread
    // to exit without calling sending data. We made sure that rawRequest is NULL in
    // loc_eng_ni_reset_on_engine_restart()
    GnssAdapter* adapter = pSession->adapter;
    GnssNiResponse resp;
    void* rawRequest = NULL;
    bool sendResponse = false;

    if (NULL != pSession->rawRequest) {
        if (pSession->resp != GNSS_NI_RESPONSE_IGNORE) {
            resp = pSession->resp;
            rawRequest = pSession->rawRequest;
            sendResponse = true;
        } else {
            free(pSession->rawRequest);
        }
        pSession->rawRequest = NULL;
    }
    pthread_mutex_unlock(&pSession->tLock);

    pSession->respTimeLeft = 0;
    pSession->reqID = 0;

    if (sendResponse) {
        adapter->gnssNiResponseCommand(resp, rawRequest);
    }

    return NULL;
}

bool
GnssAdapter::requestNiNotify(const GnssNiNotification& notify, const void* data)
{
    NiSession* pSession = NULL;
    gnssNiCallback gnssNiCb = nullptr;

    for (auto it=mClientData.begin(); it != mClientData.end(); ++it) {
        if (nullptr != it->second.gnssNiCb) {
            gnssNiCb = it->second.gnssNiCb;
            break;
        }
    }
    if (nullptr == gnssNiCb) {
        EXIT_LOG(%s, "no clients with gnssNiCb.");
        return false;
    }

    if (notify.type == GNSS_NI_TYPE_EMERGENCY_SUPL) {
        if (NULL != mNiData.sessionEs.rawRequest) {
            LOC_LOGI("%s]: supl es NI in progress, new supl es NI ignored, type: %d",
                     __func__, notify.type);
            if (NULL != data) {
                free((void*)data);
            }
        } else {
            pSession = &mNiData.sessionEs;
        }
    } else {
        if (NULL != mNiData.session.rawRequest ||
            NULL != mNiData.sessionEs.rawRequest) {
            LOC_LOGI("%s]: supl NI in progress, new supl NI ignored, type: %d",
                     __func__, notify.type);
            if (NULL != data) {
                free((void*)data);
            }
        } else {
            pSession = &mNiData.session;
        }
    }

    if (pSession) {
        /* Save request */
        pSession->rawRequest = (void*)data;
        pSession->reqID = ++mNiData.reqIDCounter;
        pSession->adapter = this;

        int sessionId = pSession->reqID;

        /* For robustness, spawn a thread at this point to timeout to clear up the notification
         * status, even though the OEM layer in java does not do so.
         **/
        pSession->respTimeLeft =
             5 + (notify.timeout != 0 ? notify.timeout : LOC_NI_NO_RESPONSE_TIME);

        int rc = 0;
        rc = pthread_create(&pSession->thread, NULL, niThreadProc, pSession);
        if (rc) {
            LOC_LOGE("%s]: Loc NI thread is not created.", __func__);
        }
        rc = pthread_detach(pSession->thread);
        if (rc) {
            LOC_LOGE("%s]: Loc NI thread is not detached.", __func__);
        }

        if (nullptr != gnssNiCb) {
            gnssNiCb(sessionId, notify);
        }
    }

    return true;
}

void
GnssAdapter::reportGnssMeasurementDataEvent(const GnssMeasurementsNotification& measurements,
                                            int msInWeek)
{
    LOC_LOGD("%s]: msInWeek=%d", __func__, msInWeek);

    struct MsgReportGnssMeasurementData : public LocMsg {
        GnssAdapter& mAdapter;
        GnssMeasurementsNotification mMeasurementsNotify;
        inline MsgReportGnssMeasurementData(GnssAdapter& adapter,
                                            const GnssMeasurementsNotification& measurements,
                                            int msInWeek) :
                LocMsg(),
                mAdapter(adapter),
                mMeasurementsNotify(measurements) {
            if (-1 != msInWeek) {
                mAdapter.getAgcInformation(mMeasurementsNotify, msInWeek);
            }
        }
        inline virtual void proc() const {
            mAdapter.reportGnssMeasurementData(mMeasurementsNotify);
        }
    };

    sendMsg(new MsgReportGnssMeasurementData(*this, measurements, msInWeek));
}

void
GnssAdapter::reportGnssMeasurementData(const GnssMeasurementsNotification& measurements)
{
    for (auto it=mClientData.begin(); it != mClientData.end(); ++it) {
        if (nullptr != it->second.gnssMeasurementsCb) {
            it->second.gnssMeasurementsCb(measurements);
        }
    }
}

void
GnssAdapter::reportSvMeasurementEvent(GnssSvMeasurementSet &svMeasurementSet)
{
    LOC_LOGD("%s]: ", __func__);

    // We send SvMeasurementSet to AmtProxy/ULPProxy to be forwarded as necessary.
    mUlpProxy->reportSvMeasurement(svMeasurementSet);
}

void
GnssAdapter::reportSvPolynomialEvent(GnssSvPolynomial &svPolynomial)
{
    LOC_LOGD("%s]: ", __func__);

    // We send SvMeasurementSet to AmtProxy/ULPProxy to be forwarded as necessary.
    mUlpProxy->reportSvPolynomial(svPolynomial);
}

bool
GnssAdapter::reportOdcpiRequestEvent(OdcpiRequestInfo& request)
{
    struct MsgReportOdcpiRequest : public LocMsg {
        GnssAdapter& mAdapter;
        OdcpiRequestInfo mOdcpiRequest;
        inline MsgReportOdcpiRequest(GnssAdapter& adapter, OdcpiRequestInfo& request) :
                LocMsg(),
                mAdapter(adapter),
                mOdcpiRequest(request) {}
        inline virtual void proc() const {
            mAdapter.reportOdcpiRequest(mOdcpiRequest);
        }
    };

    sendMsg(new MsgReportOdcpiRequest(*this, request));
    return true;
}

void GnssAdapter::reportOdcpiRequest(const OdcpiRequestInfo& request)
{
    if (nullptr != mOdcpiRequestCb) {
        LOC_LOGd("request: type %d, tbf %d, isEmergency %d"
                 " requestActive: %d timerActive: %d",
                 request.type, request.tbfMillis, request.isEmergencyMode,
                 mOdcpiRequestActive, mOdcpiTimer.isActive());
        // ODCPI START and ODCPI STOP from modem can come in quick succession
        // so the mOdcpiTimer helps avoid spamming the framework as well as
        // extending the odcpi session past 30 seconds if needed
        if (ODCPI_REQUEST_TYPE_START == request.type) {
            if (false == mOdcpiRequestActive && false == mOdcpiTimer.isActive()) {
                mOdcpiRequestCb(request);
                mOdcpiRequestActive = true;
                mOdcpiTimer.start();
            // if the current active odcpi session is non-emergency, and the new
            // odcpi request is emergency, replace the odcpi request with new request
            // and restart the timer
            } else if (false == mOdcpiRequest.isEmergencyMode &&
                       true == request.isEmergencyMode) {
                mOdcpiRequestCb(request);
                mOdcpiRequestActive = true;
                if (true == mOdcpiTimer.isActive()) {
                    mOdcpiTimer.restart();
                } else {
                    mOdcpiTimer.start();
                }
            // if ODCPI request is not active but the timer is active, then
            // just update the active state and wait for timer to expire
            // before requesting new ODCPI to avoid spamming ODCPI requests
            } else if (false == mOdcpiRequestActive && true == mOdcpiTimer.isActive()) {
                mOdcpiRequestActive = true;
            }
            mOdcpiRequest = request;
        // the request is being stopped, but allow timer to expire first
        // before stopping the timer just in case more ODCPI requests come
        // to avoid spamming more odcpi requests to the framework
        } else {
            mOdcpiRequestActive = false;
        }
    } else {
        LOC_LOGw("ODCPI request not supported");
    }
}

void GnssAdapter::initOdcpiCommand(const OdcpiRequestCallback& callback)
{
    struct MsgInitOdcpi : public LocMsg {
        GnssAdapter& mAdapter;
        OdcpiRequestCallback mOdcpiCb;
        inline MsgInitOdcpi(GnssAdapter& adapter,
                const OdcpiRequestCallback& callback) :
                LocMsg(),
                mAdapter(adapter),
                mOdcpiCb(callback) {}
        inline virtual void proc() const {
            mAdapter.initOdcpi(mOdcpiCb);
        }
    };

    sendMsg(new MsgInitOdcpi(*this, callback));
}

void GnssAdapter::initOdcpi(const OdcpiRequestCallback& callback)
{
    mOdcpiRequestCb = callback;

    /* Register for WIFI request */
    updateEvtMask(LOC_API_ADAPTER_BIT_REQUEST_WIFI,
            LOC_REGISTRATION_MASK_ENABLED);
}

void GnssAdapter::injectOdcpiCommand(const Location& location)
{
    struct MsgInjectOdcpi : public LocMsg {
        GnssAdapter& mAdapter;
        Location mLocation;
        inline MsgInjectOdcpi(GnssAdapter& adapter, const Location& location) :
                LocMsg(),
                mAdapter(adapter),
                mLocation(location) {}
        inline virtual void proc() const {
            mAdapter.injectOdcpi(mLocation);
        }
    };

    sendMsg(new MsgInjectOdcpi(*this, location));
}

void GnssAdapter::injectOdcpi(const Location& location)
{
    LOC_LOGd("ODCPI Injection: requestActive: %d timerActive: %d"
             "lat %.7f long %.7f",
            mOdcpiRequestActive, mOdcpiTimer.isActive(),
            location.latitude, location.longitude);

    loc_api_adapter_err err = mLocApi->injectPosition(location);
    if (LOC_API_ADAPTER_ERR_SUCCESS != err) {
        LOC_LOGe("Inject Position API error %d", err);
    }
}

// Called in the context of LocTimer thread
void OdcpiTimer::timeOutCallback()
{
    if (nullptr != mAdapter) {
        mAdapter->odcpiTimerExpireEvent();
    }
}

// Called in the context of LocTimer thread
void GnssAdapter::odcpiTimerExpireEvent()
{
    struct MsgOdcpiTimerExpire : public LocMsg {
        GnssAdapter& mAdapter;
        inline MsgOdcpiTimerExpire(GnssAdapter& adapter) :
                LocMsg(),
                mAdapter(adapter) {}
        inline virtual void proc() const {
            mAdapter.odcpiTimerExpire();
        }
    };
    sendMsg(new MsgOdcpiTimerExpire(*this));
}
void GnssAdapter::odcpiTimerExpire()
{
    LOC_LOGd("requestActive: %d timerActive: %d",
            mOdcpiRequestActive, mOdcpiTimer.isActive());

    // if ODCPI request is still active after timer
    // expires, request again and restart timer
    if (mOdcpiRequestActive) {
        mOdcpiRequestCb(mOdcpiRequest);
        mOdcpiTimer.restart();
    } else {
        mOdcpiTimer.stop();
    }
}

void GnssAdapter::initDefaultAgps() {
    LOC_LOGD("%s]: ", __func__);

    void *handle = nullptr;
    if ((handle = dlopen("libloc_net_iface.so", RTLD_NOW)) == nullptr) {
        LOC_LOGD("%s]: libloc_net_iface.so not found !", __func__);
        return;
    }

    LocAgpsGetAgpsCbInfo getAgpsCbInfo = (LocAgpsGetAgpsCbInfo)
            dlsym(handle, "LocNetIfaceAgps_getAgpsCbInfo");
    if (getAgpsCbInfo == nullptr) {
        LOC_LOGE("%s]: Failed to get method LocNetIfaceAgps_getStatusCb", __func__);
        return;
    }

    AgpsCbInfo& cbInfo = getAgpsCbInfo(agpsOpenResultCb, agpsCloseResultCb, this);

    if (cbInfo.statusV4Cb == nullptr) {
        LOC_LOGE("%s]: statusV4Cb is nullptr!", __func__);
        return;
    }

    initAgps(cbInfo);
}

void GnssAdapter::initDefaultAgpsCommand() {
    LOC_LOGD("%s]: ", __func__);

    struct MsgInitDefaultAgps : public LocMsg {
        GnssAdapter& mAdapter;
        inline MsgInitDefaultAgps(GnssAdapter& adapter) :
            LocMsg(),
            mAdapter(adapter) {
            }
        inline virtual void proc() const {
            mAdapter.initDefaultAgps();
        }
    };

    sendMsg(new MsgInitDefaultAgps(*this));
}

/* INIT LOC AGPS MANAGER */

void GnssAdapter::initAgps(const AgpsCbInfo& cbInfo) {
    LOC_LOGD("%s]: mAgpsCbInfo.cbPriority - %d;  cbInfo.cbPriority - %d",
            __func__, mAgpsCbInfo.cbPriority, cbInfo.cbPriority)

    if (!((ContextBase::mGps_conf.CAPABILITIES & LOC_GPS_CAPABILITY_MSB) ||
            (ContextBase::mGps_conf.CAPABILITIES & LOC_GPS_CAPABILITY_MSA))) {
        return;
    }

    if (mAgpsCbInfo.cbPriority > cbInfo.cbPriority) {
        return;
    } else {
        mAgpsCbInfo = cbInfo;

        mAgpsManager.registerFrameworkStatusCallback((AgnssStatusIpV4Cb)cbInfo.statusV4Cb);

        mAgpsManager.createAgpsStateMachines();

        /* Register for AGPS event mask */
        updateEvtMask(LOC_API_ADAPTER_BIT_LOCATION_SERVER_REQUEST,
                LOC_REGISTRATION_MASK_ENABLED);
    }
}

void GnssAdapter::initAgpsCommand(const AgpsCbInfo& cbInfo){
    LOC_LOGI("GnssAdapter::initAgpsCommand");

    /* Message to initialize AGPS module */
    struct AgpsMsgInit: public LocMsg {
        const AgpsCbInfo mCbInfo;
        GnssAdapter& mAdapter;

        inline AgpsMsgInit(const AgpsCbInfo& cbInfo,
                GnssAdapter& adapter) :
                LocMsg(), mCbInfo(cbInfo), mAdapter(adapter) {
            LOC_LOGV("AgpsMsgInit");
        }

        inline virtual void proc() const {
            LOC_LOGV("AgpsMsgInit::proc()");
            mAdapter.initAgps(mCbInfo);
        }
    };

    /* Send message to initialize AGPS Manager */
    sendMsg(new AgpsMsgInit(cbInfo, *this));
}

/* GnssAdapter::requestATL
 * Method triggered in QMI thread as part of handling below message:
 * eQMI_LOC_SERVER_REQUEST_OPEN_V02
 * Triggers the AGPS state machine to setup AGPS call for below WWAN types:
 * eQMI_LOC_WWAN_TYPE_INTERNET_V02
 * eQMI_LOC_WWAN_TYPE_AGNSS_V02 */
bool GnssAdapter::requestATL(int connHandle, LocAGpsType agpsType, LocApnTypeMask mask){

    LOC_LOGI("GnssAdapter::requestATL");

    sendMsg( new AgpsMsgRequestATL(
             &mAgpsManager, connHandle, (AGpsExtType)agpsType, mask));

    return true;
}

/* GnssAdapter::requestSuplES
 * Method triggered in QMI thread as part of handling below message:
 * eQMI_LOC_SERVER_REQUEST_OPEN_V02
 * Triggers the AGPS state machine to setup AGPS call for below WWAN types:
 * eQMI_LOC_WWAN_TYPE_AGNSS_EMERGENCY_V02 */
bool GnssAdapter::requestSuplES(int connHandle, LocApnTypeMask mask){

    LOC_LOGI("GnssAdapter::requestSuplES");

    sendMsg( new AgpsMsgRequestATL(
             &mAgpsManager, connHandle, LOC_AGPS_TYPE_SUPL_ES, mask));

    return true;
}

/* GnssAdapter::releaseATL
 * Method triggered in QMI thread as part of handling below message:
 * eQMI_LOC_SERVER_REQUEST_CLOSE_V02
 * Triggers teardown of an existing AGPS call */
bool GnssAdapter::releaseATL(int connHandle){

    LOC_LOGI("GnssAdapter::releaseATL");

    /* Release SUPL/INTERNET/SUPL_ES ATL */
    struct AgpsMsgReleaseATL: public LocMsg {

        AgpsManager* mAgpsManager;
        int mConnHandle;

        inline AgpsMsgReleaseATL(AgpsManager* agpsManager, int connHandle) :
                LocMsg(), mAgpsManager(agpsManager), mConnHandle(connHandle) {

            LOC_LOGV("AgpsMsgReleaseATL");
        }

        inline virtual void proc() const {

            LOC_LOGV("AgpsMsgReleaseATL::proc()");
            mAgpsManager->releaseATL(mConnHandle);
        }
    };

    sendMsg( new AgpsMsgReleaseATL(&mAgpsManager, connHandle));

    return true;
}

/* GnssAdapter::reportDataCallOpened
 * DS Client data call opened successfully.
 * Send message to AGPS Manager to handle. */
bool GnssAdapter::reportDataCallOpened(){

    LOC_LOGI("GnssAdapter::reportDataCallOpened");

    struct AgpsMsgSuplEsOpened: public LocMsg {

        AgpsManager* mAgpsManager;

        inline AgpsMsgSuplEsOpened(AgpsManager* agpsManager) :
                LocMsg(), mAgpsManager(agpsManager) {

            LOC_LOGV("AgpsMsgSuplEsOpened");
        }

        inline virtual void proc() const {

            LOC_LOGV("AgpsMsgSuplEsOpened::proc()");
            mAgpsManager->reportDataCallOpened();
        }
    };

    sendMsg( new AgpsMsgSuplEsOpened(&mAgpsManager));

    return true;
}

/* GnssAdapter::reportDataCallClosed
 * DS Client data call closed.
 * Send message to AGPS Manager to handle. */
bool GnssAdapter::reportDataCallClosed(){

    LOC_LOGI("GnssAdapter::reportDataCallClosed");

    struct AgpsMsgSuplEsClosed: public LocMsg {

        AgpsManager* mAgpsManager;

        inline AgpsMsgSuplEsClosed(AgpsManager* agpsManager) :
                LocMsg(), mAgpsManager(agpsManager) {

            LOC_LOGV("AgpsMsgSuplEsClosed");
        }

        inline virtual void proc() const {

            LOC_LOGV("AgpsMsgSuplEsClosed::proc()");
            mAgpsManager->reportDataCallClosed();
        }
    };

    sendMsg( new AgpsMsgSuplEsClosed(&mAgpsManager));

    return true;
}

void GnssAdapter::dataConnOpenCommand(
        AGpsExtType agpsType,
        const char* apnName, int apnLen, AGpsBearerType bearerType){

    LOC_LOGI("GnssAdapter::frameworkDataConnOpen");

    struct AgpsMsgAtlOpenSuccess: public LocMsg {

        AgpsManager* mAgpsManager;
        AGpsExtType mAgpsType;
        char* mApnName;
        int mApnLen;
        AGpsBearerType mBearerType;

        inline AgpsMsgAtlOpenSuccess(AgpsManager* agpsManager, AGpsExtType agpsType,
                const char* apnName, int apnLen, AGpsBearerType bearerType) :
                LocMsg(), mAgpsManager(agpsManager), mAgpsType(agpsType), mApnName(
                        new char[apnLen + 1]), mApnLen(apnLen), mBearerType(bearerType) {

            LOC_LOGV("AgpsMsgAtlOpenSuccess");
            if (mApnName == nullptr) {
                LOC_LOGE("%s] new allocation failed, fatal error.", __func__);
                return;
            }
            memcpy(mApnName, apnName, apnLen);
            mApnName[apnLen] = 0;
        }

        inline ~AgpsMsgAtlOpenSuccess() {
            delete[] mApnName;
        }

        inline virtual void proc() const {

            LOC_LOGV("AgpsMsgAtlOpenSuccess::proc()");
            mAgpsManager->reportAtlOpenSuccess(mAgpsType, mApnName, mApnLen, mBearerType);
        }
    };

    sendMsg( new AgpsMsgAtlOpenSuccess(
            &mAgpsManager, agpsType, apnName, apnLen, bearerType));
}

void GnssAdapter::dataConnClosedCommand(AGpsExtType agpsType){

    LOC_LOGI("GnssAdapter::frameworkDataConnClosed");

    struct AgpsMsgAtlClosed: public LocMsg {

        AgpsManager* mAgpsManager;
        AGpsExtType mAgpsType;

        inline AgpsMsgAtlClosed(AgpsManager* agpsManager, AGpsExtType agpsType) :
                LocMsg(), mAgpsManager(agpsManager), mAgpsType(agpsType) {

            LOC_LOGV("AgpsMsgAtlClosed");
        }

        inline virtual void proc() const {

            LOC_LOGV("AgpsMsgAtlClosed::proc()");
            mAgpsManager->reportAtlClosed(mAgpsType);
        }
    };

    sendMsg( new AgpsMsgAtlClosed(&mAgpsManager, (AGpsExtType)agpsType));
}

void GnssAdapter::dataConnFailedCommand(AGpsExtType agpsType){

    LOC_LOGI("GnssAdapter::frameworkDataConnFailed");

    struct AgpsMsgAtlOpenFailed: public LocMsg {

        AgpsManager* mAgpsManager;
        AGpsExtType mAgpsType;

        inline AgpsMsgAtlOpenFailed(AgpsManager* agpsManager, AGpsExtType agpsType) :
                LocMsg(), mAgpsManager(agpsManager), mAgpsType(agpsType) {

            LOC_LOGV("AgpsMsgAtlOpenFailed");
        }

        inline virtual void proc() const {

            LOC_LOGV("AgpsMsgAtlOpenFailed::proc()");
            mAgpsManager->reportAtlOpenFailed(mAgpsType);
        }
    };

    sendMsg( new AgpsMsgAtlOpenFailed(&mAgpsManager, (AGpsExtType)agpsType));
}

void GnssAdapter::convertSatelliteInfo(std::vector<GnssDebugSatelliteInfo>& out,
                                       const GnssSvType& in_constellation,
                                       const SystemStatusReports& in)
{
    uint64_t sv_mask = 0ULL;
    uint32_t svid_min = 0;
    uint32_t svid_num = 0;
    uint32_t svid_idx = 0;

    uint64_t eph_health_good_mask = 0ULL;
    uint64_t eph_health_bad_mask = 0ULL;
    uint64_t server_perdiction_available_mask = 0ULL;
    float server_perdiction_age = 0.0f;

    // set constellationi based parameters
    switch (in_constellation) {
        case GNSS_SV_TYPE_GPS:
            svid_min = GNSS_BUGREPORT_GPS_MIN;
            svid_num = GPS_NUM;
            svid_idx = 0;
            if (!in.mSvHealth.empty()) {
                eph_health_good_mask = in.mSvHealth.back().mGpsGoodMask;
                eph_health_bad_mask  = in.mSvHealth.back().mGpsBadMask;
            }
            if (!in.mXtra.empty()) {
                server_perdiction_available_mask = in.mXtra.back().mGpsXtraValid;
                server_perdiction_age = (float)(in.mXtra.back().mGpsXtraAge);
            }
            break;
        case GNSS_SV_TYPE_GLONASS:
            svid_min = GNSS_BUGREPORT_GLO_MIN;
            svid_num = GLO_NUM;
            svid_idx = GPS_NUM;
            if (!in.mSvHealth.empty()) {
                eph_health_good_mask = in.mSvHealth.back().mGloGoodMask;
                eph_health_bad_mask  = in.mSvHealth.back().mGloBadMask;
            }
            if (!in.mXtra.empty()) {
                server_perdiction_available_mask = in.mXtra.back().mGloXtraValid;
                server_perdiction_age = (float)(in.mXtra.back().mGloXtraAge);
            }
            break;
        case GNSS_SV_TYPE_QZSS:
            svid_min = GNSS_BUGREPORT_QZSS_MIN;
            svid_num = QZSS_NUM;
            svid_idx = GPS_NUM+GLO_NUM+BDS_NUM+GAL_NUM;
            if (!in.mSvHealth.empty()) {
                eph_health_good_mask = in.mSvHealth.back().mQzssGoodMask;
                eph_health_bad_mask  = in.mSvHealth.back().mQzssBadMask;
            }
            if (!in.mXtra.empty()) {
                server_perdiction_available_mask = in.mXtra.back().mQzssXtraValid;
                server_perdiction_age = (float)(in.mXtra.back().mQzssXtraAge);
            }
            break;
        case GNSS_SV_TYPE_BEIDOU:
            svid_min = GNSS_BUGREPORT_BDS_MIN;
            svid_num = BDS_NUM;
            svid_idx = GPS_NUM+GLO_NUM;
            if (!in.mSvHealth.empty()) {
                eph_health_good_mask = in.mSvHealth.back().mBdsGoodMask;
                eph_health_bad_mask  = in.mSvHealth.back().mBdsBadMask;
            }
            if (!in.mXtra.empty()) {
                server_perdiction_available_mask = in.mXtra.back().mBdsXtraValid;
                server_perdiction_age = (float)(in.mXtra.back().mBdsXtraAge);
            }
            break;
        case GNSS_SV_TYPE_GALILEO:
            svid_min = GNSS_BUGREPORT_GAL_MIN;
            svid_num = GAL_NUM;
            svid_idx = GPS_NUM+GLO_NUM+BDS_NUM;
            if (!in.mSvHealth.empty()) {
                eph_health_good_mask = in.mSvHealth.back().mGalGoodMask;
                eph_health_bad_mask  = in.mSvHealth.back().mGalBadMask;
            }
            if (!in.mXtra.empty()) {
                server_perdiction_available_mask = in.mXtra.back().mGalXtraValid;
                server_perdiction_age = (float)(in.mXtra.back().mGalXtraAge);
            }
            break;
        default:
            return;
    }

    // extract each sv info from systemstatus report
    for(uint32_t i=0; i<svid_num && (svid_idx+i)<SV_ALL_NUM; i++) {

        GnssDebugSatelliteInfo s = {};
        s.size = sizeof(s);
        s.svid = i + svid_min;
        s.constellation = in_constellation;

        if (!in.mNavData.empty()) {
            s.mEphemerisType   = in.mNavData.back().mNav[svid_idx+i].mType;
            s.mEphemerisSource = in.mNavData.back().mNav[svid_idx+i].mSource;
        }
        else {
            s.mEphemerisType   = GNSS_EPH_TYPE_UNKNOWN;
            s.mEphemerisSource = GNSS_EPH_SOURCE_UNKNOWN;
        }

        sv_mask = 0x1ULL << i;
        if (eph_health_good_mask & sv_mask) {
            s.mEphemerisHealth = GNSS_EPH_HEALTH_GOOD;
        }
        else if (eph_health_bad_mask & sv_mask) {
            s.mEphemerisHealth = GNSS_EPH_HEALTH_BAD;
        }
        else {
            s.mEphemerisHealth = GNSS_EPH_HEALTH_UNKNOWN;
        }

        if (!in.mNavData.empty()) {
            s.ephemerisAgeSeconds =
                (float)(in.mNavData.back().mNav[svid_idx+i].mAgeSec);
        }
        else {
            s.ephemerisAgeSeconds = 0.0f;
        }

        if (server_perdiction_available_mask & sv_mask) {
            s.serverPredictionIsAvailable = true;
        }
        else {
            s.serverPredictionIsAvailable = false;
        }

        s.serverPredictionAgeSeconds = server_perdiction_age;
        out.push_back(s);
    }

    return;
}

bool GnssAdapter::getDebugReport(GnssDebugReport& r)
{
    LOC_LOGD("%s]: ", __func__);

    SystemStatus* systemstatus = getSystemStatus();
    if (nullptr == systemstatus) {
        return false;
    }

    SystemStatusReports reports = {};
    systemstatus->getReport(reports, true);

    r.size = sizeof(r);

    // location block
    r.mLocation.size = sizeof(r.mLocation);
    if(!reports.mLocation.empty() && reports.mLocation.back().mValid) {
        r.mLocation.mValid = true;
        r.mLocation.mLocation.latitude =
            reports.mLocation.back().mLocation.gpsLocation.latitude;
        r.mLocation.mLocation.longitude =
            reports.mLocation.back().mLocation.gpsLocation.longitude;
        r.mLocation.mLocation.altitude =
            reports.mLocation.back().mLocation.gpsLocation.altitude;
        r.mLocation.mLocation.speed =
            (double)(reports.mLocation.back().mLocation.gpsLocation.speed);
        r.mLocation.mLocation.bearing =
            (double)(reports.mLocation.back().mLocation.gpsLocation.bearing);
        r.mLocation.mLocation.accuracy =
            (double)(reports.mLocation.back().mLocation.gpsLocation.accuracy);

        r.mLocation.verticalAccuracyMeters =
            reports.mLocation.back().mLocationEx.vert_unc;
        r.mLocation.speedAccuracyMetersPerSecond =
            reports.mLocation.back().mLocationEx.speed_unc;
        r.mLocation.bearingAccuracyDegrees =
            reports.mLocation.back().mLocationEx.bearing_unc;

        r.mLocation.mUtcReported =
            reports.mLocation.back().mUtcReported;
    }
    else if(!reports.mBestPosition.empty() && reports.mBestPosition.back().mValid) {
        r.mLocation.mValid = true;
        r.mLocation.mLocation.latitude =
                (double)(reports.mBestPosition.back().mBestLat) * RAD2DEG;
        r.mLocation.mLocation.longitude =
                (double)(reports.mBestPosition.back().mBestLon) * RAD2DEG;
        r.mLocation.mLocation.altitude = reports.mBestPosition.back().mBestAlt;
        r.mLocation.mLocation.accuracy =
                (double)(reports.mBestPosition.back().mBestHepe);

        r.mLocation.mUtcReported = reports.mBestPosition.back().mUtcReported;
    }
    else {
        r.mLocation.mValid = false;
    }

    if (r.mLocation.mValid) {
        LOC_LOGV("getDebugReport - lat=%f lon=%f alt=%f speed=%f",
            r.mLocation.mLocation.latitude,
            r.mLocation.mLocation.longitude,
            r.mLocation.mLocation.altitude,
            r.mLocation.mLocation.speed);
    }

    // time block
    r.mTime.size = sizeof(r.mTime);
    if(!reports.mTimeAndClock.empty() && reports.mTimeAndClock.back().mTimeValid) {
        r.mTime.mValid = true;
        r.mTime.timeEstimate =
            (((int64_t)(reports.mTimeAndClock.back().mGpsWeek)*7 +
                        GNSS_UTC_TIME_OFFSET)*24*60*60 -
              (int64_t)(reports.mTimeAndClock.back().mLeapSeconds))*1000ULL +
              (int64_t)(reports.mTimeAndClock.back().mGpsTowMs);

        if (reports.mTimeAndClock.back().mTimeUncNs > 0) {
            // TimeUncNs value is available
            r.mTime.timeUncertaintyNs =
                    (float)(reports.mTimeAndClock.back().mLeapSecUnc)*1000.0f +
                    (float)(reports.mTimeAndClock.back().mTimeUncNs);
        } else {
            // fall back to legacy TimeUnc
            r.mTime.timeUncertaintyNs =
                    ((float)(reports.mTimeAndClock.back().mTimeUnc) +
                     (float)(reports.mTimeAndClock.back().mLeapSecUnc))*1000.0f;
        }

        r.mTime.frequencyUncertaintyNsPerSec =
            (float)(reports.mTimeAndClock.back().mClockFreqBiasUnc);
        LOC_LOGV("getDebugReport - timeestimate=%" PRIu64 " unc=%f frequnc=%f",
                r.mTime.timeEstimate,
                r.mTime.timeUncertaintyNs, r.mTime.frequencyUncertaintyNsPerSec);
    }
    else {
        r.mTime.mValid = false;
    }

    // satellite info block
    convertSatelliteInfo(r.mSatelliteInfo, GNSS_SV_TYPE_GPS, reports);
    convertSatelliteInfo(r.mSatelliteInfo, GNSS_SV_TYPE_GLONASS, reports);
    convertSatelliteInfo(r.mSatelliteInfo, GNSS_SV_TYPE_QZSS, reports);
    convertSatelliteInfo(r.mSatelliteInfo, GNSS_SV_TYPE_BEIDOU, reports);
    convertSatelliteInfo(r.mSatelliteInfo, GNSS_SV_TYPE_GALILEO, reports);
    LOC_LOGV("getDebugReport - satellite=%zu", r.mSatelliteInfo.size());

    return true;
}

/* get AGC information from system status and fill it */
void
GnssAdapter::getAgcInformation(GnssMeasurementsNotification& measurements, int msInWeek)
{
    SystemStatus* systemstatus = getSystemStatus();

    if (nullptr != systemstatus) {
        SystemStatusReports reports = {};
        systemstatus->getReport(reports, true);

        if ((!reports.mRfAndParams.empty()) && (!reports.mTimeAndClock.empty()) &&
            (abs(msInWeek - (int)reports.mTimeAndClock.back().mGpsTowMs) < 2000)) {

            for (size_t i = 0; i < measurements.count; i++) {
                switch (measurements.measurements[i].svType) {
                case GNSS_SV_TYPE_GPS:
                case GNSS_SV_TYPE_QZSS:
                    measurements.measurements[i].agcLevelDb =
                            reports.mRfAndParams.back().mAgcGps;
                    measurements.measurements[i].flags |=
                            GNSS_MEASUREMENTS_DATA_AUTOMATIC_GAIN_CONTROL_BIT;
                    break;

                case GNSS_SV_TYPE_GALILEO:
                    measurements.measurements[i].agcLevelDb =
                            reports.mRfAndParams.back().mAgcGal;
                    measurements.measurements[i].flags |=
                            GNSS_MEASUREMENTS_DATA_AUTOMATIC_GAIN_CONTROL_BIT;
                    break;

                case GNSS_SV_TYPE_GLONASS:
                    measurements.measurements[i].agcLevelDb =
                            reports.mRfAndParams.back().mAgcGlo;
                    measurements.measurements[i].flags |=
                            GNSS_MEASUREMENTS_DATA_AUTOMATIC_GAIN_CONTROL_BIT;
                    break;

                case GNSS_SV_TYPE_BEIDOU:
                    measurements.measurements[i].agcLevelDb =
                            reports.mRfAndParams.back().mAgcBds;
                    measurements.measurements[i].flags |=
                            GNSS_MEASUREMENTS_DATA_AUTOMATIC_GAIN_CONTROL_BIT;
                    break;

                case GNSS_SV_TYPE_SBAS:
                case GNSS_SV_TYPE_UNKNOWN:
                default:
                    break;
                }
            }
        }
    }
}

/* Callbacks registered with loc_net_iface library */
static void agpsOpenResultCb (bool isSuccess, AGpsExtType agpsType, const char* apn,
        AGpsBearerType bearerType, void* userDataPtr) {
    LOC_LOGD("%s]: ", __func__);
    if (userDataPtr == nullptr) {
        LOC_LOGE("%s]: userDataPtr is nullptr.", __func__);
        return;
    }
    if (apn == nullptr) {
        LOC_LOGE("%s]: apn is nullptr.", __func__);
        return;
    }
    GnssAdapter* adapter = (GnssAdapter*)userDataPtr;
    if (isSuccess) {
        adapter->dataConnOpenCommand(agpsType, apn, strlen(apn), bearerType);
    } else {
        adapter->dataConnFailedCommand(agpsType);
    }
}

static void agpsCloseResultCb (bool isSuccess, AGpsExtType agpsType, void* userDataPtr) {
    LOC_LOGD("%s]: ", __func__);
    if (userDataPtr == nullptr) {
        LOC_LOGE("%s]: userDataPtr is nullptr.", __func__);
        return;
    }
    GnssAdapter* adapter = (GnssAdapter*)userDataPtr;
    if (isSuccess) {
        adapter->dataConnClosedCommand(agpsType);
    } else {
        adapter->dataConnFailedCommand(agpsType);
    }
}
