/* //device/system/reference-ril/reference-ril.c
**
** Copyright 2006, The Android Open Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

#include <telephony/ril_cdma_sms.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>
#include <alloca.h>
#include "atchannel.h"
#include "at_tok.h"
#include "misc.h"
#include <getopt.h>
#include <sys/socket.h>
#include <cutils/sockets.h>
#include <netutils/ifc.h>
#include <termios.h>
#include <sys/system_properties.h>
#include <regex.h>

#include "ril.h"
#include "hardware/qemu_pipe.h"

#define LOG_TAG "RIL"
#include <utils/Log.h>

#define MAX_AT_RESPONSE 0x1000

#ifdef USE_TI_COMMANDS

// Enable a workaround
// 1) Make incoming call, do not answer
// 2) Hangup remote end
// Expected: call should disappear from CLCC line
// Actual: Call shows as "ACTIVE" before disappearing
#define WORKAROUND_ERRONEOUS_ANSWER 1

// Some varients of the TI stack do not support the +CGEV unsolicited
// response. However, they seem to send an unsolicited +CME ERROR: 150
#define WORKAROUND_FAKE_CGEV 1
#endif

/* Modem Technology bits */
#define MDM_GSM         0x01
#define MDM_WCDMA       0x02
#define MDM_CDMA        0x04
#define MDM_EVDO        0x08
#define MDM_LTE         0x10

typedef struct {
    int supportedTechs; // Bitmask of supported Modem Technology bits
    int currentTech;    // Technology the modem is currently using (in the format used by modem)
    int isMultimode;

    // Preferred mode bitmask. This is actually 4 byte-sized bitmasks with different priority values,
    // in which the byte number from LSB to MSB give the priority.
    //
    //          |MSB|   |   |LSB
    // value:   |00 |00 |00 |00
    // byte #:  |3  |2  |1  |0
    //
    // Higher byte order give higher priority. Thus, a value of 0x0000000f represents
    // a preferred mode of GSM, WCDMA, CDMA, and EvDo in which all are equally preferrable, whereas
    // 0x00000201 represents a mode with GSM and WCDMA, in which WCDMA is preferred over GSM
    int32_t preferredNetworkMode;
    int subscription_source;

} ModemInfo;

static ModemInfo *sMdmInfo;
// TECH returns the current technology in the format used by the modem.
// It can be used as an l-value
#define TECH(mdminfo)                 ((mdminfo)->currentTech)
// TECH_BIT returns the bitmask equivalent of the current tech
#define TECH_BIT(mdminfo)            (1 << ((mdminfo)->currentTech))
#define IS_MULTIMODE(mdminfo)         ((mdminfo)->isMultimode)
#define TECH_SUPPORTED(mdminfo, tech) ((mdminfo)->supportedTechs & (tech))
#define PREFERRED_NETWORK(mdminfo)    ((mdminfo)->preferredNetworkMode)
// CDMA Subscription Source
#define SSOURCE(mdminfo)              ((mdminfo)->subscription_source)

static int net2modem[] = {
    MDM_GSM | MDM_WCDMA,                                 // 0  - GSM / WCDMA Pref
    MDM_GSM,                                             // 1  - GSM only
    MDM_WCDMA,                                           // 2  - WCDMA only
    MDM_GSM | MDM_WCDMA,                                 // 3  - GSM / WCDMA Auto
    MDM_CDMA | MDM_EVDO,                                 // 4  - CDMA / EvDo Auto
    MDM_CDMA,                                            // 5  - CDMA only
    MDM_EVDO,                                            // 6  - EvDo only
    MDM_GSM | MDM_WCDMA | MDM_CDMA | MDM_EVDO,           // 7  - GSM/WCDMA, CDMA, EvDo
    MDM_LTE | MDM_CDMA | MDM_EVDO,                       // 8  - LTE, CDMA and EvDo
    MDM_LTE | MDM_GSM | MDM_WCDMA,                       // 9  - LTE, GSM/WCDMA
    MDM_LTE | MDM_CDMA | MDM_EVDO | MDM_GSM | MDM_WCDMA, // 10 - LTE, CDMA, EvDo, GSM/WCDMA
    MDM_LTE,                                             // 11 - LTE only
};

static int32_t net2pmask[] = {
    MDM_GSM | (MDM_WCDMA << 8),                          // 0  - GSM / WCDMA Pref
    MDM_GSM,                                             // 1  - GSM only
    MDM_WCDMA,                                           // 2  - WCDMA only
    MDM_GSM | MDM_WCDMA,                                 // 3  - GSM / WCDMA Auto
    MDM_CDMA | MDM_EVDO,                                 // 4  - CDMA / EvDo Auto
    MDM_CDMA,                                            // 5  - CDMA only
    MDM_EVDO,                                            // 6  - EvDo only
    MDM_GSM | MDM_WCDMA | MDM_CDMA | MDM_EVDO,           // 7  - GSM/WCDMA, CDMA, EvDo
    MDM_LTE | MDM_CDMA | MDM_EVDO,                       // 8  - LTE, CDMA and EvDo
    MDM_LTE | MDM_GSM | MDM_WCDMA,                       // 9  - LTE, GSM/WCDMA
    MDM_LTE | MDM_CDMA | MDM_EVDO | MDM_GSM | MDM_WCDMA, // 10 - LTE, CDMA, EvDo, GSM/WCDMA
    MDM_LTE,                                             // 11 - LTE only
};

static int is3gpp2(int radioTech) {
    switch (radioTech) {
        case RADIO_TECH_IS95A:
        case RADIO_TECH_IS95B:
        case RADIO_TECH_1xRTT:
        case RADIO_TECH_EVDO_0:
        case RADIO_TECH_EVDO_A:
        case RADIO_TECH_EVDO_B:
        case RADIO_TECH_EHRPD:
            return 1;
        default:
            return 0;
    }
}

typedef enum {
    SIM_ABSENT = 0,
    SIM_NOT_READY = 1,
    SIM_READY = 2, /* SIM_READY means the radio state is RADIO_STATE_SIM_READY */
    SIM_PIN = 3,
    SIM_PUK = 4,
    SIM_NETWORK_PERSONALIZATION = 5,
    RUIM_ABSENT = 6,
    RUIM_NOT_READY = 7,
    RUIM_READY = 8,
    RUIM_PIN = 9,
    RUIM_PUK = 10,
    RUIM_NETWORK_PERSONALIZATION = 11
} SIM_Status;

static void onRequest (int request, void *data, size_t datalen, RIL_Token t);
static RIL_RadioState currentState();
static int onSupports (int requestCode);
static void onCancel (RIL_Token t);
static const char *getVersion();
static int isRadioOn();
static SIM_Status getSIMStatus();
static int getCardStatus(RIL_CardStatus_v6 **pp_card_status);
static void freeCardStatus(RIL_CardStatus_v6 *p_card_status);
static void onDataCallListChanged(void *param);

extern const char * requestToString(int request);

/*** Static Variables ***/
static const RIL_RadioFunctions s_callbacks = {
    RIL_VERSION,
    onRequest,
    currentState,
    onSupports,
    onCancel,
    getVersion
};

#ifdef RIL_SHLIB
static const struct RIL_Env *s_rilenv;

#define RIL_onRequestComplete(t, e, response, responselen) s_rilenv->OnRequestComplete(t,e, response, responselen)
#define RIL_onUnsolicitedResponse(a,b,c) s_rilenv->OnUnsolicitedResponse(a,b,c)
#define RIL_requestTimedCallback(a,b,c) s_rilenv->RequestTimedCallback(a,b,c)
#endif

static RIL_RadioState sState = RADIO_STATE_UNAVAILABLE;

static pthread_mutex_t s_state_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_state_cond = PTHREAD_COND_INITIALIZER;

static int s_port = -1;
static const char * s_device_path = NULL;
static int          s_device_socket = 0;
static const char * s_client_id = NULL;

/* trigger change to this with s_state_cond */
static int s_closed = 0;

static int sFD;     /* file desc of AT channel */
static char sATBuffer[MAX_AT_RESPONSE+1];
static char *sATBufferCur = NULL;

static const struct timeval TIMEVAL_SIMPOLL = {1,0};
static const struct timeval TIMEVAL_CALLSTATEPOLL = {0,500000};
static const struct timeval TIMEVAL_0 = {0,0};

#ifdef WORKAROUND_ERRONEOUS_ANSWER
// Max number of times we'll try to repoll when we think
// we have a AT+CLCC race condition
#define REPOLL_CALLS_COUNT_MAX 4

// Line index that was incoming or waiting at last poll, or -1 for none
static int s_incomingOrWaitingLine = -1;
// Number of times we've asked for a repoll of AT+CLCC
static int s_repollCallsCount = 0;
// Should we expect a call to be answered in the next CLCC?
static int s_expectAnswer = 0;
#endif /* WORKAROUND_ERRONEOUS_ANSWER */

/* TS 24.096 clause 4.1 */
#define  A_CALL_NAME_MAX_SIZE  80
/* According to 3GPP 22.083 clause 2.2.1, 3GPP 22.084 clause 1.2.1 and 3GPP
 * 22.030 clause 6.5.5.6, the case of the maximum number is reached "when
 * there comes an incoming call while we have already one active(held)
 * conference call (with 5 remote parties) and one held(active) single call."
 * The maximum number of voice calls is therefore 7.
 */
#define  A_MAX_CALL_CONNECTIONS  7

static int s_maxDataContexts = 0;

typedef struct {
    char name[ A_CALL_NAME_MAX_SIZE+1 ];
    int CNI_validity;
} CnapInfo;

// Temporary variable to hold +CNAP information, cleaned after requestGetCurrentCalls.
static CnapInfo sCnapInfo = {'\0', 0};
// CnapInfoList to hold information associated with call id.
static CnapInfo sCnapInfoList[ A_MAX_CALL_CONNECTIONS ];

static void pollSIMState (void *param);
static void setRadioState(RIL_RadioState newState);
static void setRadioTechnology(ModemInfo *mdm, int newtech);
static int query_ctec(ModemInfo *mdm, int *current, int32_t *preferred);
static int parse_technology_response(const char *response, int *current, int32_t *preferred);
static int techFromModemType(int mdmtype);

static int cmeErrorToRilError(int cmeError) {
    switch (cmeError) {
        case CME_SUCCESS:
            return RIL_E_SUCCESS;
        case CME_OPERATION_NOT_SUPPORTED:
            return RIL_E_REQUEST_NOT_SUPPORTED;
        case CME_SIM_NOT_INSERTED:
            return RIL_E_SIM_ABSENT;
        case CME_INCORRECT_PASSWORD:
            return RIL_E_PASSWORD_INCORRECT;
    }

    return RIL_E_GENERIC_FAILURE;
}

static int clccStateToRILState(int state, RIL_CallState *p_state)

{
    switch(state) {
        case 0: *p_state = RIL_CALL_ACTIVE;   return 0;
        case 1: *p_state = RIL_CALL_HOLDING;  return 0;
        case 2: *p_state = RIL_CALL_DIALING;  return 0;
        case 3: *p_state = RIL_CALL_ALERTING; return 0;
        case 4: *p_state = RIL_CALL_INCOMING; return 0;
        case 5: *p_state = RIL_CALL_WAITING;  return 0;
        default: return -1;
    }
}

/**
 * Convert CLI Validity to number presenstaion.
 *
 * CLI validity is ranged between 0 and 4 defined in TS 27.007 Clause 7.18,
 * and numberPresentation is ranged between 0 and 3 defined in ril.h.
 */
static int convertCliValidity(int cliValidity) {
    int presentation = 0;
    if (cliValidity <= 0 || cliValidity > 4) {
        return presentation;
    }

    if (cliValidity == 2 || cliValidity == 4) {
        presentation = 2;
    } else {
        presentation = cliValidity;
    }

    return presentation;
}

/**
 * Note: directly modified line and has *p_call point directly into
 * modified line
 */
static int callFromCLCCLine(char *line, RIL_Call *p_call)
{
    //+CLCC: 1,0,2,0,0,\"+18005551212\",145,\"\",2,0
    //     index,isMT,state,mode,isMpty[,<number>,<type>[,<alpha>[,<priority>[,<CLI validity>]]]]

    int err;
    int state;
    int mode;
    char *alpha;
    int priority;
    int cliValidity;

    err = at_tok_start(&line);
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &(p_call->index));
    if (err < 0) goto error;

    err = at_tok_nextbool(&line, &(p_call->isMT));
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &state);
    if (err < 0) goto error;

    err = clccStateToRILState(state, &(p_call->state));
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &mode);
    if (err < 0) goto error;

    p_call->isVoice = (mode == 0);

    err = at_tok_nextbool(&line, &(p_call->isMpty));
    if (err < 0) goto error;

    if (at_tok_hasmore(&line)) {
        err = at_tok_nextstr(&line, &(p_call->number));

        /* tolerate null here */
        if (err < 0) return 0;

        // Some lame implementations return strings
        // like "NOT AVAILABLE" in the CLCC line
        if (p_call->number != NULL
            && 0 == strspn(p_call->number, "+0123456789")
        ) {
            p_call->number = NULL;
        }

        err = at_tok_nextint(&line, &p_call->toa);
        if (err < 0) goto error;
    }

    if (at_tok_hasmore(&line)) {
        // alpha is not used yet, simply read and ignore it
        err = at_tok_nextstr(&line, &alpha);
        if (err < 0) return 0;

        if (at_tok_hasmore(&line)) {
            // priority is not used yet, simply read and ignore it
            err = at_tok_nextint(&line, &priority);
            if (err < 0) goto error;

            if (at_tok_hasmore(&line)) {
                err = at_tok_nextint(&line, &cliValidity);
                if (err < 0) goto error;
                // mapping CLI validity to numberPresentation based on the definition in ril.h
                p_call->numberPresentation = convertCliValidity(cliValidity);
            }
        }
    }

    p_call->uusInfo = NULL;

    return 0;

error:
    ALOGE("invalid CLCC line\n");
    return -1;
}


/** do post-AT+CFUN=1 initialization */
static void onRadioPowerOn()
{
#ifdef USE_TI_COMMANDS
    /*  Must be after CFUN=1 */
    /*  TI specific -- notifications for CPHS things such */
    /*  as CPHS message waiting indicator */

    at_send_command("AT%CPHS=1", NULL);

    /*  TI specific -- enable NITZ unsol notifs */
    at_send_command("AT%CTZV=1", NULL);
#endif

    pollSIMState(NULL);
}

/** do post- SIM ready initialization */
static void onSIMReady()
{
    at_send_command_singleline("AT+CSMS=1", "+CSMS:", NULL);
    /*
     * Always send SMS messages directly to the TE
     *
     * mode = 1 // discard when link is reserved (link should never be
     *             reserved)
     * mt = 2   // most messages routed to TE
     * bm = 2   // new cell BM's routed to TE
     * ds = 1   // Status reports routed to TE
     * bfr = 1  // flush buffer
     */
    at_send_command("AT+CNMI=1,2,2,1,1", NULL);
}

