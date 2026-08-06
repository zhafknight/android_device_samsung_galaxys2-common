#include <android/api-level.h>
#include "secril-shim.h"
#include "secril-sap.h"

#define ATOI_NULL_HANDLED(x) (x ? atoi(x) : 0)

/*
 * Compatibility values for legacy Samsung RIL blobs.
 *
 * Modern ril.h removed radio states 2 through 9, but old Samsung
 * libsec-ril blobs still return these numeric values.
 */
static constexpr RIL_RadioState RADIO_STATE_SIM_NOT_READY =
        static_cast<RIL_RadioState>(2);
static constexpr RIL_RadioState RADIO_STATE_SIM_LOCKED_OR_ABSENT =
        static_cast<RIL_RadioState>(3);
static constexpr RIL_RadioState RADIO_STATE_SIM_READY =
        static_cast<RIL_RadioState>(4);
static constexpr RIL_RadioState RADIO_STATE_RUIM_NOT_READY =
        static_cast<RIL_RadioState>(5);
static constexpr RIL_RadioState RADIO_STATE_RUIM_READY =
        static_cast<RIL_RadioState>(6);
static constexpr RIL_RadioState RADIO_STATE_RUIM_LOCKED_OR_ABSENT =
        static_cast<RIL_RadioState>(7);
static constexpr RIL_RadioState RADIO_STATE_NV_NOT_READY =
        static_cast<RIL_RadioState>(8);
static constexpr RIL_RadioState RADIO_STATE_NV_READY =
        static_cast<RIL_RadioState>(9);


/* A copy of the original RIL function table. */
static const RIL_RadioFunctions *origRilFunctions;

/* A copy of the ril environment passed to RIL_Init. */
static const struct RIL_Env *rilEnv;

/* Response data for RIL_REQUEST_VOICE_REGISTRATION_STATE.
 *
 * hardware/samsung/ril expects exactly 18 legacy char pointers:
 *   0..13 legacy registration fields
 *   14    PSC
 *   15    MCC
 *   16    MNC
 *   17    registered PLMN
 */
static const size_t VOICE_REGSTATE_NUM_STRINGS = 18;
static char *voiceRegStateResponse[VOICE_REGSTATE_NUM_STRINGS];
static char voiceRadioTechUmts[] = "3";

/* Store voice radio technology */
static int voiceRadioTechnology = -1;

/* Store cdma subscription source */
static int cdmaSubscriptionSource = -1;

/* Store sim ruim status */
int simRuimStatus = -1;

/* Store SIM PIN attempts */
int simPinAttempts = 3;

/*
 * Old Samsung RIL card status responses do not include an ICCID.
 * Cache EF_ICCID after the framework reads it through SIM_IO, then expose
 * it through RIL_CardStatus_v1_2 so UiccSlot can create a subscription.
 */
static const size_t ICCID_MAX_DIGITS = 32;
static char cachedIccid[ICCID_MAX_DIGITS] = {};
static bool gotIccid = false;
static RIL_Token pendingIccidReadToken = NULL;

/*
 * Legacy SETUP_DATA_CALL values expected by the Exynos4 Samsung blob.
 * Modern radio HAL adapters use "2" for UNKNOWN+2 and may supply profile -1,
 * but the old GSM/UMTS path historically uses technology 1 and profile 0.
 */
static char setupDataTechGsm[] = "1";
static char setupDataProfileDefault[] = "0";

/*
 * The device-local legacy ril.h ends at RIL_CardStatus_v6, while the
 * hardware/samsung libril consumer also accepts the v1.2 payload layout.
 * Keep a local ABI-compatible definition instead of including two different
 * ril.h headers in the same translation unit.
 */
typedef struct {
    RIL_CardStatus_v6 base;
    uint32_t physicalSlotId;
    char *atr;
    char *iccid;
} RIL_CardStatus_v1_2_compat;

/* Variables and methods for RIL_REQUEST_DEVICE_IDENTITY support */
static char imei[16];
static char imeisv[17];
static bool gotIMEI = false;
static bool gotIMEISV = false;
static bool inIMEIRequest = false;
static bool inIMEISVRequest = false;
static int requestForIMEI = 0;
static int requestForIMEISV = 0;

static bool onRequestSpoofUnsupportedRequest(int request, void *data, size_t datalen, RIL_Token t);
static void onRequestDeviceIdentity(int request, RIL_Token t);


/* Response data for RIL_REQUEST_GET_CELL_INFO_LIST */
static RIL_CellInfo_v12 cellInfoWCDMA;
static RIL_CellInfo_v12 cellInfoGSM;
static RIL_CellInfo_v12 cellInfoList[2];

static RIL_Dial dial;

static void onRequestDial(int request, void *data, RIL_Token t) {
	RIL_UUS_Info uusInfo;

	dial.address = ((RIL_Dial *) data)->address;
	dial.clir = ((RIL_Dial *) data)->clir;
	dial.uusInfo = ((RIL_Dial *) data)->uusInfo;

	if (dial.uusInfo == NULL) {
		uusInfo.uusType = (RIL_UUS_Type) 0;
		uusInfo.uusDcs = (RIL_UUS_DCS) 0;
		uusInfo.uusData = NULL;
		uusInfo.uusLength = 0;
		dial.uusInfo = &uusInfo;
	}

	origRilFunctions->onRequest(request, &dial, sizeof(dial), t);
}


static int
decodeVoiceRadioTechnology (RIL_RadioState radioState) {
    switch (radioState) {
        case RADIO_STATE_SIM_NOT_READY:
        case RADIO_STATE_SIM_LOCKED_OR_ABSENT:
        case RADIO_STATE_SIM_READY:
            return RADIO_TECH_UMTS;

        case RADIO_STATE_RUIM_NOT_READY:
        case RADIO_STATE_RUIM_READY:
        case RADIO_STATE_RUIM_LOCKED_OR_ABSENT:
        case RADIO_STATE_NV_NOT_READY:
        case RADIO_STATE_NV_READY:
            return RADIO_TECH_1xRTT;

        default:
            RLOGD("decodeVoiceRadioTechnology: Invoked with incorrect RadioState");
            return -1;
    }
}

