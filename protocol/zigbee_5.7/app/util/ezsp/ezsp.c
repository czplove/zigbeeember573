// File: ezsp.c
//
// Description: Host EZSP layer. Provides functions that allow the Host
// application to send every EZSP command to the EM260. The command and response
// parameters are defined in the datasheet.
//
// Copyright 2006-2010 by Ember Corporation. All rights reserved.           *80*
#include PLATFORM_HEADER


#include "stack/include/ember-types.h"
#include "stack/include/error.h"

#include "hal/hal.h"

#include "app/util/ezsp/ezsp-protocol.h"
#include "app/util/ezsp/ezsp.h"
#include "app/util/ezsp/serial-interface.h"
#include "app/util/ezsp/ezsp-frame-utilities.h"

#ifdef EZSP_ASH
  #include "app/ezsp-host/ezsp-host-priv.h"
  #define EZSP_ASH_TRACE(...) ezspTraceEzspVerbose(__VA_ARGS__)
#else
  #define EZSP_ASH_TRACE(...)
#endif

#if defined(EMBER_TEST)
  #define EMBER_TEST_ASSERT(x) assert(x)
#else
  #define EMBER_TEST_ASSERT(x)
#endif

//----------------------------------------------------------------
// Forward Declarations
static void startCommand(uint8_t command);
static EzspStatus sendCommand(void);
static void callbackDispatch(void);
static void callbackPointerInit(void);

//----------------------------------------------------------------
// Global Variables

#define MAX_SUPPORTED_NETWORKS 4

uint8_t emSupportedNetworks = MAX_SUPPORTED_NETWORKS;

uint8_t ezspSleepMode = EZSP_FRAME_CONTROL_IDLE;

static bool sendingCommand = false;
static uint8_t ezspSequence = 0;

// Multi-network support: this variable is equivalent to the
// emApplicationNetworkIndex vaiable for SOC. It stores the ezsp network index.
// It gets included in the frame control of every EZSP message to the NCP.
// The public APIs emberGetCurrentNetwork() and emberSetCurrentNetwork() set/get
// this value.
uint8_t ezspApplicationNetworkIndex = 0;

// Multi-network support: this variable is set when we receive a callback-related
// EZSP message from the NCP. The emberGetCallbackNetwork() API returns this
// value.
uint8_t ezspCallbackNetworkIndex = 0;

// Some callbacks from EZSP to the application include a pointer parameter. For
// example, messageContents in ezspIncomingMessageHandler(). Copying the
// callback and then giving the application a pointer to this copy means it is
// safe for the application to call EZSP functions inside the callback. To save
// RAM, the application can define EZSP_DISABLE_CALLBACK_COPY. The application
// must then not read from the pointer after calling an EZSP function inside the
// callback.
#ifndef EZSP_DISABLE_CALLBACK_COPY
static uint8_t ezspCallbackStorage[EZSP_MAX_FRAME_LENGTH];
#endif

bool ncpHasCallbacks;

//------------------------------------------------------------------------------
// Retrieving the new version info

EzspStatus ezspGetVersionStruct(EmberVersion* returnData)
{
  uint8_t data[7];  // sizeof(EmberVersion)
  uint8_t dataLength;
  EzspStatus status = ezspGetValue(EZSP_VALUE_VERSION_INFO,
                                   &dataLength,
                                   data);

  EMBER_TEST_ASSERT(dataLength == 7);

  if (status == EZSP_SUCCESS) {
    returnData->build   = data[0] + (((uint16_t)data[1]) << 8);
    returnData->major   = data[2];
    returnData->minor   = data[3];
    returnData->patch   = data[4];
    returnData->special = data[5];
    returnData->type    = data[6];
  }

  return status;
}

//------------------------------------------------------------------------------
// Functions for manipulating the endpoints flags on the NCP