static void requestRadioPower(void *data, size_t datalen, RIL_Token t)
{
    int onOff;

    int err;
    ATResponse *p_response = NULL;

    assert (datalen >= sizeof(int *));
    onOff = ((int *)data)[0];

    if (onOff == 0 && sState != RADIO_STATE_OFF) {
        err = at_send_command("AT+CFUN=0", &p_response);
       if (err < 0 || p_response->success == 0) goto error;
        setRadioState(RADIO_STATE_OFF);
    } else if (onOff > 0 && sState == RADIO_STATE_OFF) {
        err = at_send_command("AT+CFUN=1", &p_response);
        if (err < 0|| p_response->success == 0) {
            // Some stacks return an error when there is no SIM,
            // but they really turn the RF portion on
            // So, if we get an error, let's check to see if it
            // turned on anyway

            if (isRadioOn() != 1) {
                goto error;
            }
        }
        setRadioState(RADIO_STATE_ON);
    }

    at_response_free(p_response);
    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    return;
error:
    at_response_free(p_response);
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void requestOrSendDataCallList(RIL_Token *t);

static void onDataCallListChanged(void *param)
{
    requestOrSendDataCallList(NULL);
}

static void requestDataCallList(void *data, size_t datalen, RIL_Token t)
{
    requestOrSendDataCallList(&t);
}

static void freeParsedCGCONTRDP(RIL_Data_Call_Response_v6 *response)
{
    response->type = NULL;
    free(response->ifname);
    response->ifname = NULL;
    free(response->addresses);
    response->addresses = NULL;
    free(response->gateways);
    response->gateways = NULL;
    free(response->dnses);
    response->dnses = NULL;
}

static int parseCGCONTRDP(char *line, RIL_Data_Call_Response_v6 *response)
{
    int err, bearer_id;
    char *str, *dns1, *dns2;

    err = at_tok_start(&line);
    if (err < 0)
        goto error;

    err = at_tok_nextint(&line, &response->cid);
    if (err < 0)
        goto error;

    // Assume no error
    response->status = 0;
    response->active = 2;
    // Assume IP
    response->type = "IP";

    // bearer_id
    err = at_tok_nextint(&line, &bearer_id);
    if (err < 0)
        goto error;

    asprintf(&response->ifname, "rmnet%d", bearer_id);

    // APN ignored for v5
    err = at_tok_nextstr(&line, &str);
    if (err < 0)
        goto error;

    // local_addr and subnet_mask
    if (!at_tok_hasmore(&line)) {
        return 0;
    }

    // With "AT+CGPIAF=1,1,0,1" assume "a1.a2.a3.a4/mask" for IPv4 and
    // "a1:a2:a3:a4:a5:a6:a7:a8/mask" for IPv6.  Assume IPv4 for now.
    err = at_tok_nextstr(&line, &str);
    if (err < 0)
        goto error;

    response->addresses = strdup(str);

    // gw
    if (!at_tok_hasmore(&line)) {
        return 0;
    }

    err = at_tok_nextstr(&line, &str);
    if (err < 0)
        goto error;

    response->gateways = strdup(str);

    // dns_prim
    if (!at_tok_hasmore(&line)) {
        return 0;
    }

    err = at_tok_nextstr(&line, &dns1);
    if (err < 0)
        goto error;

    // dns_sec
    if (at_tok_hasmore(&line)) {
        err = at_tok_nextstr(&line, &dns2);
        if (err < 0)
            goto error;

        asprintf(&response->dnses, "%s %s", dns1, dns2);
    } else {
        response->dnses = strdup(dns1);
    }

    return 0;

error:
    freeParsedCGCONTRDP(response);
    return -1;
}

static void requestOrSendDataCallList(RIL_Token *t)
{
    ATResponse *p_response;
    ATLine *p_cur;
    int err;
    int n = 0;
    char *out;

    err = at_send_command_multiline ("AT+CGACT?", "+CGACT:", &p_response);
    if (err != 0 || p_response->success == 0) {
        if (t != NULL)
            RIL_onRequestComplete(*t, RIL_E_GENERIC_FAILURE, NULL, 0);
        else
            RIL_onUnsolicitedResponse(RIL_UNSOL_DATA_CALL_LIST_CHANGED,
                                      NULL, 0);
        return;
    }

    for (p_cur = p_response->p_intermediates; p_cur != NULL;
         p_cur = p_cur->p_next)
        n++;

    RIL_Data_Call_Response_v6 *responses =
        alloca(n * sizeof(RIL_Data_Call_Response_v6));
    RIL_Data_Call_Response_v6 tmp_rp;

    int i;
    for (i = 0; i < n; i++) {
        responses[i].status = -1;
        responses[i].suggestedRetryTime = -1;
        responses[i].cid = -1;
        responses[i].active = -1;
        responses[i].type = "";
        responses[i].ifname = "";
        responses[i].addresses = "";
        responses[i].dnses = "";
        responses[i].gateways = "";
    }

    RIL_Data_Call_Response_v6 *response = responses;
    for (p_cur = p_response->p_intermediates; p_cur != NULL;
         p_cur = p_cur->p_next) {
        char *line = p_cur->line;

        err = at_tok_start(&line);
        if (err < 0)
            goto error;

        err = at_tok_nextint(&line, &response->cid);
        if (err < 0)
            goto error;

        err = at_tok_nextint(&line, &response->active);
        if (err < 0)
            goto error;

        response++;
    }

    at_response_free(p_response);

    err = at_send_command_multiline ("AT+CGCONTRDP", "+CGCONTRDP:", &p_response);
    if (err != 0 || p_response->success == 0) {
        if (t != NULL)
            RIL_onRequestComplete(*t, RIL_E_GENERIC_FAILURE, NULL, 0);
        else
            RIL_onUnsolicitedResponse(RIL_UNSOL_DATA_CALL_LIST_CHANGED,
                                      NULL, 0);
        return;
    }

    for (p_cur = p_response->p_intermediates; p_cur != NULL;
         p_cur = p_cur->p_next) {
        if (parseCGCONTRDP(p_cur->line, &tmp_rp) < 0)
            goto error;

        for (i = 0; i < n; i++) {
            if (responses[i].cid == tmp_rp.cid)
                break;
        }

        if (i >= n) {
            /* details for a context we didn't hear about in the last request */
            freeParsedCGCONTRDP(&tmp_rp);
            continue;
        }

        responses[i].status = tmp_rp.status;
        responses[i].type = tmp_rp.type;

#define COPY_FIELD(f) \
    responses[i].f = alloca(strlen(tmp_rp.f) + 1); \
    strcpy(responses[i].f, tmp_rp.f);

        COPY_FIELD(ifname);

        if (tmp_rp.addresses) {
            COPY_FIELD(addresses);

            if (tmp_rp.gateways) {
                COPY_FIELD(gateways);

                if (tmp_rp.dnses) {
                    COPY_FIELD(dnses);
                }
            }
        }

#undef COPY_FIELD

        freeParsedCGCONTRDP(&tmp_rp);
    }

    at_response_free(p_response);

    if (t != NULL)
        RIL_onRequestComplete(*t, RIL_E_SUCCESS, responses,
                              n * sizeof(RIL_Data_Call_Response_v6));
    else
        RIL_onUnsolicitedResponse(RIL_UNSOL_DATA_CALL_LIST_CHANGED,
                                  responses,
                                  n * sizeof(RIL_Data_Call_Response_v6));

    return;

error:
    if (t != NULL)
        RIL_onRequestComplete(*t, RIL_E_GENERIC_FAILURE, NULL, 0);
    else
        RIL_onUnsolicitedResponse(RIL_UNSOL_DATA_CALL_LIST_CHANGED,
                                  NULL, 0);

    at_response_free(p_response);
}

static void requestQueryNetworkSelectionMode(
                void *data, size_t datalen, RIL_Token t)
{
    int err;
    ATResponse *p_response = NULL;
    int response = 0;
    char *line;

    err = at_send_command_singleline("AT+COPS?", "+COPS:", &p_response);

    if (err < 0 || p_response->success == 0) {
        goto error;
    }

    line = p_response->p_intermediates->line;

    err = at_tok_start(&line);

    if (err < 0) {
        goto error;
    }

    err = at_tok_nextint(&line, &response);

    if (err < 0) {
        goto error;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &response, sizeof(int));
    at_response_free(p_response);
    return;
error:
    at_response_free(p_response);
    ALOGE("requestQueryNetworkSelectionMode must never return error when radio is on");
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void requestSetNetworkSelectionManual(
                void *data, size_t datalen, RIL_Token t)
{
    int err;
    char *cmd;
    const char *network = (const char *) data;
    ATResponse *p_response;

    asprintf(&cmd, "AT+COPS=1,2,%s", network);
    err = at_send_command(cmd, &p_response);
    free(cmd);

    int rilError = RIL_E_SUCCESS;
    if (err < 0) {
        ALOGE("requestSetNetworkSelectionManual failed, err: %d", err);
        at_response_free(p_response);
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return;
    }

    if (at_get_cme_error(p_response) != CME_SUCCESS) {
        rilError = RIL_E_GENERIC_FAILURE;
    }

    at_response_free(p_response);

    RIL_onRequestComplete(t, rilError, NULL, 0);
}

static void sendCallStateChanged(void *param)
{
    RIL_onUnsolicitedResponse (
        RIL_UNSOL_RESPONSE_CALL_STATE_CHANGED,
        NULL, 0);
}

static void requestGetCurrentCalls(void *data, size_t datalen, RIL_Token t)
{
    int err;
    ATResponse *p_response;
    ATLine *p_cur;
    int countCalls;
    int countValidCalls;
    RIL_Call *p_calls;
    RIL_Call **pp_calls;
    int i, j;
    int needRepoll = 0;

#ifdef WORKAROUND_ERRONEOUS_ANSWER
    int prevIncomingOrWaitingLine;

    prevIncomingOrWaitingLine = s_incomingOrWaitingLine;
    s_incomingOrWaitingLine = -1;
#endif /*WORKAROUND_ERRONEOUS_ANSWER*/

    err = at_send_command_multiline ("AT+CLCC", "+CLCC:", &p_response);

    if (err != 0 || p_response->success == 0) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return;
    }

    /* count the calls */
    for (countCalls = 0, p_cur = p_response->p_intermediates
            ; p_cur != NULL
            ; p_cur = p_cur->p_next
    ) {
        countCalls++;
    }

    /* yes, there's an array of pointers and then an array of structures */

    pp_calls = (RIL_Call **)alloca(countCalls * sizeof(RIL_Call *));
    p_calls = (RIL_Call *)alloca(countCalls * sizeof(RIL_Call));
    memset (p_calls, 0, countCalls * sizeof(RIL_Call));

    /* init the pointer array */
    for(i = 0; i < countCalls ; i++) {
        pp_calls[i] = &(p_calls[i]);
    }

    for (countValidCalls = 0, p_cur = p_response->p_intermediates
            ; p_cur != NULL
            ; p_cur = p_cur->p_next
    ) {
        err = callFromCLCCLine(p_cur->line, p_calls + countValidCalls);

        if (err != 0) {
            continue;
        }

#ifdef WORKAROUND_ERRONEOUS_ANSWER
        if (p_calls[countValidCalls].state == RIL_CALL_INCOMING
            || p_calls[countValidCalls].state == RIL_CALL_WAITING
        ) {
            s_incomingOrWaitingLine = p_calls[countValidCalls].index;
        }
#endif /*WORKAROUND_ERRONEOUS_ANSWER*/

        if (p_calls[countValidCalls].state != RIL_CALL_ACTIVE
            && p_calls[countValidCalls].state != RIL_CALL_HOLDING
        ) {
            needRepoll = 1;
        }

        // Handle cached CNAP info
        if (p_calls[countValidCalls].state == RIL_CALL_INCOMING
            || p_calls[countValidCalls].state == RIL_CALL_WAITING
        ) {
            if (strlen(sCnapInfo.name) > 0
                || (sCnapInfo.CNI_validity > 0 && sCnapInfo.CNI_validity <= 2)) {
                sCnapInfoList[p_calls[countValidCalls].index - 1] = sCnapInfo;
                sCnapInfo.name[0] = '\0';
                sCnapInfo.CNI_validity = 0;
            }
        }

        countValidCalls++;
    }

    // Fill up RIL_Call object for name/namePresentation, or clean it.
    for (i = 0; i < A_MAX_CALL_CONNECTIONS; i++) {
         if (strlen(sCnapInfoList[i].name) > 0
             || sCnapInfoList[i].CNI_validity > 0) {
             for (j = 0; countValidCalls > 0, countValidCalls > j; j++) {
                 if (p_calls[j].index == i+1) {
                     p_calls[j].name = sCnapInfoList[i].name;
                     p_calls[j].namePresentation = sCnapInfoList[i].CNI_validity;
                     break;
                 }
             }

             // no match to current call(s), clear the related CNAP info.
             if (j >= countValidCalls) {
                 sCnapInfoList[i].name[0] = '\0';
                 sCnapInfoList[i].CNI_validity = 0;
             }
         }
    }

#ifdef WORKAROUND_ERRONEOUS_ANSWER
    // Basically:
    // A call was incoming or waiting
    // Now it's marked as active
    // But we never answered it
    //
    // This is probably a bug, and the call will probably
    // disappear from the call list in the next poll
    if (prevIncomingOrWaitingLine >= 0
            && s_incomingOrWaitingLine < 0
            && s_expectAnswer == 0
    ) {
        for (i = 0; i < countValidCalls ; i++) {

            if (p_calls[i].index == prevIncomingOrWaitingLine
                    && p_calls[i].state == RIL_CALL_ACTIVE
                    && s_repollCallsCount < REPOLL_CALLS_COUNT_MAX
            ) {
                ALOGI(
                    "Hit WORKAROUND_ERRONOUS_ANSWER case."
                    " Repoll count: %d\n", s_repollCallsCount);
                s_repollCallsCount++;
                goto error;
            }
        }
    }

    s_expectAnswer = 0;
    s_repollCallsCount = 0;
#endif /*WORKAROUND_ERRONEOUS_ANSWER*/

    RIL_onRequestComplete(t, RIL_E_SUCCESS, pp_calls,
            countValidCalls * sizeof (RIL_Call *));

    at_response_free(p_response);

#ifdef POLL_CALL_STATE
    if (countValidCalls) {  // We don't seem to get a "NO CARRIER" message from
                            // smd, so we're forced to poll until the call ends.
#else
    if (needRepoll) {
#endif
        RIL_requestTimedCallback (sendCallStateChanged, NULL, &TIMEVAL_CALLSTATEPOLL);
    }

    return;
error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

static void requestDial(void *data, size_t datalen, RIL_Token t)
{
    RIL_Dial *p_dial;
    char *cmd;
    const char *clir;
    int ret;

    p_dial = (RIL_Dial *)data;

    switch (p_dial->clir) {
        case 1: clir = "I"; break;  /*invocation*/
        case 2: clir = "i"; break;  /*suppression*/
        default:
        case 0: clir = ""; break;   /*subscription default*/
    }

    asprintf(&cmd, "ATD%s%s;", p_dial->address, clir);

    ret = at_send_command(cmd, NULL);

    free(cmd);

    /* success or failure is ignored by the upper layer here.
       it will call GET_CURRENT_CALLS and determine success that way */
    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
}

static void requestWriteSmsToSim(void *data, size_t datalen, RIL_Token t)
{
    RIL_SMS_WriteArgs *p_args;
    char *cmd;
    int length;
    int err;
    ATResponse *p_response = NULL;

    p_args = (RIL_SMS_WriteArgs *)data;

    length = strlen(p_args->pdu)/2;
    asprintf(&cmd, "AT+CMGW=%d,%d", length, p_args->status);

    err = at_send_command_sms(cmd, p_args->pdu, "+CMGW:", &p_response);

    if (err != 0 || p_response->success == 0) goto error;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    at_response_free(p_response);

    return;
error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

static void requestHangup(void *data, size_t datalen, RIL_Token t)
{
    int *p_line;

    int ret;
    char *cmd;

    p_line = (int *)data;

    // 3GPP 22.030 6.5.5
    // "Releases a specific active call X"
    asprintf(&cmd, "AT+CHLD=1%d", p_line[0]);

    ret = at_send_command(cmd, NULL);

    free(cmd);

    /* success or failure is ignored by the upper layer here.
       it will call GET_CURRENT_CALLS and determine success that way */
    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
}

static void requestLastCallFailCause(RIL_Token t)
{
    ATResponse *p_response = NULL;
    int err;
    int response = 0;
    char *line;

    err = at_send_command_singleline("AT+CEER", "+CEER:", &p_response);

    if (err < 0 || p_response->success == 0) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        goto error;
    }

    line = p_response->p_intermediates->line;
    err = at_tok_start(&line);

    if (err < 0) {
        goto error;
    }

    err = at_tok_nextint(&line, &response);

    if (err < 0) {
        goto error;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &response, sizeof(int));
    at_response_free(p_response);
    return;
error:
    at_response_free(p_response);
    ALOGE("requestLastCallFailCause error!");
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void requestConference(RIL_Token t)
{
    ATResponse *p_response = NULL;
    int err;

    // 3GPP 22.030 6.5.5
    // "Adds a held call to the conversation"
    err = at_send_command("AT+CHLD=3", &p_response);
    if (err < 0 || at_get_cme_error(p_response) != CME_SUCCESS)
        goto error;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    at_response_free(p_response);
    return;
error:
    at_response_free(p_response);
    ALOGE("requestConference error!");
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void requestSeparateConnection(void *data, size_t datalen, RIL_Token t)
{
    char  cmd[12];
    int   party = ((int*)data)[0];

    ATResponse *p_response = NULL;
    int err;

    // Make sure that party is in a valid range.
    // (Note: The Telephony middle layer imposes a range of 1 to 7.
    // It's sufficient for us to just make sure it's single digit.)
    if (party <= 0 || party >=10)
        goto error;

    sprintf(cmd, "AT+CHLD=2%d", party);
    err = at_send_command(cmd, &p_response);
    if (err < 0 || at_get_cme_error(p_response) != CME_SUCCESS)
        goto error;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    at_response_free(p_response);
    return;
error:
    at_response_free(p_response);
    ALOGE("requestSeparateConnection error!");
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static int handleSignalStrength(char *line, RIL_SignalStrength_v6 *response) {
    int num = sizeof(RIL_SignalStrength_v6) / sizeof(int);
    int *p = (int *)response;

    while (num--) {
        if (at_tok_nextint(&line, p++) < 0) {
            return -1;
        }
    }
    return 0;
}

static void requestSignalStrength(void *data, size_t datalen, RIL_Token t)
{
    ATResponse *p_response = NULL;
    int err;
    char *line;
    RIL_SignalStrength_v6 response;

    err = at_send_command_singleline("AT+CSQ", "+CSQ:", &p_response);

    if (err < 0 || p_response->success == 0) {
        goto error;
    }

    line = p_response->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0) goto error;

    err = handleSignalStrength(line, &response);
    if (err < 0) goto error;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &response, sizeof(response));

    at_response_free(p_response);
    return;

error:
    ALOGE("requestSignalStrength must never return an error when radio is on");
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

/**
 * networkModePossible. Decides whether the network mode is appropriate for the
 * specified modem
 */
static int networkModePossible(ModemInfo *mdm, int nm)
{
    if ((sizeof(net2pmask) / sizeof(int32_t)) > nm &&
        (sizeof(net2modem) / sizeof(int)) > nm &&
        (net2modem[nm] & mdm->supportedTechs) == net2modem[nm]) {
        return 1;
    }
    return 0;
}
static void requestSetPreferredNetworkType( int request, void *data,
                                            size_t datalen, RIL_Token t )
{
    ATResponse *p_response = NULL;
    char *cmd = NULL;
    int value = *(int *)data;
    int current, old;
    int err;
    int32_t preferred;

    if (!networkModePossible(sMdmInfo, value)) {
        RIL_onRequestComplete(t, RIL_E_MODE_NOT_SUPPORTED, NULL, 0);
        return;
    }

    preferred = net2pmask[value];
    ALOGD("requestSetPreferredNetworkType: current: %x. New: %x", PREFERRED_NETWORK(sMdmInfo), preferred);

    if (query_ctec(sMdmInfo, &current, NULL) < 0) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return;
    }
    old = PREFERRED_NETWORK(sMdmInfo);
    ALOGD("old != preferred: %d", old != preferred);
    if (old != preferred) {
        asprintf(&cmd, "AT+CTEC=%d,\"%x\"", current, preferred);
        ALOGD("Sending command: <%s>", cmd);
        err = at_send_command_singleline(cmd, "+CTEC:", &p_response);
        free(cmd);
        if (err || !p_response->success) {
            RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            return;
        }
        PREFERRED_NETWORK(sMdmInfo) = value;
        if (!strstr( p_response->p_intermediates->line, "DONE") ) {
            int current;
            int res = parse_technology_response(p_response->p_intermediates->line, &current, NULL);
            switch (res) {
                case -1: // Error or unable to parse
                    break;
                case 1: // Only able to parse current
                case 0: // Both current and preferred were parsed
                    setRadioTechnology(sMdmInfo, current);
                    break;
            }
        }
    }
    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
}

static void requestGetPreferredNetworkType(int request, void *data,
                                   size_t datalen, RIL_Token t)
{
    int preferred;
    unsigned i;

    switch ( query_ctec(sMdmInfo, NULL, &preferred) ) {
        case -1: // Error or unable to parse
        case 1: // Only able to parse current
            RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            break;
        case 0: // Both current and preferred were parsed
            for ( i = 0 ; i < sizeof(net2pmask) / sizeof(int32_t) ; i++ ) {
                if (preferred == net2pmask[i]) {
                    RIL_onRequestComplete(t, RIL_E_SUCCESS, &i, sizeof(int));
                    return;
                }
            }
            ALOGE("Unknown preferred mode received from modem: %d", preferred);
            RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            break;
    }

}

static void requestCdmaPrlVersion(int request, void *data,
                                   size_t datalen, RIL_Token t)
{
    int err;
    char * responseStr;
    ATResponse *p_response = NULL;
    const char *cmd;
    char *line;

    err = at_send_command_singleline("AT+WPRL?", "+WPRL:", &p_response);
    if (err < 0 || !p_response->success) goto error;
    line = p_response->p_intermediates->line;
    err = at_tok_start(&line);
    if (err < 0) goto error;
    err = at_tok_nextstr(&line, &responseStr);
    if (err < 0 || !responseStr) goto error;
    RIL_onRequestComplete(t, RIL_E_SUCCESS, responseStr, strlen(responseStr));
    at_response_free(p_response);
    return;
error:
    at_response_free(p_response);
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void requestCdmaBaseBandVersion(int request, void *data,
                                   size_t datalen, RIL_Token t)
{
    int err;
    char * responseStr;
    ATResponse *p_response = NULL;
    const char *cmd;
    const char *prefix;
    char *line, *p;
    int commas;
    int skip;
    int count = 4;

    // Fixed values. TODO: query modem
    responseStr = strdup("1.0.0.0");
    RIL_onRequestComplete(t, RIL_E_SUCCESS, responseStr, sizeof(responseStr));
    free(responseStr);
}

static void requestCdmaDeviceIdentity(int request, void *data,
                                        size_t datalen, RIL_Token t)
{
    int err;
    int response[4];
    char * responseStr[4];
    ATResponse *p_response = NULL;
    const char *cmd;
    const char *prefix;
    char *line, *p;
    int commas;
    int skip;
    int count = 4;

    // Fixed values. TODO: Query modem
    responseStr[0] = "----";
    responseStr[1] = "----";
    responseStr[2] = "77777777";

    err = at_send_command_numeric("AT+CGSN", &p_response);
    if (err < 0 || p_response->success == 0) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return;
    } else {
        responseStr[3] = p_response->p_intermediates->line;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, responseStr, count*sizeof(char*));
    at_response_free(p_response);

    return;
error:
    ALOGE("requestCdmaDeviceIdentity must never return an error when radio is on");
    at_response_free(p_response);
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void requestCdmaGetSubscriptionSource(int request, void *data,
                                        size_t datalen, RIL_Token t)
{
    int err;
    int *ss = (int *)data;
    ATResponse *p_response = NULL;
    char *cmd = NULL;
    char *line = NULL;
    int response;

    asprintf(&cmd, "AT+CCSS?");
    if (!cmd) goto error;

    err = at_send_command_singleline(cmd, "+CCSS:", &p_response);
    if (err < 0 || !p_response->success)
        goto error;

    line = p_response->p_intermediates->line;
    err = at_tok_start(&line);
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &response);
    free(cmd);
    cmd = NULL;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &response, sizeof(response));

    return;
error:
    free(cmd);
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void requestCdmaSetSubscriptionSource(int request, void *data,
                                        size_t datalen, RIL_Token t)
{
    int err;
    int *ss = (int *)data;
    ATResponse *p_response = NULL;
    char *cmd = NULL;

    if (!ss || !datalen) {
        ALOGE("RIL_REQUEST_CDMA_SET_SUBSCRIPTION without data!");
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return;
    }
    asprintf(&cmd, "AT+CCSS=%d", ss[0]);
    if (!cmd) goto error;

    err = at_send_command(cmd, &p_response);
    if (err < 0 || !p_response->success)
        goto error;
    free(cmd);
    cmd = NULL;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);

    RIL_onUnsolicitedResponse(RIL_UNSOL_CDMA_SUBSCRIPTION_SOURCE_CHANGED, ss, sizeof(ss[0]));

    return;
error:
    free(cmd);
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void requestCdmaSubscription(int request, void *data,
                                        size_t datalen, RIL_Token t)
{
    int err;
    int response[5];
    char * responseStr[5];
    ATResponse *p_response = NULL;
    const char *cmd;
    const char *prefix;
    char *line, *p;
    int commas;
    int skip;
    int count = 5;

    // Fixed values. TODO: Query modem
    responseStr[0] = "8587777777"; // MDN
    responseStr[1] = "1"; // SID
    responseStr[2] = "1"; // NID
    responseStr[3] = "8587777777"; // MIN
    responseStr[4] = "1"; // PRL Version
    RIL_onRequestComplete(t, RIL_E_SUCCESS, responseStr, count*sizeof(char*));

    return;
error:
    ALOGE("requestRegistrationState must never return an error when radio is on");
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void requestCdmaGetRoamingPreference(int request, void *data,
                                                 size_t datalen, RIL_Token t)
{
    int roaming_pref = -1;
    ATResponse *p_response = NULL;
    char *line;
    int res;

    res = at_send_command_singleline("AT+WRMP?", "+WRMP:", &p_response);
    if (res < 0 || !p_response->success) {
        goto error;
    }
    line = p_response->p_intermediates->line;

    res = at_tok_start(&line);
    if (res < 0) goto error;

    res = at_tok_nextint(&line, &roaming_pref);
    if (res < 0) goto error;

     RIL_onRequestComplete(t, RIL_E_SUCCESS, &roaming_pref, sizeof(roaming_pref));
    return;
error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void requestCdmaSetRoamingPreference(int request, void *data,
                                                 size_t datalen, RIL_Token t)
{
    int *pref = (int *)data;
    ATResponse *p_response = NULL;
    char *line;
    int res;
    char *cmd = NULL;

    asprintf(&cmd, "AT+WRMP=%d", *pref);
    if (cmd == NULL) goto error;

    res = at_send_command(cmd, &p_response);
    if (res < 0 || !p_response->success)
        goto error;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    free(cmd);
    return;
error:
    free(cmd);
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static int parseRegistrationState(char *str, int *type, int *items, int **response)
{
    int err;
    char *line = str, *p;
    int *resp = NULL;
    int skip;
    int count = 3;
    int commas;

    ALOGD("parseRegistrationState. Parsing: %s",str);
    err = at_tok_start(&line);
    if (err < 0) goto error;

    /* Ok you have to be careful here
     * The solicited version of the CREG response is
     * +CREG: n, stat, [lac, cid]
     * and the unsolicited version is
     * +CREG: stat, [lac, cid]
     * The <n> parameter is basically "is unsolicited creg on?"
     * which it should always be
     *
     * Now we should normally get the solicited version here,
     * but the unsolicited version could have snuck in
     * so we have to handle both
     *
     * Also since the LAC and CID are only reported when registered,
     * we can have 1, 2, 3, or 4 arguments here
     *
     * finally, a +CGREG: answer may have a fifth value that corresponds
     * to the network type, as in;
     *
     *   +CGREG: n, stat [,lac, cid [,networkType]]
     */

    /* count number of commas */
    commas = 0;
    for (p = line ; *p != '\0' ;p++) {
        if (*p == ',') commas++;
    }

    resp = (int *)calloc(commas + 1, sizeof(int));
    if (!resp) goto error;
    switch (commas) {
        case 0: /* +CREG: <stat> */
            err = at_tok_nextint(&line, &resp[0]);
            if (err < 0) goto error;
            resp[1] = -1;
            resp[2] = -1;
        break;

        case 1: /* +CREG: <n>, <stat> */
            err = at_tok_nextint(&line, &skip);
            if (err < 0) goto error;
            err = at_tok_nextint(&line, &resp[0]);
            if (err < 0) goto error;
            resp[1] = -1;
            resp[2] = -1;
            if (err < 0) goto error;
        break;

        case 2: /* +CREG: <stat>, <lac>, <cid> */
            err = at_tok_nextint(&line, &resp[0]);
            if (err < 0) goto error;
            err = at_tok_nexthexint(&line, &resp[1]);
            if (err < 0) goto error;
            err = at_tok_nexthexint(&line, &resp[2]);
            if (err < 0) goto error;
        break;
        case 3: /* +CREG: <n>, <stat>, <lac>, <cid> */
            err = at_tok_nextint(&line, &skip);
            if (err < 0) goto error;
            err = at_tok_nextint(&line, &resp[0]);
            if (err < 0) goto error;
            err = at_tok_nexthexint(&line, &resp[1]);
            if (err < 0) goto error;
            err = at_tok_nexthexint(&line, &resp[2]);
            if (err < 0) goto error;
        break;
        /* special case for CGREG, there is a fourth parameter
         * that is the network type (unknown/gprs/edge/umts)
         */
        case 4: /* +CGREG: <n>, <stat>, <lac>, <cid>, <networkType> */
            err = at_tok_nextint(&line, &skip);
            if (err < 0) goto error;
            err = at_tok_nextint(&line, &resp[0]);
            if (err < 0) goto error;
            err = at_tok_nexthexint(&line, &resp[1]);
            if (err < 0) goto error;
            err = at_tok_nexthexint(&line, &resp[2]);
            if (err < 0) goto error;
            err = at_tok_nexthexint(&line, &resp[3]);
            if (err < 0) goto error;
            count = 4;
        break;
        default:
            goto error;
    }
    if (response)
        *response = resp;
    if (items)
        *items = commas + 1;
    if (type)
        *type = techFromModemType(TECH(sMdmInfo));
    return 0;
error:
    free(resp);
    return -1;
}

#define REG_STATE_LEN 15
#define REG_DATA_STATE_LEN 6
static void requestRegistrationState(int request, void *data,
                                        size_t datalen, RIL_Token t)
{
    int err;
    int *registration;
    char **responseStr = NULL;
    ATResponse *p_response = NULL;
    const char *cmd;
    const char *prefix;
    char *line;
    int i = 0, j, numElements = 0;
    int count = 3;
    int type;

    ALOGD("requestRegistrationState");
    if (request == RIL_REQUEST_VOICE_REGISTRATION_STATE) {
        cmd = "AT+CREG?";
        prefix = "+CREG:";
        numElements = REG_STATE_LEN;
    } else if (request == RIL_REQUEST_DATA_REGISTRATION_STATE) {
        cmd = "AT+CGREG?";
        prefix = "+CGREG:";
        numElements = REG_DATA_STATE_LEN;
    } else {
        assert(0);
        goto error;
    }

    err = at_send_command_singleline(cmd, prefix, &p_response);

    if (err != 0) goto error;

    line = p_response->p_intermediates->line;

    if (parseRegistrationState(line, &type, &count, &registration)) goto error;

    responseStr = malloc(numElements * sizeof(char *));
    if (!responseStr) goto error;
    memset(responseStr, 0, numElements * sizeof(char *));
    /**
     * The first '4' bytes for both registration states remain the same.
     * But if the request is 'DATA_REGISTRATION_STATE',
     * the 5th and 6th byte(s) are optional.
     */
    if (is3gpp2(type) == 1) {
        ALOGD("registration state type: 3GPP2");
        // TODO: Query modem
        if(request == RIL_REQUEST_VOICE_REGISTRATION_STATE) {
            asprintf(&responseStr[3], "8");     // EvDo revA
            asprintf(&responseStr[4], "1");     // BSID
            asprintf(&responseStr[5], "123");   // Latitude
            asprintf(&responseStr[6], "222");   // Longitude
            asprintf(&responseStr[7], "0");     // CSS Indicator
            asprintf(&responseStr[8], "4");     // SID
            asprintf(&responseStr[9], "65535"); // NID
            asprintf(&responseStr[10], "0");    // Roaming indicator
            asprintf(&responseStr[11], "1");    // System is in PRL
            asprintf(&responseStr[12], "0");    // Default Roaming indicator
            asprintf(&responseStr[13], "0");    // Reason for denial
            asprintf(&responseStr[14], "0");    // Primary Scrambling Code of Current cell
      } else if (request == RIL_REQUEST_DATA_REGISTRATION_STATE) {
            asprintf(&responseStr[3], "8");   // Available data radio technology
      }
    } else { // type == RADIO_TECH_3GPP
        ALOGD("registration state type: 3GPP");
        if (registration[1] >= 0)
            asprintf(&responseStr[1], "%x", registration[1]);
        if (registration[2] >= 0)
            asprintf(&responseStr[2], "%x", registration[2]);
        if (count > 3)
            asprintf(&responseStr[3], "%d", registration[3]);
    }
    asprintf(&responseStr[0], "%d", registration[0]);

    /**
     * Optional bytes for DATA_REGISTRATION_STATE request
     * 4th byte : Registration denial code
     * 5th byte : The max. number of simultaneous Data Calls
     */
    if(request == RIL_REQUEST_DATA_REGISTRATION_STATE) {
        // asprintf(&responseStr[4], "3");
        // asprintf(&responseStr[5], "1");
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, responseStr, numElements*sizeof(responseStr));
    goto done;

error:
    ALOGE("requestRegistrationState must never return an error when radio is on");
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);

done:
    if (responseStr) {
        for (j = 0; j < numElements; j++) {
            free(responseStr[j]);
            responseStr[j] = NULL;
        }
        free(responseStr);
        responseStr = NULL;
    }
    if (registration) {
        free(registration);
        registration = NULL;
    }
    at_response_free(p_response);
}

static int parseOperatorStatus(char **line, char **status)
{
    int statusCode;

    int err = at_tok_nextint(line, &statusCode);
    if (err < 0) {
        return err;
    }

    *status = (char *) calloc(sizeof(char), 10);
    switch (statusCode) {
        case 0: // A_STATUS_UNKNOWN
            strlcpy(*status, "unknown", 10);
            break;
        case 1: // A_STATUS_AVAILABLE
            strlcpy(*status, "available", 10);
            break;
        case 2: // A_STATUS_CURRENT
            strlcpy(*status, "current", 10);
            break;
        case 3: // A_STATUS_DENIED
            strlcpy(*status, "forbidden", 10);
            break;
    }

    return 0;
}

static int copyNextStr(char **line, char **str)
{
    char *buffer;
    int bufferLen;

    int err = at_tok_nextstr(line, &buffer);
    if (err < 0) {
        return err;
    }

    bufferLen = strlen(buffer) + 1;
    *str = calloc(sizeof(char), bufferLen);
    strlcpy(*str, buffer, bufferLen);

    return 0;
}

static int parseOperatorInfo(char *info, char **p_operatorStart)
{
    int err = at_tok_start(&info);
    if (err < 0) {
        ALOGE("QUERY_AVAILABLE_NETWORKS: Error tokenizing operator status");
        return err;
    }

    char *status;
    err = parseOperatorStatus(&info, &status);
    if (err < 0) {
        ALOGE("QUERY_AVAILABLE_NETWORKS: Error parsing operator status");
        return err;
    }
    p_operatorStart[3] = status;

    // long name
    char *longName;
    err = copyNextStr(&info, &longName);
    if (err < 0) {
        ALOGE("QUERY_AVAILABLE_NETWORKS: Error copying long name from operator");
        return err;
    }
    p_operatorStart[0] = longName;

    // short name
    char *shortName;
    err = copyNextStr(&info, &shortName);
    if (err < 0) {
        ALOGE("QUERY_AVAILABLE_NETWORKS: Error copying short name from operator");
        return err;
    }
    p_operatorStart[1] = shortName;

    // numeric tuple
    char *numeric;
    err = copyNextStr(&info, &numeric);
    if (err < 0) {
        ALOGE("QUERY_AVAILABLE_NETWORKS: Error copying numeric tuple from operator");
        return err;
    }
    p_operatorStart[2] = numeric;

    return 0;
}

static int requestAvailableOperators(char ***p_operators, int *p_bufferSize)
{
    // Modem command for available operators
    ATResponse *p_response;
    int err = at_send_command_multiline ("AT+COPS=?", "+COPS:", &p_response);

    if (err < 0 || !p_response->p_intermediates) {
        ALOGE("Error: No operator list returned");
        return err;
    }

    // The operator list from the emulator is non-standard
    // so we have to jump through some  hoops to parse it
    // correctly. With the AT protocol, usually multiple
    // records are returned on a line-by-line basis
    // with a special prefix on each line.
    // With +COPS=?, the entire result is on one line,
    // with each record surrounded in parentheses, and
    // each record separated by commas. regex to the rescue!
    char *line = p_response->p_intermediates->line;
    int nMatches = 2;
    regex_t operatorRegex;
    regmatch_t operatorMatches[nMatches];

    regcomp(&operatorRegex, "\\(([^\\)]+)\\)", REG_EXTENDED);

    char *tmp = line;
    int operatorCount = 0;

    // First time around we just count the total number of operators
    while (regexec(&operatorRegex, tmp, nMatches, operatorMatches, 0) == 0) {
        tmp += operatorMatches[0].rm_eo;
        ++operatorCount;
    }

    char **operators;
    const char *prefix = "+COPS: ";
    int i = 0, prefixLen = 7;

    // 4 entries per operator: longName, shortName, numeric, status
    *p_bufferSize = operatorCount * 4;
    operators = (char **) malloc(sizeof(char *) * operatorCount * 4);

    while (regexec(&operatorRegex, line, nMatches, operatorMatches, 0) == 0) {
        regoff_t start = operatorMatches[1].rm_so;
        regoff_t end = operatorMatches[1].rm_eo;
        regoff_t length = end - start;

        // We normalize by re-building the line prefix and using
        // the standard at_tok_* functions
        char *group = (char *) calloc(sizeof(char), prefixLen + length + 1);
        strncat(group, prefix, prefixLen);
        strncat(group, line + start, length);

        err = parseOperatorInfo(group, &(operators[i * 4]));
        free(group);

        if (err < 0) {
            break;
        }

        line += length;
        ++i;
    }

    at_response_free(p_response);
    *p_operators = operators;
    return err;
}

static void requestOperator(void *data, size_t datalen, RIL_Token t)
{
    int err;
    int i;
    int skip;
    ATLine *p_cur;
    char *response[3];

    memset(response, 0, sizeof(response));

    ATResponse *p_response = NULL;

    err = at_send_command_multiline(
        "AT+COPS=3,0;+COPS?;+COPS=3,1;+COPS?;+COPS=3,2;+COPS?",
        "+COPS:", &p_response);

    /* we expect 3 lines here:
     * +COPS: 0,0,"T - Mobile"
     * +COPS: 0,1,"TMO"
     * +COPS: 0,2,"310170"
     */

    if (err != 0) goto error;

    switch (at_get_cme_error(p_response)) {
        case CME_SUCCESS:
            break;

        case CME_NO_NETWORK_SERVICE:
            goto done;

        default:
            goto error;
    }

    for (i = 0, p_cur = p_response->p_intermediates
            ; p_cur != NULL
            ; p_cur = p_cur->p_next, i++
    ) {
        char *line = p_cur->line;

        err = at_tok_start(&line);
        if (err < 0) goto error;

        err = at_tok_nextint(&line, &skip);
        if (err < 0) goto error;

        // If we're unregistered, we may just get
        // a "+COPS: 0" response
        if (!at_tok_hasmore(&line)) {
            response[i] = NULL;
            continue;
        }

        err = at_tok_nextint(&line, &skip);
        if (err < 0) goto error;

        // a "+COPS: 0, n" response is also possible
        if (!at_tok_hasmore(&line)) {
            response[i] = NULL;
            continue;
        }

        err = at_tok_nextstr(&line, &(response[i]));
        if (err < 0) goto error;
    }

    if (i != 3) {
        /* expect 3 lines exactly */
        goto error;
    }

done:
    RIL_onRequestComplete(t, RIL_E_SUCCESS, response, sizeof(response));
    at_response_free(p_response);

    return;
error:
    ALOGE("requestOperator must not return error when radio is on");
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

static void requestCdmaSendSMS(void *data, size_t datalen, RIL_Token t)
{
    int err = 1; // Set to go to error:
    RIL_SMS_Response response;
    RIL_CDMA_SMS_Message* rcsm;

    ALOGD("requestCdmaSendSMS datalen=%d, sizeof(RIL_CDMA_SMS_Message)=%d",
            datalen, sizeof(RIL_CDMA_SMS_Message));

    // verify data content to test marshalling/unmarshalling:
    rcsm = (RIL_CDMA_SMS_Message*)data;
    ALOGD("TeleserviceID=%d, bIsServicePresent=%d, \
            uServicecategory=%d, sAddress.digit_mode=%d, \
            sAddress.Number_mode=%d, sAddress.number_type=%d, ",
            rcsm->uTeleserviceID,  rcsm->bIsServicePresent,
            rcsm->uServicecategory,rcsm->sAddress.digit_mode,
            rcsm->sAddress.number_mode,rcsm->sAddress.number_type);

    if (err != 0) goto error;

    // Cdma Send SMS implementation will go here:
    // But it is not implemented yet.

    memset(&response, 0, sizeof(response));
    RIL_onRequestComplete(t, RIL_E_SUCCESS, &response, sizeof(response));
    return;

error:
    // Cdma Send SMS will always cause send retry error.
    RIL_onRequestComplete(t, RIL_E_SMS_SEND_FAIL_RETRY, NULL, 0);
}

static void requestSendSMS(void *data, size_t datalen, RIL_Token t)
{
    int err;
    const char *smsc;
    const char *pdu;
    int tpLayerLength;
    char *cmd1, *cmd2;
    RIL_SMS_Response response;
    ATResponse *p_response = NULL;

    smsc = ((const char **)data)[0];
    pdu = ((const char **)data)[1];

    tpLayerLength = strlen(pdu)/2;

    // "NULL for default SMSC"
    if (smsc == NULL) {
        smsc= "00";
    }

    asprintf(&cmd1, "AT+CMGS=%d", tpLayerLength);
    asprintf(&cmd2, "%s%s", smsc, pdu);

    err = at_send_command_sms(cmd1, cmd2, "+CMGS:", &p_response);

    if (err != 0 || p_response->success == 0) goto error;

    memset(&response, 0, sizeof(response));

    /* FIXME fill in messageRef and ackPDU */

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &response, sizeof(response));
    at_response_free(p_response);

    return;
error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

static int configureInterface(const char* ifname, const char *addr)
{
    char ip[16];
    int prefixLen, ret = -1;

    if (2 != sscanf(addr, "%[.0-9]/%d", ip, &prefixLen))
        return ret;

    if (ifc_init())
        return ret;

    if (!ifc_up(ifname)) {
        if (ifc_set_addr(ifname, inet_addr(ip)) ||
            ifc_set_prefixLength(ifname, prefixLen)) {
            ifc_down(ifname);
        } else {
            ret = 0;
        }
    }

    ifc_close();

    return ret;
}

static int deconfigureInterface(const char* ifname)
{
    int ret;

    if (ifc_init())
        return -1;

    ret = ifc_down(ifname);
    ifc_close();
    return ret;
}

static int findFreeCid()
{
    ATResponse *p_response = NULL;
    ATLine *p_cur;

    int err, new_cid = -1;
    int dataStates[s_maxDataContexts];
    memset( dataStates, 0, s_maxDataContexts * sizeof(int) );

    // Query current active pdp contexts.
    err = at_send_command_multiline ("AT+CGACT?", "+CGACT:", &p_response);
    if (err != 0 || p_response->success == 0) {
        goto error;
    }

    for (p_cur = p_response->p_intermediates; p_cur != NULL;
         p_cur = p_cur->p_next) {
        char *line = p_cur->line;
        int cid, state;

        err = at_tok_start(&line);
        if (err < 0)
            goto error;

        err = at_tok_nextint(&line, &cid);
        if (err < 0)
            goto error;

        err = at_tok_nextint(&line, &state);
        if (err < 0)
            goto error;

        // Found an inactive slot, just reuse cid.
        if (!state) {
            new_cid = cid;
            break;
        }

        // Error, cid exceeds range of supported PDP contexts.
        if (cid > s_maxDataContexts) {
            continue;
        }

        dataStates[cid - 1] = state;
    }
    at_response_free(p_response);

    if (new_cid > -1) {
        return new_cid;
    }

    int i;
    for (i = 0; i < s_maxDataContexts; i++) {
        if (dataStates[i] == 0) {
            return i + 1;
        }
    }

    return -1;

error:
    at_response_free(p_response);
    return -1;
}

static void requestSetupDataCall(void *data, size_t datalen, RIL_Token t)
{
    const char *apn;
    char *cmd;
    int err;
    ATResponse *p_response = NULL;

    apn = ((const char **)data)[2];

#ifdef USE_TI_COMMANDS
    // Config for multislot class 10 (probably default anyway eh?)
    err = at_send_command("AT%CPRIM=\"GMM\",\"CONFIG MULTISLOT_CLASS=<10>\"",
                        NULL);

    err = at_send_command("AT%DATA=2,\"UART\",1,,\"SER\",\"UART\",0", NULL);
#endif /* USE_TI_COMMANDS */

    int fd, qmistatus;
    size_t cur = 0;
    size_t len;
    ssize_t written, rlen;
    char status[32] = {0};
    int retry = 10;
    const char *pdp_type;

    ALOGD("requesting data connection to APN '%s'", apn);

    fd = open ("/dev/qmi", O_RDWR);
    if (fd >= 0) { /* the device doesn't exist on the emulator */

        ALOGD("opened the qmi device\n");
        asprintf(&cmd, "up:%s", apn);
        len = strlen(cmd);

        while (cur < len) {
            do {
                written = write (fd, cmd + cur, len - cur);
            } while (written < 0 && errno == EINTR);

            if (written < 0) {
                ALOGE("### ERROR writing to /dev/qmi");
                close(fd);
                goto error;
            }

            cur += written;
        }

        // wait for interface to come online

        do {
            sleep(1);
            do {
                rlen = read(fd, status, 31);
            } while (rlen < 0 && errno == EINTR);

            if (rlen < 0) {
                ALOGE("### ERROR reading from /dev/qmi");
                close(fd);
                goto error;
            } else {
                status[rlen] = '\0';
                ALOGD("### status: %s", status);
            }
        } while (strncmp(status, "STATE=up", 8) && strcmp(status, "online") && --retry);

        close(fd);

        if (retry == 0) {
            ALOGE("### Failed to get data connection up\n");
            goto error;
        }

        qmistatus = system("netcfg rmnet0 dhcp");

        ALOGD("netcfg rmnet0 dhcp: status %d\n", qmistatus);

        if (qmistatus < 0) goto error;

    } else {
        RIL_Data_Call_Response_v6 tmp_rp;
        int ret, cid;

        if (datalen > 6 * sizeof(char *)) {
            pdp_type = ((const char **)data)[6];
        } else {
            pdp_type = "IP";
        }

        cid = findFreeCid();
        if (cid < 0) {
            ALOGE("error: no free cid found.");
            goto error;
        }

        asprintf(&cmd, "AT+CGDCONT=%d,\"%s\",\"%s\",,0,0", cid, pdp_type, apn);
        //FIXME check for error here
        err = at_send_command(cmd, NULL);
        free(cmd);

        // Set required QoS params to default
        asprintf(&cmd, "AT+CGQREQ=%d", cid);
        err = at_send_command(cmd, NULL);
        free(cmd);

        // Set minimum QoS params to default
        asprintf(&cmd, "AT+CGQMIN=%d", cid);
        err = at_send_command(cmd, NULL);
        free(cmd);

        // packet-domain event reporting
        err = at_send_command("AT+CGEREP=1,0", NULL);

        // Hangup anything that's happening there now
        asprintf(&cmd, "AT+CGACT=0,%d", cid);
        err = at_send_command(cmd, NULL);
        free(cmd);

        // Start data on PDP context 1
        asprintf(&cmd, "ATD*99***%d#", cid);
        err = at_send_command(cmd, NULL);
        free(cmd);

        // Retrieve dynamic properties & setup kernel iface
        asprintf(&cmd, "AT+CGCONTRDP=%d", cid);
        err = at_send_command_singleline(cmd, "+CGCONTRDP:", &p_response);
        free(cmd);
        if (err < 0 || p_response->success == 0) {
            goto error;
        }

        if (parseCGCONTRDP(p_response->p_intermediates->line, &tmp_rp) < 0)
            goto error;

        ret = configureInterface(tmp_rp.ifname, tmp_rp.addresses);
        if (ret < 0) {
            deconfigureInterface(tmp_rp.ifname);
            freeParsedCGCONTRDP(&tmp_rp);
            goto error;
        }

        RIL_onRequestComplete(t, RIL_E_SUCCESS, &tmp_rp, sizeof(tmp_rp));

        freeParsedCGCONTRDP(&tmp_rp);
        at_response_free(p_response);

        return;
    }

    requestOrSendDataCallList(&t);

    at_response_free(p_response);

    return;
error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);

}

static void requestDeactivateDataCall(void *data, size_t datalen, RIL_Token t)
{
    ATResponse *p_response = NULL;
    int err, id;
    char *cmd, *cid, *ifname;

    cid = ((char **)data)[0];
    asprintf(&cmd, "AT+CGACT=0,%s", cid);
    err = at_send_command(cmd, &p_response);
    if (err < 0 || p_response->success == 0) {
        goto error;
    }
    free(cmd);

    // +CGDCONT=<cid> causes the values for context number <cid> to become
    // undefined.
    asprintf(&cmd, "AT+CGDCONT=%s", cid);
    err = at_send_command(cmd, &p_response);
    if (err < 0 || p_response->success == 0) {
        goto error;
    }

    id = atoi(cid) - 1;
    asprintf(&ifname, "rmnet%d", id);
    deconfigureInterface(ifname);
    free(ifname);

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    at_response_free(p_response);
    free(cmd);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
    free(cmd);
}

static void requestSMSAcknowledge(void *data, size_t datalen, RIL_Token t)
{
    int ackSuccess;
    int err;

    ackSuccess = ((int *)data)[0];

    if (ackSuccess == 1) {
        err = at_send_command("AT+CNMA=1", NULL);
    } else if (ackSuccess == 0)  {
        err = at_send_command("AT+CNMA=2", NULL);
    } else {
        ALOGE("unsupported arg to RIL_REQUEST_SMS_ACKNOWLEDGE\n");
        goto error;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);

}

static void  requestSIM_IO(void *data, size_t datalen, RIL_Token t)
{
    ATResponse *p_response = NULL;
    RIL_SIM_IO_Response sr;
    int err;
    char *cmd = NULL;
    RIL_SIM_IO_v6 *p_args;
    char *line;

    memset(&sr, 0, sizeof(sr));

    p_args = (RIL_SIM_IO_v6 *)data;

    /* FIXME handle pin2 */

    if (p_args->data == NULL) {
        asprintf(&cmd, "AT+CRSM=%d,%d,%d,%d,%d",
                    p_args->command, p_args->fileid,
                    p_args->p1, p_args->p2, p_args->p3);
    } else {
        asprintf(&cmd, "AT+CRSM=%d,%d,%d,%d,%d,%s",
                    p_args->command, p_args->fileid,
                    p_args->p1, p_args->p2, p_args->p3, p_args->data);
    }

    err = at_send_command_singleline(cmd, "+CRSM:", &p_response);

    if (err < 0 || p_response->success == 0) {
        goto error;
    }

    line = p_response->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &(sr.sw1));
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &(sr.sw2));
    if (err < 0) goto error;

    if (at_tok_hasmore(&line)) {
        err = at_tok_nextstr(&line, &(sr.simResponse));
        if (err < 0) goto error;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &sr, sizeof(sr));
    at_response_free(p_response);
    free(cmd);

    return;
error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
    free(cmd);

}

static int getCardLockRetryCount(char* lockType, int32_t* retryCount,
                                 int32_t* defaultRetryCount)
{
    ATResponse* p_response = NULL;
    int         err;
    char*       cmd = NULL;
    char*       line = NULL;
    char*       type = NULL;
    int         ret = CME_SUCCESS;

    // Initialize the retryCount;
    *retryCount = -1;
    *defaultRetryCount = -1;

    asprintf(&cmd, "AT+CPINR=%s", lockType);
    err = at_send_command_singleline(cmd, "+CPINR:", &p_response);
    free(cmd);

    if (err < 0 || p_response->success == 0) {
        ret = at_get_cme_error(p_response);
        goto done;
    }

    line = p_response->p_intermediates->line;

    // Parse the AT command result
    // +CPINR: <code>,<retries>[,<default_retries>]
    err = at_tok_start(&line);
    if (err < 0) {
        goto done;
    }

    err = at_tok_nextstr(&line, &type);
    if (err < 0) {
        goto done;
    }

    err = at_tok_nextint(&line, retryCount);
    if (err < 0) {
        goto done;
    }

    if (at_tok_hasmore(&line)) {
        at_tok_nextint(&line, defaultRetryCount);
    }

done:
    at_response_free(p_response);
    return ret;
}

static void  requestEnterSimPin(void*  data, size_t  datalen, RIL_Token  t)
{
    ATResponse   *p_response = NULL;
    int           err;
    char*         cmd = NULL;
    const char**  strings = (const char**)data;;

    if (getSIMStatus() != SIM_PIN) {
        int retries = -1;
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, &retries, sizeof(int));
        return;
    }

    asprintf(&cmd, "AT+CPIN=%s", strings[0]);
    err = at_send_command_singleline(cmd, "+CPIN:", &p_response);
    free(cmd);

    if (err < 0 || p_response->success == 0) {
        int retries[2] = {-1, -1};
        getCardLockRetryCount("SIM PIN", retries+0, retries+1);
        RIL_onRequestComplete(t, RIL_E_PASSWORD_INCORRECT, &retries[0], sizeof(int));
    } else {
        int retries = 0;
        RIL_onRequestComplete(t, RIL_E_SUCCESS, &retries, sizeof(int));
    }

    at_response_free(p_response);
}

static void  requestEnterSimPuk(void*  data, size_t  datalen, RIL_Token  t)
{
    ATResponse   *p_response = NULL;
    int           err;
    char*         cmd = NULL;
    const char**  strings = (const char**)data;;

    if (getSIMStatus() != SIM_PUK) {
        int retries = -1;
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, &retries, sizeof(int));
        return;
    }

    asprintf(&cmd, "AT+CPIN=%s,%s", strings[0], strings[1]);
    err = at_send_command_singleline(cmd, "+CPIN:", &p_response);
    free(cmd);

    if (err < 0 || p_response->success == 0) {
        int retries[2] = {-1, -1};
        getCardLockRetryCount("SIM PUK", retries+0, retries+1);
        RIL_onRequestComplete(t, RIL_E_PASSWORD_INCORRECT, &retries[0], sizeof(int));
    } else {
        int retries = 0;
        RIL_onRequestComplete(t, RIL_E_SUCCESS, &retries, sizeof(int));
    }

    at_response_free(p_response);
}

static void  requestChangeSimPin(void*  data, size_t  datalen, RIL_Token  t)
{
    ATResponse   *p_response = NULL;
    int           err;
    char*         cmd = NULL;
    const char**  strings = (const char**)data;;

    // Changing pin is only allowed when sim is ready.
    if (getSIMStatus() != SIM_READY) {
        int retries = -1;
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, &retries, sizeof(int));
        return;
    }

    asprintf(&cmd, "AT+CPIN=%s,%s", strings[0], strings[1]);
    err = at_send_command_singleline(cmd, "+CPIN:", &p_response);
    free(cmd);

    if (err < 0 || p_response->success == 0) {
        int retries[2] = {-1, -1};
        getCardLockRetryCount("SIM PIN", retries+0, retries+1);
        RIL_onRequestComplete(t, RIL_E_PASSWORD_INCORRECT, &retries[0], sizeof(int));
    } else {
        int retries = 0;
        RIL_onRequestComplete(t, RIL_E_SUCCESS, &retries, sizeof(int));
    }

    at_response_free(p_response);
}

static void  requestSendUSSD(void *data, size_t datalen, RIL_Token t)
{
    const char *ussdRequest;

    ussdRequest = (char *)(data);


    RIL_onRequestComplete(t, RIL_E_REQUEST_NOT_SUPPORTED, NULL, 0);

// @@@ TODO

}

static void requestExitEmergencyMode(void *data, size_t datalen, RIL_Token t)
{
    int err;
    ATResponse *p_response = NULL;

    err = at_send_command("AT+WSOS=0", &p_response);

    if (err < 0 || p_response->success == 0) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
}

static void requestGetSmscAddress(int request, void *data, size_t datalen, RIL_Token t)
{
    ATResponse *p_response = NULL;
    int err;
    char *line;

    err = at_send_command_singleline("AT+CSCA?", "+CSCA:", &p_response);

    if (err < 0 || p_response->success == 0) {
        goto error;
    }

    line = p_response->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0) goto error;

    // Skip first space
    line++;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, line, strlen(line));

    at_response_free(p_response);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
    return;
}

