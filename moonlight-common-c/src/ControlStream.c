#include "Limelight-internal.h"

// This is a private header, but it just contains some time macros
#include <enet/time.h>

#ifndef MIN
#define MIN(x, y) ((x) < (y) ? (x) : (y))
#endif

// NV control stream packet header for TCP
typedef struct _NVCTL_TCP_PACKET_HEADER {
    unsigned short type;
    unsigned short payloadLength;
} NVCTL_TCP_PACKET_HEADER, *PNVCTL_TCP_PACKET_HEADER;

typedef struct _NVCTL_ENET_PACKET_HEADER_V1 {
    unsigned short type;
} NVCTL_ENET_PACKET_HEADER_V1, *PNVCTL_ENET_PACKET_HEADER_V1;

typedef struct _NVCTL_ENET_PACKET_HEADER_V2 {
    unsigned short type;
    unsigned short payloadLength;
} NVCTL_ENET_PACKET_HEADER_V2, *PNVCTL_ENET_PACKET_HEADER_V2;

#define AES_GCM_TAG_LENGTH 16
typedef struct _NVCTL_ENCRYPTED_PACKET_HEADER {
    unsigned short encryptedHeaderType; // Always LE 0x0001
    unsigned short length; // sizeof(seq) + 16 byte tag + secondary header and data
    unsigned int seq; // Monotonically increasing sequence number (used as IV for AES-GCM)

    // encrypted NVCTL_ENET_PACKET_HEADER_V2 and payload data follow
} NVCTL_ENCRYPTED_PACKET_HEADER, *PNVCTL_ENCRYPTED_PACKET_HEADER;

typedef struct _QUEUED_FRAME_INVALIDATION_TUPLE {
    uint32_t startFrame;
    uint32_t endFrame;
    LINKED_BLOCKING_QUEUE_ENTRY entry;
} QUEUED_FRAME_INVALIDATION_TUPLE, *PQUEUED_FRAME_INVALIDATION_TUPLE;

typedef struct _QUEUED_FRAME_FEC_STATUS {
    SS_FRAME_FEC_STATUS fecStatus;
    LINKED_BLOCKING_QUEUE_ENTRY entry;
} QUEUED_FRAME_FEC_STATUS, *PQUEUED_FRAME_FEC_STATUS;

typedef struct _QUEUED_ASYNC_CALLBACK {
    int typeIndex;
    union {
        struct {
            uint16_t controllerNumber;
            uint16_t lowFreqRumble;
            uint16_t highFreqRumble;
        } rumble;
        struct {
            uint16_t controllerNumber;
            uint16_t leftTriggerMotor;
            uint16_t rightTriggerMotor;
        } rumbleTriggers;
        struct {
            uint16_t controllerNumber;
            uint16_t reportRateHz;
            uint8_t motionType;
        } setMotionEventState;
        struct {
            uint16_t controllerNumber;
            uint8_t r;
            uint8_t g;
            uint8_t b;
        } setControllerLed;
    } data;
    LINKED_BLOCKING_QUEUE_ENTRY entry;
} QUEUED_ASYNC_CALLBACK, *PQUEUED_ASYNC_CALLBACK;

static SOCKET ctlSock = INVALID_SOCKET;
static ENetHost* client;
static ENetPeer* peer;
static PLT_MUTEX enetMutex;
static bool usePeriodicPing;

static PLT_THREAD lossStatsThread;
static PLT_THREAD invalidateRefFramesThread;
static PLT_THREAD requestIdrFrameThread;
static PLT_THREAD controlReceiveThread;
static PLT_THREAD asyncCallbackThread;
static uint32_t lastGoodFrame;
static uint32_t lastSeenFrame;
static bool stopping;
static bool disconnectPending;
static bool encryptedControlStream;
static bool hdrEnabled;
static SS_HDR_METADATA hdrMetadata;

static int intervalGoodFrameCount;
static int intervalTotalFrameCount;
static uint64_t intervalStartTimeMs;
static int lastIntervalLossPercentage;
static int lastConnectionStatusUpdate;
static uint32_t currentEnetSequenceNumber;
static uint64_t firstFrameTimeMs;

static LINKED_BLOCKING_QUEUE invalidReferenceFrameTuples;
static LINKED_BLOCKING_QUEUE frameFecStatusQueue;
static LINKED_BLOCKING_QUEUE asyncCallbackQueue;
static PLT_EVENT idrFrameRequiredEvent;

static PPLT_CRYPTO_CONTEXT encryptionCtx;
static PPLT_CRYPTO_CONTEXT decryptionCtx;

#define CONN_IMMEDIATE_POOR_LOSS_RATE 30
#define CONN_CONSECUTIVE_POOR_LOSS_RATE 15
#define CONN_OKAY_LOSS_RATE 5
#define CONN_STATUS_SAMPLE_PERIOD 3000

#define IDX_START_A 0
#define IDX_REQUEST_IDR_FRAME 0
#define IDX_START_B 1
#define IDX_INVALIDATE_REF_FRAMES 2
#define IDX_LOSS_STATS 3
#define IDX_INPUT_DATA 5
#define IDX_RUMBLE_DATA 6
#define IDX_TERMINATION 7
#define IDX_HDR_INFO 8
#define IDX_RUMBLE_TRIGGER_DATA 9
#define IDX_SET_MOTION_EVENT 10
#define IDX_SET_RGB_LED 11

#define CONTROL_STREAM_TIMEOUT_SEC 10
#define CONTROL_STREAM_LINGER_TIMEOUT_SEC 2

static const short packetTypesGen3[] = {
    0x1407, // Request IDR frame
    0x1410, // Start B
    0x1404, // Invalidate reference frames
    0x140c, // Loss Stats
    0x1417, // Frame Stats (unused)
    -1,     // Input data (unused)
    -1,     // Rumble data (unused)
    -1,     // Termination (unused)
    -1,     // HDR mode (unused)
    -1,     // Rumble triggers (unused)
    -1,     // Set motion event (unused)
    -1,     // Set RGB LED (unused)
};
static const short packetTypesGen4[] = {
    0x0606, // Request IDR frame
    0x0609, // Start B
    0x0604, // Invalidate reference frames
    0x060a, // Loss Stats
    0x0611, // Frame Stats (unused)
    -1,     // Input data (unused)
    -1,     // Rumble data (unused)
    -1,     // Termination (unused)
    -1,     // HDR mode (unused)
    -1,     // Rumble triggers (unused)
    -1,     // Set motion event (unused)
    -1,     // Set RGB LED (unused)
};
static const short packetTypesGen5[] = {
    0x0305, // Start A
    0x0307, // Start B
    0x0301, // Invalidate reference frames
    0x0201, // Loss Stats
    0x0204, // Frame Stats (unused)
    0x0207, // Input data
    -1,     // Rumble data (unused)
    -1,     // Termination (unused)
    -1,     // HDR mode (unknown)
    -1,     // Rumble triggers (unused)
    -1,     // Set motion event (unused)
    -1,     // Set RGB LED (unused)
};
static const short packetTypesGen7[] = {
    0x0305, // Start A
    0x0307, // Start B
    0x0301, // Invalidate reference frames
    0x0201, // Loss Stats
    0x0204, // Frame Stats (unused)
    0x0206, // Input data
    0x010b, // Rumble data
    0x0100, // Termination
    0x010e, // HDR mode
    -1,     // Rumble triggers (unused)
    -1,     // Set motion event (unused)
    -1,     // Set RGB LED (unused)
};
static const short packetTypesGen7Enc[] = {
    0x0302, // Request IDR frame
    0x0307, // Start B
    0x0301, // Invalidate reference frames
    0x0201, // Loss Stats
    0x0204, // Frame Stats (unused)
    0x0206, // Input data
    0x010b, // Rumble data
    0x0109, // Termination (extended)
    0x010e, // HDR mode
    0x5500, // Rumble triggers (Sunshine protocol extension)
    0x5501, // Set motion event (Sunshine protocol extension)
    0x5502, // Set RGB LED (Sunshine protocol extension)
};

static const char requestIdrFrameGen3[] = { 0, 0 };
static const int startBGen3[] = { 0, 0, 0, 0xa };

static const char requestIdrFrameGen4[] = { 0, 0 };
static const char startBGen4[] = { 0 };

static const char startAGen5[] = { 0, 0 };
static const char startBGen5[] = { 0 };

static const char requestIdrFrameGen7Enc[] = { 0, 0 };

static const short payloadLengthsGen3[] = {
    sizeof(requestIdrFrameGen3), // Request IDR frame
    sizeof(startBGen3), // Start B
    24, // Invalidate reference frames
    32, // Loss Stats
    64, // Frame Stats
    -1, // Input data
};
static const short payloadLengthsGen4[] = {
    sizeof(requestIdrFrameGen4), // Request IDR frame
    sizeof(startBGen4), // Start B
    24, // Invalidate reference frames
    32, // Loss Stats
    64, // Frame Stats
    -1, // Input data
};
static const short payloadLengthsGen5[] = {
    sizeof(startAGen5), // Start A
    sizeof(startBGen5), // Start B
    24, // Invalidate reference frames
    32, // Loss Stats
    80, // Frame Stats
    -1, // Input data
};
static const short payloadLengthsGen7[] = {
    sizeof(startAGen5), // Start A
    sizeof(startBGen5), // Start B
    24, // Invalidate reference frames
    32, // Loss Stats
    80, // Frame Stats
    -1, // Input data
};
static const short payloadLengthsGen7Enc[] = {
    sizeof(requestIdrFrameGen7Enc), // Request IDR frame
    sizeof(startBGen5), // Start B
    24, // Invalidate reference frames
    32, // Loss Stats
    80, // Frame Stats
    -1, // Input data
};