EzspStatus ezspSetEndpointFlags(uint8_t endpoint,
                                EzspEndpointFlags flags)
{
  uint8_t data[3];
  data[0] = endpoint;
  data[1] = (uint8_t)flags;
  data[2] = (uint8_t)(flags >> 8);
  return ezspSetValue(EZSP_VALUE_ENDPOINT_FLAGS,
                      3,
                      data);
}

EzspStatus ezspGetEndpointFlags(uint8_t endpoint,
                                EzspEndpointFlags* returnFlags)
{
  uint8_t status;
  uint8_t value[2];
  uint8_t valueLength;

  status = ezspGetExtendedValue(EZSP_EXTENDED_VALUE_ENDPOINT_FLAGS,
                                endpoint,
                                &valueLength,
                                value);
  *returnFlags = HIGH_LOW_TO_INT(value[1], value[0]);
  return status;
}

//----------------------------------------------------------------
// Special Handling for AES functions.

// This is a copy of the function available on the SOC.  It would be a waste
// to have this be an actual EZSP call.
void emberAesMmoHashInit(EmberAesMmoHashContext *context)
{
  MEMSET(context, 0, sizeof(EmberAesMmoHashContext));
}

// Here we convert the normal Ember AES hash call to the specialized EZSP call.
// This came about because we cannot pass a block of data that is
// both input and output into EZSP.  The block must be broken up into two
// elements.  We unify the two pieces here to make it invisible to
// the users.

static EmberStatus aesMmoHash(EmberAesMmoHashContext *context,
                              bool finalize,
                              uint32_t length,
                              uint8_t *data)
{
  EmberAesMmoHashContext returnData;
  EmberStatus status;
  if (length > 255) {
    return EMBER_INVALID_CALL;
  }
  // In theory we could use 'context' structure as the 'returnData',
  // however that could be risky if the EZSP function tries to memset() the
  // 'returnData' prior to storing data in it.
  status = ezspAesMmoHash(context,
                          finalize,
                          (uint8_t)length,
                          data,
                          &returnData);
  MEMMOVE(context, &returnData, sizeof(EmberAesMmoHashContext));
  return status;
}

EmberStatus emberAesMmoHashUpdate(EmberAesMmoHashContext *context,
                                  uint32_t length,
                                  uint8_t *data)
{
  return aesMmoHash(context,
                    false,   // finalize?
                    length,
                    data);
}

EmberStatus emberAesMmoHashFinal(EmberAesMmoHashContext *context,
                                 uint32_t length,
                                 uint8_t *data)
{
  return aesMmoHash(context,
                    true,    // finalize?
                    length,
                    data);
}

// This is a convenience routine for hashing short blocks of data,
// less than 255 bytes.
EmberStatus emberAesHashSimple(uint8_t totalLength,
                               const uint8_t* data,
                               uint8_t* result)
{
  EmberStatus status;
  EmberAesMmoHashContext context;
  emberAesMmoHashInit(&context);
  status = emberAesMmoHashFinal(&context,
                                totalLength,
                                (uint8_t*)data);
  MEMMOVE(result, context.result, 16);
  return status;
}

//------------------------------------------------------------------------------
// SOC function names that are available on the host in a different
// form to save code space.

EmberStatus emberSetMfgSecurityConfig(uint32_t magicNumber,
                                      const EmberMfgSecurityStruct* settings)
{
  uint8_t data[4 + 2];  // 4 bytes for magic number, 2 bytes for key settings
  data[0] = (uint8_t)(magicNumber         & 0xFF);
  data[1] = (uint8_t)((magicNumber >> 8)  & 0xFF);
  data[2] = (uint8_t)((magicNumber >> 16) & 0xFF);
  data[3] = (uint8_t)((magicNumber >> 24) & 0xFF);
  data[4] = (uint8_t)(settings->keySettings        & 0xFF);
  data[5] = (uint8_t)((settings->keySettings >> 8) & 0xFF);
  return ezspSetValue(EZSP_VALUE_MFG_SECURITY_CONFIG, 6, data);
}