static void requestSetSmscAddress(int request, void *data, size_t datalen, RIL_Token t)
{
    ATResponse *p_response = NULL;
    int err;
    char cmd[64];

    snprintf(cmd, sizeof(cmd), "AT+CSCA=%s", data);
    err = at_send_command(cmd, &p_response);

    if (err < 0 || p_response->success == 0) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    return;
}

static void requestGetUnlockRetryCount(void*  data, size_t  datalen, RIL_Token  t)
{
    const char**  strings = (const char**)data;
    int           err;
    int           retries[2];

    if ( datalen != sizeof(char*) ) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return;
    }

    err = getCardLockRetryCount(strings[0], retries+0, retries+1);
    if (err != CME_SUCCESS) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, retries, sizeof(retries));
}

static void requestScreenState(void* data, size_t datalen, RIL_Token t)
{
    int*        on;
    int         err;
    char*       cmd = NULL;
    ATResponse* p_response = NULL;

    if ( datalen != sizeof(int) )
        goto error;

    on = data;

    asprintf(&cmd, "AT+CREG=%d", 1 + !!(on[0]));

    err = at_send_command(cmd, &p_response);
    free(cmd);
    if (err < 0 || p_response->success == 0) {
        goto error;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    at_response_free(p_response);

    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

// TODO: Use all radio types
static int techFromModemType(int mdmtype)
{
    int ret = -1;
    switch (1 << mdmtype) {
        case MDM_CDMA:
            ret = RADIO_TECH_1xRTT;
            break;
        case MDM_EVDO:
            ret = RADIO_TECH_EVDO_A;
            break;
        case MDM_GSM:
            ret = RADIO_TECH_GPRS;
            break;
        case MDM_WCDMA:
            ret = RADIO_TECH_HSPA;
            break;
        case MDM_LTE:
            ret = RADIO_TECH_LTE;
            break;
    }
    return ret;
}

static void requestQueryCallForwardStatus(void* data, size_t datalen, RIL_Token t)
{
    RIL_CallForwardInfo* p_info = (RIL_CallForwardInfo*) data;
    RIL_CallForwardInfo* p_results = NULL;
    RIL_CallForwardInfo** pp_results = NULL;
    ATResponse *p_response = NULL;
    ATLine* p_cur = NULL;
    char* cmd = NULL;
    int i = 0;
    int count = 0;
    // 0 means user doesn't input serviceClass. And according to TS 27.007,
    // the default value of class is 7 (voice, data and fax).
    int serviceClass = (p_info->serviceClass == 0) ? 7 : p_info->serviceClass;

    // Query call forwarding status.
    asprintf(&cmd, "AT+CCFC=%d,2,,,%d", p_info->reason, serviceClass);
    if (at_send_command_multiline(cmd, "+CCFC:", &p_response) < 0 ||
        at_get_cme_error(p_response) != CME_SUCCESS) {
        goto error;
    }

    // Count the call forwarding list
    for (p_cur = p_response->p_intermediates; p_cur != NULL; p_cur = p_cur->p_next) {
        count++;
    }

    // Init the pointer array
    pp_results = (RIL_CallForwardInfo**) malloc(count * sizeof(RIL_CallForwardInfo*));
    p_results = (RIL_CallForwardInfo*) malloc(count * sizeof(RIL_CallForwardInfo));
    memset(p_results, 0, count * sizeof(RIL_CallForwardInfo));

    for (i = 0; i < count; i++) {
        pp_results[i] = &p_results[i];
    }

    // Parse the AT command result
    // +CCFC: <status>,<class1>[,<number>,<type>[,<subaddr>,<satype>[,<time>]]
    for (i = 0, p_cur = p_response->p_intermediates; p_cur != NULL; p_cur = p_cur->p_next, i++) {
        char* line = p_cur->line;
        char* subaddr = NULL;
        char* satype = NULL;

        p_results[i].reason = p_info->reason;

        if (at_tok_start(&line) < 0) {
            goto error;
        }

        // Get <status>
        if (at_tok_nextint(&line, &p_results[i].status) < 0) {
            goto error;
        }

        // Get <class1>
        if (at_tok_nextint(&line, &p_results[i].serviceClass) < 0) {
            goto error;
        }

        if (!at_tok_hasmore(&line)) {
            continue;
        }

        // Get <number>
        if (at_tok_nextstr(&line, &p_results[i].number) < 0) {
            goto error;
        }

        // Get <type>
        if (at_tok_nextint(&line, &p_results[i].toa) < 0) {
            goto error;
        }

        if (!at_tok_hasmore(&line)) {
            continue;
        }

        // Get <subaddr>
        if (at_tok_nextstr(&line, &subaddr) < 0) {
            goto error;
        }

        // Get <satype>
        if (at_tok_nextstr(&line, &satype) < 0) {
            goto error;
        }

        if (!at_tok_hasmore(&line)) {
            continue;
        }

        // Get <time>
        if (at_tok_nextint(&line, &p_results[i].timeSeconds) < 0) {
            goto error;
        }
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, pp_results, count * sizeof(RIL_CallForwardInfo*));
    goto done;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);

done:
    at_response_free(p_response);
    free(pp_results);
    free(p_results);
    free(cmd);
}

static void requestSetCallForward(void* data, size_t datalen, RIL_Token t)
{
    // Modem command for call forwarding status
    RIL_CallForwardInfo* p_info = (RIL_CallForwardInfo*) data;
    ATResponse *p_response = NULL;
    char* cmd = NULL;

    switch (p_info->status) {
        case 0: /* disable */
        case 1: /* enable  */
        case 3: /* registeration */
        case 4: /* erasure */
            asprintf(&cmd, "AT+CCFC=%d,%d,\"%s\",%d,%d,,,%d", p_info->reason
                                                            , p_info->status
                                                            , p_info->number
                                                            , p_info->toa
                                                            , p_info->serviceClass
                                                            , p_info->timeSeconds);
            break;

        default:
            goto error;
    }

    if (at_send_command(cmd, &p_response) < 0 ||
        at_get_cme_error(p_response) != CME_SUCCESS) {
        goto error;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    goto done;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);

done:
    free(cmd);
    at_response_free(p_response);
}

static void requestQueryFacilityLock(void* data, size_t datalen, RIL_Token t)
{
    ATResponse   *p_response = NULL;
    ATLine       *p_cur;
    int           err;
    char*         cmd = NULL;
    const char**  strings = (const char**)data;
    int           serviceClass = 0;

    // Query facility lock. AT+CLCK=<fac>,<mode>[,<password>[,<class>]]
    asprintf(&cmd, "AT+CLCK=\"%s\",2,\"%s\",%d", strings[0], strings[1], atoi(strings[2]));
    err = at_send_command_multiline(cmd, "+CLCK:", &p_response);
    free(cmd);

    if (err < 0 || p_response->success == 0) {
        goto error;
    }

    // Parse the AT command result. +CLCK: <status>[,<class>]
    for (p_cur = p_response->p_intermediates; p_cur != NULL;
         p_cur = p_cur->p_next) {
        char *line = p_cur->line;
        int status;
        int class;

        err = at_tok_start(&line);
        if (err < 0) {
            goto error;
        }

        // Get <status>
        if (at_tok_nextint(&line, &status) < 0) {
            goto error;
        }

        if (!at_tok_hasmore(&line)) {
            continue;
        }

        // Get <class>
        if (at_tok_nextint(&line, &class) < 0) {
            goto error;
        }

        if (status == 1) {
            serviceClass |= class;
        }
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &serviceClass, sizeof(serviceClass));
    goto done;

error:
    RIL_onRequestComplete(t, cmeErrorToRilError(at_get_cme_error(p_response)), NULL, 0);

done:
    at_response_free(p_response);
}

static void requestSetFacilityLock(void* data, size_t datalen, RIL_Token t)
{
    ATResponse   *p_response = NULL;
    int           err;
    char*         cmd = NULL;
    const char**  strings = (const char**)data;

    // Set facility lock. AT+CLCK=<fac>,<mode>[,<password>[,<class>]]
    asprintf(&cmd, "AT+CLCK=\"%s\",%d,\"%s\",%d", strings[0], atoi(strings[1]),
             strings[2], atoi(strings[3]));
    err = at_send_command(cmd, &p_response);
    free(cmd);

    if (err < 0 || p_response->success == 0) {
        int retries[2] = {-1, -1};
        getCardLockRetryCount("SIM PIN", retries+0, retries+1);
        RIL_onRequestComplete(t, cmeErrorToRilError(at_get_cme_error(p_response)), &retries[0], sizeof(int));
    } else {
        int retries = 0;
        RIL_onRequestComplete(t, RIL_E_SUCCESS, &retries, sizeof(int));
    }

    at_response_free(p_response);
}

static void requestChangeBarringPassword(void* data, size_t datalen, RIL_Token t)
{
    ATResponse   *p_response = NULL;
    int           err;
    char*         cmd = NULL;
    const char**  strings = (const char**)data;

    // Change call barring password. AT+CPWD=<fac>,<oldpwd>,<newpwd>
    asprintf(&cmd, "AT+CPWD=\"%s\",\"%s\",\"%s\"", strings[0], strings[1], strings[2]);
    err = at_send_command(cmd, &p_response);
    free(cmd);

    if (err < 0 || p_response->success == 0) {
        RIL_onRequestComplete(t, cmeErrorToRilError(at_get_cme_error(p_response)), NULL, 0);
    } else {
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    }

    at_response_free(p_response);
}

static void requestStkSendTerminalResponse(const void* data, size_t datalen, RIL_Token t) {
    ATResponse *p_response = NULL;
    int err;
    char *cmd = NULL;
    const char* response = (const char*) data;

    // Send USAT terminal response: +CUSATT=<terminal_response>
    asprintf(&cmd, "AT+CUSATT=%s", response);

    err = at_send_command_singleline(cmd, "+CUSATT:", &p_response);
    free(cmd);

    if (err < 0 || p_response->success == 0) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    } else {
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    }

    at_response_free(p_response);
}

