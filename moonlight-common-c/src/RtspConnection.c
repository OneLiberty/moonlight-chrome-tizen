#include "Limelight-internal.h"
#include "Rtsp.h"

#define RTSP_CONNECT_TIMEOUT_SEC 10
#define RTSP_RECEIVE_TIMEOUT_SEC 15
#define RTSP_RETRY_DELAY_MS 500

static int currentSeqNumber;
static char rtspTargetUrl[256];
static char* sessionIdString;
static bool hasSessionId;
static int rtspClientVersion;
static char urlAddr[URLSAFESTRING_LEN];
static bool useEnet;
static char* controlStreamId;
static bool encryptedRtspEnabled;

static PPLT_CRYPTO_CONTEXT encryptionCtx;
static PPLT_CRYPTO_CONTEXT decryptionCtx;
static uint32_t encryptionSequenceNumber;

static SOCKET sock = INVALID_SOCKET;
static ENetHost* client;
static ENetPeer* peer;

#define CHAR_TO_INT(x) ((x) - '0')
#define CHAR_IS_DIGIT(x) ((x) >= '0' && (x) <= '9')

// Create RTSP Option
static POPTION_ITEM createOptionItem(char* option, char* content)
{
    POPTION_ITEM item = malloc(sizeof(*item));
    if (item == NULL) {
        return NULL;
    }

    item->option = strdup(option);
    if (item->option == NULL) {
        free(item);
        return NULL;
    }

    item->content = strdup(content);
    if (item->content == NULL) {
        free(item->option);
        free(item);
        return NULL;
    }

    item->next = NULL;
    item->flags = FLAG_ALLOCATED_OPTION_FIELDS;

    return item;
}

// Add an option to the RTSP Message
static bool addOption(PRTSP_MESSAGE msg, char* option, char* content)
{
    POPTION_ITEM item = createOptionItem(option, content);
    if (item == NULL) {
        return false;
    }

    insertOption(&msg->options, item);
    msg->flags |= FLAG_ALLOCATED_OPTION_ITEMS;

    return true;
}

// Create an RTSP Request
static bool initializeRtspRequest(PRTSP_MESSAGE msg, char* command, char* target)
{
    char sequenceNumberStr[16];
    char clientVersionStr[16];

    // FIXME: Hacked CSeq attribute due to RTSP parser bug
    createRtspRequest(msg, NULL, 0, command, target, "RTSP/1.0",
        0, NULL, NULL, 0);

    snprintf(sequenceNumberStr, sizeof(sequenceNumberStr), "%d", currentSeqNumber++);
    snprintf(clientVersionStr, sizeof(clientVersionStr), "%d", rtspClientVersion);
    if (!addOption(msg, "CSeq", sequenceNumberStr) ||
        !addOption(msg, "X-GS-ClientVersion", clientVersionStr) ||
        (!useEnet && !addOption(msg, "Host", urlAddr))) {
        freeMessage(msg);
        return false;
    }

    return true;
}

#define ENCRYPTED_RTSP_BIT 0x80000000

typedef struct _ENC_RTSP_HEADER {
    uint32_t typeAndLength; // BE
    uint32_t sequenceNumber; // BE
    uint8_t tag[16];
} ENC_RTSP_HEADER, *PENC_RTSP_HEADER;

static char* sealRtspMessage(PRTSP_MESSAGE request, int* messageLen) {
    char* serializedMessage;
    PENC_RTSP_HEADER encryptedMessage;
    int plaintextLen;
    bool success;
    uint8_t iv[12] = { 0 };

    serializedMessage = serializeRtspMessage(request, &plaintextLen);
    if (serializedMessage == NULL) {
        return NULL;
    }
    else if (!encryptedRtspEnabled) {
        *messageLen = plaintextLen;
        return serializedMessage;
    }

    encryptedMessage = (PENC_RTSP_HEADER)malloc(sizeof(ENC_RTSP_HEADER) + plaintextLen);
    if (encryptedMessage == NULL) {
        free(serializedMessage);
        return NULL;
    }

    // Populate the IV in little endian byte order
    encryptionSequenceNumber++;
    iv[3] = (uint8_t)(encryptionSequenceNumber >> 24);
    iv[2] = (uint8_t)(encryptionSequenceNumber >> 16);
    iv[1] = (uint8_t)(encryptionSequenceNumber >> 8);
    iv[0] = (uint8_t)(encryptionSequenceNumber >> 0);

    // Set high bytes to something unique to ensure no IV collisions
    iv[10] = (uint8_t)'C'; // Client originated
    iv[11] = (uint8_t)'R'; // RTSP stream

    encryptedMessage->typeAndLength = BE32(ENCRYPTED_RTSP_BIT | plaintextLen);
    encryptedMessage->sequenceNumber = BE32(encryptionSequenceNumber);

    success = PltEncryptMessage(encryptionCtx, ALGORITHM_AES_GCM, 0,
                                (uint8_t*)StreamConfig.remoteInputAesKey, sizeof(StreamConfig.remoteInputAesKey),
                                iv, sizeof(iv),
                                encryptedMessage->tag, sizeof(encryptedMessage->tag),
                                (uint8_t*)serializedMessage, plaintextLen,
                                (uint8_t*)(encryptedMessage + 1), messageLen);
    free(serializedMessage);

    if (!success) {
        free(encryptedMessage);
        return NULL;
    }

    // The size returned from PltEncryptMessage() is the payload only
    *messageLen += sizeof(ENC_RTSP_HEADER);

    return (char*)encryptedMessage;
}