EmberStatus emberGetMfgSecurityConfig(EmberMfgSecurityStruct* settings)
{
  uint8_t data[2];
  uint8_t length = 2;
  EmberStatus status = ezspGetValue(EZSP_VALUE_MFG_SECURITY_CONFIG,
                                    &length,
                                    data);
  settings->keySettings = data[0] + (data[1] << 8);
  return status;
}

EmberStatus emberStartWritingStackTokens(void)
{
  uint8_t i = 1;
  return ezspSetValue(EZSP_VALUE_STACK_TOKEN_WRITING, 1, &i);
}

EmberStatus emberStopWritingStackTokens(void)
{
  uint8_t i = 0;
  return ezspSetValue(EZSP_VALUE_STACK_TOKEN_WRITING, 1, &i);
}

bool emberWritingStackTokensEnabled(void)
{
  uint8_t value;
  uint8_t valueLength = 1;
  ezspGetValue(EZSP_VALUE_STACK_TOKEN_WRITING, &valueLength, &value);
  return value;
}

bool emberStackIsPerformingRejoin(void)
{
  uint8_t value = 0;
  uint8_t valueLength;
  ezspGetValue(EZSP_VALUE_STACK_IS_PERFORMING_REJOIN,
               &valueLength,
               &value);
  return value;
}

EmberStatus emberSendRemoveDevice(EmberNodeId destShort,
                                  EmberEUI64 destLong,
                                  EmberEUI64 deviceToRemoveLong)
{
  return ezspRemoveDevice(destShort, destLong, deviceToRemoveLong);
}

EmberStatus emberSendUnicastNetworkKeyUpdate(EmberNodeId targetShort,
                                             EmberEUI64  targetLong,
                                             EmberKeyData* newKey)
{
  return ezspUnicastNwkKeyUpdate(targetShort,
                                 targetLong,
                                 newKey);
}

EmberStatus emberSetExtendedSecurityBitmask(EmberExtendedSecurityBitmask mask)
{
  uint8_t value[2];
  value[0] = LOW_BYTE(mask);
  value[1] = HIGH_BYTE(mask);
  if (ezspSetValue(EZSP_VALUE_EXTENDED_SECURITY_BITMASK, 2, value)
      == EZSP_SUCCESS) {
    return EMBER_SUCCESS;
  } else {
    return EMBER_INVALID_CALL;
  }
}

EmberStatus emberGetExtendedSecurityBitmask(EmberExtendedSecurityBitmask* mask)
{
  uint8_t value[2];
  uint8_t valueLength;
  if (ezspGetValue(EZSP_VALUE_EXTENDED_SECURITY_BITMASK, &valueLength, value)
      == EZSP_SUCCESS) {
    *mask = HIGH_LOW_TO_INT(value[1], value[0]);
    return EMBER_SUCCESS;
  } else {
    return EMBER_INVALID_CALL;
  }
}

EmberStatus emberSetNodeId(EmberNodeId nodeId)
{
  uint8_t value[2];
  value[0] = LOW_BYTE(nodeId);
  value[1] = HIGH_BYTE(nodeId);
  if (ezspSetValue(EZSP_VALUE_NODE_SHORT_ID, 2, value) == EZSP_SUCCESS) {
    return EMBER_SUCCESS;
  } else {
    return EMBER_INVALID_CALL;
  }
}

void emberSetMaximumIncomingTransferSize(uint16_t size)
{
  uint8_t value[2];
  value[0] = LOW_BYTE(size);
  value[1] = HIGH_BYTE(size);

  ezspSetValue(EZSP_VALUE_MAXIMUM_INCOMING_TRANSFER_SIZE, 2, value);
}

void emberSetMaximumOutgoingTransferSize(uint16_t size)
{
  uint8_t value[2];
  value[0] = LOW_BYTE(size);
  value[1] = HIGH_BYTE(size);

  ezspSetValue(EZSP_VALUE_MAXIMUM_OUTGOING_TRANSFER_SIZE, 2, value);
}