static void requestStkSendEnvelopeCommand(const void * data, size_t datalen, RIL_Token t) {
    ATResponse *p_response = NULL;
    int err;
    char *cmd = NULL;
    const char* envelope = (const char*) data;

    //  Send USAT envelope command: +CUSATE=<envelope_command>
    asprintf(&cmd, "AT+CUSATE=%s", envelope);

    err = at_send_command_singleline(cmd, "+CUSATE:", &p_response);
    free(cmd);

    if (err < 0 || p_response->success == 0) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    } else {
        // TODO fill in envelope response PDU
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    }

    at_response_free(p_response);
}

/*** Callback methods from the RIL library to us ***/

/**
 * Call from RIL to us to make a RIL_REQUEST
 *
 * Must be completed with a call to RIL_onRequestComplete()
 *
 * RIL_onRequestComplete() may be called from any thread, before or after
 * this function returns.
 *
 * Will always be called from the same thread, so returning here implies
 * that the radio is ready to process another command (whether or not
 * the previous command has completed).
 */

/**
 * CDMA specific request
 */
static void
onCdmaSpecificRequest (int request, void *data, size_t datalen, RIL_Token t)
{
    switch (request) {
        case RIL_REQUEST_CDMA_SEND_SMS:
            requestCdmaSendSMS(data, datalen, t);
            break;

        case RIL_REQUEST_BASEBAND_VERSION:
            requestCdmaBaseBandVersion(request, data, datalen, t);
            break;

        case RIL_REQUEST_DEVICE_IDENTITY:
            requestCdmaDeviceIdentity(request, data, datalen, t);
            break;

        case RIL_REQUEST_CDMA_SUBSCRIPTION:
            requestCdmaSubscription(request, data, datalen, t);
            break;

        case RIL_REQUEST_CDMA_SET_SUBSCRIPTION_SOURCE:
            requestCdmaSetSubscriptionSource(request, data, datalen, t);
            break;

        case RIL_REQUEST_CDMA_GET_SUBSCRIPTION_SOURCE:
            requestCdmaGetSubscriptionSource(request, data, datalen, t);
            break;

        case RIL_REQUEST_CDMA_QUERY_ROAMING_PREFERENCE:
            requestCdmaGetRoamingPreference(request, data, datalen, t);
            break;

        case RIL_REQUEST_CDMA_SET_ROAMING_PREFERENCE:
            requestCdmaSetRoamingPreference(request, data, datalen, t);
            break;

        case RIL_REQUEST_EXIT_EMERGENCY_CALLBACK_MODE:
            requestExitEmergencyMode(data, datalen, t);
            break;

        default:
            ALOGD("Request not supported. Tech: %d", TECH(sMdmInfo));
            RIL_onRequestComplete(t, RIL_E_REQUEST_NOT_SUPPORTED, NULL, 0);
            break;
    }
}

