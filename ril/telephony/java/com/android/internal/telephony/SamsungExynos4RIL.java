/*
 * Copyright (C) 2011 The CyanogenMod Project <http://www.cyanogenmod.org>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package com.android.internal.telephony;

import static com.android.internal.telephony.RILConstants.*;

import android.content.Context;
import android.hardware.radio.V1_0.RadioResponseInfo;
import android.hardware.radio.V1_0.RadioResponseType;
import android.hardware.radio.V1_0.RadioError;
import android.os.AsyncResult;
import android.os.Handler;
import android.os.Message;
import android.os.Parcel;
import android.os.Registrant;
import android.telephony.ModemActivityInfo;
import android.telephony.Rlog;

import android.telephony.PhoneNumberUtils;

public class SamsungExynos4RIL extends RIL implements CommandsInterface {

    //SAMSUNG STATES
    static final int RIL_REQUEST_GET_CELL_BROADCAST_CONFIG = 10002;

    static final int RIL_REQUEST_SEND_ENCODED_USSD = 10005;
    static final int RIL_REQUEST_SET_PDA_MEMORY_STATUS = 10006;
    static final int RIL_REQUEST_GET_PHONEBOOK_STORAGE_INFO = 10007;
    static final int RIL_REQUEST_GET_PHONEBOOK_ENTRY = 10008;
    static final int RIL_REQUEST_ACCESS_PHONEBOOK_ENTRY = 10009;
    static final int RIL_REQUEST_DIAL_VIDEO_CALL = 10010;
    static final int RIL_REQUEST_CALL_DEFLECTION = 10011;
    static final int RIL_REQUEST_READ_SMS_FROM_SIM = 10012;
    static final int RIL_REQUEST_USIM_PB_CAPA = 10013;
    static final int RIL_REQUEST_LOCK_INFO = 10014;

    static final int RIL_REQUEST_DIAL_EMERGENCY = 10016;
    static final int RIL_REQUEST_GET_STOREAD_MSG_COUNT = 10017;
    static final int RIL_REQUEST_STK_SIM_INIT_EVENT = 10018;
    static final int RIL_REQUEST_GET_LINE_ID = 10019;
    static final int RIL_REQUEST_SET_LINE_ID = 10020;
    static final int RIL_REQUEST_GET_SERIAL_NUMBER = 10021;
    static final int RIL_REQUEST_GET_MANUFACTURE_DATE_NUMBER = 10022;
    static final int RIL_REQUEST_GET_BARCODE_NUMBER = 10023;
    static final int RIL_REQUEST_UICC_GBA_AUTHENTICATE_BOOTSTRAP = 10024;
    static final int RIL_REQUEST_UICC_GBA_AUTHENTICATE_NAF = 10025;
    static final int RIL_REQUEST_SIM_TRANSMIT_BASIC = 10026;
    static final int RIL_REQUEST_SIM_OPEN_CHANNEL = 10027;
    static final int RIL_REQUEST_SIM_CLOSE_CHANNEL = 10028;
    static final int RIL_REQUEST_SIM_TRANSMIT_CHANNEL = 10029;
    static final int RIL_REQUEST_SIM_AUTH = 10030;
    static final int RIL_REQUEST_PS_ATTACH = 10031;
    static final int RIL_REQUEST_PS_DETACH = 10032;
    static final int RIL_REQUEST_ACTIVATE_DATA_CALL = 10033;
    static final int RIL_REQUEST_CHANGE_SIM_PERSO = 10034;
    static final int RIL_REQUEST_ENTER_SIM_PERSO = 10035;
    static final int RIL_REQUEST_GET_TIME_INFO = 10036;
    static final int RIL_REQUEST_OMADM_SETUP_SESSION = 10037;
    static final int RIL_REQUEST_OMADM_SERVER_START_SESSION = 10038;
    static final int RIL_REQUEST_OMADM_CLIENT_START_SESSION = 10039;
    static final int RIL_REQUEST_OMADM_SEND_DATA = 10040;
    static final int RIL_REQUEST_CDMA_GET_DATAPROFILE = 10041;
    static final int RIL_REQUEST_CDMA_SET_DATAPROFILE = 10042;
    static final int RIL_REQUEST_CDMA_GET_SYSTEMPROPERTIES = 10043;
    static final int RIL_REQUEST_CDMA_SET_SYSTEMPROPERTIES = 10044;
    static final int RIL_REQUEST_SEND_SMS_COUNT = 10045;
    static final int RIL_REQUEST_SEND_SMS_MSG = 10046;
    static final int RIL_REQUEST_SEND_SMS_MSG_READ_STATUS = 10047;
    static final int RIL_REQUEST_MODEM_HANGUP = 10048;
    static final int RIL_REQUEST_SET_SIM_POWER = 10049;
    static final int RIL_REQUEST_SET_PREFERRED_NETWORK_LIST = 10050;
    static final int RIL_REQUEST_GET_PREFERRED_NETWORK_LIST = 10051;
    static final int RIL_REQUEST_HANGUP_VT = 10052;

    static final int RIL_UNSOL_RELEASE_COMPLETE_MESSAGE = 11001;
    static final int RIL_UNSOL_STK_SEND_SMS_RESULT = 11002;
    static final int RIL_UNSOL_STK_CALL_CONTROL_RESULT = 11003;
    static final int RIL_UNSOL_DUN_CALL_STATUS = 11004;

    static final int RIL_UNSOL_O2_HOME_ZONE_INFO = 11007;
    static final int RIL_UNSOL_DEVICE_READY_NOTI = 11008;
    static final int RIL_UNSOL_GPS_NOTI = 11009;
    static final int RIL_UNSOL_AM = 11010;
    static final int RIL_UNSOL_DUN_PIN_CONTROL_SIGNAL = 11011;
    static final int RIL_UNSOL_DATA_SUSPEND_RESUME = 11012;
    static final int RIL_UNSOL_SAP = 11013;

    static final int RIL_UNSOL_SIM_SMS_STORAGE_AVAILALE = 11015;
    static final int RIL_UNSOL_HSDPA_STATE_CHANGED = 11016;
    static final int RIL_UNSOL_WB_AMR_STATE = 11017;
    static final int RIL_UNSOL_TWO_MIC_STATE = 11018;
    static final int RIL_UNSOL_DHA_STATE = 11019;
    static final int RIL_UNSOL_UART = 11020;
    static final int RIL_UNSOL_RESPONSE_HANDOVER = 11021;
    static final int RIL_UNSOL_IPV6_ADDR = 11022;
    static final int RIL_UNSOL_NWK_INIT_DISC_REQUEST = 11023;
    static final int RIL_UNSOL_RTS_INDICATION = 11024;
    static final int RIL_UNSOL_OMADM_SEND_DATA = 11025;
    static final int RIL_UNSOL_DUN = 11026;
    static final int RIL_UNSOL_SYSTEM_REBOOT = 11027;
    static final int RIL_UNSOL_VOICE_PRIVACY_CHANGED = 11028;
    static final int RIL_UNSOL_UTS_GETSMSCOUNT = 11029;
    static final int RIL_UNSOL_UTS_GETSMSMSG = 11030;
    static final int RIL_UNSOL_UTS_GET_UNREAD_SMS_STATUS = 11031;
    static final int RIL_UNSOL_MIP_CONNECT_STATUS = 11032;

    private Object mCatProCmdBuffer;
    /* private Message mPendingGetSimStatus; */

    public SamsungExynos4RIL(Context context, int networkMode, int cdmaSubscription, Integer instanceId) {
        super(context, networkMode, cdmaSubscription, instanceId);
    }