void emberSetDescriptorCapability(uint8_t capability)
{
  uint8_t value[1];
  value[0] = capability;

  ezspSetValue(EZSP_VALUE_DESCRIPTOR_CAPABILITY, 1, value);
}

uint8_t emberGetLastStackZigDevRequestSequence(void)
{
  uint8_t value = 0;
  uint8_t valueLength;
  ezspGetValue(EZSP_VALUE_STACK_DEVICE_REQUEST_SEQUENCE_NUMBER,
               &valueLength,
               &value);
  return value;
}

uint8_t emberGetCurrentNetwork(void)
{
  return ezspApplicationNetworkIndex;
}

uint8_t emberGetCallbackNetwork(void)
{
  return ezspCallbackNetworkIndex;
}

EmberStatus emberSetCurrentNetwork(uint8_t index)
{
  if (index < emSupportedNetworks) {
    ezspApplicationNetworkIndex = index;
    return EMBER_SUCCESS;
  } else {
    return EMBER_INVALID_CALL;
  }
}

EmberStatus emberFindAndRejoinNetworkWithReason(bool haveCurrentNetworkKey,
                                                uint32_t channelMask,
                                                EmberRejoinReason reason)
{
  // If there are legacy NCP devices without the rejoin reason support we want
  // to ignore a failure to this ezspSetValue() call.
  ezspSetValue(EZSP_VALUE_NEXT_HOST_REJOIN_REASON,
               1,
               &reason);
  return ezspFindAndRejoinNetwork(haveCurrentNetworkKey,
                                  channelMask);
}

EmberStatus emberFindAndRejoinNetwork(bool haveCurrentNetworkKey,
                                      uint32_t channelMask)
{
  return emberFindAndRejoinNetworkWithReason(haveCurrentNetworkKey,
                                             channelMask,
                                             EMBER_REJOIN_DUE_TO_APP_EVENT_1);
}

EmberStatus emberFindAndRejoinNetworkWithNodeType(bool haveCurrentNetworkKey,
                                                  uint32_t channelMask,
                                                  EmberNodeType nodeType)
{
  EmberStatus status = ezspSetValue(EZSP_VALUE_RETRY_DEVICE_TYPE,
                                    1,
                                    &nodeType);

  if(status != EMBER_SUCCESS) {
    return status;
  }

  return emberFindAndRejoinNetwork(haveCurrentNetworkKey,
                                   channelMask);

}

EmberRejoinReason emberGetLastRejoinReason(void)
{
  EmberRejoinReason reason = EMBER_REJOIN_REASON_NONE;
  uint8_t length;
  ezspGetValue(EZSP_VALUE_LAST_REJOIN_REASON,
               &length,
               &reason);
  return reason;
}

EmberLeaveReason emberGetLastLeaveReason(EmberNodeId* id)
{
  uint8_t length = 3;
  uint8_t data[3];
  ezspGetExtendedValue(EZSP_EXTENDED_VALUE_LAST_LEAVE_REASON,
                       0,  // characteristics
                       &length,
                       data);
  if (id != NULL) {
    *id = (((uint16_t)data[1])
           + ((uint16_t)(data[2] << 8)));
  }

  return data[0];
}
uint8_t ezspGetSourceRouteOverhead(EmberNodeId destination)
{
  uint8_t valueLength;
  uint8_t value;
  ezspGetExtendedValue(EZSP_EXTENDED_VALUE_GET_SOURCE_ROUTE_OVERHEAD,
                       destination, // characteristics
                       &valueLength,
                       &value);
  return value;
}