static bool unsealRtspMessage(char* rawMessage, int rawMessageLen, PRTSP_MESSAGE response) {
    char* decryptedMessage;
    int decryptedMessageLen;
    bool success;

    // If the server just closed the connection without responding with anything,
    // there's no point in proceeding any further trying to parse it.
    if (rawMessageLen == 0) {
        return false;
    }

    if (encryptedRtspEnabled) {
        PENC_RTSP_HEADER encryptedMessage;
        uint32_t seq;
        uint32_t typeAndLen;
        uint32_t len;
        uint8_t iv[12] = { 0 };

        if (rawMessageLen <= (int)sizeof(ENC_RTSP_HEADER)) {
            Limelog("RTSP encrypted header too small\n");
            return false;
        }

        encryptedMessage = (PENC_RTSP_HEADER)rawMessage;
        typeAndLen = BE32(encryptedMessage->typeAndLength);

        if (!(typeAndLen & ENCRYPTED_RTSP_BIT)) {
            Limelog("Rejecting unencrypted RTSP message\n");
            return false;
        }

        len = typeAndLen & ~ENCRYPTED_RTSP_BIT;
        if (len + sizeof(ENC_RTSP_HEADER) > (uint32_t)rawMessageLen) {
            Limelog("Rejecting partial encrypted RTSP message\n");
            return false;
        }
        else if (len + sizeof(ENC_RTSP_HEADER) < (uint32_t)rawMessageLen) {
            Limelog("Rejecting encrypted RTSP message with excess data\n");
            return false;
        }

        // Populate the IV in little endian byte order
        seq = BE32(encryptedMessage->sequenceNumber);
        iv[3] = (uint8_t)(seq >> 24);
        iv[2] = (uint8_t)(seq >> 16);
        iv[1] = (uint8_t)(seq >> 8);
        iv[0] = (uint8_t)(seq >> 0);

        // Set high bytes to something unique to ensure no IV collisions
        iv[10] = (uint8_t)'H'; // Host originated
        iv[11] = (uint8_t)'R'; // RTSP stream

        decryptedMessageLen = rawMessageLen - sizeof(ENC_RTSP_HEADER);
        decryptedMessage = (char*)malloc(decryptedMessageLen);
        if (decryptedMessage == NULL) {
            return false;
        }

        success = PltDecryptMessage(decryptionCtx, ALGORITHM_AES_GCM, 0,
                                    (uint8_t*)StreamConfig.remoteInputAesKey, sizeof(StreamConfig.remoteInputAesKey),
                                    iv, sizeof(iv),
                                    encryptedMessage->tag, sizeof(encryptedMessage->tag),
                                    (uint8_t*)(encryptedMessage + 1), decryptedMessageLen,
                                    (uint8_t*)decryptedMessage, &decryptedMessageLen);
        if (!success) {
            Limelog("Failed to decrypt RTSP response\n");
            free(decryptedMessage);
            return false;
        }
    }
    else {
        decryptedMessage = rawMessage;
        decryptedMessageLen = rawMessageLen;
    }

    if (parseRtspMessage(response, decryptedMessage, decryptedMessageLen) == RTSP_ERROR_SUCCESS) {
        success = true;
    }
    else {
        Limelog("Failed to parse RTSP response\n");
        success = false;
    }

    if (decryptedMessage != rawMessage) {
        free(decryptedMessage);
    }

    return success;
}

// Send RTSP message and get response over ENet
static bool transactRtspMessageEnet(PRTSP_MESSAGE request, PRTSP_MESSAGE response, bool expectingPayload, int* error) {
    ENetEvent event;
    char* serializedMessage;
    int messageLen;
    int offset;
    ENetPacket* packet;
    char* payload;
    int payloadLength;
    bool ret;
    char* responseBuffer;

    // RTSP encryption is not supported using ENet due to our special handling
    // of the payload below. Modern versions of Sunshine use TCP for RTSP.
    LC_ASSERT(!encryptedRtspEnabled);

    *error = -1;
    ret = false;
    responseBuffer = NULL;

    // We're going to handle the payload separately, so temporarily set the payload to NULL
    payload = request->payload;
    payloadLength = request->payloadLength;
    request->payload = NULL;
    request->payloadLength = 0;
    
    // Serialize the RTSP message into a message buffer
    serializedMessage = serializeRtspMessage(request, &messageLen);
    if (serializedMessage == NULL) {
        goto Exit;
    }
    
    // Create the reliable packet that describes our outgoing message
    packet = enet_packet_create(serializedMessage, messageLen, ENET_PACKET_FLAG_RELIABLE);
    if (packet == NULL) {
        goto Exit;
    }
    
    // Send the message
    if (enet_peer_send(peer, 0, packet) < 0) {
        enet_packet_destroy(packet);
        goto Exit;
    }
    enet_host_flush(client);

    // If we have a payload to send, we'll need to send that separately
    if (payload != NULL) {
        packet = enet_packet_create(payload, payloadLength, ENET_PACKET_FLAG_RELIABLE);
        if (packet == NULL) {
            goto Exit;
        }

        // Send the payload
        if (enet_peer_send(peer, 0, packet) < 0) {
            enet_packet_destroy(packet);
            goto Exit;
        }
        
        enet_host_flush(client);
    }
    
    // Wait for a reply
    if (serviceEnetHost(client, &event, RTSP_RECEIVE_TIMEOUT_SEC * 1000) <= 0 ||
        event.type != ENET_EVENT_TYPE_RECEIVE) {
        Limelog("Failed to receive RTSP reply: %d\n", LastSocketFail());
        goto Exit;
    }

    responseBuffer = malloc(event.packet->dataLength);
    if (responseBuffer == NULL) {
        Limelog("Failed to allocate RTSP response buffer\n");
        enet_packet_destroy(event.packet);
        goto Exit;
    }

    // Copy the data out and destroy the packet
    memcpy(responseBuffer, event.packet->data, event.packet->dataLength);
    offset = (int) event.packet->dataLength;
    enet_packet_destroy(event.packet);

    // Wait for the payload if we're expecting some
    if (expectingPayload) {
        // The payload comes in a second packet
        if (serviceEnetHost(client, &event, RTSP_RECEIVE_TIMEOUT_SEC * 1000) <= 0 ||
            event.type != ENET_EVENT_TYPE_RECEIVE) {
            Limelog("Failed to receive RTSP reply payload: %d\n", LastSocketFail());
            goto Exit;
        }

        responseBuffer = extendBuffer(responseBuffer, event.packet->dataLength + offset);
        if (responseBuffer == NULL) {
            Limelog("Failed to extend RTSP response buffer\n");
            enet_packet_destroy(event.packet);
            goto Exit;
        }

        // Copy the payload out to the end of the response buffer and destroy the packet
        memcpy(&responseBuffer[offset], event.packet->data, event.packet->dataLength);
        offset += (int) event.packet->dataLength;
        enet_packet_destroy(event.packet);
    }
        
    if (parseRtspMessage(response, responseBuffer, offset) == RTSP_ERROR_SUCCESS) {
        // Successfully parsed response
        ret = true;
    }
    else {
        Limelog("Failed to parse RTSP response\n");
    }

Exit:
    // Swap back the payload pointer to avoid leaking memory later
    request->payload = payload;
    request->payloadLength = payloadLength;

    // Free the serialized buffer
    if (serializedMessage != NULL) {
        free(serializedMessage);
    }

    // Free the response buffer
    if (responseBuffer != NULL) {
        free(responseBuffer);
    }

    return ret;
}