static const char* preconstructedPayloadsGen3[] = {
    requestIdrFrameGen3,
    (char*)startBGen3
};
static const char* preconstructedPayloadsGen4[] = {
    requestIdrFrameGen4,
    startBGen4
};
static const char* preconstructedPayloadsGen5[] = {
    startAGen5,
    startBGen5
};
static const char* preconstructedPayloadsGen7[] = {
    startAGen5,
    startBGen5
};
static const char* preconstructedPayloadsGen7Enc[] = {
    requestIdrFrameGen7Enc,
    startBGen5
};

static short* packetTypes;
static short* payloadLengths;
static char**preconstructedPayloads;
static bool supportsIdrFrameRequest;

#define LOSS_REPORT_INTERVAL_MS 50
#define PERIODIC_PING_INTERVAL_MS 100

// Initializes the control stream
int initializeControlStream(void) {
    stopping = false;
    PltCreateEvent(&idrFrameRequiredEvent);
    LbqInitializeLinkedBlockingQueue(&invalidReferenceFrameTuples, 20);
    LbqInitializeLinkedBlockingQueue(&frameFecStatusQueue, 8); // Limits number of frame status reports per periodic ping interval
    LbqInitializeLinkedBlockingQueue(&asyncCallbackQueue, 30);
    PltCreateMutex(&enetMutex);

    encryptedControlStream = APP_VERSION_AT_LEAST(7, 1, 431);

    if (AppVersionQuad[0] == 3) {
        packetTypes = (short*)packetTypesGen3;
        payloadLengths = (short*)payloadLengthsGen3;
        preconstructedPayloads = (char**)preconstructedPayloadsGen3;
        supportsIdrFrameRequest = true;
    }
    else if (AppVersionQuad[0] == 4) {
        packetTypes = (short*)packetTypesGen4;
        payloadLengths = (short*)payloadLengthsGen4;
        preconstructedPayloads = (char**)preconstructedPayloadsGen4;
        supportsIdrFrameRequest = true;
    }
    else if (AppVersionQuad[0] == 5) {
        packetTypes = (short*)packetTypesGen5;
        payloadLengths = (short*)payloadLengthsGen5;
        preconstructedPayloads = (char**)preconstructedPayloadsGen5;
        supportsIdrFrameRequest = false;
    }
    else {
        if (encryptedControlStream) {
            packetTypes = (short*)packetTypesGen7Enc;
            payloadLengths = (short*)payloadLengthsGen7Enc;
            preconstructedPayloads = (char**)preconstructedPayloadsGen7Enc;
            supportsIdrFrameRequest = true;
        }
        else {
            packetTypes = (short*)packetTypesGen7;
            payloadLengths = (short*)payloadLengthsGen7;
            preconstructedPayloads = (char**)preconstructedPayloadsGen7;
            supportsIdrFrameRequest = false;
        }
    }

    lastGoodFrame = 0;
    lastSeenFrame = 0;
    disconnectPending = false;
    intervalGoodFrameCount = 0;
    intervalTotalFrameCount = 0;
    intervalStartTimeMs = 0;
    lastIntervalLossPercentage = 0;
    lastConnectionStatusUpdate = CONN_STATUS_OKAY;
    firstFrameTimeMs = 0;
    currentEnetSequenceNumber = 0;
    usePeriodicPing = APP_VERSION_AT_LEAST(7, 1, 415);
    encryptionCtx = PltCreateCryptoContext();
    decryptionCtx = PltCreateCryptoContext();
    hdrEnabled = false;
    memset(&hdrMetadata, 0, sizeof(hdrMetadata));

    return 0;
}

static void freeBasicLbqList(PLINKED_BLOCKING_QUEUE_ENTRY entry) {
    PLINKED_BLOCKING_QUEUE_ENTRY nextEntry;

    while (entry != NULL) {
        nextEntry = entry->flink;
        free(entry->data);
        entry = nextEntry;
    }
}

// Cleans up control stream
void destroyControlStream(void) {
    LC_ASSERT(stopping);
    PltDestroyCryptoContext(encryptionCtx);
    PltDestroyCryptoContext(decryptionCtx);
    PltCloseEvent(&idrFrameRequiredEvent);
    freeBasicLbqList(LbqDestroyLinkedBlockingQueue(&invalidReferenceFrameTuples));
    freeBasicLbqList(LbqDestroyLinkedBlockingQueue(&frameFecStatusQueue));
    freeBasicLbqList(LbqDestroyLinkedBlockingQueue(&asyncCallbackQueue));

    PltDeleteMutex(&enetMutex);
}

static void queueFrameInvalidationTuple(uint32_t startFrame, uint32_t endFrame) {
    LC_ASSERT(startFrame <= endFrame);

    if (isReferenceFrameInvalidationEnabled()) {
        PQUEUED_FRAME_INVALIDATION_TUPLE qfit;
        qfit = malloc(sizeof(*qfit));
        if (qfit != NULL) {
            qfit->startFrame = startFrame;
            qfit->endFrame = endFrame;
            if (LbqOfferQueueItem(&invalidReferenceFrameTuples, qfit, &qfit->entry) == LBQ_BOUND_EXCEEDED) {
                // Too many invalidation tuples, so we need an IDR frame now
                Limelog("RFI range list reached maximum size limit\n");
                free(qfit);
                LiRequestIdrFrame();
            }
        }
        else {
            LiRequestIdrFrame();
        }
    }
    else {
        LiRequestIdrFrame();
    }
}

// Request an IDR frame on demand by the decoder
void LiRequestIdrFrame(void) {
    // Any reference frame invalidation requests should be dropped now.
    // We require a full IDR frame to recover.
    freeBasicLbqList(LbqFlushQueueItems(&invalidReferenceFrameTuples));

    // Request the IDR frame
    PltSetEvent(&idrFrameRequiredEvent);
}

// Invalidate reference frames lost by the network
void connectionDetectedFrameLoss(uint32_t startFrame, uint32_t endFrame) {
    queueFrameInvalidationTuple(startFrame, endFrame);
}

// When we receive a frame, update the number of our current frame
void connectionReceivedCompleteFrame(uint32_t frameIndex) {
    lastGoodFrame = frameIndex;
    intervalGoodFrameCount++;
}

void connectionSendFrameFecStatus(PSS_FRAME_FEC_STATUS fecStatus) {
    // This is a Sunshine protocol extension
    if (!IS_SUNSHINE()) {
        return;
    }

    // Queue a frame FEC status message. This is best-effort only.
    PQUEUED_FRAME_FEC_STATUS queuedFecStatus = malloc(sizeof(*queuedFecStatus));
    if (queuedFecStatus != NULL) {
        queuedFecStatus->fecStatus = *fecStatus;
        if (LbqOfferQueueItem(&frameFecStatusQueue, queuedFecStatus, &queuedFecStatus->entry) == LBQ_BOUND_EXCEEDED) {
            free(queuedFecStatus);
        }
    }
}

void connectionSawFrame(uint32_t frameIndex) {
    LC_ASSERT_VT(!isBefore16(frameIndex, lastSeenFrame));

    uint64_t now = PltGetMillis();

    // Suppress connection status warnings for the first sampling period
    // to allow the network and host to settle.
    if (lastSeenFrame == 0) {
        lastSeenFrame = frameIndex;
        firstFrameTimeMs = now;
        return;
    }
    else if (now - firstFrameTimeMs < CONN_STATUS_SAMPLE_PERIOD) {
        lastSeenFrame = frameIndex;
        return;
    }

    if (now - intervalStartTimeMs >= CONN_STATUS_SAMPLE_PERIOD) {
        if (intervalTotalFrameCount != 0) {
            // Notify the client of connection status changes based on frame loss rate
            int frameLossPercent = 100 - (intervalGoodFrameCount * 100) / intervalTotalFrameCount;
            if (lastConnectionStatusUpdate != CONN_STATUS_POOR &&
                    (frameLossPercent >= CONN_IMMEDIATE_POOR_LOSS_RATE ||
                     (frameLossPercent >= CONN_CONSECUTIVE_POOR_LOSS_RATE && lastIntervalLossPercentage >= CONN_CONSECUTIVE_POOR_LOSS_RATE))) {
                // We require 2 consecutive intervals above CONN_CONSECUTIVE_POOR_LOSS_RATE or a single
                // interval above CONN_IMMEDIATE_POOR_LOSS_RATE to notify of a poor connection.
                ListenerCallbacks.connectionStatusUpdate(CONN_STATUS_POOR);
                lastConnectionStatusUpdate = CONN_STATUS_POOR;
            }
            else if (frameLossPercent <= CONN_OKAY_LOSS_RATE && lastConnectionStatusUpdate != CONN_STATUS_OKAY) {
                ListenerCallbacks.connectionStatusUpdate(CONN_STATUS_OKAY);
                lastConnectionStatusUpdate = CONN_STATUS_OKAY;
            }

            lastIntervalLossPercentage = frameLossPercent;
        }

        // Reset interval
        intervalStartTimeMs = now;
        intervalGoodFrameCount = intervalTotalFrameCount = 0;
    }

    intervalTotalFrameCount += frameIndex - lastSeenFrame;
    lastSeenFrame = frameIndex;
}