EmberStatus emberSetPreinstalledCbkeData283k1(EmberPublicKey283k1Data *caPublic,
                                              EmberCertificate283k1Data *myCert,
                                              EmberPrivateKey283k1Data *myKey)
{
  ezspSetValue(EZSP_VALUE_CERTIFICATE_283K1, EMBER_CERTIFICATE_283K1_SIZE, emberCertificate283k1Contents(myCert));
  ezspSetValue(EZSP_VALUE_PUBLIC_KEY_283K1, EMBER_PUBLIC_KEY_283K1_SIZE, emberPublicKey283k1Contents(caPublic));
  ezspSetValue(EZSP_VALUE_PRIVATE_KEY_283K1, EMBER_PRIVATE_KEY_283K1_SIZE, emberPrivateKey283k1Contents(myKey));
  return ezspSavePreinstalledCbkeData283k1();
}

//------------------------------------------------------------------------------

#include "command-functions.h"

//----------------------------------------------------------------
// EZSP Utilities
static void startCommand(uint8_t command)
{
  ezspWritePointer = ezspFrameContents + EZSP_PARAMETERS_INDEX;
  serialSetCommandByte(EZSP_FRAME_ID_INDEX, command);
}

enum {
  RESPONSE_SUCCESS,
  RESPONSE_WAITING,
  RESPONSE_ERROR
};

static uint8_t responseReceived(void)
{
  EzspStatus status;
  uint8_t responseFrameControl;

  status = serialResponseReceived();

  if (status == EZSP_SPI_WAITING_FOR_RESPONSE
      || status == EZSP_NO_RX_DATA) {
    return RESPONSE_WAITING;
  }

  ezspReadPointer = ezspFrameContents + EZSP_PARAMETERS_INDEX;
  if (status == EZSP_SUCCESS) {
    responseFrameControl = serialGetResponseByte(EZSP_FRAME_CONTROL_INDEX);
    if (serialGetResponseByte(EZSP_FRAME_ID_INDEX) == EZSP_INVALID_COMMAND){
      status = serialGetResponseByte(EZSP_PARAMETERS_INDEX);
    }
    if ((responseFrameControl & EZSP_FRAME_CONTROL_DIRECTION_MASK)
        != EZSP_FRAME_CONTROL_RESPONSE){
      status = EZSP_ERROR_WRONG_DIRECTION;
    }
    if ((responseFrameControl & EZSP_FRAME_CONTROL_TRUNCATED_MASK)
        == EZSP_FRAME_CONTROL_TRUNCATED){
      status = EZSP_ERROR_TRUNCATED;
    }
    if ((responseFrameControl & EZSP_FRAME_CONTROL_OVERFLOW_MASK)
        == EZSP_FRAME_CONTROL_OVERFLOW_MASK){
      status = EZSP_ERROR_OVERFLOW;
    }
    if ((responseFrameControl & EZSP_FRAME_CONTROL_PENDING_CB_MASK)
        == EZSP_FRAME_CONTROL_PENDING_CB) {
      ncpHasCallbacks = true;
    } else {
      ncpHasCallbacks = false;
    }

    // Set the callback network
    ezspCallbackNetworkIndex =
        (responseFrameControl & EZSP_FRAME_CONTROL_NETWORK_INDEX_MASK)
         >> EZSP_FRAME_CONTROL_NETWORK_INDEX_OFFSET;
  }
  if (status != EZSP_SUCCESS) {
    EZSP_ASH_TRACE("responseReceived(): ezspErrorHandler(): 0x%x", status);
    ezspErrorHandler(status);
    return status;
  } else {
    return status;
  }
}