// Send RTSP message and get response over TCP
static bool transactRtspMessageTcp(PRTSP_MESSAGE request, PRTSP_MESSAGE response, int* error) {
    SOCK_RET err;
    bool ret;
    int offset;
    char* serializedMessage = NULL;
    int messageLen;
    char* responseBuffer;
    int responseBufferSize;
    int connectRetries;

    *error = -1;
    ret = false;
    responseBuffer = NULL;
    connectRetries = 0;

    // Retry up to 10 seconds if we receive ECONNREFUSED errors from the host PC.
    // This can happen with GFE 3.22 when initially launching a session because it
    // returns HTTP 200 OK for the /launch request before the RTSP handshake port
    // is listening.
    do {
        sock = connectTcpSocket(&RemoteAddr, AddrLen, RtspPortNumber, RTSP_CONNECT_TIMEOUT_SEC);
        if (sock == INVALID_SOCKET) {
            *error = LastSocketError();
            if (*error == ECONNREFUSED) {
                // Try again after 500 ms on ECONNREFUSED
                PltSleepMs(RTSP_RETRY_DELAY_MS);
            }
            else {
                // Fail if we get some other error
                break;
            }
        }
        else {
            // We successfully connected
            break;
        }
    } while (connectRetries++ < (RTSP_CONNECT_TIMEOUT_SEC * 1000) / RTSP_RETRY_DELAY_MS && !ConnectionInterrupted);
    if (sock == INVALID_SOCKET) {
        return ret;
    }

    serializedMessage = sealRtspMessage(request, &messageLen);
    if (serializedMessage == NULL) {
        closeSocket(sock);
        sock = INVALID_SOCKET;
        return ret;
    }

    // Send our message split into smaller chunks to avoid MTU issues.
    // enableNoDelay() must have been called for sendMtuSafe() to work.
    enableNoDelay(sock);
    err = sendMtuSafe(sock, serializedMessage, messageLen);
    if (err == SOCKET_ERROR) {
        *error = LastSocketError();
        Limelog("Failed to send RTSP message: %d\n", *error);
        goto Exit;
    }

    // Read the response until the server closes the connection
    offset = 0;
    responseBufferSize = 0;
    for (;;) {
        struct pollfd pfd;

        if (offset >= responseBufferSize) {
            responseBufferSize = offset + 16384;
            responseBuffer = extendBuffer(responseBuffer, responseBufferSize);
            if (responseBuffer == NULL) {
                Limelog("Failed to allocate RTSP response buffer\n");
                goto Exit;
            }
        }

        pfd.fd = sock;
        pfd.events = POLLIN;
        err = pollSockets(&pfd, 1, RTSP_RECEIVE_TIMEOUT_SEC * 1000);
        if (err == 0) {
            *error = ETIMEDOUT;
            Limelog("RTSP request timed out\n");
            goto Exit;
        }
        else if (err < 0) {
            *error = LastSocketError();
            Limelog("Failed to wait for RTSP response: %d\n", *error);
            goto Exit;
        }

        err = recv(sock, &responseBuffer[offset], responseBufferSize - offset, 0);
        if (err < 0) {
            // Error reading
            *error = LastSocketError();
            Limelog("Failed to read RTSP response: %d\n", *error);
            goto Exit;
        }
        else if (err == 0) {
            // Done reading
            break;
        }
        else {
            offset += err;
        }
    }

    // Decrypt (if necessary) and deserialize the RTSP response
    ret = unsealRtspMessage(responseBuffer, offset, response);

    // Fetch the local address for this socket if it's not populated yet
    if (LocalAddr.ss_family == 0) {
        SOCKADDR_LEN addrLen = (SOCKADDR_LEN)sizeof(LocalAddr);
        if (getsockname(sock, (struct sockaddr*)&LocalAddr, &addrLen) < 0) {
            Limelog("Failed to get local address: %d\n", LastSocketError());
            memset(&LocalAddr, 0, sizeof(LocalAddr));
        }
        else {
            LC_ASSERT(addrLen == AddrLen);
        }
    }

Exit:
    if (serializedMessage != NULL) {
        free(serializedMessage);
    }

    if (responseBuffer != NULL) {
        free(responseBuffer);
    }

    closeSocket(sock);
    sock = INVALID_SOCKET;
    return ret;
}

static bool transactRtspMessage(PRTSP_MESSAGE request, PRTSP_MESSAGE response, bool expectingPayload, int* error) {
    if (ConnectionInterrupted) {
        *error = -1;
        return false;
    }

    if (useEnet) {
        return transactRtspMessageEnet(request, response, expectingPayload, error);
    }
    else {
        return transactRtspMessageTcp(request, response, error);
    }
}

// Send RTSP OPTIONS request
static bool requestOptions(PRTSP_MESSAGE response, int* error) {
    RTSP_MESSAGE request;
    bool ret;

    *error = -1;

    ret = initializeRtspRequest(&request, "OPTIONS", rtspTargetUrl);
    if (ret) {
        ret = transactRtspMessage(&request, response, false, error);
        freeMessage(&request);
    }

    return ret;
}

// Send RTSP DESCRIBE request
static bool requestDescribe(PRTSP_MESSAGE response, int* error) {
    RTSP_MESSAGE request;
    bool ret;

    *error = -1;

    ret = initializeRtspRequest(&request, "DESCRIBE", rtspTargetUrl);
    if (ret) {
        if (addOption(&request, "Accept",
            "application/sdp") &&
            addOption(&request, "If-Modified-Since",
                "Thu, 01 Jan 1970 00:00:00 GMT")) {
            ret = transactRtspMessage(&request, response, true, error);
        }
        else {
            ret = false;
        }
        freeMessage(&request);
    }

    return ret;
}