// Reads an NV control stream packet from the TCP connection
static PNVCTL_TCP_PACKET_HEADER readNvctlPacketTcp(void) {
    NVCTL_TCP_PACKET_HEADER staticHeader;
    PNVCTL_TCP_PACKET_HEADER fullPacket;
    SOCK_RET err;

    err = recv(ctlSock, (char*)&staticHeader, sizeof(staticHeader), 0);
    if (err != sizeof(staticHeader)) {
        return NULL;
    }

    staticHeader.type = LE16(staticHeader.type);
    staticHeader.payloadLength = LE16(staticHeader.payloadLength);

    fullPacket = (PNVCTL_TCP_PACKET_HEADER)malloc(staticHeader.payloadLength + sizeof(staticHeader));
    if (fullPacket == NULL) {
        return NULL;
    }

    memcpy(fullPacket, &staticHeader, sizeof(staticHeader));
    if (staticHeader.payloadLength != 0) {
        err = recv(ctlSock, (char*)(fullPacket + 1), staticHeader.payloadLength, 0);
        if (err != staticHeader.payloadLength) {
            free(fullPacket);
            return NULL;
        }
    }

    return fullPacket;
}

static bool encryptControlMessage(PNVCTL_ENCRYPTED_PACKET_HEADER encPacket, PNVCTL_ENET_PACKET_HEADER_V2 packet) {
    unsigned char iv[16] = { 0 };
    int ivSize;
    int encryptedSize = sizeof(*packet) + packet->payloadLength;

    // NB: Setting the IV must happen while encPacket->seq is still in native byte-order!
    if (EncryptionFeaturesEnabled & SS_ENC_CONTROL_V2) {
        // Populate the IV in little endian byte order
        iv[3] = (unsigned char)(encPacket->seq >> 24);
        iv[2] = (unsigned char)(encPacket->seq >> 16);
        iv[1] = (unsigned char)(encPacket->seq >> 8);
        iv[0] = (unsigned char)(encPacket->seq >> 0);

        // Set high bytes to something unique to ensure no IV collisions
        iv[10] = (unsigned char)'C'; // Client originated
        iv[11] = (unsigned char)'C'; // Control stream

        // Use 12-byte IV which is ideal for AES-GCM
        ivSize = 12;
    }
    else {
        // This is a truncating cast, but it's what Nvidia does, so we have to mimic it.
        iv[0] = (unsigned char)encPacket->seq;

        // Nvidia's old style encryption uses a 16-byte IV
        ivSize = 16;
    }

    encPacket->encryptedHeaderType = LE16(encPacket->encryptedHeaderType);
    encPacket->length = LE16(encPacket->length);
    encPacket->seq = LE32(encPacket->seq);

    packet->type = LE16(packet->type);
    packet->payloadLength = LE16(packet->payloadLength);

    LC_ASSERT(ivSize <= (int)sizeof(iv));
    LC_ASSERT(ivSize == 12 || ivSize == 16);
    return PltEncryptMessage(encryptionCtx, ALGORITHM_AES_GCM, 0,
                             (unsigned char*)StreamConfig.remoteInputAesKey, sizeof(StreamConfig.remoteInputAesKey),
                             iv, ivSize,
                             (unsigned char*)(encPacket + 1), AES_GCM_TAG_LENGTH, // Write tag into the space after the encrypted header
                             (unsigned char*)packet, encryptedSize,
                             ((unsigned char*)(encPacket + 1)) + AES_GCM_TAG_LENGTH, &encryptedSize); // Write ciphertext after the GCM tag
}

// Caller must free() *packet on success!!!
static bool decryptControlMessageToV1(PNVCTL_ENCRYPTED_PACKET_HEADER encPacket, int encPacketLength, PNVCTL_ENET_PACKET_HEADER_V1* packet, int* packetLength) {
    unsigned char iv[16] = { 0 };
    int ivSize;

    *packet = NULL;

    // It must be an encrypted packet to begin with
    LC_ASSERT(encPacket->encryptedHeaderType == 0x0001);

    // Make sure the host isn't lying to us about the packet length
    int expectedEncLength = encPacket->length + sizeof(encPacket->encryptedHeaderType) + sizeof(encPacket->length);
    LC_ASSERT(encPacketLength == expectedEncLength);
    if (encPacketLength < expectedEncLength) {
        Limelog("Length exceeds packet boundary (needed %d, got %d)\n", expectedEncLength, encPacketLength);
        return false;
    }

    // Check length first so we don't underflow
    if (encPacket->length < sizeof(encPacket->seq) + AES_GCM_TAG_LENGTH + sizeof(NVCTL_ENET_PACKET_HEADER_V2)) {
        Limelog("Received runt packet (%d). Unable to decrypt.\n", encPacket->length);
        return false;
    }

    if (EncryptionFeaturesEnabled & SS_ENC_CONTROL_V2) {
        // Populate the IV in little endian byte order
        iv[3] = (unsigned char)(encPacket->seq >> 24);
        iv[2] = (unsigned char)(encPacket->seq >> 16);
        iv[1] = (unsigned char)(encPacket->seq >> 8);
        iv[0] = (unsigned char)(encPacket->seq >> 0);

        // Set high bytes to something unique to ensure no IV collisions
        iv[10] = (unsigned char)'H'; // Host originated
        iv[11] = (unsigned char)'C'; // Control stream

        // Use 12-byte IV which is ideal for AES-GCM
        ivSize = 12;
    }
    else {
        // This is a truncating cast, but it's what Nvidia does, so we have to mimic it.
        iv[0] = (unsigned char)encPacket->seq;

        // Nvidia's old style encryption uses a 16-byte IV
        ivSize = 16;
    }

    int plaintextLength = encPacket->length - sizeof(encPacket->seq) - AES_GCM_TAG_LENGTH;
    *packet = malloc(plaintextLength);
    if (*packet == NULL) {
        return false;
    }

    LC_ASSERT(ivSize <= (int)sizeof(iv));
    LC_ASSERT(ivSize == 12 || ivSize == 16);
    if (!PltDecryptMessage(decryptionCtx, ALGORITHM_AES_GCM, 0,
                           (unsigned char*)StreamConfig.remoteInputAesKey, sizeof(StreamConfig.remoteInputAesKey),
                           iv, ivSize,
                           (unsigned char*)(encPacket + 1), AES_GCM_TAG_LENGTH, // The tag is located right after the header
                           ((unsigned char*)(encPacket + 1)) + AES_GCM_TAG_LENGTH, plaintextLength, // The ciphertext is after the tag
                           (unsigned char*)*packet, &plaintextLength)) {
        free(*packet);
        return false;
    }

    // Now we do an in-place V2 to V1 header conversion, so our existing parsing code doesn't have to change.
    // All we need to do is eliminate the new length field in V2 by shifting everything by 2 bytes.
    memmove(((unsigned char*)*packet) + 2, ((unsigned char*)*packet) + 4, plaintextLength - 4);
    *packetLength = plaintextLength - 2;

    return true;
}

static void enetPacketFreeCb(ENetPacket* packet) {
    if (packet->userData) {
        // userData contains a bool that we will set when freed
        *(volatile bool*)packet->userData = true;
    }
}


// Must be called with enetMutex held
static bool isPacketSentWaitingForAck(ENetPacket* packet) {
    ENetOutgoingCommand* outgoingCommand = NULL;
    ENetListIterator currentCommand;

    // Look for our packet on the sent commands list
    for (currentCommand = enet_list_begin(&peer->sentReliableCommands);
         currentCommand != enet_list_end(&peer->sentReliableCommands);
         currentCommand = enet_list_next(currentCommand))
    {
        outgoingCommand = (ENetOutgoingCommand*)currentCommand;
        if (outgoingCommand->packet == packet) {
            return true;
        }
    }

    return false;
}