static EzspStatus sendCommand(void)
{
  EzspStatus status;
  uint16_t length = ezspWritePointer - ezspFrameContents;
  serialSetCommandByte(EZSP_SEQUENCE_INDEX, ezspSequence);
  ezspSequence++;
  serialSetCommandByte(EZSP_FRAME_CONTROL_INDEX,
                       (EZSP_FRAME_CONTROL_COMMAND
                        | ezspSleepMode
                        | ezspApplicationNetworkIndex // we always set the network index in the
                          << EZSP_FRAME_CONTROL_NETWORK_INDEX_OFFSET)); // ezsp frame control.
  if (length > EZSP_MAX_FRAME_LENGTH) {
    ezspErrorHandler(EZSP_ERROR_COMMAND_TOO_LONG);
    return EZSP_ERROR_COMMAND_TOO_LONG;
  }
  serialSetCommandLength(length);
  // Ensure that a second command is not sent before the response to the first
  // command has been processed.
  assert(!sendingCommand);
  sendingCommand = true;
  status = serialSendCommand();
  if (status == EZSP_SUCCESS) {
    status = responseReceived();
    while (status == RESPONSE_WAITING) {
      status = responseReceived();
      ezspWaitingForResponse();
    }
  } else {
    EZSP_ASH_TRACE("sendCommand(): ezspErrorHandler(): 0x%x", status);
    ezspErrorHandler(status);
  }
  sendingCommand = false;
  return status;
}

static void callbackPointerInit(void)
{
#ifndef EZSP_DISABLE_CALLBACK_COPY
  MEMMOVE(ezspCallbackStorage, ezspFrameContents, EZSP_MAX_FRAME_LENGTH);
  ezspReadPointer = ezspCallbackStorage + EZSP_PARAMETERS_INDEX;
#endif
}

void ezspTick(void)
{
  uint8_t count = serialPendingResponseCount() + 1;
  // Ensure that we are not being called from within a command.
  assert(!sendingCommand);
  while (count > 0 && responseReceived() == RESPONSE_SUCCESS) {
    callbackDispatch();
    count--;
  }
  simulatedTimePasses();
}

// ZLL methods

#ifndef XAP2B
EmberStatus zllNetworkOps(EmberZllNetwork* networkInfo,
                          EzspZllNetworkOperation op,
                          uint8_t radioTxPower)
{
  return ezspZllNetworkOps(networkInfo, op, radioTxPower);
}

/** @brief This will set the device type as a router or end device
 * (depending on the passed nodeType) and setup a ZLL
 * commissioning network with the passed parameters.  If panId is 0xFFFF,
 * a random PAN ID will be generated.  If extendedPanId is set to all F's,
 * then a random extended pan ID will be generated.  If channel is 0xFF,
 * then channel 11 will be used.
 * If all F values are passed for PAN Id or Extended PAN ID then the
 * randomly generated values will be returned in the passed structure.
 *
 * @param networkInfo A pointer to an ::EmberZllNetwork struct indicating the
 *   network parameters to use when forming the network.  If random values are
 *   requested, the stack's randomly generated values will be returned in the
 *   structure.
 * @param radioTxPower the radio output power at which a node is to operate.
 *
 * @return An ::EmberStatus value indicating whether the operation
 *   succeeded, or why it failed.
 */
EmberStatus emberZllFormNetwork(EmberZllNetwork* networkInfo,
                                int8_t radioTxPower)
{
  return zllNetworkOps(networkInfo, EZSP_ZLL_FORM_NETWORK, radioTxPower);
}

/** @brief This call will cause the device to send a NWK start or join to the
 *  target device and cause the remote AND local device to start operating
 *  on a network together.  If the local device is a factory new device
 *  then it will send a ZLL NWK start to the target requesting that the
 *  target generate new network parameters.  If the device is
 *  not factory new then the local device will send a NWK join request
 *  using the current network parameters.
 *
 * @param targetNetworkInfo A pointer to an ::EmberZllNetwork structure that
 *   indicates the info about what device to send the NWK start/join
 *   request to.  This information must have previously been returned
 *   from a ZLL scan.
 *
 * @return An ::EmberStatus value indicating whether the operation
 *   succeeded, or why it failed.
 */
EmberStatus emberZllJoinTarget(const EmberZllNetwork* targetNetworkInfo)
{
  return zllNetworkOps((EmberZllNetwork *) targetNetworkInfo, EZSP_ZLL_JOIN_TARGET, 0);
}