/**
 * GSM specific request
 */
static void
onGsmSpecificRequest (int request, void *data, size_t datalen, RIL_Token t)
{
    switch (request) {
        case RIL_REQUEST_SEND_SMS:
            requestSendSMS(data, datalen, t);
            break;

        case RIL_REQUEST_SET_NETWORK_SELECTION_AUTOMATIC: {
            int err = at_send_command("AT+COPS=0", NULL);
            if (err < 0) {
              RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            } else {
              RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            }
            break;
        }

        case RIL_REQUEST_SET_NETWORK_SELECTION_MANUAL:
            requestSetNetworkSelectionManual(data, datalen, t);
            break;

        case RIL_REQUEST_QUERY_NETWORK_SELECTION_MODE:
            requestQueryNetworkSelectionMode(data, datalen, t);
            break;

        case RIL_REQUEST_QUERY_AVAILABLE_NETWORKS: {
            char **operators;
            int i, err, entryCount;

            err = requestAvailableOperators(&operators, &entryCount);

            if (err < 0) {
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            } else {
                RIL_onRequestComplete(t, RIL_E_SUCCESS,
                    (void *) operators, entryCount * sizeof(char *));
            }

            for (i = 0; i < entryCount; i++) {
                free(operators[i]);
            }
            free(operators);

            break;
        }

        default:
            ALOGD("Request not supported. Tech: %d",TECH(sMdmInfo));
            RIL_onRequestComplete(t, RIL_E_REQUEST_NOT_SUPPORTED, NULL, 0);
            break;
    }
}