static bool sendMessageEnet(short ptype, short paylen, const void* payload, uint8_t channelId, uint32_t flags, bool moreData) {
    ENetPacket* enetPacket;
    int err;

    LC_ASSERT(AppVersionQuad[0] >= 5);

    // Only send reliable packets to GFE
    if (!IS_SUNSHINE()) {
        flags = ENET_PACKET_FLAG_RELIABLE;
    }

    if (encryptedControlStream) {
        PNVCTL_ENCRYPTED_PACKET_HEADER encPacket;
        PNVCTL_ENET_PACKET_HEADER_V2 packet;
        char tempBuffer[256];

        enetPacket = enet_packet_create(NULL,
                                        sizeof(*encPacket) + AES_GCM_TAG_LENGTH + sizeof(*packet) + paylen,
                                        flags);
        if (enetPacket == NULL) {
            return false;
        }

        // We (ab)use the enetMutex to protect currentEnetSequenceNumber and the cipherContext
        // used inside encryptControlMessage().
        PltLockMutex(&enetMutex);

        encPacket = (PNVCTL_ENCRYPTED_PACKET_HEADER)enetPacket->data;
        encPacket->encryptedHeaderType = 0x0001;
        encPacket->length = sizeof(encPacket->seq) + AES_GCM_TAG_LENGTH + sizeof(*packet) + paylen;
        encPacket->seq = currentEnetSequenceNumber++;

        // Construct the plaintext data for encryption
        LC_ASSERT(sizeof(*packet) + paylen < sizeof(tempBuffer));
        packet = (PNVCTL_ENET_PACKET_HEADER_V2)tempBuffer;
        packet->type = ptype;
        packet->payloadLength = paylen;
        memcpy(&packet[1], payload, paylen);

        // Encrypt the data into the final packet (and byteswap for BE machines)
        if (!encryptControlMessage(encPacket, packet)) {
            Limelog("Failed to encrypt control stream message\n");
            enet_packet_destroy(enetPacket);
            PltUnlockMutex(&enetMutex);
            return false;
        }

        // enetMutex still locked here
    }
    else {
        PNVCTL_ENET_PACKET_HEADER_V1 packet;
        enetPacket = enet_packet_create(NULL, sizeof(*packet) + paylen,
                                        flags);
        if (enetPacket == NULL) {
            return false;
        }

        packet = (PNVCTL_ENET_PACKET_HEADER_V1)enetPacket->data;
        packet->type = LE16(ptype);
        memcpy(&packet[1], payload, paylen);

        PltLockMutex(&enetMutex);
    }

    volatile bool packetFreed = false;

    // Set a callback to use to let us know if the packet has been freed.
    // Freeing can only happen when the packet is acked or send fails.
    enetPacket->userData = (void*)&packetFreed;
    enetPacket->freeCallback = enetPacketFreeCb;

    // Always use channel 0 for GFE and if the requested channel exceeds
    // the peer's supported channel count.
    if (!IS_SUNSHINE() || channelId >= peer->channelCount) {
        channelId = 0;
    }

    // Queue the packet to be sent
    err = enet_peer_send(peer, channelId, enetPacket);
    bool packetQueued = (err == 0);

    // If there is no more data coming soon, send the packet now
    if (!moreData && packetQueued) {
        err = enet_host_service(client, NULL, 0);

        // Wait until the packet is actually sent to provide backpressure on senders
        if (flags & ENET_PACKET_FLAG_RELIABLE) {
            // Don't wait longer than 10 milliseconds to avoid blocking callers for too long
            for (int i = 0; err >= 0 && i < 10; i++) {
                // Break on disconnected, acked/freed, or sent (pending ack).
                if (peer->state != ENET_PEER_STATE_CONNECTED || packetFreed || isPacketSentWaitingForAck(enetPacket)) {
                    break;
                }

                // Release the lock before sleeping to allow another thread to send/receive
                PltUnlockMutex(&enetMutex);
                PltSleepMs(1);
                PltLockMutex(&enetMutex);

                // Try to send the packet again
                err = enet_host_service(client, NULL, 0);
            }

            if (err >= 0 && peer->state == ENET_PEER_STATE_CONNECTED && !packetFreed && !isPacketSentWaitingForAck(enetPacket)) {
                Limelog("Control message took over 10 ms to send (net latency: %u ms | packet loss: %f%%)\n",
                        peer->roundTripTime, peer->packetLoss / (float)ENET_PEER_PACKET_LOSS_SCALE);
            }
        }
    }

    // Remove the free callback now that the packet was sent
    if (!packetFreed) {
        enetPacket->userData = NULL;
        enetPacket->freeCallback = NULL;
    }

    PltUnlockMutex(&enetMutex);

    if (err < 0) {
        Limelog("Failed to send ENet control packet\n");
        if (!packetQueued) {
            enet_packet_destroy(enetPacket);
        }
        return false;
    }

    return true;
}

static bool sendMessageTcp(short ptype, short paylen, const void* payload) {
    PNVCTL_TCP_PACKET_HEADER packet;
    SOCK_RET err;

    LC_ASSERT(AppVersionQuad[0] < 5);

    packet = malloc(sizeof(*packet) + paylen);
    if (packet == NULL) {
        return false;
    }

    packet->type = LE16(ptype);
    packet->payloadLength = LE16(paylen);
    memcpy(&packet[1], payload, paylen);

    err = send(ctlSock, (char*) packet, sizeof(*packet) + paylen, 0);
    free(packet);

    if (err != (SOCK_RET)(sizeof(*packet) + paylen)) {
        return false;
    }

    return true;
}

static bool sendMessageAndForget(short ptype, short paylen, const void* payload, uint8_t channelId, uint32_t flags, bool moreData) {
    bool ret;

    // Unlike regular sockets, ENet sockets aren't safe to invoke from multiple
    // threads at once. We have to synchronize them with a lock.
    if (AppVersionQuad[0] >= 5) {
        ret = sendMessageEnet(ptype, paylen, payload, channelId, flags, moreData);
    }
    else {
        ret = sendMessageTcp(ptype, paylen, payload);
    }

    return ret;
}

static bool sendMessageAndDiscardReply(short ptype, short paylen, const void* payload, uint8_t channelId, uint32_t flags, bool moreData) {
    if (AppVersionQuad[0] >= 5) {
        if (!sendMessageEnet(ptype, paylen, payload, channelId, flags, moreData)) {
            return false;
        }
    }
    else {
        PNVCTL_TCP_PACKET_HEADER reply;

        if (!sendMessageTcp(ptype, paylen, payload)) {
            return false;
        }

        // Discard the response
        reply = readNvctlPacketTcp();
        if (reply == NULL) {
            return false;
        }

        free(reply);
    }

    return true;
}

// This intercept function drops disconnect events to allow us to process
// pending receives first. It works around what appears to be a bug in ENet
// where pending disconnects can cause loss of unprocessed received data.
static int ignoreDisconnectIntercept(ENetHost* host, ENetEvent* event) {
    if (host->receivedDataLength == sizeof(ENetProtocolHeader) + sizeof(ENetProtocolDisconnect)) {
        ENetProtocolHeader* protoHeader = (ENetProtocolHeader*)host->receivedData;
        ENetProtocolDisconnect* disconnect = (ENetProtocolDisconnect*)(protoHeader + 1);

        if ((disconnect->header.command & ENET_PROTOCOL_COMMAND_MASK) == ENET_PROTOCOL_COMMAND_DISCONNECT) {
            Limelog("ENet disconnect event pending\n");
            disconnectPending = true;
            if (event) {
                event->type = ENET_EVENT_TYPE_NONE;
            }
            return 1;
        }
    }

    return 0;
}

static void asyncCallbackThreadFunc(void* context) {
    PQUEUED_ASYNC_CALLBACK queuedCb, nextCb;

    while (LbqWaitForQueueElement(&asyncCallbackQueue, (void**)&queuedCb) == LBQ_SUCCESS) {
        switch (queuedCb->typeIndex) {
        case IDX_RUMBLE_DATA:
            // Look for another rumble packet to batch with
            while (LbqPeekQueueElement(&asyncCallbackQueue, (void**)&nextCb) == LBQ_SUCCESS) {
                // Don't batch with the next packet if it is a different type or controller number
                if (nextCb->typeIndex != queuedCb->typeIndex ||
                        nextCb->data.rumble.controllerNumber != queuedCb->data.rumble.controllerNumber) {
                    break;
                }

                // This entry is batchable, so pop it off the queue
                if (LbqPollQueueElement(&asyncCallbackQueue, (void**)&nextCb) != LBQ_SUCCESS) {
                    break;
                }

                // Replace the old entry with the new one
                free(queuedCb);
                queuedCb = nextCb;
            }

            ListenerCallbacks.rumble(queuedCb->data.rumble.controllerNumber,
                                     queuedCb->data.rumble.lowFreqRumble,
                                     queuedCb->data.rumble.highFreqRumble);
            break;
        case IDX_RUMBLE_TRIGGER_DATA:
            // Look for another rumble triggers packet to batch with
            while (LbqPeekQueueElement(&asyncCallbackQueue, (void**)&nextCb) == LBQ_SUCCESS) {
                // Don't batch with the next packet if it is a different type or controller number
                if (nextCb->typeIndex != queuedCb->typeIndex ||
                        nextCb->data.rumbleTriggers.controllerNumber != queuedCb->data.rumbleTriggers.controllerNumber) {
                    break;
                }

                // This entry is batchable, so pop it off the queue
                if (LbqPollQueueElement(&asyncCallbackQueue, (void**)&nextCb) != LBQ_SUCCESS) {
                    break;
                }

                // Replace the old entry with the new one
                free(queuedCb);
                queuedCb = nextCb;
            }

            ListenerCallbacks.rumbleTriggers(queuedCb->data.rumbleTriggers.controllerNumber,
                                             queuedCb->data.rumbleTriggers.leftTriggerMotor,
                                             queuedCb->data.rumbleTriggers.rightTriggerMotor);
            break;
        case IDX_SET_RGB_LED:
            // Look for another controller LED packet to batch with
            while (LbqPeekQueueElement(&asyncCallbackQueue, (void**)&nextCb) == LBQ_SUCCESS) {
                // Don't batch with the next packet if it is a different type or controller number
                if (nextCb->typeIndex != queuedCb->typeIndex ||
                        nextCb->data.setControllerLed.controllerNumber != queuedCb->data.setControllerLed.controllerNumber) {
                    break;
                }

                // This entry is batchable, so pop it off the queue
                if (LbqPollQueueElement(&asyncCallbackQueue, (void**)&nextCb) != LBQ_SUCCESS) {
                    break;
                }

                // Replace the old entry with the new one
                free(queuedCb);
                queuedCb = nextCb;
            }

            ListenerCallbacks.setControllerLED(queuedCb->data.setControllerLed.controllerNumber,
                                               queuedCb->data.setControllerLed.r,
                                               queuedCb->data.setControllerLed.g,
                                               queuedCb->data.setControllerLed.b);
            break;
        case IDX_HDR_INFO:
            // HDR state is maintained globally, so we just invoke the client callback here.
            // These events are stateless, so we can consume all of them now.
            while (LbqPeekQueueElement(&asyncCallbackQueue, (void**)&nextCb) == LBQ_SUCCESS && nextCb->typeIndex == queuedCb->typeIndex) {
                // This entry is batchable, so pop it off the queue
                if (LbqPollQueueElement(&asyncCallbackQueue, (void**)&nextCb) != LBQ_SUCCESS) {
                    break;
                }

                // Replace the old entry with the new one
                free(queuedCb);
                queuedCb = nextCb;
            }

            ListenerCallbacks.setHdrMode(hdrEnabled);
            break;

        case IDX_SET_MOTION_EVENT:
            // These events are infrequent and cannot be batched
            ListenerCallbacks.setMotionEventState(queuedCb->data.setMotionEventState.controllerNumber,
                                                  queuedCb->data.setMotionEventState.motionType,
                                                  queuedCb->data.setMotionEventState.reportRateHz);
            break;
        default:
            // Unhandled packet type from queueAsyncCallback()
            LC_ASSERT(false);
            break;
        }

        free(queuedCb);
    }
}