static void OnRequestGetCellInfoList(int request, void *data, size_t datalen, RIL_Token t) {
	RLOGI("%s: got request %s (data:%p datalen:%d)\n", __FUNCTION__,
		requestToString(request),
		data, datalen);

	cellInfoWCDMA.cellInfoType = RIL_CELL_INFO_TYPE_WCDMA;
	cellInfoWCDMA.CellInfo.wcdma.cellIdentityWcdma.mcc = -1;
	cellInfoWCDMA.CellInfo.wcdma.cellIdentityWcdma.mnc = -1;
	cellInfoWCDMA.CellInfo.wcdma.cellIdentityWcdma.psc = -1;

	cellInfoGSM.cellInfoType = RIL_CELL_INFO_TYPE_GSM;
	cellInfoGSM.CellInfo.gsm.cellIdentityGsm.mcc = -1;
	cellInfoGSM.CellInfo.gsm.cellIdentityGsm.mnc = -1;

	if (cellInfoGSM.CellInfo.gsm.cellIdentityGsm.lac > -1 &&
	    cellInfoGSM.CellInfo.gsm.cellIdentityGsm.cid > -1) {
		cellInfoList[0] = cellInfoGSM;
		cellInfoList[1] = cellInfoWCDMA;
		rilEnv->OnRequestComplete(t, RIL_E_SUCCESS, &cellInfoList, sizeof(cellInfoList));
	} else {
		rilEnv->OnRequestComplete(t, RIL_E_SUCCESS, &cellInfoWCDMA, sizeof(cellInfoWCDMA));
	}
}

static void onRequestVoiceRadioTech(int request, void *data, size_t datalen, RIL_Token t) {
	RLOGI("%s: got request %s (data:%p datalen:%d)\n", __FUNCTION__,
		requestToString(request),
		data, datalen);
        RIL_RadioState radioState = origRilFunctions->onStateRequest();

	voiceRadioTechnology = decodeVoiceRadioTechnology(radioState);
	if (voiceRadioTechnology < 0) {
		rilEnv->OnRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
		return;
	}
	rilEnv->OnRequestComplete(t, RIL_E_SUCCESS, &voiceRadioTechnology, sizeof(voiceRadioTechnology));
}

static int
decodeCdmaSubscriptionSource (RIL_RadioState radioState) {
    switch (radioState) {
        case RADIO_STATE_SIM_NOT_READY:
        case RADIO_STATE_SIM_LOCKED_OR_ABSENT:
        case RADIO_STATE_SIM_READY:
        case RADIO_STATE_RUIM_NOT_READY:
        case RADIO_STATE_RUIM_READY:
        case RADIO_STATE_RUIM_LOCKED_OR_ABSENT:
            return CDMA_SUBSCRIPTION_SOURCE_RUIM_SIM;

        case RADIO_STATE_NV_NOT_READY:
        case RADIO_STATE_NV_READY:
            return CDMA_SUBSCRIPTION_SOURCE_NV;

        default:
            RLOGD("decodeCdmaSubscriptionSource: Invoked with incorrect RadioState");
            return -1;
    }
}

static void onRequestCdmaGetSubscriptionSource(int request, void *data, size_t datalen, RIL_Token t) {
	RLOGI("%s: got request %s (data:%p datalen:%d)\n", __FUNCTION__,
		requestToString(request),
		data, datalen);
        RIL_RadioState radioState = (RIL_RadioState)origRilFunctions->onStateRequest();

	cdmaSubscriptionSource = decodeCdmaSubscriptionSource(radioState);
	if (cdmaSubscriptionSource < 0) {
		rilEnv->OnRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
		return;
	}
	rilEnv->OnRequestComplete(t, RIL_E_SUCCESS, &cdmaSubscriptionSource, sizeof(cdmaSubscriptionSource));
}

static void onRequestDeviceIdentity(int request, RIL_Token t) {
	RIL_Errno e = (gotIMEI && gotIMEISV) ? RIL_E_SUCCESS : RIL_E_GENERIC_FAILURE;

	char empty[1] = "";
	char *deviceIdentityResponse[4];
	deviceIdentityResponse[0] = imei;
	deviceIdentityResponse[1] = imeisv;
	deviceIdentityResponse[2] = empty;
	deviceIdentityResponse[3] = empty;

	RLOGD("%s:\t\t\t<<< REQUEST-COMPLETE: %s: (data:%p datalen:%d token:%p error:%d) \n", __FUNCTION__, requestToString(request),
		deviceIdentityResponse,
		sizeof(deviceIdentityResponse),
		t,
		e);

	rilEnv->OnRequestComplete(t, e, deviceIdentityResponse, sizeof(deviceIdentityResponse));
}

static bool onRequestEnterSimPin(int request, void *data, size_t datalen, RIL_Token t) {
	int length = (int)datalen/ sizeof(char *);
	if (length == 2) {
		char **field = (char **) data;
		char *pin = field[0];
		if (pin == NULL) {
			RLOGD("%s: got request %s: Simulating remaining attempts of %d\n", __FUNCTION__, requestToString(request), simPinAttempts);
			rilEnv->OnRequestComplete(t, RIL_E_SUCCESS, &simPinAttempts, sizeof(simPinAttempts));
			return true;
		}
	}
	return false;
}