static void
onRequest (int request, void *data, size_t datalen, RIL_Token t)
{
    ATResponse *p_response;
    int err;

    ALOGD("onRequest: %s", requestToString(request));

    /* Ignore all requests except RIL_REQUEST_GET_SIM_STATUS
     * when RADIO_STATE_UNAVAILABLE.
     */
    if (sState == RADIO_STATE_UNAVAILABLE
        && request != RIL_REQUEST_GET_SIM_STATUS
    ) {
        RIL_onRequestComplete(t, RIL_E_RADIO_NOT_AVAILABLE, NULL, 0);
        return;
    }

    switch (request) {
        case RIL_REQUEST_GET_SIM_STATUS: {
            RIL_CardStatus_v6 *p_card_status;
            char *p_buffer;
            int buffer_size;

            int result = getCardStatus(&p_card_status);
            if (result == RIL_E_SUCCESS) {
                p_buffer = (char *)p_card_status;
                buffer_size = sizeof(*p_card_status);
            } else {
                p_buffer = NULL;
                buffer_size = 0;
            }
            RIL_onRequestComplete(t, result, p_buffer, buffer_size);
            freeCardStatus(p_card_status);
            break;
        }
        case RIL_REQUEST_GET_CURRENT_CALLS:
            requestGetCurrentCalls(data, datalen, t);
            break;
        case RIL_REQUEST_DIAL:
            requestDial(data, datalen, t);
            break;
        case RIL_REQUEST_HANGUP:
            requestHangup(data, datalen, t);
            break;
        case RIL_REQUEST_HANGUP_WAITING_OR_BACKGROUND:
            // 3GPP 22.030 6.5.5
            // "Releases all held calls or sets User Determined User Busy
            //  (UDUB) for a waiting call."
            at_send_command("AT+CHLD=0", NULL);

            /* success or failure is ignored by the upper layer here.
               it will call GET_CURRENT_CALLS and determine success that way */
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            break;
        case RIL_REQUEST_HANGUP_FOREGROUND_RESUME_BACKGROUND:
            // 3GPP 22.030 6.5.5
            // "Releases all active calls (if any exist) and accepts
            //  the other (held or waiting) call."
            at_send_command("AT+CHLD=1", NULL);

            /* success or failure is ignored by the upper layer here.
               it will call GET_CURRENT_CALLS and determine success that way */
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            break;
        case RIL_REQUEST_SWITCH_WAITING_OR_HOLDING_AND_ACTIVE:
            // 3GPP 22.030 6.5.5
            // "Places all active calls (if any exist) on hold and accepts
            //  the other (held or waiting) call."
            p_response = NULL;
            err = at_send_command("AT+CHLD=2", &p_response);

#ifdef WORKAROUND_ERRONEOUS_ANSWER
            s_expectAnswer = 1;
#endif /* WORKAROUND_ERRONEOUS_ANSWER */

            if (err < 0 || p_response->success == 0) {
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            } else {
                RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            }
            at_response_free(p_response);
            break;
        case RIL_REQUEST_ANSWER:
            at_send_command("ATA", NULL);

#ifdef WORKAROUND_ERRONEOUS_ANSWER
            s_expectAnswer = 1;
#endif /* WORKAROUND_ERRONEOUS_ANSWER */

            /* success or failure is ignored by the upper layer here.
               it will call GET_CURRENT_CALLS and determine success that way */
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            break;
        case RIL_REQUEST_CONFERENCE:
            requestConference(t);
            break;

        case RIL_REQUEST_UDUB:
            /* user determined user busy */
            /* sometimes used: ATH */
            at_send_command("ATH", NULL);

            /* success or failure is ignored by the upper layer here.
               it will call GET_CURRENT_CALLS and determine success that way */
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            break;

        case RIL_REQUEST_LAST_CALL_FAIL_CAUSE:
            requestLastCallFailCause(t);
            break;

        case RIL_REQUEST_SEPARATE_CONNECTION:
            requestSeparateConnection(data, datalen, t);
            break;

        case RIL_REQUEST_SIGNAL_STRENGTH:
            requestSignalStrength(data, datalen, t);
            break;
        case RIL_REQUEST_VOICE_REGISTRATION_STATE:
        case RIL_REQUEST_DATA_REGISTRATION_STATE:
            requestRegistrationState(request, data, datalen, t);
            break;
        case RIL_REQUEST_OPERATOR:
            requestOperator(data, datalen, t);
            break;
        case RIL_REQUEST_RADIO_POWER:
            requestRadioPower(data, datalen, t);
            break;
        case RIL_REQUEST_DTMF: {
            char c = ((char *)data)[0];
            char *cmd;
            asprintf(&cmd, "AT+VTS=%c", (int)c);
            at_send_command(cmd, NULL);
            free(cmd);
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            break;
        }
        case RIL_REQUEST_SETUP_DATA_CALL:
            requestSetupDataCall(data, datalen, t);
            break;
        case RIL_REQUEST_DEACTIVATE_DATA_CALL:
            requestDeactivateDataCall(data, datalen, t);
            break;
        case RIL_REQUEST_SMS_ACKNOWLEDGE:
            requestSMSAcknowledge(data, datalen, t);
            break;

        case RIL_REQUEST_GET_IMSI:
            p_response = NULL;
            err = at_send_command_numeric("AT+CIMI", &p_response);

            if (err < 0 || p_response->success == 0) {
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            } else {
                RIL_onRequestComplete(t, RIL_E_SUCCESS,
                    p_response->p_intermediates->line, sizeof(char *));
            }
            at_response_free(p_response);
            break;

        case RIL_REQUEST_GET_IMEI:
            p_response = NULL;
            err = at_send_command_numeric("AT+CGSN", &p_response);

            if (err < 0 || p_response->success == 0) {
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            } else {
                RIL_onRequestComplete(t, RIL_E_SUCCESS,
                    p_response->p_intermediates->line, sizeof(char *));
            }
            at_response_free(p_response);
            break;

        case RIL_REQUEST_SIM_IO:
            requestSIM_IO(data,datalen,t);
            break;

        case RIL_REQUEST_SEND_USSD:
            requestSendUSSD(data, datalen, t);
            break;

        case RIL_REQUEST_CANCEL_USSD:
            p_response = NULL;
            err = at_send_command_numeric("AT+CUSD=2", &p_response);

            if (err < 0 || p_response->success == 0) {
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            } else {
                RIL_onRequestComplete(t, RIL_E_SUCCESS,
                    p_response->p_intermediates->line, sizeof(char *));
            }
            at_response_free(p_response);
            break;

        case RIL_REQUEST_DATA_CALL_LIST:
            requestDataCallList(data, datalen, t);
            break;

        case RIL_REQUEST_OEM_HOOK_RAW:
            // echo back data
            RIL_onRequestComplete(t, RIL_E_SUCCESS, data, datalen);
            break;


        case RIL_REQUEST_OEM_HOOK_STRINGS: {
            int i;
            const char ** cur;

            ALOGD("got OEM_HOOK_STRINGS: 0x%8p %lu", data, (long)datalen);


            for (i = (datalen / sizeof (char *)), cur = (const char **)data ;
                    i > 0 ; cur++, i --) {
                ALOGD("> '%s'", *cur);
            }

            // echo back strings
            RIL_onRequestComplete(t, RIL_E_SUCCESS, data, datalen);
            break;
        }

        case RIL_REQUEST_WRITE_SMS_TO_SIM:
            requestWriteSmsToSim(data, datalen, t);
            break;

        case RIL_REQUEST_DELETE_SMS_ON_SIM: {
            char * cmd;
            p_response = NULL;
            asprintf(&cmd, "AT+CMGD=%d", ((int *)data)[0]);
            err = at_send_command(cmd, &p_response);
            free(cmd);
            if (err < 0 || p_response->success == 0) {
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            } else {
                RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            }
            at_response_free(p_response);
            break;
        }

        case RIL_REQUEST_ENTER_SIM_PIN:
            requestEnterSimPin(data, datalen, t);
            pollSIMState(NULL);
            break;

        case RIL_REQUEST_ENTER_SIM_PUK:
            requestEnterSimPuk(data, datalen, t);
            pollSIMState(NULL);
            break;

        case RIL_REQUEST_CHANGE_SIM_PIN:
            requestChangeSimPin(data, datalen, t);
            break;

        case RIL_REQUEST_ENTER_SIM_PIN2:
        case RIL_REQUEST_ENTER_SIM_PUK2:
        case RIL_REQUEST_CHANGE_SIM_PIN2:
            // We don't support pin2 and puk2 in qemu currently.
            break;

        case RIL_REQUEST_GET_UNLOCK_RETRY_COUNT:
            requestGetUnlockRetryCount(data, datalen, t);
            break;

        case RIL_REQUEST_SCREEN_STATE:
            requestScreenState(data, datalen, t);
            break;

        case RIL_REQUEST_VOICE_RADIO_TECH:
            {
                int tech = techFromModemType(TECH(sMdmInfo));
                if (tech < 0 )
                    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
                else
                    RIL_onRequestComplete(t, RIL_E_SUCCESS, &tech, sizeof(tech));
            }
            break;
        case RIL_REQUEST_SET_PREFERRED_NETWORK_TYPE:
            requestSetPreferredNetworkType(request, data, datalen, t);
            break;

        case RIL_REQUEST_GET_PREFERRED_NETWORK_TYPE:
            requestGetPreferredNetworkType(request, data, datalen, t);
            break;

        case RIL_REQUEST_GET_SMSC_ADDRESS:
            requestGetSmscAddress(request, data, datalen, t);
            break;

        case RIL_REQUEST_SET_SMSC_ADDRESS:
            requestSetSmscAddress(request, data, datalen, t);
            break;

        case RIL_REQUEST_QUERY_CALL_FORWARD_STATUS:
            requestQueryCallForwardStatus(data, datalen, t);
            break;

        case RIL_REQUEST_SET_CALL_FORWARD:
            requestSetCallForward(data, datalen, t);
            break;

        case RIL_REQUEST_QUERY_FACILITY_LOCK:
            requestQueryFacilityLock(data, datalen, t);
            break;

        case RIL_REQUEST_SET_FACILITY_LOCK:
            requestSetFacilityLock(data, datalen, t);
            break;

        case RIL_REQUEST_CHANGE_BARRING_PASSWORD:
            requestChangeBarringPassword(data, datalen, t);
            break;

        case RIL_REQUEST_STK_SEND_TERMINAL_RESPONSE:
            requestStkSendTerminalResponse(data, datalen, t);
            break;

        case RIL_REQUEST_STK_SEND_ENVELOPE_COMMAND:
            requestStkSendEnvelopeCommand(data, datalen, t);
            break;

        default:
            if (TECH_BIT(sMdmInfo) & (MDM_CDMA | MDM_EVDO)) {
                onCdmaSpecificRequest(request, data, datalen, t);
            } else {
                onGsmSpecificRequest(request, data, datalen, t);
            }
            break;
    }
}

/**
 * Synchronous call from the RIL to us to return current radio state.
 * RADIO_STATE_UNAVAILABLE should be the initial state.
 */
static RIL_RadioState
currentState()
{
    return sState;
}
/**
 * Call from RIL to us to find out whether a specific request code
 * is supported by this implementation.
 *
 * Return 1 for "supported" and 0 for "unsupported"
 */

static int
onSupports (int requestCode)
{
    //@@@ todo

    return 1;
}

static void onCancel (RIL_Token t)
{
    //@@@todo

}

static const char * getVersion(void)
{
    return "android reference-ril 1.0";
}

static void
setRadioTechnology(ModemInfo *mdm, int newtech)
{
    ALOGD("setRadioTechnology(%d)", newtech);

    int oldtech = TECH(mdm);

    if (newtech != oldtech) {
        ALOGD("Tech change (%d => %d)", oldtech, newtech);
        TECH(mdm) = newtech;
        if (techFromModemType(newtech) != techFromModemType(oldtech)) {
            int tech = techFromModemType(TECH(sMdmInfo));
            if (tech > 0 ) {
                RIL_onUnsolicitedResponse(RIL_UNSOL_VOICE_RADIO_TECH_CHANGED,
                                          &tech, sizeof(tech));
            }
        }
    }
}

static void
setRadioState(RIL_RadioState newState)
{
    ALOGD("setRadioState(%d)", newState);
    RIL_RadioState oldState;

    pthread_mutex_lock(&s_state_mutex);

    oldState = sState;

    if (s_closed > 0) {
        // If we're closed, the only reasonable state is
        // RADIO_STATE_UNAVAILABLE
        // This is here because things on the main thread
        // may attempt to change the radio state after the closed
        // event happened in another thread
        newState = RADIO_STATE_UNAVAILABLE;
    }

    if (sState != newState || s_closed > 0) {
        sState = newState;

        pthread_cond_broadcast (&s_state_cond);
    }

    pthread_mutex_unlock(&s_state_mutex);


    /* do these outside of the mutex */
    if (sState != oldState) {
        RIL_onUnsolicitedResponse (RIL_UNSOL_RESPONSE_RADIO_STATE_CHANGED,
                                    NULL, 0);

        /* FIXME onSimReady() and onRadioPowerOn() cannot be called
         * from the AT reader thread
         * Currently, this doesn't happen, but if that changes then these
         * will need to be dispatched on the request thread
         */
        if (sState == RADIO_STATE_ON) {
            onRadioPowerOn();
        }
    }
}