static bool needsAsyncCallback(unsigned short packetType) {
    return packetType == packetTypes[IDX_RUMBLE_DATA] ||
           packetType == packetTypes[IDX_RUMBLE_TRIGGER_DATA] ||
           packetType == packetTypes[IDX_SET_MOTION_EVENT] ||
           packetType == packetTypes[IDX_SET_RGB_LED] ||
           packetType == packetTypes[IDX_HDR_INFO];
}

static void queueAsyncCallback(PNVCTL_ENET_PACKET_HEADER_V1 ctlHdr, int packetLength) {
    BYTE_BUFFER bb;
    PQUEUED_ASYNC_CALLBACK queuedCb;
    int err;

    LC_ASSERT(needsAsyncCallback(ctlHdr->type));

    queuedCb = malloc(sizeof(*queuedCb));
    if (!queuedCb) {
        return;
    }

    BbInitializeWrappedBuffer(&bb, (char*)ctlHdr, sizeof(*ctlHdr), packetLength - sizeof(*ctlHdr), BYTE_ORDER_LITTLE);

    if (ctlHdr->type == packetTypes[IDX_RUMBLE_DATA]) {
        BbAdvanceBuffer(&bb, 4);

        BbGet16(&bb, &queuedCb->data.rumble.controllerNumber);
        BbGet16(&bb, &queuedCb->data.rumble.lowFreqRumble);
        BbGet16(&bb, &queuedCb->data.rumble.highFreqRumble);

        queuedCb->typeIndex = IDX_RUMBLE_DATA;
    }
    else if (ctlHdr->type == packetTypes[IDX_RUMBLE_TRIGGER_DATA]) {
        BbGet16(&bb, &queuedCb->data.rumbleTriggers.controllerNumber);
        BbGet16(&bb, &queuedCb->data.rumbleTriggers.leftTriggerMotor);
        BbGet16(&bb, &queuedCb->data.rumbleTriggers.rightTriggerMotor);

        queuedCb->typeIndex = IDX_RUMBLE_TRIGGER_DATA;
    }
    else if (ctlHdr->type == packetTypes[IDX_SET_MOTION_EVENT]) {
        BbGet16(&bb, &queuedCb->data.setMotionEventState.controllerNumber);
        BbGet16(&bb, &queuedCb->data.setMotionEventState.reportRateHz);
        BbGet8(&bb, &queuedCb->data.setMotionEventState.motionType);

        queuedCb->typeIndex = IDX_SET_MOTION_EVENT;
    }
    else if (ctlHdr->type == packetTypes[IDX_SET_RGB_LED]) {
        BbGet16(&bb, &queuedCb->data.setControllerLed.controllerNumber);
        BbGet8(&bb, &queuedCb->data.setControllerLed.r);
        BbGet8(&bb, &queuedCb->data.setControllerLed.g);
        BbGet8(&bb, &queuedCb->data.setControllerLed.b);

        queuedCb->typeIndex = IDX_SET_RGB_LED;
    }
    else if (ctlHdr->type == packetTypes[IDX_HDR_INFO]) {
        queuedCb->typeIndex = IDX_HDR_INFO;
    }
    else {
        // Unhandled packet type from needsAsyncCallback()
        LC_ASSERT(false);
        free(queuedCb);
        return;
    }

    err = LbqOfferQueueItem(&asyncCallbackQueue, queuedCb, &queuedCb->entry);
    if (err != LBQ_SUCCESS) {
        Limelog("Failed to queue async callback: %d\n", err);
        free(queuedCb);
    }
}