/*    @Override
    public void setRadioPower(boolean on, Message result) {
        IRadio radioProxy = getRadioProxy(result);
        if (radioProxy != null) {
            RILRequest rr = obtainRequest(RIL_REQUEST_RADIO_POWER, result,
                    mRILDefaultWorkSource);

            if (RILJ_LOGD) {
                riljLog(rr.serialString() + "> " + requestToString(rr.mRequest)
                        + " on = " + on);
            }

            try {
                radioProxy.setRadioPower(rr.mSerial, on);
            } caREQUESTtch (RemoteException | RuntimeException e) {
                handleRadioProxyExceptionForRR(rr, "setRadioPower", e);
            }
        }
    }
*/


    @Override
    protected RILRequest processResponse(RadioResponseInfo responseInfo) {
        int serial = responseInfo.serial;
        int error = responseInfo.error;
        int type = responseInfo.type;
Rlog.w(RILJ_LOG_TAG, "EXYNOS4RIL: " + serial);
        RILRequest rr = null;

        if (type == RadioResponseType.SOLICITED_ACK) {
            synchronized (mRequestList) {
                rr = mRequestList.get(serial);
            }
            if (rr == null) {
                Rlog.w(RILJ_LOG_TAG, "Unexpected solicited ack response! sn: " + serial);
            } else {
                decrementWakeLock(rr);
                if (RILJ_LOGD) {
                    riljLog(rr.serialString() + " Ack < " + requestToString(rr.mRequest));
                }
            }
            return rr;
        }

        rr = findAndRemoveRequestFromList(serial);
        if (rr == null) {
            Rlog.e(RIL.RILJ_LOG_TAG, "processResponse: Unexpected response! serial: " + serial
                    + " error: " + error);
            return null;
        }


        switch (rr.mRequest) {
            case RIL_REQUEST_GET_SIM_STATUS:
            case RIL_REQUEST_ENTER_SIM_PIN:
            case RIL_REQUEST_ENTER_SIM_PUK: 
            case RIL_REQUEST_ENTER_SIM_PIN2:
            case RIL_REQUEST_ENTER_SIM_PUK2:
            case RIL_REQUEST_CHANGE_SIM_PIN:
            case RIL_REQUEST_CHANGE_SIM_PIN2:
            case RIL_REQUEST_ENTER_NETWORK_DEPERSONALIZATION:
            case RIL_REQUEST_GET_CURRENT_CALLS:
            case RIL_REQUEST_DIAL:
            case RIL_REQUEST_DIAL_EMERGENCY:
            case RIL_REQUEST_GET_IMSI:
            case RIL_REQUEST_HANGUP:
            case RIL_REQUEST_HANGUP_WAITING_OR_BACKGROUND:
            case RIL_REQUEST_HANGUP_FOREGROUND_RESUME_BACKGROUND:
            case RIL_REQUEST_SWITCH_WAITING_OR_HOLDING_AND_ACTIVE:
            case RIL_REQUEST_CONFERENCE:
            case RIL_REQUEST_UDUB:
            case RIL_REQUEST_LAST_CALL_FAIL_CAUSE:
            case RIL_REQUEST_SIGNAL_STRENGTH:
            case RIL_REQUEST_VOICE_REGISTRATION_STATE:
            case RIL_REQUEST_DATA_REGISTRATION_STATE:
            case RIL_REQUEST_OPERATOR:
            case RIL_REQUEST_RADIO_POWER:
            case RIL_REQUEST_DTMF:
            case RIL_REQUEST_SEND_SMS:
            case RIL_REQUEST_SEND_SMS_EXPECT_MORE:
            case RIL_REQUEST_SETUP_DATA_CALL:
            case RIL_REQUEST_SIM_IO:
            case RIL_REQUEST_SEND_USSD:
            case RIL_REQUEST_CANCEL_USSD:
            case RIL_REQUEST_GET_CLIR:
            case RIL_REQUEST_SET_CLIR:
            case RIL_REQUEST_QUERY_CALL_FORWARD_STATUS:
            case RIL_REQUEST_SET_CALL_FORWARD:
            case RIL_REQUEST_QUERY_CALL_WAITING:
            case RIL_REQUEST_SET_CALL_WAITING:
            case RIL_REQUEST_SMS_ACKNOWLEDGE:
            case RIL_REQUEST_GET_IMEI:
            case RIL_REQUEST_GET_IMEISV:
            case RIL_REQUEST_ANSWER:
            case RIL_REQUEST_DEACTIVATE_DATA_CALL:
            case RIL_REQUEST_QUERY_FACILITY_LOCK:
            case RIL_REQUEST_SET_FACILITY_LOCK:
            case RIL_REQUEST_CHANGE_BARRING_PASSWORD:
            case RIL_REQUEST_QUERY_NETWORK_SELECTION_MODE:
            case RIL_REQUEST_SET_NETWORK_SELECTION_AUTOMATIC:
            case RIL_REQUEST_SET_NETWORK_SELECTION_MANUAL:
            case RIL_REQUEST_QUERY_AVAILABLE_NETWORKS:
            case RIL_REQUEST_DTMF_START:
            case RIL_REQUEST_DTMF_STOP:
            case RIL_REQUEST_BASEBAND_VERSION:
            case RIL_REQUEST_SEPARATE_CONNECTION:
            case RIL_REQUEST_SET_MUTE:
            case RIL_REQUEST_GET_MUTE:
            case RIL_REQUEST_QUERY_CLIP:
            case RIL_REQUEST_LAST_DATA_CALL_FAIL_CAUSE:
            case RIL_REQUEST_DATA_CALL_LIST:
            case RIL_REQUEST_RESET_RADIO:
            case RIL_REQUEST_OEM_HOOK_RAW:
            case RIL_REQUEST_OEM_HOOK_STRINGS:
            case RIL_REQUEST_SCREEN_STATE:
            case RIL_REQUEST_SET_SUPP_SVC_NOTIFICATION:
            case RIL_REQUEST_WRITE_SMS_TO_SIM:
            case RIL_REQUEST_DELETE_SMS_ON_SIM:
            case RIL_REQUEST_SET_BAND_MODE:
            case RIL_REQUEST_QUERY_AVAILABLE_BAND_MODE:
            case RIL_REQUEST_STK_GET_PROFILE:
            case RIL_REQUEST_STK_SET_PROFILE:
            case RIL_REQUEST_STK_SEND_ENVELOPE_COMMAND:
            case RIL_REQUEST_STK_SEND_TERMINAL_RESPONSE:
            case RIL_REQUEST_STK_HANDLE_CALL_SETUP_REQUESTED_FROM_SIM:
            case RIL_REQUEST_EXPLICIT_CALL_TRANSFER:
            case RIL_REQUEST_SET_PREFERRED_NETWORK_TYPE:
            case RIL_REQUEST_GET_PREFERRED_NETWORK_TYPE:
            case RIL_REQUEST_GET_NEIGHBORING_CELL_IDS:
            case RIL_REQUEST_SET_LOCATION_UPDATES:
            case RIL_REQUEST_CDMA_SET_SUBSCRIPTION_SOURCE:
            case RIL_REQUEST_CDMA_SET_ROAMING_PREFERENCE:
            case RIL_REQUEST_CDMA_QUERY_ROAMING_PREFERENCE:
            case RIL_REQUEST_SET_TTY_MODE:
            case RIL_REQUEST_QUERY_TTY_MODE:
            case RIL_REQUEST_CDMA_SET_PREFERRED_VOICE_PRIVACY_MODE:
            case RIL_REQUEST_CDMA_QUERY_PREFERRED_VOICE_PRIVACY_MODE:
            case RIL_REQUEST_CDMA_FLASH:
            case RIL_REQUEST_CDMA_BURST_DTMF:
            case RIL_REQUEST_CDMA_SEND_SMS:
            case RIL_REQUEST_CDMA_SMS_ACKNOWLEDGE:
            case RIL_REQUEST_GSM_GET_BROADCAST_CONFIG:
            case RIL_REQUEST_GSM_SET_BROADCAST_CONFIG:
            case RIL_REQUEST_GSM_BROADCAST_ACTIVATION:
            case RIL_REQUEST_CDMA_GET_BROADCAST_CONFIG:
            case RIL_REQUEST_CDMA_SET_BROADCAST_CONFIG:
            case RIL_REQUEST_CDMA_BROADCAST_ACTIVATION:
            case RIL_REQUEST_CDMA_VALIDATE_AND_WRITE_AKEY:
            case RIL_REQUEST_CDMA_SUBSCRIPTION:
            case RIL_REQUEST_CDMA_WRITE_SMS_TO_RUIM:
            case RIL_REQUEST_CDMA_DELETE_SMS_ON_RUIM:
            case RIL_REQUEST_DEVICE_IDENTITY:
            case RIL_REQUEST_GET_SMSC_ADDRESS:
            case RIL_REQUEST_SET_SMSC_ADDRESS:
            case RIL_REQUEST_EXIT_EMERGENCY_CALLBACK_MODE:
            case RIL_REQUEST_REPORT_SMS_MEMORY_STATUS:
            case RIL_REQUEST_REPORT_STK_SERVICE_IS_RUNNING:
            case RIL_REQUEST_CDMA_GET_SUBSCRIPTION_SOURCE:
            case RIL_REQUEST_ISIM_AUTHENTICATION:
            case RIL_REQUEST_ACKNOWLEDGE_INCOMING_GSM_SMS_WITH_PDU:
            case RIL_REQUEST_STK_SEND_ENVELOPE_WITH_STATUS:
            case RIL_REQUEST_VOICE_RADIO_TECH:
            case RIL_REQUEST_GET_CELL_INFO_LIST:
            case RIL_REQUEST_SET_UNSOL_CELL_INFO_LIST_RATE:
            case RIL_REQUEST_SET_INITIAL_ATTACH_APN:
            case RIL_REQUEST_SET_DATA_PROFILE:
            case RIL_REQUEST_IMS_REGISTRATION_STATE:
            case RIL_REQUEST_IMS_SEND_SMS:
            case RIL_REQUEST_SIM_TRANSMIT_APDU_BASIC:
            case RIL_REQUEST_SIM_OPEN_CHANNEL:
            case RIL_REQUEST_SIM_CLOSE_CHANNEL:
            case RIL_REQUEST_SIM_TRANSMIT_APDU_CHANNEL:
//            case RIL_REQUEST_SIM_GET_ATTR:
            case RIL_REQUEST_NV_READ_ITEM:
            case RIL_REQUEST_NV_WRITE_ITEM:
            case RIL_REQUEST_NV_WRITE_CDMA_PRL:
            case RIL_REQUEST_NV_RESET_CONFIG:
            case RIL_REQUEST_SET_UICC_SUBSCRIPTION:
            case RIL_REQUEST_ALLOW_DATA:
            case RIL_REQUEST_GET_HARDWARE_CONFIG:
            case RIL_REQUEST_SIM_AUTHENTICATION:
            case RIL_REQUEST_SHUTDOWN:
            case RIL_REQUEST_GET_RADIO_CAPABILITY:
            case RIL_REQUEST_SET_RADIO_CAPABILITY:
            case RIL_REQUEST_START_LCE:
            case RIL_REQUEST_STOP_LCE:
            case RIL_REQUEST_PULL_LCEDATA:
            case RIL_REQUEST_GET_ACTIVITY_INFO:
                riljLog(rr.serialString() + " - SUPPORTED! "
                        + requestToString(rr.mRequest));
                break;
            default:
                riljLog(rr.serialString() + " - NOT SUPPORTED! "
                        + requestToString(rr.mRequest));
                return null;
        }

        // Time logging for RIL command and storing it in TelephonyHistogram.
        addToRilHistogram(rr);

        if (type == RadioResponseType.SOLICITED_ACK_EXP) {
            sendAck();
            if (RIL.RILJ_LOGD) {
                riljLog("Response received for " + rr.serialString() + " "
                        + RIL.requestToString(rr.mRequest) + " Sending ack to ril.cpp");
            }
        } else {
            // ack sent for SOLICITED_ACK_EXP above; nothing to do for SOLICITED response
        }

        // Here and below fake RIL_UNSOL_RESPONSE_SIM_STATUS_CHANGED, see b/7255789.
        // This is needed otherwise we don't automatically transition to the main lock
        // screen when the pin or puk is entered incorrectly.
        switch (rr.mRequest) {
            case RIL_REQUEST_ENTER_SIM_PUK:
            case RIL_REQUEST_ENTER_SIM_PUK2:
                if (mIccStatusChangedRegistrants != null) {
                    if (RILJ_LOGD) {
                        riljLog("ON enter sim puk fakeSimStatusChanged: reg count="
                                + mIccStatusChangedRegistrants.size());
                    }
                    mIccStatusChangedRegistrants.notifyRegistrants();
                }
                break;
            case RIL_REQUEST_SHUTDOWN:
                setRadioState(RadioState.RADIO_UNAVAILABLE);
                break;
        }

        if (error != RadioError.NONE) {
            switch (rr.mRequest) {
                case RIL_REQUEST_ENTER_SIM_PIN:
                case RIL_REQUEST_ENTER_SIM_PIN2:
                case RIL_REQUEST_CHANGE_SIM_PIN:
                case RIL_REQUEST_CHANGE_SIM_PIN2:
                case RIL_REQUEST_SET_FACILITY_LOCK:
                    if (mIccStatusChangedRegistrants != null) {
                        if (RILJ_LOGD) {
                            riljLog("ON some errors fakeSimStatusChanged: reg count="
                                    + mIccStatusChangedRegistrants.size());
                        }
                        mIccStatusChangedRegistrants.notifyRegistrants();
                    }
                    break;
/*
MOVED TO ril_services.cpp
		case RIL_REQUEST_GET_RADIO_CAPABILITY: {
                    // Ideally RIL's would support this or at least give NOT_SUPPORTED
                    // but the hammerhead RIL reports GENERIC :(
                    // TODO - remove GENERIC_FAILURE catching: b/21079604
                    if (REQUEST_NOT_SUPPORTED == error ||
                            GENERIC_FAILURE == error) {
                        // we should construct the RAF bitmask the radio
                        // supports based on preferred network bitmasks
                        ret = makeStaticRadioCapability();
                        error = 0;
                    }
                    break;
                }
*/
            }
        } else {
            switch (rr.mRequest) {
                case RIL_REQUEST_HANGUP_FOREGROUND_RESUME_BACKGROUND:
                if (mTestingEmergencyCall.getAndSet(false)) {
                    if (mEmergencyCallbackModeRegistrant != null) {
                        riljLog("testing emergency call, notify ECM Registrants");
                        mEmergencyCallbackModeRegistrant.notifyRegistrant();
                    }
                }
            }
        }
        return rr;
    }

    @Override
    protected void processResponseDone(RILRequest rr, RadioResponseInfo responseInfo, Object ret) {
Rlog.w(RILJ_LOG_TAG, "EXYNOS4RIL-processResponseDone");

        if (responseInfo.error == 0) {
            if (RILJ_LOGD) {
                riljLog(rr.serialString() + "< " + requestToString(rr.mRequest)
                        + " " + retToString(rr.mRequest, ret));
            }
        } else {
            if (RILJ_LOGD) {
                riljLog(rr.serialString() + "< " + requestToString(rr.mRequest)
                        + " error " + responseInfo.error);
            }
            rr.onError(responseInfo.error, ret);
        }
        mMetrics.writeOnRilSolicitedResponse(mPhoneId, rr.mSerial, responseInfo.error,
                rr.mRequest, ret);
        if (rr != null) {
            if (responseInfo.type == RadioResponseType.SOLICITED) {
                decrementWakeLock(rr);
            }
            rr.release();
        }
    }
}