static bool onRequestSpoofUnsupportedRequest(int request, void *data, size_t datalen, RIL_Token t) {
	bool handled = false;
	RequestInfo *pRI = (RequestInfo *)t;
	if (pRI != NULL && pRI->pCI != NULL) {
		if (!gotIMEI && !inIMEIRequest) {
			// Use this unsupported request to extract IMEI
			inIMEIRequest = true;
			requestForIMEI = request;
			RLOGI("%s: >>> REQUEST\t\t\t: %s (RIL_REQUEST_DEVICE_IDENTITY [1/6]): Using this unsupported request to extract IMEI in preparation for upcoming RIL_REQUEST_DEVICE_IDENTITY\n", __FUNCTION__, requestToString(requestForIMEI));
			pRI->pCI->requestNumber = RIL_REQUEST_GET_IMEI;
			RLOGI("%s: >>> REQUEST\t\t\t: %s (RIL_REQUEST_DEVICE_IDENTITY [2/6])", __FUNCTION__, requestToString(pRI->pCI->requestNumber)); 
			origRilFunctions->onRequest(pRI->pCI->requestNumber, NULL, 0, t);
			handled = true;
		} else if (!gotIMEISV && !inIMEISVRequest) {
			// Use this unsupported request to extract IMEISV
			inIMEISVRequest = true;
			requestForIMEISV = request;
			RLOGI("%s: >>> REQUEST\t\t\t: %s (RIL_REQUEST_DEVICE_IDENTITY [4/6]): Using this unsupported request to extract IMEISV in preparation for upcoming RIL_REQUEST_DEVICE_IDENTITY\n", __FUNCTION__, requestToString(requestForIMEISV));
			pRI->pCI->requestNumber = RIL_REQUEST_GET_IMEISV;
			RLOGI("%s: >>> REQUEST\t\t\t: %s (RIL_REQUEST_DEVICE_IDENTITY [5/6])", __FUNCTION__, requestToString(pRI->pCI->requestNumber)); 
			origRilFunctions->onRequest(pRI->pCI->requestNumber, NULL, 0, t);
			handled = true;
		}
	}
	return handled;
}
static void onRequestUnsupportedRequest(int request, RIL_Token t) {
	RLOGE("%s:\t\t<<< REQUEST-COMPLETE: %s: (token:%p): Request not send to RIL. Sending REQUEST_NOT_SUPPORTED back to libril.\n",
		__FUNCTION__,
		requestToString(request),
		t);
	rilEnv->OnRequestComplete(t, RIL_E_REQUEST_NOT_SUPPORTED, NULL, 0);
}


static bool is3gpp2(int radioTech) {
    switch (radioTech) {
        case RADIO_TECH_IS95A:
        case RADIO_TECH_IS95B:
        case RADIO_TECH_1xRTT:
        case RADIO_TECH_EVDO_0:
        case RADIO_TECH_EVDO_A:
        case RADIO_TECH_EVDO_B:
        case RADIO_TECH_EHRPD:
            return true;
        default:
            return false;
    }
}

static int
decodeSimStatus (RIL_RadioState radioState) {
   switch (radioState) {
       case RADIO_STATE_SIM_NOT_READY:
       case RADIO_STATE_RUIM_NOT_READY:
       case RADIO_STATE_NV_NOT_READY:
       case RADIO_STATE_NV_READY:
           return -1;
       case RADIO_STATE_SIM_LOCKED_OR_ABSENT:
       case RADIO_STATE_SIM_READY:
       case RADIO_STATE_RUIM_READY:
       case RADIO_STATE_RUIM_LOCKED_OR_ABSENT:
           return radioState;
       default:
           RLOGD("decodeSimStatus: Invoked with incorrect RadioState");
           return -1;
   }
}

static RIL_RadioState
processRadioState(RIL_RadioState newRadioState) {
    if((newRadioState > RADIO_STATE_UNAVAILABLE) && (newRadioState < RADIO_STATE_ON)) {
        int newVoiceRadioTech;
        int newCdmaSubscriptionSource;
        int newSimStatus;

        /* This is old RIL. Decode Subscription source and Voice Radio Technology
           from Radio State and send change notifications if there has been a change */
        newVoiceRadioTech = decodeVoiceRadioTechnology(newRadioState);
        if(newVoiceRadioTech != voiceRadioTechnology) {
            voiceRadioTechnology = newVoiceRadioTech;
            rilEnv->OnUnsolicitedResponse(RIL_UNSOL_VOICE_RADIO_TECH_CHANGED,
                &voiceRadioTechnology, sizeof(voiceRadioTechnology));
        }
        if(is3gpp2(newVoiceRadioTech)) {
            newCdmaSubscriptionSource = decodeCdmaSubscriptionSource(newRadioState);
            if(newCdmaSubscriptionSource != cdmaSubscriptionSource) {
                cdmaSubscriptionSource = newCdmaSubscriptionSource;
                rilEnv->OnUnsolicitedResponse(RIL_UNSOL_CDMA_SUBSCRIPTION_SOURCE_CHANGED,
                        &cdmaSubscriptionSource, sizeof(cdmaSubscriptionSource));
            }
        }
        newSimStatus = decodeSimStatus(newRadioState);
        if(newSimStatus != simRuimStatus) {
            simRuimStatus = newSimStatus;
            rilEnv->OnUnsolicitedResponse(RIL_UNSOL_RESPONSE_SIM_STATUS_CHANGED, NULL, 0);
        }

        /* Send RADIO_ON to telephony */
        newRadioState = RADIO_STATE_ON;
    }

    return newRadioState;
}

static bool onRequestGetRadioCapability(RIL_Token t)
{
	RIL_RadioCapability rc[1] =
	{
		{ /* rc[0] */
			RIL_RADIO_CAPABILITY_VERSION, /* version */
			0, /* session */
			RC_PHASE_CONFIGURED, /* phase */
			RAF_GSM | RAF_GPRS | RAF_EDGE | RAF_HSUPA | RAF_HSDPA | RAF_HSPA | RAF_HSPAP | RAF_UMTS, /* rat */
			{ /* logicalModemUuid */
				0,
			},
			RC_STATUS_SUCCESS /* status */
		}
	};
	rilEnv->OnRequestComplete(t, RIL_E_SUCCESS, rc, sizeof(rc));
	return true;
}

static bool onCompleteGetActivityInfo(RIL_Token t)
{
	RIL_ActivityStatsInfo stats[1];
        stats[0].sleep_mode_time_ms = 0;
	stats[0].idle_mode_time_ms = 0;
	for(int i = 0; i < RIL_NUM_TX_POWER_LEVELS; i++) {
		stats[0].tx_mode_time_ms[i] = 0;
	}
        stats[0].rx_mode_time_ms = 0;

	rilEnv->OnRequestComplete(t, RIL_E_SUCCESS, stats, sizeof(stats));
	return true;
}