static void controlReceiveThreadFunc(void* context) {
    int err;

    // This is only used for ENet
    if (AppVersionQuad[0] < 5) {
        return;
    }

    while (!PltIsThreadInterrupted(&controlReceiveThread)) {
        ENetEvent event;
        enet_uint32 waitTimeMs;

        PltLockMutex(&enetMutex);

        // Poll for new packets and process retransmissions
        err = serviceEnetHost(client, &event, 0);

        // Compute the next time we need to wake up to handle
        // the RTO timer or a ping.
        if (err == 0) {
            if (ENET_TIME_LESS(peer->nextTimeout, client->serviceTime)) {
                // This can happen when we have no unacked reliable messages
                waitTimeMs = 10;
            }
            else {
                // We add 1 ms just to ensure we're unlikely to undershoot the sleep() and have to
                // do a tiny sleep for another iteration before the timeout is ready to be serviced.
                waitTimeMs = ENET_TIME_DIFFERENCE(peer->nextTimeout, client->serviceTime) + 1;
            }

            // Ensure we don't sleep through a ping
            if (peer->lastReceiveTime && peer->lastSendTime) {
                enet_uint32 timeSinceLastRecv = ENET_TIME_DIFFERENCE(client->serviceTime, peer->lastReceiveTime);
                enet_uint32 timeSinceLastSend = ENET_TIME_DIFFERENCE(client->serviceTime, peer->lastSendTime);
                enet_uint32 timeSinceLastComm = MIN(timeSinceLastSend, timeSinceLastRecv);

                if (timeSinceLastComm >= peer->pingInterval) {
                    // Ping is due now for this peer
                    waitTimeMs = 0;
                } else {
                    waitTimeMs = MIN(waitTimeMs, peer->pingInterval - timeSinceLastComm);
                }
            }
            else {
                waitTimeMs = MIN(waitTimeMs, peer->pingInterval);
            }
        }

        PltUnlockMutex(&enetMutex);

        if (err == 0) {
            // Handle a pending disconnect after unsuccessfully polling
            // for new events to handle.
            if (disconnectPending) {
                PltLockMutex(&enetMutex);
                // Wait 100 ms for pending receives after a disconnect and
                // 1 second for the pending disconnect to be processed after
                // removing the intercept callback.
                err = serviceEnetHost(client, &event, client->intercept ? 100 : 1000);
                if (err == 0) {
                    if (client->intercept) {
                        // Now that no pending receive events remain, we can
                        // remove our intercept hook and allow the server's
                        // disconnect to be processed as expected. We will wait
                        // 1 second for this disconnect to be processed before
                        // we tear down the connection anyway.
                        client->intercept = NULL;
                        PltUnlockMutex(&enetMutex);
                        continue;
                    }
                    else {
                        // The 1 second timeout has expired with no disconnect event
                        // retransmission after the first notification. We can only
                        // assume the server died tragically, so go ahead and tear down.
                        PltUnlockMutex(&enetMutex);
                        Limelog("Disconnect event timeout expired\n");
                        ListenerCallbacks.connectionTerminated(-1);
                        return;
                    }
                }
                else {
                    PltUnlockMutex(&enetMutex);
                }
            }
            else {
                // No events ready - wait for readability or a local RTO timer to expire
                enet_uint32 condition = ENET_SOCKET_WAIT_RECEIVE;
                enet_socket_wait(client->socket, &condition, waitTimeMs);
                continue;
            }
        }

        if (err < 0) {
            // The error from serviceEnetHost() should be propagated via LastSocketError()
            LC_ASSERT(err == -1);

            err = LastSocketFail();
            Limelog("Control stream connection failed: %d\n", err);
            ListenerCallbacks.connectionTerminated(err);
            return;
        }

        if (event.type == ENET_EVENT_TYPE_RECEIVE) {
            PNVCTL_ENET_PACKET_HEADER_V1 ctlHdr;
            int packetLength;

            if (event.packet->dataLength < sizeof(*ctlHdr)) {
                Limelog("Discarding runt control packet: %d < %d\n", event.packet->dataLength, (int)sizeof(*ctlHdr));
                enet_packet_destroy(event.packet);
                continue;
            }

            ctlHdr = (PNVCTL_ENET_PACKET_HEADER_V1)event.packet->data;
            ctlHdr->type = LE16(ctlHdr->type);

            if (encryptedControlStream) {
                // V2 headers can be interpreted as V1 headers for the purpose of examining type,
                // so this check is safe.
                if (ctlHdr->type == 0x0001) {
                    PNVCTL_ENCRYPTED_PACKET_HEADER encHdr;

                    if (event.packet->dataLength < sizeof(NVCTL_ENCRYPTED_PACKET_HEADER)) {
                        Limelog("Discarding runt encrypted control packet: %d < %d\n", event.packet->dataLength, (int)sizeof(NVCTL_ENCRYPTED_PACKET_HEADER));
                        enet_packet_destroy(event.packet);
                        continue;
                    }

                    // encryptedHeaderType is already byteswapped by aliasing through ctlHdr above
                    encHdr = (PNVCTL_ENCRYPTED_PACKET_HEADER)event.packet->data;
                    encHdr->length = LE16(encHdr->length);
                    encHdr->seq = LE32(encHdr->seq);

                    ctlHdr = NULL;
                    packetLength = (int)event.packet->dataLength;
                    if (!decryptControlMessageToV1(encHdr, packetLength, &ctlHdr, &packetLength)) {
                        Limelog("Failed to decrypt control packet of size %d\n", event.packet->dataLength);
                        enet_packet_destroy(event.packet);
                        continue;
                    }

                    // We need to byteswap the unsealed header too
                    ctlHdr->type = LE16(ctlHdr->type);
                }
                else {
                    LC_ASSERT_VT(false);
                    Limelog("Discarding unencrypted packet on encrypted control stream: %04x\n", ctlHdr->type);
                    enet_packet_destroy(event.packet);
                    continue;
                }
            }
            else {
                // Take ownership of the packet data directly for the non-encrypted case
                packetLength = (int)event.packet->dataLength;
                event.packet->data = NULL;
            }

            // We're done with the packet struct
            enet_packet_destroy(event.packet);

            // All below codepaths must free ctlHdr!!!

            // Process HDR data immediately to update global HDR enabled state and HDR metadata.
            // The actual client callback will be invoked in the async callback thread.
            if (ctlHdr->type == packetTypes[IDX_HDR_INFO]) {
                BYTE_BUFFER bb;
                uint8_t enableByte;

                BbInitializeWrappedBuffer(&bb, (char*)ctlHdr, sizeof(*ctlHdr), packetLength - sizeof(*ctlHdr), BYTE_ORDER_LITTLE);

                BbGet8(&bb, &enableByte);
                if (IS_SUNSHINE()) {
                    // Zero the metadata buffer to properly handle older servers if we have to add new fields
                    memset(&hdrMetadata, 0, sizeof(hdrMetadata));

                    // Sunshine sends HDR metadata in this message too
                    for (int i = 0; i < 3; i++) {
                        BbGet16(&bb, &hdrMetadata.displayPrimaries[i].x);
                        BbGet16(&bb, &hdrMetadata.displayPrimaries[i].y);
                    }
                    BbGet16(&bb, &hdrMetadata.whitePoint.x);
                    BbGet16(&bb, &hdrMetadata.whitePoint.y);
                    BbGet16(&bb, &hdrMetadata.maxDisplayLuminance);
                    BbGet16(&bb, &hdrMetadata.minDisplayLuminance);
                    BbGet16(&bb, &hdrMetadata.maxContentLightLevel);
                    BbGet16(&bb, &hdrMetadata.maxFrameAverageLightLevel);
                    BbGet16(&bb, &hdrMetadata.maxFullFrameLuminance);
                }

                hdrEnabled = (enableByte != 0);
            }

            // Process client callbacks in a separate thread
            if (needsAsyncCallback(ctlHdr->type)) {
                queueAsyncCallback(ctlHdr, packetLength);
            }
            else if (ctlHdr->type == packetTypes[IDX_TERMINATION]) {
                BYTE_BUFFER bb;


                uint32_t terminationErrorCode;

                if (packetLength >= 6) {
                    // This is the extended termination message which contains a full HRESULT
                    BbInitializeWrappedBuffer(&bb, (char*)ctlHdr, sizeof(*ctlHdr), packetLength - sizeof(*ctlHdr), BYTE_ORDER_BIG);
                    BbGet32(&bb, &terminationErrorCode);

                    Limelog("Server notified termination reason: 0x%08x\n", terminationErrorCode);

                    // Normalize the termination error codes for specific values we recognize
                    switch (terminationErrorCode) {
                    case 0x800e9403: // NVST_DISCONN_SERVER_VIDEO_ENCODER_CONVERT_INPUT_FRAME_FAILED
                        terminationErrorCode = ML_ERROR_FRAME_CONVERSION;
                        break;
                    case 0x800e9302: // NVST_DISCONN_SERVER_VFP_PROTECTED_CONTENT
                        terminationErrorCode = ML_ERROR_PROTECTED_CONTENT;
                        break;
                    case 0x80030023: // NVST_DISCONN_SERVER_TERMINATED_CLOSED
                        if (lastSeenFrame != 0) {
                            // Pass error code 0 to notify the client that this was not an error
                            terminationErrorCode = ML_ERROR_GRACEFUL_TERMINATION;
                        }
                        else {
                            // We never saw a frame, so this is probably an error that caused
                            // NvStreamer to terminate prior to sending any frames.
                            terminationErrorCode = ML_ERROR_UNEXPECTED_EARLY_TERMINATION;
                        }
                        break;
                    default:
                        break;
                    }
                }
                else {
                    uint16_t terminationReason;

                    // This is the short termination message
                    BbInitializeWrappedBuffer(&bb, (char*)ctlHdr, sizeof(*ctlHdr), packetLength - sizeof(*ctlHdr), BYTE_ORDER_LITTLE);
                    BbGet16(&bb, &terminationReason);

                    Limelog("Server notified termination reason: 0x%04x\n", terminationReason);

                    // SERVER_TERMINATED_INTENDED
                    if (terminationReason == 0x0100) {
                        if (lastSeenFrame != 0) {
                            // Pass error code 0 to notify the client that this was not an error
                            terminationErrorCode = ML_ERROR_GRACEFUL_TERMINATION;
                        }
                        else {
                            // We never saw a frame, so this is probably an error that caused
                            // NvStreamer to terminate prior to sending any frames.
                            terminationErrorCode = ML_ERROR_UNEXPECTED_EARLY_TERMINATION;
                        }
                    }
                    else {
                        // Otherwise pass the reason unmodified
                        terminationErrorCode = terminationReason;
                    }
                }

                // We used to wait for a ENET_EVENT_TYPE_DISCONNECT event, but since
                // GFE 3.20.3.63 we don't get one for 10 seconds after we first get
                // this termination message. The termination message should be reliable
                // enough to end the stream now, rather than waiting for an explicit
                // disconnect. The server will also not acknowledge our disconnect
                // message once it sends this message, so we mark the peer as fully
                // disconnected now to avoid delays waiting for an ack that will
                // never arrive.
                PltLockMutex(&enetMutex);
                enet_peer_disconnect_now(peer, 0);
                PltUnlockMutex(&enetMutex);
                ListenerCallbacks.connectionTerminated((int)terminationErrorCode);
                free(ctlHdr);
                return;
            }

            free(ctlHdr);
        }
        else if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
            Limelog("Control stream received unexpected disconnect event\n");
            ListenerCallbacks.connectionTerminated(-1);
            return;
        }
    }
}

static void lossStatsThreadFunc(void* context) {
    BYTE_BUFFER byteBuffer;

    if (usePeriodicPing) {
        char periodicPingPayload[8];

        BbInitializeWrappedBuffer(&byteBuffer, periodicPingPayload, 0, sizeof(periodicPingPayload), BYTE_ORDER_LITTLE);
        BbPut16(&byteBuffer, 4); // Length of payload
        BbPut32(&byteBuffer, 0); // Timestamp?

        while (!PltIsThreadInterrupted(&lossStatsThread)) {
            // For Sunshine servers, send the more detailed per-frame FEC messages
            if (IS_SUNSHINE()) {
                PQUEUED_FRAME_FEC_STATUS queuedFrameStatus;

                // Sunshine should always use ENet for control messages
                LC_ASSERT(peer != NULL);

                while (LbqPollQueueElement(&frameFecStatusQueue, (void**)&queuedFrameStatus) == LBQ_SUCCESS) {
                    // Send as an unreliable packet, since it's not a critical message
                    if (!sendMessageEnet(SS_FRAME_FEC_PTYPE,
                                         sizeof(queuedFrameStatus->fecStatus),
                                         &queuedFrameStatus->fecStatus,
                                         CTRL_CHANNEL_GENERIC,
                                         ENET_PACKET_FLAG_UNSEQUENCED,
                                         LbqGetItemCount(&frameFecStatusQueue) > 0)) {
                        Limelog("Loss Stats: Sending frame FEC status message failed: %d\n", (int)LastSocketError());
                        ListenerCallbacks.connectionTerminated(LastSocketFail());
                        free(queuedFrameStatus);
                        return;
                    }

                    free(queuedFrameStatus);
                }
            }

            // Send the message (and don't expect a response)
            //
            // NB: We send this periodic message as reliable to ensure the RTT is recomputed
            // regularly. This only happens when an ACK is received to a reliable packet.
            // Since the other traffic on this channel is unsequenced, it doesn't really
            // cause any negative HOL blocking side-effects.
            if (!sendMessageAndForget(0x0200,
                                      sizeof(periodicPingPayload),
                                      periodicPingPayload,
                                      CTRL_CHANNEL_GENERIC,
                                      ENET_PACKET_FLAG_RELIABLE,
                                      false)) {
                Limelog("Loss Stats: Transaction failed: %d\n", (int)LastSocketError());
                ListenerCallbacks.connectionTerminated(LastSocketFail());
                return;
            }

            // Wait a bit
            PltSleepMsInterruptible(&lossStatsThread, PERIODIC_PING_INTERVAL_MS);
        }
    }
    else {
        char* lossStatsPayload;

        // Sunshine should use the newer codepath above
        LC_ASSERT(!IS_SUNSHINE());

        lossStatsPayload = malloc(payloadLengths[IDX_LOSS_STATS]);
        if (lossStatsPayload == NULL) {
            Limelog("Loss Stats: malloc() failed\n");
            ListenerCallbacks.connectionTerminated(-1);
            return;
        }

        while (!PltIsThreadInterrupted(&lossStatsThread)) {
            // Construct the payload
            BbInitializeWrappedBuffer(&byteBuffer, lossStatsPayload, 0, payloadLengths[IDX_LOSS_STATS], BYTE_ORDER_LITTLE);
            BbPut32(&byteBuffer, 0);
            BbPut32(&byteBuffer, LOSS_REPORT_INTERVAL_MS);
            BbPut32(&byteBuffer, 1000);
            BbPut64(&byteBuffer, lastGoodFrame);
            BbPut32(&byteBuffer, 0);
            BbPut32(&byteBuffer, 0);
            BbPut32(&byteBuffer, 0x14);

            // Send the message (and don't expect a response)
            if (!sendMessageAndForget(packetTypes[IDX_LOSS_STATS],
                                      payloadLengths[IDX_LOSS_STATS],
                                      lossStatsPayload,
                                      CTRL_CHANNEL_GENERIC,
                                      0,
                                      false)) {
                free(lossStatsPayload);
                Limelog("Loss Stats: Transaction failed: %d\n", (int)LastSocketError());
                ListenerCallbacks.connectionTerminated(LastSocketFail());
                return;
            }

            // Wait a bit
            PltSleepMsInterruptible(&lossStatsThread, LOSS_REPORT_INTERVAL_MS);
        }

        free(lossStatsPayload);
    }
}