/** Returns RUIM_NOT_READY on error */
static SIM_Status
getRUIMStatus()
{
    ATResponse *p_response = NULL;
    int err;
    int ret;
    char *cpinLine;
    char *cpinResult;

    if (sState == RADIO_STATE_OFF) {
        ret = SIM_ABSENT;
        goto done;
    }
    if (sState == RADIO_STATE_UNAVAILABLE) {
        ret = SIM_NOT_READY;
        goto done;
    }

    err = at_send_command_singleline("AT+CPIN?", "+CPIN:", &p_response);

    if (err != 0) {
        ret = SIM_NOT_READY;
        goto done;
    }

    switch (at_get_cme_error(p_response)) {
        case CME_SUCCESS:
            break;

        case CME_SIM_NOT_INSERTED:
            ret = SIM_ABSENT;
            goto done;

        default:
            ret = SIM_NOT_READY;
            goto done;
    }

    /* CPIN? has succeeded, now look at the result */

    cpinLine = p_response->p_intermediates->line;
    err = at_tok_start (&cpinLine);

    if (err < 0) {
        ret = SIM_NOT_READY;
        goto done;
    }

    err = at_tok_nextstr(&cpinLine, &cpinResult);

    if (err < 0) {
        ret = SIM_NOT_READY;
        goto done;
    }

    if (0 == strcmp (cpinResult, "SIM PIN")) {
        ret = SIM_PIN;
        goto done;
    } else if (0 == strcmp (cpinResult, "SIM PUK")) {
        ret = SIM_PUK;
        goto done;
    } else if (0 == strcmp (cpinResult, "PH-NET PIN")) {
        return SIM_NETWORK_PERSONALIZATION;
    } else if (0 != strcmp (cpinResult, "READY"))  {
        /* we're treating unsupported lock types as "sim absent" */
        ret = SIM_ABSENT;
        goto done;
    }

    at_response_free(p_response);
    p_response = NULL;
    cpinResult = NULL;

    ret = SIM_READY;

done:
    at_response_free(p_response);
    return ret;
}

/** Returns SIM_NOT_READY on error */
static SIM_Status
getSIMStatus()
{
    ATResponse *p_response = NULL;
    int err;
    int ret;
    char *cpinLine;
    char *cpinResult;

    ALOGD("getSIMStatus(). sState: %d",sState);
    if (sState == RADIO_STATE_OFF) {
        ret = SIM_ABSENT;
        goto done;
    }
    if (sState == RADIO_STATE_UNAVAILABLE) {
        ret = SIM_NOT_READY;
        goto done;
    }

    err = at_send_command_singleline("AT+CPIN?", "+CPIN:", &p_response);

    if (err != 0) {
        ret = SIM_NOT_READY;
        goto done;
    }

    switch (at_get_cme_error(p_response)) {
        case CME_SUCCESS:
            break;

        case CME_SIM_NOT_INSERTED:
            ret = SIM_ABSENT;
            goto done;

        default:
            ret = SIM_NOT_READY;
            goto done;
    }

    /* CPIN? has succeeded, now look at the result */

    cpinLine = p_response->p_intermediates->line;
    err = at_tok_start (&cpinLine);

    if (err < 0) {
        ret = SIM_NOT_READY;
        goto done;
    }

    err = at_tok_nextstr(&cpinLine, &cpinResult);

    if (err < 0) {
        ret = SIM_NOT_READY;
        goto done;
    }

    if (0 == strcmp (cpinResult, "SIM PIN")) {
        ret = SIM_PIN;
        goto done;
    } else if (0 == strcmp (cpinResult, "SIM PUK")) {
        ret = SIM_PUK;
        goto done;
    } else if (0 == strcmp (cpinResult, "PH-NET PIN")) {
        return SIM_NETWORK_PERSONALIZATION;
    } else if (0 != strcmp (cpinResult, "READY"))  {
        /* we're treating unsupported lock types as "sim absent" */
        ret = SIM_ABSENT;
        goto done;
    }

    at_response_free(p_response);
    p_response = NULL;
    cpinResult = NULL;

    ret = SIM_READY;

done:
    at_response_free(p_response);
    return ret;
}


/**
 * Get the current card status.
 *
 * This must be freed using freeCardStatus.
 * @return: On success returns RIL_E_SUCCESS
 */
static int getCardStatus(RIL_CardStatus_v6 **pp_card_status) {
    static RIL_AppStatus app_status_array[] = {
        // SIM_ABSENT = 0
        { RIL_APPTYPE_UNKNOWN, RIL_APPSTATE_UNKNOWN, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN },
        // SIM_NOT_READY = 1
        { RIL_APPTYPE_SIM, RIL_APPSTATE_DETECTED, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN },
        // SIM_READY = 2
        { RIL_APPTYPE_SIM, RIL_APPSTATE_READY, RIL_PERSOSUBSTATE_READY,
          NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN },
        // SIM_PIN = 3
        { RIL_APPTYPE_SIM, RIL_APPSTATE_PIN, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN },
        // SIM_PUK = 4
        { RIL_APPTYPE_SIM, RIL_APPSTATE_PUK, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_ENABLED_BLOCKED, RIL_PINSTATE_UNKNOWN },
        // SIM_NETWORK_PERSONALIZATION = 5
        { RIL_APPTYPE_SIM, RIL_APPSTATE_SUBSCRIPTION_PERSO, RIL_PERSOSUBSTATE_SIM_NETWORK,
          NULL, NULL, 0, RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN },
        // RUIM_ABSENT = 6
        { RIL_APPTYPE_UNKNOWN, RIL_APPSTATE_UNKNOWN, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN },
        // RUIM_NOT_READY = 7
        { RIL_APPTYPE_RUIM, RIL_APPSTATE_DETECTED, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN },
        // RUIM_READY = 8
        { RIL_APPTYPE_RUIM, RIL_APPSTATE_READY, RIL_PERSOSUBSTATE_READY,
          NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN },
        // RUIM_PIN = 9
        { RIL_APPTYPE_RUIM, RIL_APPSTATE_PIN, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN },
        // RUIM_PUK = 10
        { RIL_APPTYPE_RUIM, RIL_APPSTATE_PUK, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_ENABLED_BLOCKED, RIL_PINSTATE_UNKNOWN },
        // RUIM_NETWORK_PERSONALIZATION = 11
        { RIL_APPTYPE_RUIM, RIL_APPSTATE_SUBSCRIPTION_PERSO, RIL_PERSOSUBSTATE_SIM_NETWORK,
           NULL, NULL, 0, RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN }
    };
    RIL_CardState card_state;
    int num_apps;

    int sim_status = getSIMStatus();
    if (sim_status == SIM_ABSENT) {
        card_state = RIL_CARDSTATE_ABSENT;
        num_apps = 0;
    } else {
        card_state = RIL_CARDSTATE_PRESENT;
        num_apps = 2;
    }

    // Allocate and initialize base card status.
    RIL_CardStatus_v6 *p_card_status = malloc(sizeof(RIL_CardStatus_v6));
    p_card_status->card_state = card_state;
    p_card_status->universal_pin_state = RIL_PINSTATE_UNKNOWN;
    p_card_status->gsm_umts_subscription_app_index = RIL_CARD_MAX_APPS;
    p_card_status->cdma_subscription_app_index = RIL_CARD_MAX_APPS;
    p_card_status->ims_subscription_app_index = RIL_CARD_MAX_APPS;
    p_card_status->num_applications = num_apps;

    // Initialize application status
    int i;
    for (i = 0; i < RIL_CARD_MAX_APPS; i++) {
        p_card_status->applications[i] = app_status_array[SIM_ABSENT];
    }

    // Pickup the appropriate application status
    // that reflects sim_status for gsm.
    if (num_apps != 0) {
        // Only support one app, gsm
        p_card_status->num_applications = 2;
        p_card_status->gsm_umts_subscription_app_index = 0;
        p_card_status->cdma_subscription_app_index = 1;

        // Get the correct app status
        p_card_status->applications[0] = app_status_array[sim_status];
        p_card_status->applications[1] = app_status_array[sim_status + RUIM_ABSENT];
    }

    *pp_card_status = p_card_status;
    return RIL_E_SUCCESS;
}

/**
 * Free the card status returned by getCardStatus
 */
static void freeCardStatus(RIL_CardStatus_v6 *p_card_status) {
    free(p_card_status);
}

/**
 * SIM ready means any commands that access the SIM will work, including:
 *  AT+CPIN, AT+CSMS, AT+CNMI, AT+CRSM
 *  (all SMS-related commands)
 */

static void pollSIMState (void *param)
{
    ATResponse *p_response;
    int ret;

    if (sState != RADIO_STATE_ON) {
        // no longer valid to poll
        return;
    }

    switch(getSIMStatus()) {
        case SIM_ABSENT:
        case SIM_PIN:
        case SIM_PUK:
        case SIM_NETWORK_PERSONALIZATION:
        default:
            ALOGI("SIM ABSENT or LOCKED");
            RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_SIM_STATUS_CHANGED, NULL, 0);
        return;

        case SIM_NOT_READY:
            RIL_requestTimedCallback (pollSIMState, NULL, &TIMEVAL_SIMPOLL);
        return;

        case SIM_READY:
            ALOGI("SIM_READY");
            onSIMReady();
            RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_SIM_STATUS_CHANGED, NULL, 0);
        return;
    }
}

/** returns 1 if on, 0 if off, and -1 on error */
static int isRadioOn()
{
    ATResponse *p_response = NULL;
    int err;
    char *line;
    char ret;

    err = at_send_command_singleline("AT+CFUN?", "+CFUN:", &p_response);

    if (err < 0 || p_response->success == 0) {
        // assume radio is off
        goto error;
    }

    line = p_response->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0) goto error;

    err = at_tok_nextbool(&line, &ret);
    if (err < 0) goto error;

    at_response_free(p_response);

    return (int)ret;

error:

    at_response_free(p_response);
    return -1;
}

/**
 * Parse the response generated by a +CTEC AT command
 * The values read from the response are stored in current and preferred.
 * Both current and preferred may be null. The corresponding value is ignored in that case.
 *
 * @return: -1 if some error occurs (or if the modem doesn't understand the +CTEC command)
 *          1 if the response includes the current technology only
 *          0 if the response includes both current technology and preferred mode
 */
int parse_technology_response( const char *response, int *current, int32_t *preferred )
{
    int err;
    char *line, *p;
    int ct;
    int32_t pt = 0;
    char *str_pt;

    line = p = strdup(response);
    ALOGD("Response: %s", line);
    err = at_tok_start(&p);
    if (err || !at_tok_hasmore(&p)) {
        ALOGD("err: %d. p: %s", err, p);
        free(line);
        return -1;
    }

    err = at_tok_nextint(&p, &ct);
    if (err) {
        free(line);
        return -1;
    }
    if (current) *current = ct;

    ALOGD("line remaining after int: %s", p);

    err = at_tok_nexthexint(&p, &pt);
    if (err) {
        free(line);
        return 1;
    }
    if (preferred) {
        *preferred = pt;
    }
    free(line);

    return 0;
}

int query_supported_techs( ModemInfo *mdm, int *supported )
{
    ATResponse *p_response;
    int err, val, techs = 0;
    char *tok;
    char *line;

    ALOGD("query_supported_techs");
    err = at_send_command_singleline("AT+CTEC=?", "+CTEC:", &p_response);
    if (err || !p_response->success)
        goto error;
    line = p_response->p_intermediates->line;
    err = at_tok_start(&line);
    if (err || !at_tok_hasmore(&line))
        goto error;
    while (!at_tok_nextint(&line, &val)) {
        techs |= ( 1 << val );
    }
    if (supported) *supported = techs;
    return 0;
error:
    at_response_free(p_response);
    return -1;
}

/**
 * query_ctec. Send the +CTEC AT command to the modem to query the current
 * and preferred modes. It leaves values in the addresses pointed to by
 * current and preferred. If any of those pointers are NULL, the corresponding value
 * is ignored, but the return value will still reflect if retreiving and parsing of the
 * values suceeded.
 *
 * @mdm Currently unused
 * @current A pointer to store the current mode returned by the modem. May be null.
 * @preferred A pointer to store the preferred mode returned by the modem. May be null.
 * @return -1 on error (or failure to parse)
 *         1 if only the current mode was returned by modem (or failed to parse preferred)
 *         0 if both current and preferred were returned correctly
 */
int query_ctec(ModemInfo *mdm, int *current, int32_t *preferred)
{
    ATResponse *response = NULL;
    int err;
    int res;

    ALOGD("query_ctec. current: %d, preferred: %d", (int)current, (int) preferred);
    err = at_send_command_singleline("AT+CTEC?", "+CTEC:", &response);
    if (!err && response->success) {
        res = parse_technology_response(response->p_intermediates->line, current, preferred);
        at_response_free(response);
        return res;
    }
    ALOGE("Error executing command: %d. response: %x. status: %d", err, (int)response, response? response->success : -1);
    at_response_free(response);
    return -1;
}

int is_multimode_modem(ModemInfo *mdm)
{
    ATResponse *response;
    int err;
    char *line;
    int tech;
    int32_t preferred;

    if (query_ctec(mdm, &tech, &preferred) == 0) {
        mdm->currentTech = tech;
        mdm->preferredNetworkMode = preferred;
        if (query_supported_techs(mdm, &mdm->supportedTechs)) {
            return 0;
        }
        return 1;
    }
    return 0;
}

/**
 * Find out if our modem is GSM, CDMA or both (Multimode)
 */
static void probeForModemMode(ModemInfo *info)
{
    ATResponse *response;
    int err;
    assert (info);
    // Currently, our only known multimode modem is qemu's android modem,
    // which implements the AT+CTEC command to query and set mode.
    // Try that first

    if (is_multimode_modem(info)) {
        ALOGI("Found Multimode Modem. Supported techs mask: %8.8x. Current tech: %d",
            info->supportedTechs, info->currentTech);
        info->isMultimode = 1;
        return;
    }

    /* Being here means that our modem is not multimode */
    info->isMultimode = 0;

    /* CDMA Modems implement the AT+WNAM command */
    err = at_send_command_singleline("AT+WNAM","+WNAM:", &response);
    if (!err && response->success) {
        at_response_free(response);
        // TODO: find out if we really support EvDo
        info->supportedTechs = MDM_CDMA | MDM_EVDO;
        info->currentTech = MDM_CDMA;
        ALOGI("Found CDMA Modem");
        return;
    }
    if (!err) at_response_free(response);
    // TODO: find out if modem really supports WCDMA/LTE
    info->supportedTechs = MDM_GSM | MDM_WCDMA | MDM_LTE;
    info->currentTech = MDM_GSM;
    ALOGI("Found GSM Modem");
}

static void queryNumOfDataContexts()
{
    ATResponse *p_response;
    ATLine *p_cur;
    int err;

    // +CGDCONT=? is used to query the ranges of supported PDP Contexts.
    err = at_send_command_multiline("AT+CGDCONT=?", "+CGDCONT:", &p_response);
    if (err < 0 || p_response->success == 0) {
        goto error;
    }

    for (p_cur = p_response->p_intermediates; p_cur != NULL;
         p_cur = p_cur->p_next) {
        char *line = p_cur->line;
        char *range;
        int start, end;

        err = at_tok_start(&line);
        if (err < 0)
            goto error;

        err = at_tok_nextstr(&line, &range);
        if (err < 0)
            goto error;

        sscanf(range, "(%d-%d)", &start, &end);
        // Assign to s_maxDataContexts the maximum range found.
        if (end > s_maxDataContexts) {
            s_maxDataContexts = end;
        }
    }
    ALOGI("Number of data contexts: %d", s_maxDataContexts);

    at_response_free(p_response);
    return;

error:
    ALOGE("Error getting number of data contexts.");
    at_response_free(p_response);
}

/**
 * Initialize everything that can be configured while we're still in
 * AT+CFUN=0
 */
static void initializeCallback(void *param)
{
    ATResponse *p_response = NULL;
    int err;

    setRadioState (RADIO_STATE_OFF);

    at_handshake();

    probeForModemMode(sMdmInfo);

    queryNumOfDataContexts();

    /* note: we don't check errors here. Everything important will
       be handled in onATTimeout and onATReaderClosed */

    /*  atchannel is tolerant of echo but it must */
    /*  have verbose result codes */
    at_send_command("ATE0Q0V1", NULL);

    /*  No auto-answer */
    at_send_command("ATS0=0", NULL);

    /*  Extended errors */
    at_send_command("AT+CMEE=1", NULL);

    /*  Network registration events */
    err = at_send_command("AT+CREG=2", &p_response);

    /* some handsets -- in tethered mode -- don't support CREG=2 */
    if (err < 0 || p_response->success == 0) {
        at_send_command("AT+CREG=1", NULL);
    }

    at_response_free(p_response);

    /*  GPRS registration events */
    at_send_command("AT+CGREG=1", NULL);

    /*  Call Waiting notifications */
    at_send_command("AT+CCWA=1", NULL);

    /*  Alternating voice/data off */
    at_send_command("AT+CMOD=0", NULL);

    /*  Not muted */
    at_send_command("AT+CMUT=0", NULL);

    /*  +CSSU unsolicited supp service notifications */
    at_send_command("AT+CSSN=0,1", NULL);

    /*  no connected line identification */
    at_send_command("AT+COLP=0", NULL);

    /*  HEX character set */
    at_send_command("AT+CSCS=\"HEX\"", NULL);

    /*  USSD unsolicited */
    at_send_command("AT+CUSD=1", NULL);

    /*  Enable +CGEV GPRS event notifications, but don't buffer */
    at_send_command("AT+CGEREP=1,0", NULL);

    /*  SMS PDU mode */
    at_send_command("AT+CMGF=0", NULL);

#ifdef USE_TI_COMMANDS

    at_send_command("AT%CPI=3", NULL);

    /*  TI specific -- notifications when SMS is ready (currently ignored) */
    at_send_command("AT%CSTAT=1", NULL);

#endif /* USE_TI_COMMANDS */


    /* assume radio is off on error */
    if (isRadioOn() > 0) {
        setRadioState (RADIO_STATE_ON);
    }
}