static RIL_RadioState onStateRequestShim() {
    RIL_RadioState radioState = RADIO_STATE_OFF;
    RIL_RadioState newRadioState = RADIO_STATE_OFF;

    radioState = origRilFunctions->onStateRequest();
    newRadioState = processRadioState(radioState);

    RLOGI("%s: RIL legacy radiostate converted from %d to %d\n", __FUNCTION__, radioState, newRadioState);
    return newRadioState;
}

static int hexNibble(char value) {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

static bool cacheIccidFromSimIo(void *response, size_t responselen) {
    if (response == NULL || responselen < sizeof(RIL_SIM_IO_Response)) {
        return false;
    }

    RIL_SIM_IO_Response *simIo =
            static_cast<RIL_SIM_IO_Response *>(response);

    if (simIo->simResponse == NULL ||
        (simIo->sw1 != 0x90 && simIo->sw1 != 0x91 &&
         simIo->sw1 != 0x9e && simIo->sw1 != 0x9f)) {
        return false;
    }

    const char *raw = simIo->simResponse;
    const size_t rawLength = strlen(raw);
    if (rawLength == 0 || (rawLength % 2) != 0) {
        return false;
    }

    char decoded[ICCID_MAX_DIGITS] = {};
    size_t output = 0;

    /*
     * EF_ICCID is BCD encoded with the low nibble first.
     * Example: 98 06 51 ... becomes 89 60 15 ...
     */
    for (size_t index = 0;
         index + 1 < rawLength && output + 1 < sizeof(decoded);
         index += 2) {
        const int high = hexNibble(raw[index]);
        const int low = hexNibble(raw[index + 1]);
        if (high < 0 || low < 0) {
            return false;
        }

        decoded[output++] =
                static_cast<char>(low < 10 ? ('0' + low) : ('A' + low - 10));

        /* 0xF is padding in the final ICCID nibble. */
        if (high != 0xF && output + 1 < sizeof(decoded)) {
            decoded[output++] =
                    static_cast<char>(
                            high < 10 ? ('0' + high) : ('A' + high - 10));
        }
    }

    decoded[output] = '\0';
    if (output < 10) {
        return false;
    }

    strlcpy(cachedIccid, decoded, sizeof(cachedIccid));
    gotIccid = true;

    RLOGI("%s: cached EF_ICCID (%zu digits)",
            __FUNCTION__, strlen(cachedIccid));
    return true;
}

static void onRequestShim(int request, void *data, size_t datalen, RIL_Token t)
{
	RLOGD("%s:\t\t\t\t\t>>> REQUEST\t\t\t: %s: data:%p datalen:%d token:%p\n", __FUNCTION__, requestToString(request), data, datalen, t);

	switch (request) {
                case RIL_REQUEST_SIM_IO:
                        if (data != NULL &&
                            datalen >= sizeof(RIL_SIM_IO_v6)) {
                                RIL_SIM_IO_v6 *simIo =
                                        static_cast<RIL_SIM_IO_v6 *>(data);

                                if (simIo->command == 0xb0 &&
                                    simIo->fileid == 0x2fe2) {
                                        /*
                                         * Invalidate a previously cached card
                                         * before reading the current SIM.
                                         */
                                        gotIccid = false;
                                        cachedIccid[0] = '\0';
                                        pendingIccidReadToken = t;

                                        RLOGI("%s: tracking EF_ICCID "
                                              "READ_BINARY token %p",
                                                __FUNCTION__, t);
                                }
                        }
                        break;
                /* Our RIL doesn't support this, so we implement this ourself */
                case RIL_REQUEST_GET_CELL_INFO_LIST:
			OnRequestGetCellInfoList(request, data, datalen, t);
			return;
                /* Our RIL doesn't support this, so we implement this ourself */
                case RIL_REQUEST_VOICE_RADIO_TECH:
			onRequestVoiceRadioTech(request, data, datalen, t);
			return;
                /* Our RIL doesn't support this, so we implement this ourself */
                case RIL_REQUEST_CDMA_GET_SUBSCRIPTION_SOURCE:
			onRequestCdmaGetSubscriptionSource(request, data, datalen, t);
			return;
		/* RIL_REQUEST_GET_IMEI and RIL_REQUEST_GET_IMEISV is deprecated */
		case RIL_REQUEST_DEVICE_IDENTITY:
			onRequestDeviceIdentity(request, t);
			return;
                /*
                 * Current radio compatibility layers can send the extended
                 * SETUP_DATA_CALL request as 15/16 string pointers. The old
                 * Samsung Exynos4 blob accepts the legacy seven-string form:
                 *
                 *   radioTechnology, profileId, APN, user, password,
                 *   authType, protocol
                 */
                case RIL_REQUEST_SETUP_DATA_CALL:
                        if (data != NULL &&
                            datalen >= 7 * sizeof(char *) &&
                            datalen % sizeof(char *) == 0) {
                                char **args = static_cast<char **>(data);
                                /*
                                 * Do not forward the modern first two values:
                                 *
                                 *   args[0] = "2"  (UNKNOWN + 2)
                                 *   args[1] = "-1" (invalid profile)
                                 *
                                 * The legacy Exynos4 GSM/UMTS implementation
                                 * expects technology 1 and default profile 0.
                                 */
                                char *legacyArgs[7] = {
                                        setupDataTechGsm,
                                        setupDataProfileDefault,
                                        args[2],
                                        args[3],
                                        args[4],
                                        args[5],
                                        args[6],
                                };

                                RLOGI("%s: SETUP_DATA_CALL legacy conversion "
                                      "%zu -> 7 strings "
                                      "(original tech=%s profile=%s; "
                                      "forwarded tech=%s profile=%s apn=%s "
                                      "auth=%s protocol=%s)",
                                        __FUNCTION__,
                                        datalen / sizeof(char *),
                                        args[0] != NULL
                                                ? args[0] : "(null)",
                                        args[1] != NULL
                                                ? args[1] : "(null)",
                                        legacyArgs[0],
                                        legacyArgs[1],
                                        legacyArgs[2] != NULL
                                                ? legacyArgs[2] : "(null)",
                                        legacyArgs[5] != NULL
                                                ? legacyArgs[5] : "(null)",
                                        legacyArgs[6] != NULL
                                                ? legacyArgs[6] : "(null)");

                                origRilFunctions->onRequest(
                                        request,
                                        legacyArgs,
                                        sizeof(legacyArgs),
                                        t);
                                return;
                        }
                        break;

		/* The Samsung RIL crashes if uusInfo is NULL... */
		case RIL_REQUEST_DIAL:
			if (datalen == sizeof(RIL_Dial) && data != NULL) {
				onRequestDial(request, data, t);
				return;
			}
			break;

		/* Necessary; RILJ may fake this for us if we reply not supported, but we can just implement it. */
		case RIL_REQUEST_GET_RADIO_CAPABILITY:
			onRequestGetRadioCapability(t);
			return;
		/* The Samsung RIL doesn't support RIL_REQUEST_SEND_SMS_EXPECT_MORE, reply with RIL_REQUEST_SEND_SMS instead */
		case RIL_REQUEST_SEND_SMS_EXPECT_MORE:
			origRilFunctions->onRequest(RIL_REQUEST_SEND_SMS, data, datalen, t);
			return;
		case RIL_REQUEST_ENTER_SIM_PIN:
			if (onRequestEnterSimPin(request, data, datalen, t)) {
				/* The token was already completed by onRequestEnterSimPin(). */
				return;
			}
			origRilFunctions->onRequest(request, data, datalen, t);
			return;
		/* The following requests were introduced post-4.3. */
		case RIL_REQUEST_SIM_TRANSMIT_APDU_BASIC:
		case RIL_REQUEST_SIM_OPEN_CHANNEL: /* !!! */
		case RIL_REQUEST_SIM_CLOSE_CHANNEL:
		case RIL_REQUEST_SIM_TRANSMIT_APDU_CHANNEL:
		case RIL_REQUEST_NV_READ_ITEM:
		case RIL_REQUEST_NV_WRITE_ITEM:
		case RIL_REQUEST_NV_WRITE_CDMA_PRL:
		case RIL_REQUEST_NV_RESET_CONFIG:
		case RIL_REQUEST_SET_UICC_SUBSCRIPTION:
		case RIL_REQUEST_ALLOW_DATA:
		case RIL_REQUEST_GET_HARDWARE_CONFIG:
		case RIL_REQUEST_SIM_AUTHENTICATION:
		case RIL_REQUEST_GET_DC_RT_INFO:
		case RIL_REQUEST_SET_DC_RT_INFO_RATE:
		case RIL_REQUEST_SET_DATA_PROFILE:
		case RIL_REQUEST_SHUTDOWN: /* TODO: Is there something we can do for RIL_REQUEST_SHUTDOWN ? */
		case RIL_REQUEST_SET_RADIO_CAPABILITY:
		case RIL_REQUEST_START_LCE:
		case RIL_REQUEST_STOP_LCE:
		case RIL_REQUEST_PULL_LCEDATA:
			if (!onRequestSpoofUnsupportedRequest(request, data, datalen, t)) {
				onRequestUnsupportedRequest(request, t);
			}
			return;
	}

	origRilFunctions->onRequest(request, data, datalen, t);
}

static void onCompleteRequestGetSimStatus(
        RIL_Token t, RIL_Errno e, void *response) {
    RIL_CardStatus_v5_samsung *current =
            static_cast<RIL_CardStatus_v5_samsung *>(response);
    RIL_CardStatus_v1_2_compat fixed = {};

    fixed.base.card_state = current->card_state;
    fixed.base.universal_pin_state = current->universal_pin_state;
    fixed.base.gsm_umts_subscription_app_index =
            current->gsm_umts_subscription_app_index;
    fixed.base.cdma_subscription_app_index =
            current->cdma_subscription_app_index;
    fixed.base.ims_subscription_app_index = -1;
    fixed.base.num_applications = current->num_applications;
    if (fixed.base.num_applications < 0) {
        fixed.base.num_applications = 0;
    } else if (fixed.base.num_applications > RIL_CARD_MAX_APPS) {
        RLOGW("%s: invalid num_applications=%d, clamping to %d",
                __FUNCTION__,
                fixed.base.num_applications,
                RIL_CARD_MAX_APPS);
        fixed.base.num_applications = RIL_CARD_MAX_APPS;
    }

    for (int index = 0; index < fixed.base.num_applications; ++index) {
        memcpy(
                &fixed.base.applications[index],
                &current->applications[index],
                sizeof(RIL_AppStatus));
    }

    if (current->card_state != RIL_CARDSTATE_PRESENT) {
        gotIccid = false;
        cachedIccid[0] = '\0';
        pendingIccidReadToken = NULL;
    }

    fixed.physicalSlotId = 0;
    fixed.atr = NULL;
    fixed.iccid = gotIccid ? cachedIccid : NULL;

    RLOGI("%s: returning RIL_CardStatus_v1_2 "
          "physicalSlotId=0 iccidAvailable=%d",
            __FUNCTION__, gotIccid ? 1 : 0);

    rilEnv->OnRequestComplete(
            t, e, &fixed, sizeof(RIL_CardStatus_v1_2_compat));
}

static void onRequestCompleteVoiceRegistrationState(
        RIL_Token t, RIL_Errno e, void *response, size_t responselen) {
    if (response == NULL || responselen == 0 ||
        responselen % sizeof(char *) != 0) {
        rilEnv->OnRequestComplete(t, e, response, responselen);
        return;
    }

    char **resp = static_cast<char **>(response);
    const size_t inputCount = responselen / sizeof(char *);
    const size_t copyCount =
            inputCount < VOICE_REGSTATE_NUM_STRINGS
                    ? inputCount
                    : VOICE_REGSTATE_NUM_STRINGS;

    memset(voiceRegStateResponse, 0, sizeof(voiceRegStateResponse));

    /*
     * Depending on registration state, the old Samsung blob may return
     * only 3 strings (12 bytes on 32-bit) or as many as 14 strings.
     * Modern hardware/samsung/ril requires exactly 18 pointer slots.
     */
    for (size_t index = 0; index < copyCount; ++index) {
        voiceRegStateResponse[index] = resp[index];
    }

    /*
     * The Exynos4 blob omits RAT from voice registration responses.
     * Preserve the established shim behaviour and report UMTS.
     */
    if (inputCount <= 3 || voiceRegStateResponse[3] == NULL ||
        voiceRegStateResponse[3][0] == '\0') {
        voiceRegStateResponse[3] = voiceRadioTechUmts;
    }

    /*
     * LAC and CID are hexadecimal strings in the legacy RIL response.
     * Do not use atoi(), which truncates values such as "5dfa" to 5.
     */
    if (inputCount > 1 && resp[1] != NULL && resp[1][0] != '\0') {
        cellInfoWCDMA.CellInfo.wcdma.cellIdentityWcdma.lac =
                strtol(resp[1], NULL, 16);
    }

    if (inputCount > 2 && resp[2] != NULL && resp[2][0] != '\0') {
        cellInfoWCDMA.CellInfo.wcdma.cellIdentityWcdma.cid =
                strtol(resp[2], NULL, 16);
    }

    RLOGI("%s: expanded voice registration response from %zu to %zu strings "
          "(regState=%s lac=%s cid=%s rat=%s)",
            __FUNCTION__,
            inputCount,
            VOICE_REGSTATE_NUM_STRINGS,
            inputCount > 0 && resp[0] != NULL ? resp[0] : "(null)",
            inputCount > 1 && resp[1] != NULL ? resp[1] : "(null)",
            inputCount > 2 && resp[2] != NULL ? resp[2] : "(null)",
            voiceRegStateResponse[3] != NULL
                    ? voiceRegStateResponse[3]
                    : "(null)");

    rilEnv->OnRequestComplete(
            t,
            e,
            voiceRegStateResponse,
            sizeof(voiceRegStateResponse));
}

static bool parseHexCellId(const char *value, int *result) {
    if (value == NULL || value[0] == '\0' || result == NULL) {
        return false;
    }

    char *end = NULL;
    const long parsed = strtol(value, &end, 16);
    if (end == value || *end != '\0' || parsed < 0 || parsed > INT_MAX) {
        return false;
    }

    *result = static_cast<int>(parsed);
    return true;
}

static bool isGsmFamilyRadioTech(int radioTech) {
    return radioTech == RADIO_TECH_GPRS ||
           radioTech == RADIO_TECH_EDGE ||
           radioTech == RADIO_TECH_GSM;
}

static bool isWcdmaFamilyRadioTech(int radioTech) {
    return radioTech == RADIO_TECH_UMTS ||
           radioTech == RADIO_TECH_HSDPA ||
           radioTech == RADIO_TECH_HSUPA ||
           radioTech == RADIO_TECH_HSPA ||
           radioTech == RADIO_TECH_HSPAP;
}

static void onRequestCompleteDataRegistrationState(
        RIL_Token t, RIL_Errno e, void *response, size_t responselen) {
    if (response == NULL || responselen < 4 * sizeof(char *) ||
        responselen % sizeof(char *) != 0) {
        rilEnv->OnRequestComplete(t, e, response, responselen);
        return;
    }

    char **resp = static_cast<char **>(response);
    const int radioTech = ATOI_NULL_HANDLED(resp[3]);
    int lac = -1;
    int cid = -1;

    if (!parseHexCellId(resp[1], &lac) || !parseHexCellId(resp[2], &cid)) {
        rilEnv->OnRequestComplete(t, e, response, responselen);
        return;
    }

    /* Cache cell identity only; do not rewrite registration state or RAT. */
    if (isGsmFamilyRadioTech(radioTech)) {
        cellInfoGSM.CellInfo.gsm.cellIdentityGsm.lac = lac;
        cellInfoGSM.CellInfo.gsm.cellIdentityGsm.cid = cid;
        cellInfoWCDMA.CellInfo.wcdma.cellIdentityWcdma.lac = -1;
        cellInfoWCDMA.CellInfo.wcdma.cellIdentityWcdma.cid = -1;
    } else if (isWcdmaFamilyRadioTech(radioTech)) {
        cellInfoGSM.CellInfo.gsm.cellIdentityGsm.lac = -1;
        cellInfoGSM.CellInfo.gsm.cellIdentityGsm.cid = -1;
        cellInfoWCDMA.CellInfo.wcdma.cellIdentityWcdma.lac = lac;
        cellInfoWCDMA.CellInfo.wcdma.cellIdentityWcdma.cid = cid;
    }

    rilEnv->OnRequestComplete(t, e, response, responselen);
}

static bool copyRilStringResponse(
        char *destination,
        size_t destinationSize,
        RIL_Errno e,
        const void *response,
        size_t responselen) {
    if (destination == NULL || destinationSize == 0 ||
        e != RIL_E_SUCCESS || response == NULL || responselen == 0) {
        return false;
    }

    const char *source = static_cast<const char *>(response);
    const size_t sourceLength = strnlen(source, responselen);
    if (sourceLength == 0) {
        return false;
    }

    const size_t copyLength =
            sourceLength < destinationSize - 1
                    ? sourceLength
                    : destinationSize - 1;
    memcpy(destination, source, copyLength);
    destination[copyLength] = '\0';
    return true;
}

static void onRequestCompleteGetImei(
        RIL_Token t, RIL_Errno e, void *response, size_t responselen) {
    gotIMEI = copyRilStringResponse(
            imei, sizeof(imei), e, response, responselen);
    RLOGI("%s: %s: IMEI available=%d error=%d",
            __FUNCTION__, requestToString(requestForIMEI), gotIMEI ? 1 : 0, e);
    onRequestUnsupportedRequest(requestForIMEI, t);
    inIMEIRequest = false;
}

static void onRequestCompleteGetImeiSv(
        RIL_Token t, RIL_Errno e, void *response, size_t responselen) {
    gotIMEISV = copyRilStringResponse(
            imeisv, sizeof(imeisv), e, response, responselen);
    RLOGI("%s: %s: IMEISV available=%d error=%d",
            __FUNCTION__, requestToString(requestForIMEISV), gotIMEISV ? 1 : 0, e);
    onRequestUnsupportedRequest(requestForIMEISV, t);
    inIMEISVRequest = false;
}

static void fixupDataCallList(void *response, size_t responselen) {
	RIL_Data_Call_Response_v6 *p_cur = (RIL_Data_Call_Response_v6 *) response;
	int num = responselen / sizeof(RIL_Data_Call_Response_v6);

	int i;
	for (i = 0; i < num; ++i)
		p_cur[i].gateways = p_cur[i].addresses;
}

static bool isSupportedSignalStrengthPayload(size_t responselen) {
    return responselen == sizeof(RIL_SignalStrength_v5) ||
           responselen == sizeof(RIL_SignalStrength_v6) ||
           responselen == sizeof(RIL_SignalStrength_v8) ||
           responselen == sizeof(RIL_SignalStrength_v10);
}

static void fixupSignalStrength(void *response, size_t responselen) {
    if (response == NULL || !isSupportedSignalStrengthPayload(responselen)) {
        return;
    }

    RIL_SignalStrength_v5 *legacy =
            static_cast<RIL_SignalStrength_v5 *>(response);
    const int rawGsmSignalStrength =
            legacy->GW_SignalStrength.signalStrength;
    int gsmSignalStrength = rawGsmSignalStrength & 0xFF;

    if (gsmSignalStrength > 31 && rawGsmSignalStrength != 99) {
        gsmSignalStrength = legacy->CDMA_SignalStrength.dbm;
    }
    legacy->GW_SignalStrength.signalStrength = gsmSignalStrength;

    /* N7000 has no LTE. Touch LTE fields only when the payload contains them. */
    if (responselen >= sizeof(RIL_SignalStrength_v6)) {
        RIL_SignalStrength_v6 *v6 =
                static_cast<RIL_SignalStrength_v6 *>(response);
        v6->LTE_SignalStrength.signalStrength = 99;
        v6->LTE_SignalStrength.rsrp = INT_MAX;
        v6->LTE_SignalStrength.rsrq = INT_MAX;
        v6->LTE_SignalStrength.rssnr = INT_MAX;
        v6->LTE_SignalStrength.cqi = INT_MAX;
    }

    if (responselen >= sizeof(RIL_SignalStrength_v8)) {
        RIL_SignalStrength_v8 *v8 =
                static_cast<RIL_SignalStrength_v8 *>(response);
        v8->LTE_SignalStrength.timingAdvance = INT_MAX;
    }
}

static void onRequestCompleteShim(RIL_Token t, RIL_Errno e, void *response, size_t responselen) {
	int request;
	RequestInfo *pRI;

	pRI = (RequestInfo *)t;

	/* If pRI is null, this entire function is useless. */
	if (pRI == NULL)
		goto null_token_exit;

	/* If pCI is null, this entire function is useless. */
	if (pRI->pCI == NULL)
		goto null_token_exit;

	request = pRI->pCI->requestNumber;

	RLOGD("%s:\t\t\t<<< REQUEST-COMPLETE: %s: response:%p responselen:%d token:%p error:%d\n", __FUNCTION__, requestToString(request), response, responselen, t, e);

	switch (request) {
		case RIL_REQUEST_GET_IMEI:
			onRequestCompleteGetImei(t, e, response, responselen);
			return;
		case RIL_REQUEST_GET_IMEISV:
			onRequestCompleteGetImeiSv(t, e, response, responselen);
			return;
                case RIL_REQUEST_VOICE_REGISTRATION_STATE:
                        /*
                         * The Samsung blob returns between 3 and 14 legacy
                         * strings, depending on registration state. Expand
                         * every short, pointer-aligned response to 18 slots.
                         */
                        if (response != NULL &&
                            responselen > 0 &&
                            responselen % sizeof(char *) == 0 &&
                            responselen <
                                    VOICE_REGSTATE_NUM_STRINGS * sizeof(char *)) {
                                onRequestCompleteVoiceRegistrationState(
                                        t, e, response, responselen);
                                return;
                        }
                        break;
                case RIL_REQUEST_SIM_IO:
                        if (t == pendingIccidReadToken) {
                                const bool updated =
                                        e == RIL_E_SUCCESS &&
                                        cacheIccidFromSimIo(
                                                response, responselen);

                                pendingIccidReadToken = NULL;
                                rilEnv->OnRequestComplete(
                                        t, e, response, responselen);

                                if (updated) {
                                        /*
                                         * Make UiccController request card
                                         * status again. The next response
                                         * includes cachedIccid and slot 0.
                                         */
                                        rilEnv->OnUnsolicitedResponse(
                                                RIL_UNSOL_RESPONSE_SIM_STATUS_CHANGED,
                                                NULL,
                                                0);
                                }
                                return;
                        }
                        break;
                case RIL_REQUEST_DATA_REGISTRATION_STATE:
			onRequestCompleteDataRegistrationState(t, e, response, responselen);
			return;
		case RIL_REQUEST_GET_SIM_STATUS:
			/* Remove unused extra elements from RIL_AppStatus */
			if (response != NULL && responselen == sizeof(RIL_CardStatus_v5_samsung)) {
				onCompleteRequestGetSimStatus(t, e, response);
				return;
			}
			break;
		case RIL_REQUEST_LAST_CALL_FAIL_CAUSE:
			/* Remove extra element (ignored on pre-M, now crashing the framework) */
			if (responselen > sizeof(int)) {
				rilEnv->OnRequestComplete(t, e, response, sizeof(int));
				return;
			}
			break;
		case RIL_REQUEST_DATA_CALL_LIST:
		case RIL_REQUEST_SETUP_DATA_CALL:
			/* According to the Samsung RIL, the addresses are the gateways?
			 * This fixes mobile data. */
			if (response != NULL && responselen != 0 && (responselen % sizeof(RIL_Data_Call_Response_v6) == 0)) {
				fixupDataCallList(response, responselen);
				rilEnv->OnRequestComplete(t, e, response, responselen);
				return;
			}
			break;
		case RIL_REQUEST_SIGNAL_STRENGTH:
			/* The Samsung RIL reports the signal strength in a strange way... */
			if (response != NULL && responselen >= sizeof(RIL_SignalStrength_v5)) {
				fixupSignalStrength(response, responselen);
				rilEnv->OnRequestComplete(t, e, response, responselen);
				return;
			}
			break;
		case RIL_REQUEST_GET_ACTIVITY_INFO:
			onCompleteGetActivityInfo(t);
			return;
	}

null_token_exit:
	rilEnv->OnRequestComplete(t, e, response, responselen);
}

static void onUnsolicitedResponseShim(int unsolResponse, const void *data, size_t datalen)
{
	switch (unsolResponse) {
		case RIL_UNSOL_DATA_CALL_LIST_CHANGED:
			/* According to the Samsung RIL, the addresses are the gateways?
			 * This fixes mobile data. */
			if (data != NULL && datalen != 0 && (datalen % sizeof(RIL_Data_Call_Response_v6) == 0))
				fixupDataCallList((void*) data, datalen);
			break;
		case RIL_UNSOL_SIGNAL_STRENGTH:
			/* The Samsung RIL reports the signal strength in a strange way... */
			if (data != NULL && datalen >= sizeof(RIL_SignalStrength_v5))
				fixupSignalStrength((void *)data, datalen);
			break;
	}

	rilEnv->OnUnsolicitedResponse(unsolResponse, data, datalen);
}

static void patchMem(void *libHandle) {
	/*
	 * MAX_TIMEOUT is used for a call to pthread_cond_timedwait_relative_np.
	 * The issue is bionic has switched to using absolute timeouts instead of
	 * relative timeouts, and a maximum time value can cause an overflow in
	 * the function converting relative to absolute timespecs if unpatched.
	 *
	 * By patching this to 0x01FFFFFF from 0x7FFFFFFF, the timeout should
	 * expire in about a year rather than 68 years, and the RIL should be good
	 * up until the year 2036 or so.
	 */
	uint32_t *MAX_TIMEOUT;

	MAX_TIMEOUT = (uint32_t *)dlsym(libHandle, "MAX_TIMEOUT");
	if (CC_UNLIKELY(!MAX_TIMEOUT)) {
		RLOGE("%s: MAX_TIMEOUT could not be found!", __FUNCTION__);
		return;
	}
	RLOGD("%s: MAX_TIMEOUT found at %p!", __FUNCTION__, MAX_TIMEOUT);
	RLOGD("%s: MAX_TIMEOUT is currently 0x%" PRIX32, __FUNCTION__, *MAX_TIMEOUT);
	if (CC_LIKELY(*MAX_TIMEOUT == 0x7FFFFFFF)) {
		*MAX_TIMEOUT = 0x01FFFFFF;
		RLOGI("%s: MAX_TIMEOUT was changed to 0x0%" PRIX32, __FUNCTION__, *MAX_TIMEOUT);
	} else {
		RLOGW("%s: MAX_TIMEOUT was not 0x7FFFFFFF; leaving alone", __FUNCTION__);
	}

}

const RIL_RadioFunctions* RIL_Init(const struct RIL_Env *env, int argc, char **argv)
{
	RIL_RadioFunctions const* (*origRilInit)(const struct RIL_Env *env, int argc, char **argv);
	static RIL_RadioFunctions shimmedFunctions;
	static struct RIL_Env shimmedEnv;
	void *origRil;

	/* Shim the RIL_Env passed to the real RIL, saving a copy of the original */
	rilEnv = env;
	shimmedEnv = *env;
	shimmedEnv.OnRequestComplete = onRequestCompleteShim;
	shimmedEnv.OnUnsolicitedResponse = onUnsolicitedResponseShim;

	/* Open and Init the original RIL. */
	origRil = dlopen(RIL_LIB_PATH, RTLD_GLOBAL);
	if (CC_UNLIKELY(!origRil)) {
		RLOGE("%s: failed to load '" RIL_LIB_PATH  "': %s\n", __FUNCTION__, dlerror());
		return NULL;
	}

	origRilInit = (const RIL_RadioFunctions *(*)(const struct RIL_Env *, int, char **))(dlsym(origRil, "RIL_Init"));
	if (CC_UNLIKELY(!origRilInit)) {
		RLOGE("%s: couldn't find original RIL_Init!\n", __FUNCTION__);
		goto fail_after_dlopen;
	}

	// Fix RIL issues by patching memory
	patchMem(origRil);

	/* Remove "-c <clientId>"; the legacy Samsung RIL does not support it. */
	for (int i = 0; i + 1 < argc;) {
		if (!strcmp(argv[i], "-c")) {
			const int remaining = argc - i - 2;
			if (remaining > 0) {
				memmove(argv + i, argv + i + 2,
				        static_cast<size_t>(remaining) * sizeof(*argv));
			}
			argc -= 2;
			continue;
		}
		++i;
	}

	origRilFunctions = origRilInit(GetEnv(&shimmedEnv), argc, argv);
	if (CC_UNLIKELY(!origRilFunctions)) {
		RLOGE("%s: the original RIL_Init derped.\n", __FUNCTION__);
		goto fail_after_dlopen;
	}
	SetRadioFunctions(origRilFunctions);

	/* Shim functions as needed. */
	shimmedFunctions = *origRilFunctions;
	shimmedFunctions.onRequest = onRequestShim;
	shimmedFunctions.onStateRequest = onStateRequestShim;

	return &shimmedFunctions;

fail_after_dlopen:
	dlclose(origRil);
	return NULL;
}