// Send RTSP SETUP request
static bool setupStream(PRTSP_MESSAGE response, char* target, int* error) {
    RTSP_MESSAGE request;
    bool ret;
    char* transportValue;

    *error = -1;

    ret = initializeRtspRequest(&request, "SETUP", target);
    if (ret) {
        if (hasSessionId) {
            if (!addOption(&request, "Session", sessionIdString)) {
                ret = false;
                goto FreeMessage;
            }
        }

        if (AppVersionQuad[0] >= 6) {
            // It looks like GFE doesn't care what we say our port is but
            // we need to give it some port to successfully complete the
            // handshake process.
            transportValue = "unicast;X-GS-ClientPort=50000-50001";
        }
        else {
            transportValue = " ";
        }
        
        if (addOption(&request, "Transport", transportValue) &&
            addOption(&request, "If-Modified-Since",
                "Thu, 01 Jan 1970 00:00:00 GMT")) {
            ret = transactRtspMessage(&request, response, false, error);
        }
        else {
            ret = false;
        }

    FreeMessage:
        freeMessage(&request);
    }

    return ret;
}

// Send RTSP PLAY request
static bool playStream(PRTSP_MESSAGE response, char* target, int* error) {
    RTSP_MESSAGE request;
    bool ret;

    *error = -1;

    ret = initializeRtspRequest(&request, "PLAY", target);
    if (ret != 0) {
        if (addOption(&request, "Session", sessionIdString)) {
            ret = transactRtspMessage(&request, response, false, error);
        }
        else {
            ret = false;
        }
        freeMessage(&request);
    }

    return ret;
}

// Send RTSP ANNOUNCE message
static bool sendVideoAnnounce(PRTSP_MESSAGE response, int* error) {
    RTSP_MESSAGE request;
    bool ret;
    int payloadLength;
    char payloadLengthStr[16];

    *error = -1;

    ret = initializeRtspRequest(&request, "ANNOUNCE",
                                APP_VERSION_AT_LEAST(7, 1, 431) ? controlStreamId : "streamid=video");
    if (ret) {
        ret = false;

        if (!addOption(&request, "Session", sessionIdString) ||
            !addOption(&request, "Content-type", "application/sdp")) {
            goto FreeMessage;
        }

        request.payload = getSdpPayloadForStreamConfig(rtspClientVersion, &payloadLength);
        if (request.payload == NULL) {
            goto FreeMessage;
        }
        request.flags |= FLAG_ALLOCATED_PAYLOAD;
        request.payloadLength = payloadLength;

        snprintf(payloadLengthStr, sizeof(payloadLengthStr), "%d", payloadLength);
        if (!addOption(&request, "Content-length", payloadLengthStr)) {
            goto FreeMessage;
        }

        ret = transactRtspMessage(&request, response, false, error);

    FreeMessage:
        freeMessage(&request);
    }

    return ret;
}

static int parseOpusConfigFromParamString(char* paramStr, int channelCount, POPUS_MULTISTREAM_CONFIGURATION opusConfig) {
    int i;

    if (channelCount > AUDIO_CONFIGURATION_MAX_CHANNEL_COUNT) {
        Limelog("Invalid channel count: %d\n", channelCount);
        return -1;
    }

    // Set channel count (included in the prefix, so not parsed below)
    opusConfig->channelCount = channelCount;

    // Parse the remaining data from the surround-params value
    if (!CHAR_IS_DIGIT(*paramStr)) {
        Limelog("Invalid stream count: %c\n", *paramStr);
        return -1;
    }
    opusConfig->streams = CHAR_TO_INT(*paramStr);
    paramStr++;

    if (!CHAR_IS_DIGIT(*paramStr)) {
        Limelog("Invalid coupled stream count: %c\n", *paramStr);
        return -2;
    }
    opusConfig->coupledStreams = CHAR_TO_INT(*paramStr);
    paramStr++;

    for (i = 0; i < opusConfig->channelCount; i++) {
        if (!CHAR_IS_DIGIT(*paramStr)) {
            Limelog("Invalid mapping value at %d: %c\n", i, *paramStr);
            return -3;
        }

        opusConfig->mapping[i] = CHAR_TO_INT(*paramStr);
        paramStr++;
    }

    return 0;
}

// Parse the server port from the Transport header
// Example: unicast;server_port=48000-48001;source=192.168.35.177
static bool parseServerPortFromTransport(PRTSP_MESSAGE response, uint16_t* port) {
    char* transport;
    char* portStart;

    transport = getOptionContent(response->options, "Transport");
    if (transport == NULL) {
        return false;
    }

    // Look for the server_port= entry in the Transport option
    portStart = strstr(transport, "server_port=");
    if (portStart == NULL) {
        return false;
    }

    // Skip the prefix
    portStart += strlen("server_port=");

    // Validate the port number
    long int rawPort = strtol(portStart, NULL, 10);
    if (rawPort <= 0 || rawPort > 65535) {
        return false;
    }

    *port = (uint16_t)rawPort;
    return true;
}