static void requestIdrFrame(void) {
    // If this server does not have a known IDR frame request
    // message, we'll accomplish the same thing by creating a
    // reference frame invalidation request.
    if (!supportsIdrFrameRequest) {
        int64_t payload[3];

        // Form the payload
        if (lastSeenFrame < 0x20) {
            payload[0] = 0;
            payload[1] = LE64(lastSeenFrame);
        }
        else {
            payload[0] = LE64(lastSeenFrame - 0x20);
            payload[1] = LE64(lastSeenFrame);
        }

        payload[2] = 0;

        // Send the reference frame invalidation request and read the response
        if (!sendMessageAndDiscardReply(packetTypes[IDX_INVALIDATE_REF_FRAMES],
                                        sizeof(payload),
                                        payload,
                                        CTRL_CHANNEL_URGENT,
                                        ENET_PACKET_FLAG_RELIABLE,
                                        false)) {
            Limelog("Request IDR Frame: Transaction failed: %d\n", (int)LastSocketError());
            ListenerCallbacks.connectionTerminated(LastSocketFail());
            return;
        }
    }
    else {
        // Send IDR frame request and read the response
        if (!sendMessageAndDiscardReply(packetTypes[IDX_REQUEST_IDR_FRAME],
                                        payloadLengths[IDX_REQUEST_IDR_FRAME],
                                        preconstructedPayloads[IDX_REQUEST_IDR_FRAME],
                                        CTRL_CHANNEL_URGENT,
                                        ENET_PACKET_FLAG_RELIABLE,
                                        false)) {
            Limelog("Request IDR Frame: Transaction failed: %d\n", (int)LastSocketError());
            ListenerCallbacks.connectionTerminated(LastSocketFail());
            return;
        }
    }

    Limelog("IDR frame request sent\n");
}

static void requestInvalidateReferenceFrames(uint32_t startFrame, uint32_t endFrame) {
    int64_t payload[3];

    LC_ASSERT(startFrame <= endFrame);
    LC_ASSERT(isReferenceFrameInvalidationEnabled());

    payload[0] = LE64(startFrame);
    payload[1] = LE64(endFrame);
    payload[2] = 0;

    // Send the reference frame invalidation request and read the response
    if (!sendMessageAndDiscardReply(packetTypes[IDX_INVALIDATE_REF_FRAMES],
                                    sizeof(payload),
                                    payload, CTRL_CHANNEL_URGENT,
                                    ENET_PACKET_FLAG_RELIABLE,
                                    false)) {
        Limelog("Request Invaldiate Reference Frames: Transaction failed: %d\n", (int)LastSocketError());
        ListenerCallbacks.connectionTerminated(LastSocketFail());
        return;
    }

    Limelog("Invalidate reference frame request sent (%d to %d)\n", startFrame, endFrame);
}

static void invalidateRefFramesFunc(void* context) {
    LC_ASSERT(isReferenceFrameInvalidationEnabled());

    while (!PltIsThreadInterrupted(&invalidateRefFramesThread)) {
        PQUEUED_FRAME_INVALIDATION_TUPLE qfit;
        uint32_t startFrame;
        uint32_t endFrame;

        // Wait for a reference frame invalidation request or a request to shutdown
        if (LbqWaitForQueueElement(&invalidReferenceFrameTuples, (void**)&qfit) != LBQ_SUCCESS) {
            // Bail if we're stopping
            return;
        }

        startFrame = qfit->startFrame;
        endFrame = qfit->endFrame;

        // Aggregate all lost frames into one range
        do {
            LC_ASSERT(qfit->endFrame >= endFrame);
            endFrame = qfit->endFrame;
            free(qfit);
        } while (LbqPollQueueElement(&invalidReferenceFrameTuples, (void**)&qfit) == LBQ_SUCCESS);

        // Send the reference frame invalidation request
        requestInvalidateReferenceFrames(startFrame, endFrame);
    }
}

static void requestIdrFrameFunc(void* context) {
    while (!PltIsThreadInterrupted(&requestIdrFrameThread)) {
        PltWaitForEvent(&idrFrameRequiredEvent);
        PltClearEvent(&idrFrameRequiredEvent);

        if (stopping) {
            // Bail if we're stopping
            return;
        }

        // Any pending reference frame invalidation requests are now redundant
        freeBasicLbqList(LbqFlushQueueItems(&invalidReferenceFrameTuples));

        // Request the IDR frame
        requestIdrFrame();
    }
}

// Stops the control stream
int stopControlStream(void) {
    stopping = true;
    LbqSignalQueueShutdown(&invalidReferenceFrameTuples);
    LbqSignalQueueShutdown(&frameFecStatusQueue);
    LbqSignalQueueDrain(&asyncCallbackQueue);
    PltSetEvent(&idrFrameRequiredEvent);

    // This must be set to stop in a timely manner
    LC_ASSERT(ConnectionInterrupted);

    if (ctlSock != INVALID_SOCKET) {
        shutdownTcpSocket(ctlSock);
    }

    PltInterruptThread(&lossStatsThread);
    PltInterruptThread(&requestIdrFrameThread);
    PltInterruptThread(&controlReceiveThread);
    PltInterruptThread(&asyncCallbackThread);

    PltJoinThread(&lossStatsThread);
    PltJoinThread(&requestIdrFrameThread);
    PltJoinThread(&controlReceiveThread);
    PltJoinThread(&asyncCallbackThread);

    // We will only have an RFI thread if RFI is enabled
    if (isReferenceFrameInvalidationEnabled()) {
        PltInterruptThread(&invalidateRefFramesThread);
        PltJoinThread(&invalidateRefFramesThread);
    }

    if (peer != NULL) {
        // Gracefully disconnect to ensure the remote host receives all of our final
        // outbound traffic, including any key up events that might be sent.
        gracefullyDisconnectEnetPeer(client, peer, CONTROL_STREAM_LINGER_TIMEOUT_SEC * 1000);
        peer = NULL;
    }
    if (client != NULL) {
        enet_host_destroy(client);
        client = NULL;
    }

    if (ctlSock != INVALID_SOCKET) {
        closeSocket(ctlSock);
        ctlSock = INVALID_SOCKET;
    }

    return 0;
}

// Called by the input stream to send a packet for Gen 5+ servers
int sendInputPacketOnControlStream(unsigned char* data, int length, uint8_t channelId, uint32_t flags, bool moreData) {
    LC_ASSERT(AppVersionQuad[0] >= 5);

    // Send the input data (no reply expected)
    if (sendMessageAndForget(packetTypes[IDX_INPUT_DATA], length, data, channelId, flags, moreData) == 0) {
        return -1;
    }

    return 0;
}

// Called by the input stream to flush queued packets before a batching wait
void flushInputOnControlStream(void) {
    if (AppVersionQuad[0] >= 5) {
        PltLockMutex(&enetMutex);
        enet_host_flush(client);
        PltUnlockMutex(&enetMutex);
    }
}

bool isControlDataInTransit(void) {
    bool ret = false;

    PltLockMutex(&enetMutex);
    if (peer != NULL && peer->state == ENET_PEER_STATE_CONNECTED) {
        if (peer->reliableDataInTransit != 0) {
            ret = true;
        }
    }
    PltUnlockMutex(&enetMutex);

    return ret;
}

bool LiGetEstimatedRttInfo(uint32_t* estimatedRtt, uint32_t* estimatedRttVariance) {
    bool ret = false;

    PltLockMutex(&enetMutex);
    if (peer != NULL && peer->state == ENET_PEER_STATE_CONNECTED) {
        if (estimatedRtt != NULL) {
            *estimatedRtt = peer->roundTripTime;
        }

        if (estimatedRttVariance != NULL) {
            *estimatedRttVariance = peer->roundTripTimeVariance;
        }

        ret = true;
    }
    PltUnlockMutex(&enetMutex);

    return ret;
}