/** @brief This call will cause the device to setup the security information
 *    used in its network.  It must be called prior to forming, starting, or
 *    joining a network.
 *
 * @param networkKey is a pointer to an ::EmberKeyData structure containing the
 *   value for the network key.  If the value is set to all F's, then a random
 *   network key will be generated.
 * @param securityState The security configuration to be set.
 *
 * @return An ::EmberStatus value indicating whether the operation
 *   succeeded, or why it failed.
 */
EmberStatus emberZllSetInitialSecurityState(const EmberKeyData *networkKey,
                                            const EmberZllInitialSecurityState *securityState)
{
  return ezspZllSetInitialSecurityState((EmberKeyData *) networkKey,
                                        (EmberZllInitialSecurityState *) securityState);
}


/**
 * @brief This call will initiate a ZLL network scan on all the specified
 *   channels.  Results will be returned in ::emberZllNetworkFoundHandler().
 *
 * @param channelMask indicating the range of channels to scan.
 * @param radioPowerForScan the radio output power used for the scan requests.
 * @param nodeType the the node type of the local device.
 *
 * @return An ::EmberStatus value indicating whether the operation
 *   succeeded, or why it failed.
 */
EmberStatus emberZllStartScan(uint32_t channelMask,
                              int8_t radioPowerForScan,
                              EmberNodeType nodeType)
{
  return ezspZllStartScan(channelMask, radioPowerForScan, nodeType);
}

/**
 * @brief This call will change the mode of the radio so that the receiver is
 * on when the device is idle.  Changing the idle mode permits the application
 * to communicate with the target of a touch link before the network is
 * established.  The receiver will remain on until the duration elapses, the
 * duration is set to zero, or ::emberZllJoinTarget is called.
 *
 * @param durationMs The duration in milliseconds to leave the radio on.
 *
 * @return An ::EmberStatus value indicating whether the operation succeeded or
 * why it failed.
 */
EmberStatus emberZllSetRxOnWhenIdle(uint16_t durationMs)
{
  return ezspZllSetRxOnWhenIdle(durationMs);
}

EmberStatus emSetLogicalAndRadioChannel(uint8_t channel)
{
  return ezspSetLogicalAndRadioChannel(channel);
}

uint8_t emGetLogicalChannel(void)
{
  return ezspGetLogicalChannel();
}

void zllGetTokens(EmberTokTypeStackZllData *data,
                  EmberTokTypeStackZllSecurity *security)
{
  ezspZllGetTokens(data, security);
}

void emberZllGetTokenStackZllData(EmberTokTypeStackZllData *token)
{
  EmberTokTypeStackZllSecurity security;
  zllGetTokens(token, &security);
}

void emberZllGetTokenStackZllSecurity(EmberTokTypeStackZllSecurity *token)
{
  EmberTokTypeStackZllData data;
  zllGetTokens(&data, token);
}

void emberZllSetTokenStackZllData(EmberTokTypeStackZllData *token)
{
  ezspZllSetDataToken(token);
}

void emberZllSetNonZllNetwork(void)
{
  ezspZllSetNonZllNetwork();
}
#endif // XAP2B

bool emberIsZllNetwork(void)
{
  return ezspIsZllNetwork();
}

//This function is used to pull the source Routing Overhead from the ezsp
//command frame EZSP_INCOMING_MESSAGE_HANDLER.
// While new NCP's will actively send a Source Route Overhead, old ones will
// not. The old NCP's send exactly 22 bytes as accounted in em260/stack-callbacks.c
// If there is an additional byte, then a valid sourceRoute Overhead exists.
uint8_t getSourceRouteOverhead(uint8_t messageLength)
{
  uint8_t sourceRouteOverhead = EZSP_SOURCE_ROUTE_OVERHEAD_UNKNOWN;
  if(serialGetCommandLength() - messageLength == 23){
    sourceRouteOverhead = fetchInt8u();
  }
  return sourceRouteOverhead;
}