// Parses the Opus configuration from an RTSP DESCRIBE response
static int parseOpusConfigurations(PRTSP_MESSAGE response) {
    HighQualitySurroundSupported = false;
    memset(&NormalQualityOpusConfig, 0, sizeof(NormalQualityOpusConfig));
    memset(&HighQualityOpusConfig, 0, sizeof(HighQualityOpusConfig));

    // Sample rate is always 48 KHz
    HighQualityOpusConfig.sampleRate = NormalQualityOpusConfig.sampleRate = 48000;

    // Stereo doesn't have any surround-params elements in the RTSP data
    if (CHANNEL_COUNT_FROM_AUDIO_CONFIGURATION(StreamConfig.audioConfiguration) == 2) {
        NormalQualityOpusConfig.channelCount = 2;
        NormalQualityOpusConfig.streams = 1;
        NormalQualityOpusConfig.coupledStreams = 1;
        NormalQualityOpusConfig.mapping[0] = 0;
        NormalQualityOpusConfig.mapping[1] = 1;
    }
    else {
        char paramsPrefix[128];
        char* paramStart;
        int err;
        int channelCount;

        channelCount = CHANNEL_COUNT_FROM_AUDIO_CONFIGURATION(StreamConfig.audioConfiguration);

        // Find the correct audio parameter value
        snprintf(paramsPrefix, sizeof(paramsPrefix), "a=fmtp:97 surround-params=%d", channelCount);
        paramStart = strstr(response->payload, paramsPrefix);
        if (paramStart) {
            // Skip the prefix
            paramStart += strlen(paramsPrefix);

            // Parse the normal quality Opus config
            err = parseOpusConfigFromParamString(paramStart, channelCount, &NormalQualityOpusConfig);
            if (err != 0) {
                LC_ASSERT(err == 0);
                return err;
            }

            // GFE's normal-quality channel mapping differs from the one our clients use.
            // They use FL FR C RL RR SL SR LFE, but we use FL FR C LFE RL RR SL SR. We'll need
            // to swap the mappings to match the expected values.
            if (channelCount == 6 || channelCount == 8) {
                OPUS_MULTISTREAM_CONFIGURATION originalMapping = NormalQualityOpusConfig;

                // LFE comes after C
                NormalQualityOpusConfig.mapping[3] = originalMapping.mapping[channelCount - 1];

                // Slide everything else up
                memcpy(&NormalQualityOpusConfig.mapping[4],
                       &originalMapping.mapping[3],
                       channelCount - 4);
            }

            // If this configuration is compatible with high quality mode, we may have another
            // matching surround-params value for high quality mode.
            paramStart = strstr(paramStart, paramsPrefix);
            if (paramStart) {
                // Skip the prefix
                paramStart += strlen(paramsPrefix);

                // Parse the high quality Opus config
                err = parseOpusConfigFromParamString(paramStart, channelCount, &HighQualityOpusConfig);
                if (err != 0) {
                    LC_ASSERT(err == 0);
                    return err;
                }

                // We can request high quality audio
                HighQualitySurroundSupported = true;
            }
        }
        else {
            Limelog("No surround parameters found for channel count: %d\n", channelCount);

            // It's unknown whether all GFE versions that supported surround sound included these
            // surround sound parameters. In case they didn't, we'll specifically handle 5.1 surround
            // sound using a hardcoded configuration like we used to before this parsing code existed.
            //
            // It is not necessary to provide HighQualityOpusConfig here because high quality mode
            // can only be enabled from seeing the required "surround-params=" value, so there's no
            // chance it could regress from implementing this parser.
            if (channelCount == 6) {
                NormalQualityOpusConfig.channelCount = 6;
                NormalQualityOpusConfig.streams = 4;
                NormalQualityOpusConfig.coupledStreams = 2;
                NormalQualityOpusConfig.mapping[0] = 0;
                NormalQualityOpusConfig.mapping[1] = 4;
                NormalQualityOpusConfig.mapping[2] = 1;
                NormalQualityOpusConfig.mapping[3] = 5;
                NormalQualityOpusConfig.mapping[4] = 2;
                NormalQualityOpusConfig.mapping[5] = 3;
            }
            else {
                // We don't have a hardcoded fallback mapping, so we have no choice but to fail.
                return -4;
            }
        }
    }

    return 0;
}

static bool parseUrlAddrFromRtspUrlString(const char* rtspUrlString, char* destination, size_t destinationLength) {
    char* rtspUrlScratchBuffer;
    char* portSeparator;
    char* v6EscapeEndChar;
    char* urlPathSeparator;
    int prefixLen;

    // Create a copy that we can modify
    rtspUrlScratchBuffer = strdup(rtspUrlString);
    if (rtspUrlScratchBuffer == NULL) {
        return false;
    }

    // If we have a v6 address, we want to stop one character after the closing ]
    // If we have a v4 address, we want to stop at the port separator
    portSeparator = strrchr(rtspUrlScratchBuffer, ':');
    v6EscapeEndChar = strchr(rtspUrlScratchBuffer, ']');

    // Count the prefix length to skip past the initial rtsp:// or rtspru:// part
    for (prefixLen = 2; rtspUrlScratchBuffer[prefixLen - 2] != 0 && (rtspUrlScratchBuffer[prefixLen - 2] != '/' || rtspUrlScratchBuffer[prefixLen - 1] != '/'); prefixLen++);

    // If we hit the end of the string prior to parsing the prefix, we cannot proceed
    if (rtspUrlScratchBuffer[prefixLen - 2] == 0) {
        free(rtspUrlScratchBuffer);
        return false;
    }

    // Look for a slash at the end of the host portion of the URL (may not be present)
    urlPathSeparator = strchr(rtspUrlScratchBuffer + prefixLen, '/');

    // Check for a v6 address first since they also have colons
    if (v6EscapeEndChar) {
        // Terminate the string at the next character
        *(v6EscapeEndChar + 1) = 0;
    }
    else if (portSeparator) {
        // Terminate the string prior to the port separator
        *portSeparator = 0;
    }
    else if (urlPathSeparator) {
        // Terminate the string prior to the path separator
        *urlPathSeparator = 0;
    }

    if (!PltSafeStrcpy(destination, destinationLength, rtspUrlScratchBuffer + prefixLen)) {
        free(rtspUrlScratchBuffer);
        return false;
    }

    free(rtspUrlScratchBuffer);
    return true;
}

// SDP attributes are in the form:
// a=x-nv-bwe.bwuSafeZoneLowLimit:70\r\n
bool parseSdpAttributeToUInt(const char* payload, const char* name, unsigned int* val) {
    // Find the entry for the specified attribute name
    char* attribute = strstr(payload, name);
    if (!attribute) {
        return false;
    }

    // Locate the start of the value
    char* valst = strstr(attribute, ":");
    if (!valst) {
        return false;
    }

    // Read the integer up to the newline at the end of the SDP attribute
    *val = strtoul(valst + 1, NULL, 0);

    return true;
}

bool parseSdpAttributeToInt(const char* payload, const char* name, int* val) {
    // Find the entry for the specified attribute name
    char* attribute = strstr(payload, name);
    if (!attribute) {
        return false;
    }

    // Locate the start of the value
    char* valst = strstr(attribute, ":");
    if (!valst) {
        return false;
    }

    // Read the integer up to the newline at the end of the SDP attribute
    *val = strtol(valst + 1, NULL, 0);

    return true;
}