// Starts the control stream
int startControlStream(void) {
    int err;

    if (AppVersionQuad[0] >= 5) {
        ENetAddress remoteAddress, localAddress;
        ENetEvent event;

        LC_ASSERT(ControlPortNumber != 0);

        enet_address_set_address(&localAddress, (struct sockaddr *)&LocalAddr, AddrLen);
#ifdef __3DS__
        // binding to wildcard port is broken on the 3DS, so we need to define a port manually
        enet_address_set_port(&localAddress, htons(n3ds_udp_port++));
#else
        enet_address_set_port(&localAddress, 0); // Wildcard port
#endif

        enet_address_set_address(&remoteAddress, (struct sockaddr *)&RemoteAddr, AddrLen);
        enet_address_set_port(&remoteAddress, ControlPortNumber);

        // Create a client
        client = enet_host_create(RemoteAddr.ss_family,
                                  LocalAddr.ss_family != 0 ? &localAddress : NULL,
                                  1, CTRL_CHANNEL_COUNT, 0, 0);
        if (client == NULL) {
            stopping = true;
            return -1;
        }

        client->intercept = ignoreDisconnectIntercept;

        // Enable high priority QoS marking on control stream traffic
        //
        // NB: It is important to do this before connecting because there's logic in the connect
        // retransmission code to detect QoS-intolerant routes and disable QoS marking for those.
        enet_socket_set_option (client->socket, ENET_SOCKOPT_QOS, 1);

        // Connect to the host
        peer = enet_host_connect(client, &remoteAddress, CTRL_CHANNEL_COUNT, ControlConnectData);
        if (peer == NULL) {
            stopping = true;
            enet_host_destroy(client);
            client = NULL;
            return -1;
        }

        // Wait for the connect to complete
        err = serviceEnetHost(client, &event, CONTROL_STREAM_TIMEOUT_SEC * 1000);
        if (err <= 0 || event.type != ENET_EVENT_TYPE_CONNECT) {
            if (err < 0) {
                Limelog("Failed to establish ENet connection on UDP port %u: error %d\n", ControlPortNumber, LastSocketFail());
            }
            else if (err == 0) {
                Limelog("Failed to establish ENet connection on UDP port %u: timed out\n", ControlPortNumber);
            }
            else {
                Limelog("Failed to establish ENet connection on UDP port %u: unexpected event %d (error: %d)\n", ControlPortNumber, (int)event.type, LastSocketError());
            }

            stopping = true;
            enet_peer_reset(peer);
            peer = NULL;
            enet_host_destroy(client);
            client = NULL;

            if (err == 0) {
                return ETIMEDOUT;
            }
            else if (err > 0 && event.type != ENET_EVENT_TYPE_CONNECT && LastSocketError() == 0) {
                // If we got an unexpected event type and have no other error to return, return the event type
                LC_ASSERT(event.type != ENET_EVENT_TYPE_NONE);
                return event.type != ENET_EVENT_TYPE_NONE ? (int)event.type : LastSocketFail();
            }
            else {
                return LastSocketFail();
            }
        }

        // Ensure the connect verify ACK is sent immediately
        enet_host_flush(client);

#ifdef __3DS__
        // Set the peer timeout to 1 minute and limit backoff to 2x RTT
        // The 3DS can take a bit longer to set up when starting fresh
        enet_peer_timeout(peer, 2, 60000, 60000);
#else
        // Set the peer timeout to 10 seconds and limit backoff to 2x RTT
        enet_peer_timeout(peer, 2, 10000, 10000);
#endif
    }
    else {
        // NB: Do NOT use ControlPortNumber here. 47995 is correct for these old versions.
        LC_ASSERT(ControlPortNumber == 0);
        ctlSock = connectTcpSocket(&RemoteAddr, AddrLen,
            47995, CONTROL_STREAM_TIMEOUT_SEC);
        if (ctlSock == INVALID_SOCKET) {
            stopping = true;
            return LastSocketFail();
        }

        enableNoDelay(ctlSock);
    }

    err = PltCreateThread("ControlRecv", controlReceiveThreadFunc, NULL, &controlReceiveThread);
    if (err != 0) {
        stopping = true;
        if (ctlSock != INVALID_SOCKET) {
            closeSocket(ctlSock);
            ctlSock = INVALID_SOCKET;
        }
        else {
            enet_peer_disconnect_now(peer, 0);
            peer = NULL;
            enet_host_destroy(client);
            client = NULL;
        }
        return err;
    }

    // Send START A
    if (!sendMessageAndDiscardReply(packetTypes[IDX_START_A],
                                    payloadLengths[IDX_START_A],
                                    preconstructedPayloads[IDX_START_A],
                                    CTRL_CHANNEL_GENERIC,
                                    ENET_PACKET_FLAG_RELIABLE,
                                    false)) {
        Limelog("Start A failed: %d\n", (int)LastSocketError());
        err = LastSocketFail();
        stopping = true;

        if (ctlSock != INVALID_SOCKET) {
            shutdownTcpSocket(ctlSock);
        }
        else {
            ConnectionInterrupted = true;
        }

        PltInterruptThread(&controlReceiveThread);
        PltJoinThread(&controlReceiveThread);

        if (ctlSock != INVALID_SOCKET) {
            closeSocket(ctlSock);
            ctlSock = INVALID_SOCKET;
        }
        else {
            enet_peer_disconnect_now(peer, 0);
            peer = NULL;
            enet_host_destroy(client);
            client = NULL;
        }
        return err;
    }

    // Send START B
    if (!sendMessageAndDiscardReply(packetTypes[IDX_START_B],
                                    payloadLengths[IDX_START_B],
                                    preconstructedPayloads[IDX_START_B],
                                    CTRL_CHANNEL_GENERIC,
                                    ENET_PACKET_FLAG_RELIABLE,
                                    false)) {
        Limelog("Start B failed: %d\n", (int)LastSocketError());
        err = LastSocketFail();
        stopping = true;

        if (ctlSock != INVALID_SOCKET) {
            shutdownTcpSocket(ctlSock);
        }
        else {
            ConnectionInterrupted = true;
        }

        PltInterruptThread(&controlReceiveThread);
        PltJoinThread(&controlReceiveThread);

        if (ctlSock != INVALID_SOCKET) {
            closeSocket(ctlSock);
            ctlSock = INVALID_SOCKET;
        }
        else {
            enet_peer_disconnect_now(peer, 0);
            peer = NULL;
            enet_host_destroy(client);
            client = NULL;
        }
        return err;
    }

    err = PltCreateThread("LossStats", lossStatsThreadFunc, NULL, &lossStatsThread);
    if (err != 0) {
        stopping = true;

        if (ctlSock != INVALID_SOCKET) {
            shutdownTcpSocket(ctlSock);
        }
        else {
            ConnectionInterrupted = true;
        }

        PltInterruptThread(&controlReceiveThread);
        PltJoinThread(&controlReceiveThread);

        if (ctlSock != INVALID_SOCKET) {
            closeSocket(ctlSock);
            ctlSock = INVALID_SOCKET;
        }
        else {
            enet_peer_disconnect_now(peer, 0);
            peer = NULL;
            enet_host_destroy(client);
            client = NULL;
        }
        return err;
    }

    err = PltCreateThread("ReqIdrFrame", requestIdrFrameFunc, NULL, &requestIdrFrameThread);
    if (err != 0) {
        stopping = true;

        if (ctlSock != INVALID_SOCKET) {
            shutdownTcpSocket(ctlSock);
        }
        else {
            ConnectionInterrupted = true;
        }

        PltInterruptThread(&lossStatsThread);
        PltJoinThread(&lossStatsThread);

        PltInterruptThread(&controlReceiveThread);
        PltJoinThread(&controlReceiveThread);

        if (ctlSock != INVALID_SOCKET) {
            closeSocket(ctlSock);
            ctlSock = INVALID_SOCKET;
        }
        else {
            enet_peer_disconnect_now(peer, 0);
            peer = NULL;
            enet_host_destroy(client);
            client = NULL;
        }

        return err;
    }

    err = PltCreateThread("CtrlAsyncCb", asyncCallbackThreadFunc, NULL, &asyncCallbackThread);
    if (err != 0) {
        stopping = true;
        PltSetEvent(&idrFrameRequiredEvent);

        if (ctlSock != INVALID_SOCKET) {
            shutdownTcpSocket(ctlSock);
        }
        else {
            ConnectionInterrupted = true;
        }

        PltInterruptThread(&lossStatsThread);
        PltJoinThread(&lossStatsThread);

        PltInterruptThread(&controlReceiveThread);
        PltJoinThread(&controlReceiveThread);

        PltInterruptThread(&requestIdrFrameThread);
        PltJoinThread(&requestIdrFrameThread);

        if (ctlSock != INVALID_SOCKET) {
            closeSocket(ctlSock);
            ctlSock = INVALID_SOCKET;
        }
        else {
            enet_peer_disconnect_now(peer, 0);
            peer = NULL;
            enet_host_destroy(client);
            client = NULL;
        }

        return err;
    }

    // Only create the reference frame invalidation thread if RFI is enabled
    if (isReferenceFrameInvalidationEnabled()) {
        err = PltCreateThread("InvRefFrames", invalidateRefFramesFunc, NULL, &invalidateRefFramesThread);
        if (err != 0) {
            stopping = true;
            PltSetEvent(&idrFrameRequiredEvent);
            LbqSignalQueueShutdown(&asyncCallbackQueue);

            if (ctlSock != INVALID_SOCKET) {
                shutdownTcpSocket(ctlSock);
            }
            else {
                ConnectionInterrupted = true;
            }

            PltInterruptThread(&lossStatsThread);
            PltJoinThread(&lossStatsThread);

            PltInterruptThread(&controlReceiveThread);
            PltJoinThread(&controlReceiveThread);

            PltInterruptThread(&requestIdrFrameThread);
            PltJoinThread(&requestIdrFrameThread);

            PltInterruptThread(&asyncCallbackThread);
            PltJoinThread(&asyncCallbackThread);

            if (ctlSock != INVALID_SOCKET) {
                closeSocket(ctlSock);
                ctlSock = INVALID_SOCKET;
            }
            else {
                enet_peer_disconnect_now(peer, 0);
                peer = NULL;
                enet_host_destroy(client);
                client = NULL;
            }

            return err;
        }
    }

    return 0;
}

bool LiGetCurrentHostDisplayHdrMode(void) {
    return hdrEnabled;
}

bool LiGetHdrMetadata(PSS_HDR_METADATA metadata) {
    if (!IS_SUNSHINE() || !hdrEnabled) {
        return false;
    }

    *metadata = hdrMetadata;
    return true;
}