static void waitForClose()
{
    pthread_mutex_lock(&s_state_mutex);

    while (s_closed == 0) {
        pthread_cond_wait(&s_state_cond, &s_state_mutex);
    }

    pthread_mutex_unlock(&s_state_mutex);
}

/**
 * Called by atchannel when an unsolicited line appears
 * This is called on atchannel's reader thread. AT commands may
 * not be issued here
 */
static void onUnsolicited (const char *s, const char *sms_pdu)
{
    char *line = NULL, *p;
    int err;

    /* Ignore unsolicited responses until we're initialized.
     * This is OK because the RIL library will poll for initial state
     */
    if (sState == RADIO_STATE_UNAVAILABLE) {
        return;
    }

    if (strStartsWith(s, "%CTZV:")) {
        /* TI specific -- NITZ time */
        char *response;

        line = p = strdup(s);
        at_tok_start(&p);

        err = at_tok_nextstr(&p, &response);

        free(line);
        if (err != 0) {
            ALOGE("invalid NITZ line %s\n", s);
        } else {
            RIL_onUnsolicitedResponse (
                RIL_UNSOL_NITZ_TIME_RECEIVED,
                response, strlen(response));
        }
    } else if (strStartsWith(s,"+CRING:")
                || strStartsWith(s,"RING")
    ) {
        RIL_onUnsolicitedResponse (
            RIL_UNSOL_CALL_RING,
            NULL, 0);
        RIL_onUnsolicitedResponse (
            RIL_UNSOL_RESPONSE_CALL_STATE_CHANGED,
            NULL, 0);
    } else if (strStartsWith(s,"NO CARRIER")
                || strStartsWith(s,"+CCWA")
                || strStartsWith(s, "CALL STATE CHANGED")
    ) {
        RIL_onUnsolicitedResponse (
            RIL_UNSOL_RESPONSE_CALL_STATE_CHANGED,
            NULL, 0);
#ifdef WORKAROUND_FAKE_CGEV
        RIL_requestTimedCallback (onDataCallListChanged, NULL, NULL); //TODO use new function
#endif /* WORKAROUND_FAKE_CGEV */
    } else if(strStartsWith(s,"+CUSATP:")) {
        char *pStkPdu = 0;
        line = strdup(s);
        err = at_tok_start(&line);
        if (err < 0) {
            ALOGE("Error  %d \t %s\n ", err, line);
        }
        err = at_tok_nextstr(&line, &pStkPdu);
        if (err < 0) {
            ALOGE("Error:  %d \t %s\n ", err, line);
        }
        ALOGI("STK Command PDU : %s \n", pStkPdu);
        if(NULL != pStkPdu) {
            RIL_onUnsolicitedResponse (RIL_UNSOL_STK_PROACTIVE_COMMAND,
                                       pStkPdu, strlen(pStkPdu));
        }
        free(line);
    } else if (strStartsWith(s,"+CREG:")
                || strStartsWith(s,"+CGREG:")
    ) {
        RIL_onUnsolicitedResponse (
            RIL_UNSOL_RESPONSE_VOICE_NETWORK_STATE_CHANGED,
            NULL, 0);
#ifdef WORKAROUND_FAKE_CGEV
        RIL_requestTimedCallback (onDataCallListChanged, NULL, NULL);
#endif /* WORKAROUND_FAKE_CGEV */
    } else if (strStartsWith(s, "+CMT:")) {
        RIL_onUnsolicitedResponse (
            RIL_UNSOL_RESPONSE_NEW_SMS,
            sms_pdu, strlen(sms_pdu));
    } else if (strStartsWith(s, "+CDS:")) {
        RIL_onUnsolicitedResponse (
            RIL_UNSOL_RESPONSE_NEW_SMS_STATUS_REPORT,
            sms_pdu, strlen(sms_pdu));
    } else if (strStartsWith(s, "+CBM:")) {
        const int str_len = strlen(sms_pdu);
        const int pdu_len = str_len / 2;
        unsigned char *pdu, *p;
        char c;
        int i = 0;
        p = pdu = (unsigned char *) malloc(sizeof(unsigned char) * pdu_len);
        while (i < pdu_len * 2) {
            c = sms_pdu[i++]; // High byte
            *p = (((c >= 'a') ? (c - 'a' + 10) : (c - '0')) << 4) & 0xF0;
            c = sms_pdu[i++]; // Low byte
            *p |= ((c >= 'a') ? (c - 'a' + 10) : (c - '0')) & 0x0F;

            ++p;
        }
        RIL_onUnsolicitedResponse (
            RIL_UNSOL_RESPONSE_NEW_BROADCAST_SMS,
            pdu, pdu_len);
       free((char*)pdu);
    } else if (strStartsWith(s, "+CGEV:")) {
        /* Really, we can ignore NW CLASS and ME CLASS events here,
         * but right now we don't since extranous
         * RIL_UNSOL_DATA_CALL_LIST_CHANGED calls are tolerated
         */
        /* can't issue AT commands here -- call on main thread */
        RIL_requestTimedCallback (onDataCallListChanged, NULL, NULL);
#ifdef WORKAROUND_FAKE_CGEV
    } else if (strStartsWith(s, "+CME ERROR: 150")) {
        RIL_requestTimedCallback (onDataCallListChanged, NULL, NULL);
#endif /* WORKAROUND_FAKE_CGEV */
    } else if (strStartsWith(s, "+CTEC: ")) {
        int tech, mask;
        switch (parse_technology_response(s, &tech, NULL))
        {
            case -1: // no argument could be parsed.
                ALOGE("invalid CTEC line %s\n", s);
                break;
            case 1: // current mode correctly parsed
            case 0: // preferred mode correctly parsed
                mask = 1 << tech;
                if (mask != MDM_GSM && mask != MDM_CDMA && mask != MDM_EVDO &&
                     mask != MDM_WCDMA && mask != MDM_LTE) {
                    ALOGE("Unknown technology %d\n", tech);
                } else {
                    setRadioTechnology(sMdmInfo, tech);
                }
                break;
        }
    } else if (strStartsWith(s, "+CCSS: ")) {
        int source = 0;
        line = p = strdup(s);
        if (!line) {
            ALOGE("+CCSS: Unable to allocate memory");
            return;
        }
        if (at_tok_start(&p) < 0) {
            free(line);
            return;
        }
        if (at_tok_nextint(&p, &source) < 0) {
            ALOGE("invalid +CCSS response: %s", line);
            free(line);
            return;
        }
        SSOURCE(sMdmInfo) = source;
        RIL_onUnsolicitedResponse(RIL_UNSOL_CDMA_SUBSCRIPTION_SOURCE_CHANGED,
                                  &source, sizeof(source));
    } else if (strStartsWith(s, "+WSOS: ")) {
        char state = 0;
        int unsol;
        line = p = strdup(s);
        if (!line) {
            ALOGE("+WSOS: Unable to allocate memory");
            return;
        }
        if (at_tok_start(&p) < 0) {
            free(line);
            return;
        }
        if (at_tok_nextbool(&p, &state) < 0) {
            ALOGE("invalid +WSOS response: %s", line);
            free(line);
            return;
        }
        free(line);

        unsol = state ?
                RIL_UNSOL_ENTER_EMERGENCY_CALLBACK_MODE : RIL_UNSOL_EXIT_EMERGENCY_CALLBACK_MODE;

        RIL_onUnsolicitedResponse(unsol, NULL, 0);

    } else if (strStartsWith(s, "+WPRL: ")) {
        int version = -1;
        line = p = strdup(s);
        if (!line) {
            ALOGE("+WPRL: Unable to allocate memory");
            return;
        }
        if (at_tok_start(&p) < 0) {
            ALOGE("invalid +WPRL response: %s", s);
            free(line);
            return;
        }
        if (at_tok_nextint(&p, &version) < 0) {
            ALOGE("invalid +WPRL response: %s", s);
            free(line);
            return;
        }
        free(line);
        RIL_onUnsolicitedResponse(RIL_UNSOL_CDMA_PRL_CHANGED, &version, sizeof(version));
    } else if (strStartsWith(s, "+CFUN:")) {
        int state = -1;
        line = p = strdup(s);
        if (!line) {
            ALOGE("+CFUN: Unable to allocate memory");
            return;
        }
        if (at_tok_start(&p) < 0) {
            ALOGE("invalid +CFUN response: %s", s);
            free(line);
            return;
        }
        if (at_tok_nextint(&p, &state) < 0) {
            ALOGE("invalid +CFUN response: %s", s);
            free(line);
            return;
        }
        free(line);
        switch (state) {
            case 0:
                setRadioState(RADIO_STATE_OFF);
                break;
            case 1:
                setRadioState(RADIO_STATE_ON);
                break;
            default:
                ALOGE("invalid +CFUN response: %s", s);
                return;
        }
    } else if (strStartsWith(s, "+CSQ:")) {
        RIL_SignalStrength_v6 response;
        line = p = strdup(s);
        if (!line) {
            ALOGE("+CSQ: Unable to allocate memory");
            return;
        }
        if (at_tok_start(&p) < 0) {
            ALOGE("invalid +CSQ response: %s", s);
            free(line);
            return;
        }
        if (handleSignalStrength(p, &response) < 0) {
            free(line);
            return;
        }
        free(line);
        RIL_onUnsolicitedResponse(RIL_UNSOL_SIGNAL_STRENGTH, &response, sizeof(response));
    } else if (strStartsWith(s, "+CNAP:")) {
        char* name = NULL;
        int namePresentation = 0;
        int len = 0;
        line = p = strdup(s);
        if (!line) {
            ALOGE("+CNAP: Unable to allocate memory");
            return;
        }
        if (at_tok_start(&p) < 0) {
            ALOGE("invalid +CNAP response: %s", s);
            free(line);
            return;
        }
        if (at_tok_nextstr(&p, &name) < 0) {
            ALOGE("invalid +CNAP response: %s", s);
            free(line);
            return;
        }
        if (at_tok_nextint(&p, &namePresentation) < 0) {
            ALOGE("invalid +CNAP response: %s", s);
            free(line);
            return;
        }

        if (sCnapInfo.CNI_validity == 0) {
          len  = strlen(name);
          if (len >= A_CALL_NAME_MAX_SIZE) {
            len = A_CALL_NAME_MAX_SIZE-1;
          }
          strncpy(sCnapInfo.name, name, len);
        }
        sCnapInfo.name[len] = '\0';
        sCnapInfo.CNI_validity = namePresentation;

        free(line);
    }
}

/* Called on command or reader thread */
static void onATReaderClosed()
{
    ALOGI("AT channel closed\n");
    at_close();
    s_closed = 1;

    setRadioState (RADIO_STATE_UNAVAILABLE);
}

/* Called on command thread */
static void onATTimeout()
{
    ALOGI("AT channel timeout; closing\n");
    at_close();

    s_closed = 1;

    /* FIXME cause a radio reset here */

    setRadioState (RADIO_STATE_UNAVAILABLE);
}

static void usage(char *s)
{
#ifdef RIL_SHLIB
    fprintf(stderr, "reference-ril requires: -p <tcp port> or -d /dev/tty_device\n");
#else
    fprintf(stderr, "usage: %s [-p <tcp port>] [-d /dev/tty_device]\n", s);
    exit(-1);
#endif
}

static void *
mainLoop(void *param)
{
    int fd;
    int ret;

    AT_DUMP("== ", "entering mainLoop()", -1 );
    at_set_on_reader_closed(onATReaderClosed);
    at_set_on_timeout(onATTimeout);

    for (;;) {
        fd = -1;
        while  (fd < 0) {
            if (s_port > 0) {
                fd = socket_loopback_client(s_port, SOCK_STREAM);
            } else if (s_device_socket) {
                if (!s_client_id) {
                    s_client_id = "";
                }

                if (!strcmp(s_device_path, "/dev/socket/qemud")) {
                    /* Before trying to connect to /dev/socket/qemud (which is
                     * now another "legacy" way of communicating with the
                     * emulator), we will try to connecto to gsm service via
                     * qemu pipe. */
                    char buffer[32];
                    snprintf(buffer, sizeof(buffer), "qemud:gsm%s", s_client_id);
                    fd = qemu_pipe_open(buffer);
                    if (fd < 0) {
                        /* Qemu-specific control socket */
                        fd = socket_local_client( "qemud",
                                                  ANDROID_SOCKET_NAMESPACE_RESERVED,
                                                  SOCK_STREAM );
                        if (fd >= 0 ) {
                            char  answer[2];
                            int len = snprintf(buffer, sizeof(buffer), "gsm%s", s_client_id);
                            if ( write(fd, buffer, len) != len ||
                                 read(fd, answer, 2) != 2 ||
                                 memcmp(answer, "OK", 2) != 0)
                            {
                                close(fd);
                                fd = -1;
                            }
                       }
                    }
                }
                else
                    fd = socket_local_client( s_device_path,
                                            ANDROID_SOCKET_NAMESPACE_FILESYSTEM,
                                            SOCK_STREAM );
            } else if (s_device_path != NULL) {
                fd = open (s_device_path, O_RDWR);
                if ( fd >= 0 && !memcmp( s_device_path, "/dev/ttyS", 9 ) ) {
                    /* disable echo on serial ports */
                    struct termios  ios;
                    tcgetattr( fd, &ios );
                    ios.c_lflag = 0;  /* disable ECHO, ICANON, etc... */
                    tcsetattr( fd, TCSANOW, &ios );
                }
            }

            if (fd < 0) {
                perror ("opening AT interface. retrying...");
                sleep(10);
                /* never returns */
            }
        }

        s_closed = 0;
        ret = at_open(fd, onUnsolicited);

        if (ret < 0) {
            ALOGE ("AT error %d on at_open\n", ret);
            return 0;
        }

        RIL_requestTimedCallback(initializeCallback, NULL, &TIMEVAL_0);

        // Give initializeCallback a chance to dispatched, since
        // we don't presently have a cancellation mechanism
        sleep(1);

        waitForClose();
        ALOGI("Re-opening after close");
    }
}

#ifdef RIL_SHLIB

pthread_t s_tid_mainloop;

const RIL_RadioFunctions *RIL_Init(const struct RIL_Env *env, int argc, char **argv)
{
    int ret;
    int fd = -1;
    int opt;
    pthread_attr_t attr;

    s_rilenv = env;

    while ( -1 != (opt = getopt(argc, argv, "p:d:s:c:"))) {
        switch (opt) {
            case 'p':
                s_port = atoi(optarg);
                if (s_port == 0) {
                    usage(argv[0]);
                    return NULL;
                }
                ALOGI("Opening loopback port %d\n", s_port);
            break;

            case 'd':
                s_device_path = optarg;
                ALOGI("Opening tty device %s\n", s_device_path);
            break;

            case 's':
                s_device_path   = optarg;
                s_device_socket = 1;
                ALOGI("Opening socket %s\n", s_device_path);
            break;

            case 'c':
                s_client_id = optarg;
                ALOGI("Client ID %s\n", s_client_id);
            break;

            default:
                usage(argv[0]);
                return NULL;
        }
    }

    if ((s_port < 0 && s_device_path == NULL)
        || (s_client_id && !s_device_socket)) {
        usage(argv[0]);
        return NULL;
    }

    sMdmInfo = calloc(1, sizeof(ModemInfo));
    if (!sMdmInfo) {
        ALOGE("Unable to alloc memory for ModemInfo");
        return NULL;
    }
    pthread_attr_init (&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    ret = pthread_create(&s_tid_mainloop, &attr, mainLoop, NULL);

    return &s_callbacks;
}
#else /* RIL_SHLIB */
int main (int argc, char **argv)
{
    int ret;
    int fd = -1;
    int opt;

    while ( -1 != (opt = getopt(argc, argv, "p:d:s:c:"))) {
        switch (opt) {
            case 'p':
                s_port = atoi(optarg);
                if (s_port == 0) {
                    usage(argv[0]);
                }
                ALOGI("Opening loopback port %d\n", s_port);
            break;

            case 'd':
                s_device_path = optarg;
                ALOGI("Opening tty device %s\n", s_device_path);
            break;

            case 's':
                s_device_path   = optarg;
                s_device_socket = 1;
                ALOGI("Opening socket %s\n", s_device_path);
            break;

            case 'c':
                s_client_id = optarg;
                ALOGI("Client ID %s\n", s_client_id);
            break;

            default:
                usage(argv[0]);
        }
    }

    if ((s_port < 0 && s_device_path == NULL)
        || (s_client_id && !s_device_socket)) {
        usage(argv[0]);
    }

    RIL_register(&s_callbacks);

    mainLoop(NULL);

    return 0;
}

#endif /* RIL_SHLIB */