// Perform RTSP Handshake with the streaming server machine as part of the connection process
int performRtspHandshake(PSERVER_INFORMATION serverInfo) {
    int ret;

    LC_ASSERT(RtspPortNumber != 0);

    // Initialize global state
    useEnet = (AppVersionQuad[0] >= 5) && (AppVersionQuad[0] <= 7) && (AppVersionQuad[2] < 404);
    currentSeqNumber = 1;
    hasSessionId = false;
    controlStreamId = APP_VERSION_AT_LEAST(7, 1, 431) ? "streamid=control/13/0" : "streamid=control/1/0";
    AudioEncryptionEnabled = false;
    encryptedRtspEnabled = serverInfo->rtspSessionUrl && strstr(serverInfo->rtspSessionUrl, "rtspenc://");
    encryptionCtx = PltCreateCryptoContext();
    decryptionCtx = PltCreateCryptoContext();

    // HACK: In order to get GFE to respect our request for a lower audio bitrate, we must
    // fake our target address so it doesn't match any of the PC's local interfaces. It seems
    // that the only way to get it to give you "low quality" stereo audio nowadays is if it
    // thinks you are remote (target address != any local address).
    //
    // We will enable high quality audio if the following are all true:
    // 1. Video bitrate is higher than 15 Mbps (to ensure most bandwidth is reserved for video)
    // 2. The audio decoder has not declared that it is slow
    // 3. The stream is either local or not surround sound (to prevent MTU issues over the Internet)
    LC_ASSERT(StreamConfig.streamingRemotely != STREAM_CFG_AUTO);
    if (StreamConfig.bitrate >= HIGH_AUDIO_BITRATE_THRESHOLD &&
            (AudioCallbacks.capabilities & CAPABILITY_SLOW_OPUS_DECODER) == 0 &&
            (StreamConfig.streamingRemotely != STREAM_CFG_REMOTE || CHANNEL_COUNT_FROM_AUDIO_CONFIGURATION(StreamConfig.audioConfiguration) <= 2)) {
        // If we have an RTSP URL string and it was successfully parsed and copied, use that string
        if (serverInfo->rtspSessionUrl == NULL ||
                !parseUrlAddrFromRtspUrlString(serverInfo->rtspSessionUrl, urlAddr, sizeof(urlAddr)) ||
                !PltSafeStrcpy(rtspTargetUrl, sizeof(rtspTargetUrl), serverInfo->rtspSessionUrl)) {
            // If an RTSP URL string was not provided or failed to parse, we will construct one now as best we can.
            //
            // NB: If the remote address is not a LAN address, the host will likely not enable high quality
            // audio since it only does that for local streaming normally. We can avoid this limitation,
            // but only if the caller gave us the RTSP session URL that it received from the host during launch.
            addrToUrlSafeString(&RemoteAddr, urlAddr, sizeof(urlAddr));
            snprintf(rtspTargetUrl, sizeof(rtspTargetUrl), "rtsp%s://%s:%u", useEnet ? "ru" : "", urlAddr, RtspPortNumber);
        }
    }
    else {
        PltSafeStrcpy(urlAddr, sizeof(urlAddr), "0.0.0.0");
        snprintf(rtspTargetUrl, sizeof(rtspTargetUrl), "rtsp%s://%s:%u", useEnet ? "ru" : "", urlAddr, RtspPortNumber);
    }

    switch (AppVersionQuad[0]) {
        case 3:
            rtspClientVersion = 10;
            break;
        case 4:
            rtspClientVersion = 11;
            break;
        case 5:
            rtspClientVersion = 12;
            break;
        case 6:
            // Gen 6 has never been seen in the wild
            rtspClientVersion = 13;
            break;
        case 7:
        default:
            rtspClientVersion = 14;
            break;
    }
    
    // Setup ENet if required by this GFE version
    if (useEnet) {
        ENetAddress address;
        ENetEvent event;
        
        enet_address_set_address(&address, (struct sockaddr *)&RemoteAddr, AddrLen);
        enet_address_set_port(&address, RtspPortNumber);
        
        // Create a client that can use 1 outgoing connection and 1 channel
        client = enet_host_create(RemoteAddr.ss_family, NULL, 1, 1, 0, 0);
        if (client == NULL) {
            return -1;
        }
    
        // Connect to the host
        peer = enet_host_connect(client, &address, 1, 0);
        if (peer == NULL) {
            enet_host_destroy(client);
            client = NULL;
            return -1;
        }
    
        // Wait for the connect to complete
        if (serviceEnetHost(client, &event, RTSP_CONNECT_TIMEOUT_SEC * 1000) <= 0 ||
            event.type != ENET_EVENT_TYPE_CONNECT) {
            Limelog("RTSP: Failed to connect to UDP port %u: error %d\n", RtspPortNumber, LastSocketFail());
            enet_peer_reset(peer);
            peer = NULL;
            enet_host_destroy(client);
            client = NULL;
            return -1;
        }

        // Ensure the connect verify ACK is sent immediately
        enet_host_flush(client);
    }

    {
        RTSP_MESSAGE response;
        int error = -1;

        if (!requestOptions(&response, &error)) {
            Limelog("RTSP OPTIONS request failed: %d\n", error);
            ret = error;
            goto Exit;
        }

        if (response.message.response.statusCode != 200) {
            Limelog("RTSP OPTIONS request failed: %d\n",
                response.message.response.statusCode);
            ret = response.message.response.statusCode;
            goto Exit;
        }

        freeMessage(&response);
    }

    {
        RTSP_MESSAGE response;
        int error = -1;

        if (!requestDescribe(&response, &error)) {
            Limelog("RTSP DESCRIBE request failed: %d\n", error);
            ret = error;
            goto Exit;
        }

        if (response.message.response.statusCode != 200) {
            Limelog("RTSP DESCRIBE request failed: %d\n",
                response.message.response.statusCode);
            ret = response.message.response.statusCode;
            goto Exit;
        }

        if (!response.payload) {
            Limelog("RTSP DESCRIBE no content in response\n");
            ret = -1;
            goto Exit;
        }
        
        if ((StreamConfig.supportedVideoFormats & VIDEO_FORMAT_MASK_AV1) && strstr(response.payload, "AV1/90000")) {
            if ((serverInfo->serverCodecModeSupport & SCM_AV1_HIGH10_444) && (StreamConfig.supportedVideoFormats & VIDEO_FORMAT_AV1_HIGH10_444)) {
                NegotiatedVideoFormat = VIDEO_FORMAT_AV1_HIGH10_444;
            }
            else if ((serverInfo->serverCodecModeSupport & SCM_AV1_MAIN10) && (StreamConfig.supportedVideoFormats & VIDEO_FORMAT_AV1_MAIN10)) {
                NegotiatedVideoFormat = VIDEO_FORMAT_AV1_MAIN10;
            }
            else if ((serverInfo->serverCodecModeSupport & SCM_AV1_HIGH8_444) && (StreamConfig.supportedVideoFormats & VIDEO_FORMAT_AV1_HIGH8_444)) {
                NegotiatedVideoFormat = VIDEO_FORMAT_AV1_HIGH8_444;
            }
            else {
                NegotiatedVideoFormat = VIDEO_FORMAT_AV1_MAIN8;
            }
        }
        else if ((StreamConfig.supportedVideoFormats & VIDEO_FORMAT_MASK_H265) && strstr(response.payload, "sprop-parameter-sets=AAAAAU")) {
            // The RTSP DESCRIBE reply will contain a collection of SDP media attributes that
            // describe the various supported video stream formats and include the SPS, PPS,
            // and VPS (if applicable). We will use this information to determine whether the
            // server can support HEVC. For some reason, they still set the MIME type of the HEVC
            // format to H264, so we can't just look for the HEVC MIME type. What we'll do instead is
            // look for the base 64 encoded VPS NALU prefix that is unique to the HEVC bitstream.
            if ((serverInfo->serverCodecModeSupport & SCM_HEVC_REXT10_444) && (StreamConfig.supportedVideoFormats & VIDEO_FORMAT_H265_REXT10_444)) {
                NegotiatedVideoFormat = VIDEO_FORMAT_H265_REXT10_444;
            }
            else if ((serverInfo->serverCodecModeSupport & SCM_HEVC_MAIN10) && (StreamConfig.supportedVideoFormats & VIDEO_FORMAT_H265_MAIN10)) {
                NegotiatedVideoFormat = VIDEO_FORMAT_H265_MAIN10;
            }
            else if ((serverInfo->serverCodecModeSupport & SCM_HEVC_REXT8_444) && (StreamConfig.supportedVideoFormats & VIDEO_FORMAT_H265_REXT8_444)) {
                NegotiatedVideoFormat = VIDEO_FORMAT_H265_REXT8_444;
            }
            else {
                NegotiatedVideoFormat = VIDEO_FORMAT_H265;
            }
        }
        else {
            if ((serverInfo->serverCodecModeSupport & SCM_H264_HIGH8_444) && (StreamConfig.supportedVideoFormats & VIDEO_FORMAT_H264_HIGH8_444)) {
                NegotiatedVideoFormat = VIDEO_FORMAT_H264_HIGH8_444;
            }
            else {
                NegotiatedVideoFormat = VIDEO_FORMAT_H264;
            }

            // Dimensions over 4096 are only supported with HEVC on NVENC
            if (StreamConfig.width > 4096 || StreamConfig.height > 4096) {
                Limelog("WARNING: Host PC doesn't support HEVC. Streaming at resolutions above 4K using H.264 will likely fail!\n");
            }
        }

        // Look for the SDP attribute that indicates we're dealing with a server that supports RFI
        ReferenceFrameInvalidationSupported = strstr(response.payload, "x-nv-video[0].refPicInvalidation") != NULL;
        if (!ReferenceFrameInvalidationSupported) {
            Limelog("Reference frame invalidation is not supported by this host\n");
        }

        // Look for the Sunshine feature flags in the SDP attributes
        if (!parseSdpAttributeToUInt(response.payload, "x-ss-general.featureFlags", &SunshineFeatureFlags)) {
            SunshineFeatureFlags = 0;
        }

        // Look for the Sunshine encryption flags in the SDP attributes
        if (!parseSdpAttributeToUInt(response.payload, "x-ss-general.encryptionSupported", &EncryptionFeaturesSupported)) {
            EncryptionFeaturesSupported = 0;
        }
        if (!parseSdpAttributeToUInt(response.payload, "x-ss-general.encryptionRequested", &EncryptionFeaturesRequested)) {
            EncryptionFeaturesRequested = 0;
        }
        EncryptionFeaturesEnabled = 0;

        // Parse the Opus surround parameters out of the RTSP DESCRIBE response.
        ret = parseOpusConfigurations(&response);
        if (ret != 0) {
            goto Exit;
        }

        freeMessage(&response);
    }

    {
        RTSP_MESSAGE response;
        char* sessionId;
        char* pingPayload;
        int error = -1;
        char* strtokCtx = NULL;

        if (!setupStream(&response,
                         AppVersionQuad[0] >= 5 ? "streamid=audio/0/0" : "streamid=audio",
                         &error)) {
            Limelog("RTSP SETUP streamid=audio request failed: %d\n", error);
            ret = error;
            goto Exit;
        }

        if (response.message.response.statusCode != 200) {
            Limelog("RTSP SETUP streamid=audio request failed: %d\n",
                response.message.response.statusCode);
            ret = response.message.response.statusCode;
            goto Exit;
        }

        // Parse the audio port out of the RTSP SETUP response
        LC_ASSERT(AudioPortNumber == 0);
        if (!parseServerPortFromTransport(&response, &AudioPortNumber)) {
            // Use the well known port if parsing fails
            AudioPortNumber = 48000;

            Limelog("Audio port: %u (RTSP parsing failed)\n", AudioPortNumber);
        }
        else {
            Limelog("Audio port: %u\n", AudioPortNumber);
        }

        // Parse the Sunshine ping payload protocol extension if present
        memset(&AudioPingPayload, 0, sizeof(AudioPingPayload));
        pingPayload = getOptionContent(response.options, "X-SS-Ping-Payload");
        if (pingPayload != NULL && strlen(pingPayload) == sizeof(AudioPingPayload.payload)) {
            memcpy(AudioPingPayload.payload, pingPayload, sizeof(AudioPingPayload.payload));
        }

        // Let the audio stream know the port number is now finalized.
        // NB: This is needed because audio stream init happens before RTSP,
        // which is not the case for the video stream.
        notifyAudioPortNegotiationComplete();

        sessionId = getOptionContent(response.options, "Session");

        if (sessionId == NULL) {
            Limelog("RTSP SETUP streamid=audio is missing session attribute\n");
            ret = -1;
            goto Exit;
        }

        // Given there is a non-null session id, get the
        // first token of the session until ";", which 
        // resolves any 454 session not found errors on
        // standard RTSP server implementations.
        // (i.e - sessionId = "DEADBEEFCAFE;timeout = 90") 
        sessionIdString = strdup(strtok_r(sessionId, ";", &strtokCtx));
        if (sessionIdString == NULL) {
            Limelog("Failed to duplicate session ID string\n");
            ret = -1;
            goto Exit;
        }

        hasSessionId = true;

        freeMessage(&response);
    }

    {
        RTSP_MESSAGE response;
        int error = -1;
        char* pingPayload;

        if (!setupStream(&response,
                         AppVersionQuad[0] >= 5 ? "streamid=video/0/0" : "streamid=video",
                         &error)) {
            Limelog("RTSP SETUP streamid=video request failed: %d\n", error);
            ret = error;
            goto Exit;
        }

        if (response.message.response.statusCode != 200) {
            Limelog("RTSP SETUP streamid=video request failed: %d\n",
                response.message.response.statusCode);
            ret = response.message.response.statusCode;
            goto Exit;
        }

        // Parse the Sunshine ping payload protocol extension if present
        memset(&VideoPingPayload, 0, sizeof(VideoPingPayload));
        pingPayload = getOptionContent(response.options, "X-SS-Ping-Payload");
        if (pingPayload != NULL && strlen(pingPayload) == sizeof(VideoPingPayload.payload)) {
            memcpy(VideoPingPayload.payload, pingPayload, sizeof(VideoPingPayload.payload));
        }

        // Parse the video port out of the RTSP SETUP response
        LC_ASSERT(VideoPortNumber == 0);
        if (!parseServerPortFromTransport(&response, &VideoPortNumber)) {
            // Use the well known port if parsing fails
            VideoPortNumber = 47998;

            Limelog("Video port: %u (RTSP parsing failed)\n", VideoPortNumber);
        }
        else {
            Limelog("Video port: %u\n", VideoPortNumber);
        }

        freeMessage(&response);
    }
    
    if (AppVersionQuad[0] >= 5) {
        RTSP_MESSAGE response;
        int error = -1;
        char* connectData;

        if (!setupStream(&response,
                         controlStreamId,
                         &error)) {
            Limelog("RTSP SETUP streamid=control request failed: %d\n", error);
            ret = error;
            goto Exit;
        }

        if (response.message.response.statusCode != 200) {
            Limelog("RTSP SETUP streamid=control request failed: %d\n",
                response.message.response.statusCode);
            ret = response.message.response.statusCode;
            goto Exit;
        }

        // Parse the Sunshine control connect data extension if present
        connectData = getOptionContent(response.options, "X-SS-Connect-Data");
        if (connectData != NULL) {
            ControlConnectData = (uint32_t)strtoul(connectData, NULL, 0);
        }
        else {
            ControlConnectData = 0;
        }

        // Parse the control port out of the RTSP SETUP response
        LC_ASSERT(ControlPortNumber == 0);
        if (!parseServerPortFromTransport(&response, &ControlPortNumber)) {
            // Use the well known port if parsing fails
            ControlPortNumber = 47999;

            Limelog("Control port: %u (RTSP parsing failed)\n", ControlPortNumber);
        }
        else {
            Limelog("Control port: %u\n", ControlPortNumber);
        }

        freeMessage(&response);
    }

    {
        RTSP_MESSAGE response;
        int error = -1;

        if (!sendVideoAnnounce(&response, &error)) {
            Limelog("RTSP ANNOUNCE request failed: %d\n", error);
            ret = error;
            goto Exit;
        }

        if (response.message.response.statusCode != 200) {
            Limelog("RTSP ANNOUNCE request failed: %d\n",
                response.message.response.statusCode);
            ret = response.message.response.statusCode;
            goto Exit;
        }

        freeMessage(&response);
    }

    // GFE 3.22 uses a single PLAY message
    if (APP_VERSION_AT_LEAST(7, 1, 431)) {
        RTSP_MESSAGE response;
        int error = -1;

        if (!playStream(&response, "/", &error)) {
            Limelog("RTSP PLAY request failed: %d\n", error);
            ret = error;
            goto Exit;
        }

        if (response.message.response.statusCode != 200) {
            Limelog("RTSP PLAY failed: %d\n",
                response.message.response.statusCode);
            ret = response.message.response.statusCode;
            goto Exit;
        }

        freeMessage(&response);
    }
    else {
        {
            RTSP_MESSAGE response;
            int error = -1;

            if (!playStream(&response, "streamid=video", &error)) {
                Limelog("RTSP PLAY streamid=video request failed: %d\n", error);
                ret = error;
                goto Exit;
            }

            if (response.message.response.statusCode != 200) {
                Limelog("RTSP PLAY streamid=video failed: %d\n",
                    response.message.response.statusCode);
                ret = response.message.response.statusCode;
                goto Exit;
            }

            freeMessage(&response);
        }

        {
            RTSP_MESSAGE response;
            int error = -1;

            if (!playStream(&response, "streamid=audio", &error)) {
                Limelog("RTSP PLAY streamid=audio request failed: %d\n", error);
                ret = error;
                goto Exit;
            }

            if (response.message.response.statusCode != 200) {
                Limelog("RTSP PLAY streamid=audio failed: %d\n",
                    response.message.response.statusCode);
                ret = response.message.response.statusCode;
                goto Exit;
            }

            freeMessage(&response);
        }
    }

    
    ret = 0;
    
Exit:
    // Cleanup the ENet stuff
    if (useEnet) {
        if (peer != NULL) {
            enet_peer_disconnect_now(peer, 0);
            peer = NULL;
        }
        
        if (client != NULL) {
            enet_host_destroy(client);
            client = NULL;
        }
    }

    if (sessionIdString != NULL) {
        free(sessionIdString);
        sessionIdString = NULL;
    }

    PltDestroyCryptoContext(encryptionCtx);
    PltDestroyCryptoContext(decryptionCtx);
    decryptionCtx = encryptionCtx = NULL;

    return ret;
}
