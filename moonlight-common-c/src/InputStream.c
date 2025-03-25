#include "Limelight-internal.h"

static SOCKET inputSock = INVALID_SOCKET;
static unsigned char currentAesIv[16];
static bool initialized;
static bool encryptedControlStream;
static bool needsBatchedScroll;
static int batchedScrollDelta;
static PPLT_CRYPTO_CONTEXT cryptoContext;

static LINKED_BLOCKING_QUEUE packetQueue;
static LINKED_BLOCKING_QUEUE packetHolderFreeList;
static PLT_THREAD inputSendThread;

static float absCurrentPosX;
static float absCurrentPosY;

// Limited by number of bits in activeGamepadMask
#define MAX_GAMEPADS 16

// Accelerometer and gyro
#define MAX_MOTION_EVENTS 2

static uint8_t currentPenButtonState;

static PLT_MUTEX batchedInputMutex;
static struct {
    float x, y, z;
    bool dirty; // Update ready to send (queued packet holder in packetQueue)
} currentGamepadSensorState[MAX_GAMEPADS][MAX_MOTION_EVENTS];
static struct {
    int deltaX, deltaY;
    bool dirty; // Update ready to send (queued packet holder in packetQueue)
} currentRelativeMouseState;
static struct {
    int x, y;
    int width, height;
    bool dirty; // Update ready to send (queued packet holder in packetQueue)
} currentAbsoluteMouseState;

#define CLAMP(val, min, max) (((val) < (min)) ? (min) : (((val) > (max)) ? (max) : (val)))

#define MAX_INPUT_PACKET_SIZE 128
#define INPUT_STREAM_TIMEOUT_SEC 10

#define MAX_QUEUED_INPUT_PACKETS 150

#define PAYLOAD_SIZE(x) BE32((x)->packet.header.size)
#define PACKET_SIZE(x) (PAYLOAD_SIZE(x) + sizeof(uint32_t))

// Matches Win32 WHEEL_DELTA definition
#define LI_WHEEL_DELTA 120

// If we try to send more than one gamepad or mouse motion event
// per millisecond, we'll wait a little bit to try to batch with
// the next one. This batching wait paradoxically _decreases_
// effective input latency by avoiding packet queuing in ENet.
#define CONTROLLER_BATCHING_INTERVAL_MS 1
#define MOUSE_BATCHING_INTERVAL_MS 1
#define PEN_BATCHING_INTERVAL_MS 1

// Don't batch up/down/cancel events
#define TOUCH_EVENT_IS_BATCHABLE(x) ((x) == LI_TOUCH_EVENT_HOVER || (x) == LI_TOUCH_EVENT_MOVE)

// Contains input stream packets
typedef struct _PACKET_HOLDER {
    LINKED_BLOCKING_QUEUE_ENTRY entry;
    uint32_t enetPacketFlags;
    uint8_t channelId;

    // The union must be the last member since we abuse the NV_UNICODE_PACKET
    // text field to store variable length data which gets split before being
    // sent to the host.
    union {
        NV_INPUT_HEADER header;
        NV_KEYBOARD_PACKET keyboard;
        NV_REL_MOUSE_MOVE_PACKET mouseMoveRel;
        NV_ABS_MOUSE_MOVE_PACKET mouseMoveAbs;
        NV_MOUSE_BUTTON_PACKET mouseButton;
        NV_CONTROLLER_PACKET controller;
        NV_MULTI_CONTROLLER_PACKET multiController;
        NV_SCROLL_PACKET scroll;
        SS_HSCROLL_PACKET hscroll;
        NV_HAPTICS_PACKET haptics;
        SS_TOUCH_PACKET touch;
        SS_PEN_PACKET pen;
        SS_CONTROLLER_ARRIVAL_PACKET controllerArrival;
        SS_CONTROLLER_TOUCH_PACKET controllerTouch;
        SS_CONTROLLER_MOTION_PACKET controllerMotion;
        SS_CONTROLLER_BATTERY_PACKET controllerBattery;
        NV_UNICODE_PACKET unicode;
    } packet;
} PACKET_HOLDER, *PPACKET_HOLDER;

// Initializes the input stream
int initializeInputStream(void) {
    memcpy(currentAesIv, StreamConfig.remoteInputAesIv, sizeof(currentAesIv));
    
    // Set a high maximum queue size limit to ensure input isn't dropped
    // while the input send thread is blocked for short periods.
    LbqInitializeLinkedBlockingQueue(&packetQueue, MAX_QUEUED_INPUT_PACKETS);
    LbqInitializeLinkedBlockingQueue(&packetHolderFreeList, MAX_QUEUED_INPUT_PACKETS);

    cryptoContext = PltCreateCryptoContext();
    encryptedControlStream = APP_VERSION_AT_LEAST(7, 1, 431);

    // FIXME: Unsure if this is exactly right, but it's probably good enough.
    //
    // GFE 3.13.1.30 is not using NVVHCI for mouse/keyboard (and is confirmed unaffected)
    // GFE 3.15.0.164 seems to be the first release using NVVHCI for mouse/keyboard
    //
    // Sunshine also uses SendInput() so it's not affected either.
    needsBatchedScroll = APP_VERSION_AT_LEAST(7, 1, 409) && !IS_SUNSHINE();
    batchedScrollDelta = 0;

    currentPenButtonState = 0;

    // Start with the virtual mouse centered
    absCurrentPosX = absCurrentPosY = 0.5f;

    memset(currentGamepadSensorState, 0, sizeof(currentGamepadSensorState));
    memset(&currentRelativeMouseState, 0, sizeof(currentRelativeMouseState));
    memset(&currentAbsoluteMouseState, 0, sizeof(currentAbsoluteMouseState));
    PltCreateMutex(&batchedInputMutex);

    return 0;
}

// Destroys and cleans up the input stream
void destroyInputStream(void) {
    PLINKED_BLOCKING_QUEUE_ENTRY entry, nextEntry;
    
    PltDestroyCryptoContext(cryptoContext);

    entry = LbqDestroyLinkedBlockingQueue(&packetQueue);

    while (entry != NULL) {
        nextEntry = entry->flink;

        // The entry is stored in the data buffer
        free(entry->data);

        entry = nextEntry;
    }

    entry = LbqDestroyLinkedBlockingQueue(&packetHolderFreeList);

    while (entry != NULL) {
        nextEntry = entry->flink;

        // The entry is stored in the data buffer
        free(entry->data);

        entry = nextEntry;
    }

    PltDeleteMutex(&batchedInputMutex);
}

static int encryptData(unsigned char* plaintext, int plaintextLen,
                       unsigned char* ciphertext, int* ciphertextLen) {
    // Starting in Gen 7, AES GCM is used for encryption
    if (AppVersionQuad[0] >= 7) {
        if (!PltEncryptMessage(cryptoContext, ALGORITHM_AES_GCM, 0,
                               (unsigned char*)StreamConfig.remoteInputAesKey, sizeof(StreamConfig.remoteInputAesKey),
                               currentAesIv, sizeof(currentAesIv),
                               ciphertext, 16,
                               plaintext, plaintextLen,
                               &ciphertext[16], ciphertextLen)) {
            return -1;
        }

        // Increment the ciphertextLen to account for the tag
        *ciphertextLen += 16;
        return 0;
    }
    else {
        // PKCS7 padding may need to be added in-place, so we must copy this into a buffer
        // that can safely be modified.
        unsigned char paddedData[ROUND_TO_PKCS7_PADDED_LEN(MAX_INPUT_PACKET_SIZE)];

        memcpy(paddedData, plaintext, plaintextLen);

        // Prior to Gen 7, 128-bit AES CBC is used for encryption with each message padded
        // to the block size to ensure messages are not delayed within the cipher.
        return PltEncryptMessage(cryptoContext, ALGORITHM_AES_CBC, CIPHER_FLAG_PAD_TO_BLOCK_SIZE,
                                 (unsigned char*)StreamConfig.remoteInputAesKey, sizeof(StreamConfig.remoteInputAesKey),
                                 currentAesIv, sizeof(currentAesIv),
                                 NULL, 0,
                                 paddedData, plaintextLen,
                                 ciphertext, ciphertextLen) ? 0 : -1;
    }
}

static void freePacketHolder(PPACKET_HOLDER holder) {
    LC_ASSERT(holder->packet.header.size != 0);

    // Place the packet holder back into the free list if it's a standard size entry
    if (PACKET_SIZE(holder) > (int)sizeof(*holder) || LbqOfferQueueItem(&packetHolderFreeList, holder, &holder->entry) != LBQ_SUCCESS) {
        free(holder);
    }
}

static PPACKET_HOLDER allocatePacketHolder(int extraLength) {
    PPACKET_HOLDER holder;
    int err;

    // If we're using an extended packet holder, we can't satisfy
    // this allocation from the packet holder free list.
    if (extraLength > 0) {
        // We over-allocate here a bit since we're always adding sizeof(*holder),
        // but this is on purpose. It allows us assume we have a full holder even
        // if packetLength < sizeof(*holder) and put this allocation into the free
        // list.
        return malloc(sizeof(*holder) + extraLength);
    }

    // Grab an entry from the free list (if available)
    err = LbqPollQueueElement(&packetHolderFreeList, (void**)&holder);
    if (err == LBQ_SUCCESS) {
        return holder;
    }
    else if (err == LBQ_INTERRUPTED) {
        // We're shutting down. Don't bother allocating.
        return NULL;
    }
    else {
        LC_ASSERT(err == LBQ_NO_ELEMENT);

        // Otherwise we'll have to allocate
        return malloc(sizeof(*holder));
    }
}

static bool sendInputPacket(PPACKET_HOLDER holder, bool moreData) {
    SOCK_RET err;

    // On GFE 3.22, the entire control stream is encrypted (and support for separate RI encrypted)
    // has been removed. We send the plaintext packet through and the control stream code will do
    // the encryption.
    if (encryptedControlStream) {
        err = (SOCK_RET)sendInputPacketOnControlStream((unsigned char*)&holder->packet,
                                                        PACKET_SIZE(holder),
                                                        holder->channelId,
                                                        holder->enetPacketFlags,
                                                        moreData);
        if (err < 0) {
            Limelog("Input: sendInputPacketOnControlStream() failed: %d\n", (int) err);
            ListenerCallbacks.connectionTerminated(err);
            return false;
        }
    }
    else {
        char encryptedBuffer[MAX_INPUT_PACKET_SIZE];
        uint32_t encryptedSize;
        uint32_t encryptedLengthPrefix;

        // Encrypt the message into the output buffer while leaving room for the length
        encryptedSize = sizeof(encryptedBuffer) - sizeof(encryptedLengthPrefix);
        err = encryptData((unsigned char*)&holder->packet, PACKET_SIZE(holder),
            (unsigned char*)&encryptedBuffer[sizeof(encryptedLengthPrefix)], (int*)&encryptedSize);
        if (err != 0) {
            Limelog("Input: Encryption failed: %d\n", (int)err);
            ListenerCallbacks.connectionTerminated(err);
            return false;
        }

        // Prepend the length to the message
        encryptedLengthPrefix = BE32(encryptedSize);
        memcpy(&encryptedBuffer[0], &encryptedLengthPrefix, sizeof(encryptedLengthPrefix));

        if (AppVersionQuad[0] < 5) {
            // Send the encrypted payload
            err = send(inputSock, (const char*) encryptedBuffer,
                (int) (encryptedSize + sizeof(encryptedLengthPrefix)), 0);
            if (err <= 0) {
                Limelog("Input: send() failed: %d\n", (int) LastSocketError());
                ListenerCallbacks.connectionTerminated(LastSocketFail());
                return false;
            }
        }
        else {
            // For reasons that I can't understand, NVIDIA decides to use the last 16
            // bytes of ciphertext in the most recent game controller packet as the IV for
            // future encryption. I think it may be a buffer overrun on their end but we'll have
            // to mimic it to work correctly.
            if (AppVersionQuad[0] >= 7 && encryptedSize >= 16 + sizeof(currentAesIv)) {
                memcpy(currentAesIv,
                       &encryptedBuffer[4 + encryptedSize - sizeof(currentAesIv)],
                       sizeof(currentAesIv));
            }

            err = (SOCK_RET)sendInputPacketOnControlStream((unsigned char*) encryptedBuffer,
                                                            (int)(encryptedSize + sizeof(encryptedLengthPrefix)),
                                                            holder->channelId,
                                                            holder->enetPacketFlags,
                                                            moreData);
            if (err < 0) {
                Limelog("Input: sendInputPacketOnControlStream() failed: %d\n", (int) err);
                ListenerCallbacks.connectionTerminated(err);
                return false;
            }
        }
    }

    return true;
}

static void floatToNetfloat(float in, netfloat out) {
    if (IS_LITTLE_ENDIAN()) {
        memcpy(out, &in, sizeof(in));
    }
    else {
        uint8_t* inb = (uint8_t*)&in;
        out[0] = inb[3];
        out[1] = inb[2];
        out[2] = inb[1];
        out[3] = inb[0];
    }
}

// Input thread proc
static void inputSendThreadProc(void* context) {
    SOCK_RET err;
    PPACKET_HOLDER holder;
    uint32_t multiControllerMagicLE;
    uint32_t relMouseMagicLE;

    if (AppVersionQuad[0] >= 5) {
        multiControllerMagicLE = LE32(MULTI_CONTROLLER_MAGIC_GEN5);
        relMouseMagicLE = LE32(MOUSE_MOVE_REL_MAGIC_GEN5);
    }
    else {
        multiControllerMagicLE = LE32(MULTI_CONTROLLER_MAGIC);
        relMouseMagicLE = LE32(MOUSE_MOVE_REL_MAGIC);
    }

    uint64_t lastControllerPacketTime[MAX_GAMEPADS] = { 0 };
    uint64_t lastMousePacketTime = 0;
    uint64_t lastPenPacketTime = 0;

    while (!PltIsThreadInterrupted(&inputSendThread)) {
        err = LbqWaitForQueueElement(&packetQueue, (void**)&holder);
        if (err != LBQ_SUCCESS) {
            return;
        }

        // If it's a multi-controller packet we can do batching
        if (holder->packet.header.magic == multiControllerMagicLE) {
            PPACKET_HOLDER controllerBatchHolder;
            PNV_MULTI_CONTROLLER_PACKET origPkt;
            short controllerNumber = LE16(holder->packet.multiController.controllerNumber);
            uint64_t now = PltGetMillis();

            LC_ASSERT(controllerNumber < MAX_GAMEPADS);

            // Delay for batching if required
            if (now < lastControllerPacketTime[controllerNumber] + CONTROLLER_BATCHING_INTERVAL_MS) {
                flushInputOnControlStream();
                PltSleepMs((int)(lastControllerPacketTime[controllerNumber] + CONTROLLER_BATCHING_INTERVAL_MS - now));
                now = PltGetMillis();
            }

            origPkt = &holder->packet.multiController;
            for (;;) {
                PNV_MULTI_CONTROLLER_PACKET newPkt;

                // Peek at the next packet
                if (LbqPeekQueueElement(&packetQueue, (void**)&controllerBatchHolder) != LBQ_SUCCESS) {
                    break;
                }

                // If it's not a controller packet, we're done
                if (controllerBatchHolder->packet.header.magic != multiControllerMagicLE) {
                    break;
                }

                // Check if it's able to be batched
                // NB: GFE does some discarding of gamepad packets received very soon after another.
                // Thus, this batching is needed for correctness in some cases, as GFE will inexplicably
                // drop *newer* packets in that scenario. The brokenness can be tested with consecutive
                // calls to LiSendMultiControllerEvent() with different values for analog sticks (max -> zero).
                newPkt = &controllerBatchHolder->packet.multiController;
                if (newPkt->buttonFlags != origPkt->buttonFlags ||
                    newPkt->buttonFlags2 != origPkt->buttonFlags2 ||
                    newPkt->controllerNumber != origPkt->controllerNumber ||
                    newPkt->activeGamepadMask != origPkt->activeGamepadMask) {
                    // Batching not allowed
                    break;
                }

                // Remove the batchable controller packet
                if (LbqPollQueueElement(&packetQueue, (void**)&controllerBatchHolder) != LBQ_SUCCESS) {
                    break;
                }

                // Update the original packet
                origPkt->leftTrigger = newPkt->leftTrigger;
                origPkt->rightTrigger = newPkt->rightTrigger;
                origPkt->leftStickX = newPkt->leftStickX;
                origPkt->leftStickY = newPkt->leftStickY;
                origPkt->rightStickX = newPkt->rightStickX;
                origPkt->rightStickY = newPkt->rightStickY;

                // Free the batched packet holder
                freePacketHolder(controllerBatchHolder);
            }

            lastControllerPacketTime[controllerNumber] = now;
        }
        // If it's a relative mouse move packet, we can also do batching
        else if (holder->packet.header.magic == relMouseMagicLE) {
            uint64_t now = PltGetMillis();

            // Delay for batching if required
            if (now < lastMousePacketTime + MOUSE_BATCHING_INTERVAL_MS) {
                flushInputOnControlStream();
                PltSleepMs((int)(lastMousePacketTime + MOUSE_BATCHING_INTERVAL_MS - now));
                now = PltGetMillis();
            }

            PltLockMutex(&batchedInputMutex);

            // Send as many packets as it takes to get the entire delta through
            while (currentRelativeMouseState.deltaX != 0 || currentRelativeMouseState.deltaY != 0) {
                bool more = false;

                if (currentRelativeMouseState.deltaX < INT16_MIN) {
                    holder->packet.mouseMoveRel.deltaX = BE16(INT16_MIN);
                    currentRelativeMouseState.deltaX -= INT16_MIN;
                    more = true;
                }
                else if (currentRelativeMouseState.deltaX > INT16_MAX) {
                    holder->packet.mouseMoveRel.deltaX = BE16(INT16_MAX);
                    currentRelativeMouseState.deltaX -= INT16_MAX;
                    more = true;
                }
                else {
                    holder->packet.mouseMoveRel.deltaX = BE16(currentRelativeMouseState.deltaX);
                    currentRelativeMouseState.deltaX = 0;
                }

                if (currentRelativeMouseState.deltaY < INT16_MIN) {
                    holder->packet.mouseMoveRel.deltaY = BE16(INT16_MIN);
                    currentRelativeMouseState.deltaY -= INT16_MIN;
                    more = true;
                }
                else if (currentRelativeMouseState.deltaY > INT16_MAX) {
                    holder->packet.mouseMoveRel.deltaY = BE16(INT16_MAX);
                    currentRelativeMouseState.deltaY -= INT16_MAX;
                    more = true;
                }
                else {
                    holder->packet.mouseMoveRel.deltaY = BE16(currentRelativeMouseState.deltaY);
                    currentRelativeMouseState.deltaY = 0;
                }

                // Don't hold the batching lock while we're doing network I/O
                PltUnlockMutex(&batchedInputMutex);

                // Encrypt and send the split packet
                if (!sendInputPacket(holder, more)) {
                    freePacketHolder(holder);
                    return;
                }

                PltLockMutex(&batchedInputMutex);
            }

            // The state change is no longer pending
            currentRelativeMouseState.dirty = false;

            PltUnlockMutex(&batchedInputMutex);

            lastMousePacketTime = now;

            // We sent everything we needed in the loop above, so we can just free the
            // holder of the original packet and wait for another input event.
            freePacketHolder(holder);
            continue;
        }
        // If it's an absolute mouse move packet, we should only send the latest
        else if (holder->packet.header.magic == LE32(MOUSE_MOVE_ABS_MAGIC)) {
            uint64_t now = PltGetMillis();

            // Delay for batching if required
            if (now < lastMousePacketTime + MOUSE_BATCHING_INTERVAL_MS) {
                flushInputOnControlStream();
                PltSleepMs((int)(lastMousePacketTime + MOUSE_BATCHING_INTERVAL_MS - now));
                now = PltGetMillis();
            }

            PltLockMutex(&batchedInputMutex);

            // Populate the packet with the latest state
            holder->packet.mouseMoveAbs.x = BE16(currentAbsoluteMouseState.x);
            holder->packet.mouseMoveAbs.y = BE16(currentAbsoluteMouseState.y);

            // There appears to be a rounding error in GFE's scaling calculation which prevents
            // the cursor from reaching the far edge of the screen when streaming at smaller
            // resolutions with a higher desktop resolution (like streaming 720p with a desktop
            // resolution of 1080p, or streaming 720p/1080p with a desktop resolution of 4K).
            // Subtracting one from the reference dimensions seems to work around this issue.
            holder->packet.mouseMoveAbs.width = BE16(currentAbsoluteMouseState.width - 1);
            holder->packet.mouseMoveAbs.height = BE16(currentAbsoluteMouseState.height - 1);

            // The state change is no longer pending
            currentAbsoluteMouseState.dirty = false;

            PltUnlockMutex(&batchedInputMutex);

            lastMousePacketTime = now;
        }
        // If it's a pen packet, we should only send the latest move or hover events
        else if (holder->packet.header.magic == LE32(SS_PEN_MAGIC) && TOUCH_EVENT_IS_BATCHABLE(holder->packet.pen.eventType)) {
            uint64_t now = PltGetMillis();

            // Delay for batching if required
            if (now < lastPenPacketTime + PEN_BATCHING_INTERVAL_MS) {
                flushInputOnControlStream();
                PltSleepMs((int)(lastPenPacketTime + PEN_BATCHING_INTERVAL_MS - now));
                now = PltGetMillis();
            }

            for (;;) {
                PPACKET_HOLDER penBatchHolder;

                // Peek at the next packet
                if (LbqPeekQueueElement(&packetQueue, (void**)&penBatchHolder) != LBQ_SUCCESS) {
                    break;
                }

                // If it's not a pen packet, we're done
                if (penBatchHolder->packet.header.magic != LE32(SS_PEN_MAGIC)) {
                    break;
                }

                // If the buttons or event type is different, we cannot batch
                if (holder->packet.pen.penButtons != penBatchHolder->packet.pen.penButtons ||
                    holder->packet.pen.eventType != penBatchHolder->packet.pen.eventType) {
                    break;
                }

                // Remove the next packet
                if (LbqPollQueueElement(&packetQueue, (void**)&penBatchHolder) != LBQ_SUCCESS) {
                    break;
                }

                // Replace the current packet with the new one
                freePacketHolder(holder);
                holder = penBatchHolder;
            }

            lastPenPacketTime = now;
        }
        // If it's a motion packet, only send the latest for each sensor type
        else if (holder->packet.header.magic == LE32(SS_CONTROLLER_MOTION_MAGIC)) {
            uint8_t controllerNumber = holder->packet.controllerMotion.controllerNumber;
            uint8_t motionType = holder->packet.controllerMotion.motionType;

            LC_ASSERT(controllerNumber < MAX_GAMEPADS);
            LC_ASSERT(motionType - 1 < MAX_MOTION_EVENTS);

            PltLockMutex(&batchedInputMutex);

            // LI_MOTION_TYPE_* values are 1-based, so we have to subtract 1 to index into our state array
            float x = currentGamepadSensorState[controllerNumber][motionType - 1].x;
            float y = currentGamepadSensorState[controllerNumber][motionType - 1].y;
            float z = currentGamepadSensorState[controllerNumber][motionType - 1].z;

            // Motion events are so rapid that we can just drop any events that are lost in transit,
            // but we will treat (0, 0, 0) as a special value for gyro events to allow clients to
            // reliably set the gyro to a null state when sensor events are halted due to focus loss
            // or similar client-side constraints.
            if (motionType == LI_MOTION_TYPE_GYRO && x == 0.0f && y == 0.0f && z == 0.0f) {
                holder->enetPacketFlags = ENET_PACKET_FLAG_RELIABLE;
            }
            else {
                holder->enetPacketFlags = 0;
            }

            // Populate the packet with the latest state
            floatToNetfloat(x, holder->packet.controllerMotion.x);
            floatToNetfloat(y, holder->packet.controllerMotion.y);
            floatToNetfloat(z, holder->packet.controllerMotion.z);

            // The state change is no longer pending
            currentGamepadSensorState[controllerNumber][motionType - 1].dirty = false;

            PltUnlockMutex(&batchedInputMutex);
        }
        // If it's a UTF-8 text packet, we may need to split it into a several packets to send
        else if (holder->packet.header.magic == LE32(UTF8_TEXT_EVENT_MAGIC)) {
            PACKET_HOLDER splitPacket;
            uint32_t totalLength = PAYLOAD_SIZE(holder) - sizeof(uint32_t);
            uint32_t i = 0;

            // HACK: This is a workaround for the fact that GFE doesn't appear to synchronize keyboard
            // and UTF-8 text events with each other. We need to make sure any previous keyboard events
            // have been processed prior to sending these UTF-8 events to avoid interference between
            // the two (especially with modifier keys).
            flushInputOnControlStream();
            while (!PltIsThreadInterrupted(&inputSendThread) && isControlDataInTransit()) {
                PltSleepMs(10);
            }

            // Finally, sleep an additional 50 ms to allow the events to be processed by Windows
            PltSleepMs(50);

            // We send each Unicode code point individually. This way we can always ensure they will
            // never straddle a packet boundary (which will cause a parsing error on the host).
            while (i < totalLength && !PltIsThreadInterrupted(&inputSendThread)) {
                uint32_t codePointLength;
                uint8_t firstByte = (uint8_t)holder->packet.unicode.text[i];
                if ((firstByte & 0x80) == 0x00) {
                    // 1 byte code point
                    codePointLength = 1;
                }
                else if ((firstByte & 0xE0) == 0xC0) {
                    // 2 byte code point
                    codePointLength = 2;
                }
                else if ((firstByte & 0xF0) == 0xE0) {
                    // 3 byte code point
                    codePointLength = 3;
                }
                else if ((firstByte & 0xF8) == 0xF0) {
                    // 4 byte code point
                    codePointLength = 4;
                }
                else {
                    Limelog("Invalid unicode code point starting byte: %02x\n", firstByte);
                    break;
                }

                // Use the original packet as a template and fixup to send one code point at a time
                splitPacket = *holder;
                splitPacket.packet.unicode.header.size = BE32(sizeof(uint32_t) + codePointLength);
                memcpy(splitPacket.packet.unicode.text, &holder->packet.unicode.text[i], codePointLength);

                // Encrypt and send the split packet
                if (!sendInputPacket(&splitPacket, i + 1 < totalLength)) {
                    freePacketHolder(holder);
                    return;
                }

                i += codePointLength;
            }

            freePacketHolder(holder);
            continue;
        }

        // Encrypt and send the input packet
        if (!sendInputPacket(holder, LbqGetItemCount(&packetQueue) > 0)) {
            freePacketHolder(holder);
            return;
        }

        freePacketHolder(holder);
    }
}

// This function tells GFE that we support haptics and it should send rumble events to us
static int sendEnableHaptics(void) {
    PPACKET_HOLDER holder;
    int err;

    // Avoid sending this on earlier server versions, since they may terminate
    // the connection upon receiving an unexpected packet.
    if (!APP_VERSION_AT_LEAST(7, 1, 0)) {
        return 0;
    }

    holder = allocatePacketHolder(0);
    if (holder == NULL) {
        return -1;
    }

    holder->channelId = CTRL_CHANNEL_GENERIC;
    holder->enetPacketFlags = ENET_PACKET_FLAG_RELIABLE;
    holder->packet.haptics.header.size = BE32(sizeof(NV_HAPTICS_PACKET) - sizeof(uint32_t));
    holder->packet.haptics.header.magic = LE32(ENABLE_HAPTICS_MAGIC);
    holder->packet.haptics.enable = LE16(1);

    err = LbqOfferQueueItem(&packetQueue, holder, &holder->entry);
    if (err != LBQ_SUCCESS) {
        LC_ASSERT(err == LBQ_BOUND_EXCEEDED);
        Limelog("Input queue reached maximum size limit\n");
        freePacketHolder(holder);
    }

    return err;
}

// Begin the input stream
int startInputStream(void) {
    int err;

    // After Gen 5, we send input on the control stream
    if (AppVersionQuad[0] < 5) {
        inputSock = connectTcpSocket(&RemoteAddr, AddrLen,
            35043, INPUT_STREAM_TIMEOUT_SEC);
        if (inputSock == INVALID_SOCKET) {
            return LastSocketFail();
        }

        enableNoDelay(inputSock);
    }

    err = PltCreateThread("InputSend", inputSendThreadProc, NULL, &inputSendThread);
    if (err != 0) {
        if (inputSock != INVALID_SOCKET) {
            closeSocket(inputSock);
            inputSock = INVALID_SOCKET;
        }
        return err;
    }

    // Allow input packets to be queued now
    initialized = true;

    // GFE will not send haptics events without this magic packet first
    sendEnableHaptics();

    return err;
}

// Stops the input stream
int stopInputStream(void) {
    // No more packets should be queued now
    initialized = false;
    LbqSignalQueueShutdown(&packetHolderFreeList);

    // Signal the input send thread to drain all pending
    // input packets before shutting down.
    LbqSignalQueueDrain(&packetQueue);
    PltJoinThread(&inputSendThread);

    if (inputSock != INVALID_SOCKET) {
        shutdownTcpSocket(inputSock);
    }
    
    if (inputSock != INVALID_SOCKET) {
        closeSocket(inputSock);
        inputSock = INVALID_SOCKET;
    }

    return 0;
}

// Send a mouse move event to the streaming machine
int LiSendMouseMoveEvent(short deltaX, short deltaY) {
    PPACKET_HOLDER holder;
    int err;

    if (!initialized) {
        return -2;
    }

    if (deltaX == 0 && deltaY == 0) {
        return 0;
    }

    PltLockMutex(&batchedInputMutex);

    // Combine the previous deltas with the new one
    currentRelativeMouseState.deltaX += deltaX;
    currentRelativeMouseState.deltaY += deltaY;

    // Queue a packet holder if this is the only pending relative mouse event
    if (!currentRelativeMouseState.dirty) {
        holder = allocatePacketHolder(0);
        if (holder == NULL) {
            PltUnlockMutex(&batchedInputMutex);
            return -1;
        }

        holder->channelId = CTRL_CHANNEL_MOUSE;

        // TODO: Send this as unreliable sequenced when we have a delayed reliable retransmission thread
        // and protocol updates to allow us to determine which unreliable messages were dropped.
        holder->enetPacketFlags = ENET_PACKET_FLAG_RELIABLE;

        holder->packet.mouseMoveRel.header.size = BE32(sizeof(NV_REL_MOUSE_MOVE_PACKET) - sizeof(uint32_t));
        if (AppVersionQuad[0] >= 5) {
            holder->packet.mouseMoveRel.header.magic = LE32(MOUSE_MOVE_REL_MAGIC_GEN5);
        }
        else {
            holder->packet.mouseMoveRel.header.magic = LE32(MOUSE_MOVE_REL_MAGIC);
        }

        // Remaining fields are set in the input thread based on the latest currentRelativeMouseState values

        err = LbqOfferQueueItem(&packetQueue, holder, &holder->entry);
        if (err == LBQ_SUCCESS) {
            currentRelativeMouseState.dirty = true;
        }
        else {
            LC_ASSERT(err == LBQ_BOUND_EXCEEDED);
            Limelog("Input queue reached maximum size limit\n");
            freePacketHolder(holder);
        }
    }
    else {
        // There's already a packet holder queued to send this event
        err = 0;
    }

    PltUnlockMutex(&batchedInputMutex);

    return err;
}

// Send a mouse position update to the streaming machine
int LiSendMousePositionEvent(short x, short y, short referenceWidth, short referenceHeight) {
    PPACKET_HOLDER holder;
    int err;

    if (!initialized) {
        return -2;
    }

    PltLockMutex(&batchedInputMutex);

    // Overwrite the previous mouse location with the new one
    currentAbsoluteMouseState.x = x;
    currentAbsoluteMouseState.y = y;
    currentAbsoluteMouseState.width = referenceWidth;
    currentAbsoluteMouseState.height = referenceHeight;

    // Queue a packet holder if this is the only pending absolute mouse event
    if (!currentAbsoluteMouseState.dirty) {
        holder = allocatePacketHolder(0);
        if (holder == NULL) {
            PltUnlockMutex(&batchedInputMutex);
            return -1;
        }

        holder->channelId = CTRL_CHANNEL_MOUSE;

        // TODO: Send this as unreliable sequenced when we have a delayed reliable retransmission thread
        holder->enetPacketFlags = ENET_PACKET_FLAG_RELIABLE;

        holder->packet.mouseMoveAbs.header.size = BE32(sizeof(NV_ABS_MOUSE_MOVE_PACKET) - sizeof(uint32_t));
        holder->packet.mouseMoveAbs.header.magic = LE32(MOUSE_MOVE_ABS_MAGIC);
        holder->packet.mouseMoveAbs.unused = 0;

        // Remaining fields are set in the input thread based on the latest currentAbsoluteMouseState values

        err = LbqOfferQueueItem(&packetQueue, holder, &holder->entry);
        if (err == LBQ_SUCCESS) {
            currentAbsoluteMouseState.dirty = true;
        }
        else {
            LC_ASSERT(err == LBQ_BOUND_EXCEEDED);
            Limelog("Input queue reached maximum size limit\n");
            freePacketHolder(holder);
        }
    }
    else {
        // There's already a packet holder queued to send this event
        err = 0;
    }

    PltUnlockMutex(&batchedInputMutex);

    // This is not thread safe, but it's not a big deal because callers that want to
    // use LiSendRelativeMotionAsMousePositionEvent() must not mix these function
    // without synchronization (otherwise the state of the cursor on the host is
    // undefined anyway).
    absCurrentPosX = CLAMP(x, 0, referenceWidth - 1) / (float)(referenceWidth - 1);
    absCurrentPosY = CLAMP(y, 0, referenceHeight - 1) / (float)(referenceHeight - 1);

    return err;
}

// Send a relative motion event using absolute position to the streaming machine
int LiSendMouseMoveAsMousePositionEvent(short deltaX, short deltaY, short referenceWidth, short referenceHeight) {
    // Convert the current position to be relative to the provided reference dimensions
    short oldPositionX = (short)(absCurrentPosX * referenceWidth);
    short oldPositionY = (short)(absCurrentPosY * referenceHeight);

    return LiSendMousePositionEvent(CLAMP(oldPositionX + deltaX, 0, referenceWidth),
                                    CLAMP(oldPositionY + deltaY, 0, referenceHeight),
                                    referenceWidth, referenceHeight);
}

// Send a mouse button event to the streaming machine
int LiSendMouseButtonEvent(char action, int button) {
    PPACKET_HOLDER holder;
    int err;

    if (!initialized) {
        return -2;
    }

    holder = allocatePacketHolder(0);
    if (holder == NULL) {
        return -1;
    }

    holder->channelId = CTRL_CHANNEL_MOUSE;
    holder->enetPacketFlags = ENET_PACKET_FLAG_RELIABLE;
    holder->packet.mouseButton.header.size = BE32(sizeof(NV_MOUSE_BUTTON_PACKET) - sizeof(uint32_t));
    holder->packet.mouseButton.header.magic = (uint8_t)action;
    if (AppVersionQuad[0] >= 5) {
        holder->packet.mouseButton.header.magic++;
    }
    holder->packet.mouseButton.header.magic = LE32(holder->packet.mouseButton.header.magic);
    holder->packet.mouseButton.button = (uint8_t)button;

    err = LbqOfferQueueItem(&packetQueue, holder, &holder->entry);
    if (err != LBQ_SUCCESS) {
        LC_ASSERT(err == LBQ_BOUND_EXCEEDED);
        Limelog("Input queue reached maximum size limit\n");
        freePacketHolder(holder);
    }

    return err;
}

// Send a key press event to the streaming machine
int LiSendKeyboardEvent2(short keyCode, char keyAction, char modifiers, char flags) {
    PPACKET_HOLDER holder;
    int err;

    if (!initialized) {
        return -2;
    }

    holder = allocatePacketHolder(0);
    if (holder == NULL) {
        return -1;
    }

    holder->channelId = CTRL_CHANNEL_KEYBOARD;
    holder->enetPacketFlags = ENET_PACKET_FLAG_RELIABLE;

    // For proper behavior, the MODIFIER flag must not be set on the modifier key down event itself
    // for the extended modifiers on the right side of the keyboard. If the MODIFIER flag is set,
    // GFE will synthesize an errant key down event for the non-extended key, causing that key to be
    // stuck down after the extended modifier key is raised. For non-extended keys, we must set the
    // MODIFIER flag for correct behavior.
    if (!IS_SUNSHINE()) {
        switch (keyCode & 0xFF) {
            case 0x5B: // VK_LWIN
            case 0x5C: // VK_RWIN
                // Any keyboard event with the META modifier flag is dropped by all known GFE versions.
                // This prevents us from sending shortcuts involving the meta key (Win+X, Win+Tab, etc).
                // The catch is that the meta key event itself would actually work if it didn't set its
                // own modifier flag, so we'll clear that here. This should be safe even if a new GFE
                // release comes out that stops dropping events with MODIFIER_META flag.
                modifiers &= ~MODIFIER_META;
                break;

            case 0xA0: // VK_LSHIFT
                modifiers |= MODIFIER_SHIFT;
                break;
            case 0xA1: // VK_RSHIFT
                modifiers &= ~MODIFIER_SHIFT;
                break;

            case 0xA2: // VK_LCONTROL
                modifiers |= MODIFIER_CTRL;
                break;
            case 0xA3: // VK_RCONTROL
                modifiers &= ~MODIFIER_CTRL;
                break;

            case 0xA4: // VK_LMENU
                modifiers |= MODIFIER_ALT;
                break;
            case 0xA5: // VK_RMENU
                modifiers &= ~MODIFIER_ALT;
                break;

            default:
                // No fixups
                break;
        }
    }

    holder->packet.keyboard.header.size = BE32(sizeof(NV_KEYBOARD_PACKET) - sizeof(uint32_t));
    holder->packet.keyboard.header.magic = LE32((uint32_t)keyAction);
    holder->packet.keyboard.flags = IS_SUNSHINE() ? flags : 0;
    holder->packet.keyboard.keyCode = LE16(keyCode);
    holder->packet.keyboard.modifiers = modifiers;
    holder->packet.keyboard.zero2 = 0;

    err = LbqOfferQueueItem(&packetQueue, holder, &holder->entry);
    if (err != LBQ_SUCCESS) {
        LC_ASSERT(err == LBQ_BOUND_EXCEEDED);
        Limelog("Input queue reached maximum size limit\n");
        freePacketHolder(holder);
    }

    return err;
}

int LiSendKeyboardEvent(short keyCode, char keyAction, char modifiers) {
    return LiSendKeyboardEvent2(keyCode, keyAction, modifiers, 0);
}

int LiSendUtf8TextEvent(const char *text, unsigned int length) {
    PPACKET_HOLDER holder;
    int err;

    if (!initialized) {
        return -2;
    }

    holder = allocatePacketHolder(length);
    if (holder == NULL) {
        return -1;
    }

    holder->channelId = CTRL_CHANNEL_UTF8;
    holder->enetPacketFlags = ENET_PACKET_FLAG_RELIABLE;

    // Magic + string length
    holder->packet.unicode.header.size = BE32(sizeof(uint32_t) + length);
    holder->packet.unicode.header.magic = LE32(UTF8_TEXT_EVENT_MAGIC);
    memcpy(holder->packet.unicode.text, text, length);

    err = LbqOfferQueueItem(&packetQueue, holder, &holder->entry);
    if (err != LBQ_SUCCESS) {
        LC_ASSERT(err == LBQ_BOUND_EXCEEDED);
        Limelog("Input queue reached maximum size limit\n");
        freePacketHolder(holder);
    }

    return err;
}

static int sendControllerEventInternal(short controllerNumber, short activeGamepadMask,
    int buttonFlags, unsigned char leftTrigger, unsigned char rightTrigger,
    short leftStickX, short leftStickY, short rightStickX, short rightStickY)
{
    PPACKET_HOLDER holder;
    int err;

    if (!initialized) {
        return -2;
    }

    // HACK: We previously used a short for the buttonFlags argument, but we switched to an
    // int to support additional buttons with Sunshine. Unfortunately, some clients still pass
    // a short, which gets sign extended to an int. This causes all the new button flags to be
    // set any time the user presses the Y button on their gamepad (since Y is 0x8000). To deal
    // with these clients, we will detect this condition by checking if the sign bit is set.
    // Since we know there's no valid button flag that uses the 31st bit, any case where the
    // input value is negative is an instance of bug so only the botton 16 bits are valid.
    if (buttonFlags < 0) {
        buttonFlags &= 0xFFFF;
    }

    if (!IS_SUNSHINE()) {
        // GFE only supports a maximum of 4 controllers
        controllerNumber %= 4;
        activeGamepadMask &= 0xF;

        // GFE doesn't support buttons that aren't present on an Xbox 360 controller,
        // so the extended button flags won't even be sent. For convenience, let's
        // map the MISC button to the SPECIAL (Guide) button. Some platforms reserve
        // the Guide button for OS functionality (Game Bar, Home button, etc.), so
        // this allows otherwise unused buttons to activate that functionality.
        if (buttonFlags & MISC_FLAG) {
            buttonFlags |= SPECIAL_FLAG;
        }
    }
    else {
        // Sunshine supports up to 16 (max number of bits in activeGamepadMask)
        controllerNumber %= MAX_GAMEPADS;
    }

    holder = allocatePacketHolder(0);
    if (holder == NULL) {
        return -1;
    }

    // Send each controller on a separate channel
    holder->channelId = CTRL_CHANNEL_GAMEPAD_BASE + controllerNumber;

    // TODO: Send this as unreliable sequenced when we have a delayed reliable retransmission thread
    holder->enetPacketFlags = ENET_PACKET_FLAG_RELIABLE;

    if (AppVersionQuad[0] == 3) {
        // Generation 3 servers don't support multiple controllers so we send
        // the legacy packet
        holder->packet.controller.header.size = BE32(sizeof(NV_CONTROLLER_PACKET) - sizeof(uint32_t));
        holder->packet.controller.header.magic = LE32(CONTROLLER_MAGIC);
        holder->packet.controller.headerB = LE16(C_HEADER_B);
        holder->packet.controller.buttonFlags = LE16(buttonFlags);
        holder->packet.controller.leftTrigger = leftTrigger;
        holder->packet.controller.rightTrigger = rightTrigger;
        holder->packet.controller.leftStickX = LE16(leftStickX);
        holder->packet.controller.leftStickY = LE16(leftStickY);
        holder->packet.controller.rightStickX = LE16(rightStickX);
        holder->packet.controller.rightStickY = LE16(rightStickY);
        holder->packet.controller.tailA = LE32(C_TAIL_A);
        holder->packet.controller.tailB = LE16(C_TAIL_B);
    }
    else {
        // Generation 4+ servers support passing the controller number
        holder->packet.multiController.header.size = BE32(sizeof(NV_MULTI_CONTROLLER_PACKET) - sizeof(uint32_t));

        // On Gen 5 servers, the header code is decremented by one
        if (AppVersionQuad[0] >= 5) {
            holder->packet.multiController.header.magic = LE32(MULTI_CONTROLLER_MAGIC_GEN5);
        }
        else {
            holder->packet.multiController.header.magic = LE32(MULTI_CONTROLLER_MAGIC);
        }

        holder->packet.multiController.headerB = LE16(MC_HEADER_B);
        holder->packet.multiController.controllerNumber = LE16(controllerNumber);
        holder->packet.multiController.activeGamepadMask = LE16(activeGamepadMask);
        holder->packet.multiController.midB = LE16(MC_MID_B);
        holder->packet.multiController.buttonFlags = LE16((short)buttonFlags);
        holder->packet.multiController.leftTrigger = leftTrigger;
        holder->packet.multiController.rightTrigger = rightTrigger;
        holder->packet.multiController.leftStickX = LE16(leftStickX);
        holder->packet.multiController.leftStickY = LE16(leftStickY);
        holder->packet.multiController.rightStickX = LE16(rightStickX);
        holder->packet.multiController.rightStickY = LE16(rightStickY);
        holder->packet.multiController.tailA = LE16(MC_TAIL_A);
        holder->packet.multiController.buttonFlags2 = IS_SUNSHINE() ? LE16((short)(buttonFlags >> 16)) : 0;
        holder->packet.multiController.tailB = LE16(MC_TAIL_B);
    }

    err = LbqOfferQueueItem(&packetQueue, holder, &holder->entry);
    if (err != LBQ_SUCCESS) {
        LC_ASSERT(err == LBQ_BOUND_EXCEEDED);
        Limelog("Input queue reached maximum size limit\n");
        freePacketHolder(holder);
    }

    return err;
}

// Send a controller event to the streaming machine
int LiSendControllerEvent(int buttonFlags, unsigned char leftTrigger, unsigned char rightTrigger,
    short leftStickX, short leftStickY, short rightStickX, short rightStickY)
{
    return sendControllerEventInternal(0, 0x1, buttonFlags, leftTrigger, rightTrigger,
        leftStickX, leftStickY, rightStickX, rightStickY);
}

// Send a controller event to the streaming machine
int LiSendMultiControllerEvent(short controllerNumber, short activeGamepadMask,
    int buttonFlags, unsigned char leftTrigger, unsigned char rightTrigger,
    short leftStickX, short leftStickY, short rightStickX, short rightStickY)
{
    return sendControllerEventInternal(controllerNumber, activeGamepadMask,
        buttonFlags, leftTrigger, rightTrigger,
        leftStickX, leftStickY, rightStickX, rightStickY);
}

// Send a high resolution scroll event to the streaming machine
int LiSendHighResScrollEvent(short scrollAmount) {
    PPACKET_HOLDER holder;
    int err;

    if (!initialized) {
        return -2;
    }

    if (scrollAmount == 0) {
        return 0;
    }

    // Newer version of GFE that use virtual HID devices have a bug that requires
    // the scroll events to be batched to WHEEL_DELTA. Due to the their HID report
    // descriptor, they don't actually support smooth scrolling. _Any_ scroll gets
    // converted into a full WHEEL_DELTA scroll, even if the actual delta is tiny.
    // Similarly, large scrolls are capped at +/- WHEEL_DELTA too so we'll need to
    // split those up too.
    if (needsBatchedScroll) {
        if ((batchedScrollDelta < 0 && scrollAmount > 0) ||
            (batchedScrollDelta > 0 && scrollAmount < 0)) {
            // Reset the accumulated scroll delta when the direction changes
            // FIXME: Maybe reset accumulated delta based on time too?
            batchedScrollDelta = 0;
        }

        batchedScrollDelta += scrollAmount;

        while (abs(batchedScrollDelta) >= LI_WHEEL_DELTA) {
            scrollAmount = batchedScrollDelta > 0 ? LI_WHEEL_DELTA : -LI_WHEEL_DELTA;

            holder = allocatePacketHolder(0);
            if (holder == NULL) {
                return -1;
            }

            holder->channelId = CTRL_CHANNEL_MOUSE;
            holder->enetPacketFlags = ENET_PACKET_FLAG_RELIABLE;

            holder->packet.scroll.header.size = BE32(sizeof(NV_SCROLL_PACKET) - sizeof(uint32_t));
            if (AppVersionQuad[0] >= 5) {
                holder->packet.scroll.header.magic = LE32(SCROLL_MAGIC_GEN5);
            }
            else {
                holder->packet.scroll.header.magic = LE32(SCROLL_MAGIC);
            }
            holder->packet.scroll.scrollAmt1 = BE16(scrollAmount);
            holder->packet.scroll.scrollAmt2 = holder->packet.scroll.scrollAmt1;
            holder->packet.scroll.zero3 = 0;

            err = LbqOfferQueueItem(&packetQueue, holder, &holder->entry);
            if (err != LBQ_SUCCESS) {
                LC_ASSERT(err == LBQ_BOUND_EXCEEDED);
                Limelog("Input queue reached maximum size limit\n");
                freePacketHolder(holder);
                return err;
            }

            batchedScrollDelta -= scrollAmount;
        }

        err = 0;
    }
    else {
        holder = allocatePacketHolder(0);
        if (holder == NULL) {
            return -1;
        }

        holder->channelId = CTRL_CHANNEL_MOUSE;
        holder->enetPacketFlags = ENET_PACKET_FLAG_RELIABLE;

        holder->packet.scroll.header.size = BE32(sizeof(NV_SCROLL_PACKET) - sizeof(uint32_t));
        if (AppVersionQuad[0] >= 5) {
            holder->packet.scroll.header.magic = LE32(SCROLL_MAGIC_GEN5);
        }
        else {
            holder->packet.scroll.header.magic = LE32(SCROLL_MAGIC);
        }
        holder->packet.scroll.scrollAmt1 = BE16(scrollAmount);
        holder->packet.scroll.scrollAmt2 = holder->packet.scroll.scrollAmt1;
        holder->packet.scroll.zero3 = 0;

        err = LbqOfferQueueItem(&packetQueue, holder, &holder->entry);
        if (err != LBQ_SUCCESS) {
            LC_ASSERT(err == LBQ_BOUND_EXCEEDED);
            Limelog("Input queue reached maximum size limit\n");
            freePacketHolder(holder);
        }
    }

    return err;
}

// Send a scroll event to the streaming machine
int LiSendScrollEvent(signed char scrollClicks) {
    return LiSendHighResScrollEvent(scrollClicks * LI_WHEEL_DELTA);
}

// Send a high resolution horizontal scroll event
int LiSendHighResHScrollEvent(short scrollAmount) {
    PPACKET_HOLDER holder;
    int err;

    if (!initialized) {
        return -2;
    }

    // This is a protocol extension only supported with Sunshine
    if (!IS_SUNSHINE()) {
        return LI_ERR_UNSUPPORTED;
    }

    if (scrollAmount == 0) {
        return 0;
    }

    holder = allocatePacketHolder(0);
    if (holder == NULL) {
        return -1;
    }

    holder->channelId = CTRL_CHANNEL_MOUSE;
    holder->enetPacketFlags = ENET_PACKET_FLAG_RELIABLE;

    holder->packet.hscroll.header.size = BE32(sizeof(SS_HSCROLL_PACKET) - sizeof(uint32_t));
    holder->packet.hscroll.header.magic = LE32(SS_HSCROLL_MAGIC);
    holder->packet.hscroll.scrollAmount = BE16(scrollAmount);

    err = LbqOfferQueueItem(&packetQueue, holder, &holder->entry);
    if (err != LBQ_SUCCESS) {
        LC_ASSERT(err == LBQ_BOUND_EXCEEDED);
        Limelog("Input queue reached maximum size limit\n");
        freePacketHolder(holder);
    }

    return err;
}

int LiSendHScrollEvent(signed char scrollClicks) {
    return LiSendHighResHScrollEvent(scrollClicks * LI_WHEEL_DELTA);
}

int LiSendTouchEvent(uint8_t eventType, uint32_t pointerId, float x, float y, float pressureOrDistance,
                     float contactAreaMajor, float contactAreaMinor, uint16_t rotation) {
    PPACKET_HOLDER holder;
    int err;

    if (!initialized) {
        return -2;
    }

    // This is a protocol extension only supported with Sunshine
    if (!(SunshineFeatureFlags & LI_FF_PEN_TOUCH_EVENTS)) {
        return LI_ERR_UNSUPPORTED;
    }

    holder = allocatePacketHolder(0);
    if (holder == NULL) {
        return -1;
    }

    holder->channelId = CTRL_CHANNEL_TOUCH;

    // Allow move and hover events to be dropped if a newer one arrives, but don't allow
    // state changing events like up/down/leave events to be dropped.
    holder->enetPacketFlags = TOUCH_EVENT_IS_BATCHABLE(eventType) ? 0 : ENET_PACKET_FLAG_RELIABLE;

    holder->packet.touch.header.size = BE32(sizeof(SS_TOUCH_PACKET) - sizeof(uint32_t));
    holder->packet.touch.header.magic = LE32(SS_TOUCH_MAGIC);
    holder->packet.touch.eventType = eventType;
    holder->packet.touch.pointerId = LE32(pointerId);
    holder->packet.touch.rotation = LE16(rotation);
    memset(holder->packet.touch.zero, 0, sizeof(holder->packet.touch.zero));
    floatToNetfloat(x, holder->packet.touch.x);
    floatToNetfloat(y, holder->packet.touch.y);
    floatToNetfloat(pressureOrDistance, holder->packet.touch.pressureOrDistance);
    floatToNetfloat(contactAreaMajor, holder->packet.touch.contactAreaMajor);
    floatToNetfloat(contactAreaMinor, holder->packet.touch.contactAreaMinor);

    err = LbqOfferQueueItem(&packetQueue, holder, &holder->entry);
    if (err != LBQ_SUCCESS) {
        LC_ASSERT(err == LBQ_BOUND_EXCEEDED);
        Limelog("Input queue reached maximum size limit\n");
        freePacketHolder(holder);
    }

    return err;
}

int LiSendPenEvent(uint8_t eventType, uint8_t toolType, uint8_t penButtons,
                   float x, float y, float pressureOrDistance,
                   float contactAreaMajor, float contactAreaMinor,
                   uint16_t rotation, uint8_t tilt) {
    PPACKET_HOLDER holder;
    int err;

    if (!initialized) {
        return -2;
    }

    // This is a protocol extension only supported with Sunshine
    if (!(SunshineFeatureFlags & LI_FF_PEN_TOUCH_EVENTS)) {
        return LI_ERR_UNSUPPORTED;
    }

    holder = allocatePacketHolder(0);
    if (holder == NULL) {
        return -1;
    }

    holder->channelId = CTRL_CHANNEL_PEN;

    // Allow move and hover events to be dropped if a newer one arrives (if no buttons changed),
    // but don't allow state changing events like up/down/leave events to be dropped.
    holder->enetPacketFlags = (TOUCH_EVENT_IS_BATCHABLE(eventType) && !(penButtons ^ currentPenButtonState)) ? 0 : ENET_PACKET_FLAG_RELIABLE;
    currentPenButtonState = penButtons;

    holder->packet.pen.header.size = BE32(sizeof(SS_PEN_PACKET) - sizeof(uint32_t));
    holder->packet.pen.header.magic = LE32(SS_PEN_MAGIC);
    holder->packet.pen.eventType = eventType;
    holder->packet.pen.toolType = toolType;
    holder->packet.pen.penButtons = penButtons;
    memset(holder->packet.pen.zero, 0, sizeof(holder->packet.pen.zero));
    floatToNetfloat(x, holder->packet.pen.x);
    floatToNetfloat(y, holder->packet.pen.y);
    floatToNetfloat(pressureOrDistance, holder->packet.pen.pressureOrDistance);
    holder->packet.pen.rotation = LE16(rotation);
    holder->packet.pen.tilt = tilt;
    memset(holder->packet.pen.zero2, 0, sizeof(holder->packet.pen.zero2));
    floatToNetfloat(contactAreaMajor, holder->packet.pen.contactAreaMajor);
    floatToNetfloat(contactAreaMinor, holder->packet.pen.contactAreaMinor);

    err = LbqOfferQueueItem(&packetQueue, holder, &holder->entry);
    if (err != LBQ_SUCCESS) {
        LC_ASSERT(err == LBQ_BOUND_EXCEEDED);
        Limelog("Input queue reached maximum size limit\n");
        freePacketHolder(holder);
    }

    return err;
}

int LiSendControllerArrivalEvent(uint8_t controllerNumber, uint16_t activeGamepadMask, uint8_t type,
                                 uint32_t supportedButtonFlags, uint16_t capabilities) {
    PPACKET_HOLDER holder;
    int err;

    if (!initialized) {
        return -2;
    }

    // Sunshine supports up to 16 controllers
    controllerNumber %= MAX_GAMEPADS;

    // The arrival event is only supported by Sunshine
    if (IS_SUNSHINE()) {
        holder = allocatePacketHolder(0);
        if (holder == NULL) {
            return -1;
        }

        // Send each controller on a separate channel
        holder->channelId = CTRL_CHANNEL_GAMEPAD_BASE + controllerNumber;
        holder->enetPacketFlags = ENET_PACKET_FLAG_RELIABLE;

        holder->packet.controllerArrival.header.size = BE32(sizeof(SS_CONTROLLER_ARRIVAL_PACKET) - sizeof(uint32_t));
        holder->packet.controllerArrival.header.magic = LE32(SS_CONTROLLER_ARRIVAL_MAGIC);
        holder->packet.controllerArrival.controllerNumber = controllerNumber;
        holder->packet.controllerArrival.type = type;
        holder->packet.controllerArrival.capabilities = LE16(capabilities);
        holder->packet.controllerArrival.supportedButtonFlags = LE32(supportedButtonFlags);

        err = LbqOfferQueueItem(&packetQueue, holder, &holder->entry);
        if (err != LBQ_SUCCESS) {
            LC_ASSERT(err == LBQ_BOUND_EXCEEDED);
            Limelog("Input queue reached maximum size limit\n");
            freePacketHolder(holder);
            return err;
        }
    }

    // Send a MC event just in case the host software doesn't support arrival events.
    return LiSendMultiControllerEvent(controllerNumber, activeGamepadMask, 0, 0, 0, 0, 0, 0, 0);
}

int LiSendControllerTouchEvent(uint8_t controllerNumber, uint8_t eventType, uint32_t pointerId, float x, float y, float pressure) {
    PPACKET_HOLDER holder;
    int err;

    if (!initialized) {
        return -2;
    }

    // This is a protocol extension only supported with Sunshine
    if (!(SunshineFeatureFlags & LI_FF_CONTROLLER_TOUCH_EVENTS)) {
        return LI_ERR_UNSUPPORTED;
    }

    // Sunshine supports up to 16 controllers
    controllerNumber %= MAX_GAMEPADS;

    holder = allocatePacketHolder(0);
    if (holder == NULL) {
        return -1;
    }

    // Send each controller on a separate channel
    holder->channelId = CTRL_CHANNEL_GAMEPAD_BASE + controllerNumber;

    // Allow move and hover events to be dropped if a newer one arrives, but don't allow
    // state changing events like up/down/leave events to be dropped.
    holder->enetPacketFlags = TOUCH_EVENT_IS_BATCHABLE(eventType) ? 0 : ENET_PACKET_FLAG_RELIABLE;

    holder->packet.controllerTouch.header.size = BE32(sizeof(SS_CONTROLLER_TOUCH_PACKET) - sizeof(uint32_t));
    holder->packet.controllerTouch.header.magic = LE32(SS_CONTROLLER_TOUCH_MAGIC);
    holder->packet.controllerTouch.controllerNumber = controllerNumber;
    holder->packet.controllerTouch.eventType = eventType;
    memset(holder->packet.controllerTouch.zero, 0, sizeof(holder->packet.controllerTouch.zero));
    holder->packet.controllerTouch.pointerId = LE32(pointerId);
    floatToNetfloat(x, holder->packet.controllerTouch.x);
    floatToNetfloat(y, holder->packet.controllerTouch.y);
    floatToNetfloat(pressure, holder->packet.controllerTouch.pressure);

    err = LbqOfferQueueItem(&packetQueue, holder, &holder->entry);
    if (err != LBQ_SUCCESS) {
        LC_ASSERT(err == LBQ_BOUND_EXCEEDED);
        Limelog("Input queue reached maximum size limit\n");
        freePacketHolder(holder);
    }

    return err;
}

int LiSendControllerMotionEvent(uint8_t controllerNumber, uint8_t motionType, float x, float y, float z) {
    PPACKET_HOLDER holder;
    int err;

    if (!initialized) {
        return -2;
    }

    // Check for valid motion type values
    if (motionType - 1 >= MAX_MOTION_EVENTS) {
        LC_ASSERT(motionType - 1 < MAX_MOTION_EVENTS);
        return -3;
    }

    // This is a protocol extension only supported with Sunshine
    if (!(SunshineFeatureFlags & LI_FF_CONTROLLER_TOUCH_EVENTS)) {
        return LI_ERR_UNSUPPORTED;
    }

    // Sunshine supports up to 16 controllers
    controllerNumber %= MAX_GAMEPADS;

    PltLockMutex(&batchedInputMutex);

    currentGamepadSensorState[controllerNumber][motionType - 1].x = x;
    currentGamepadSensorState[controllerNumber][motionType - 1].y = y;
    currentGamepadSensorState[controllerNumber][motionType - 1].z = z;

    // Queue a packet holder if this is the only pending sensor event
    if (!currentGamepadSensorState[controllerNumber][motionType - 1].dirty) {
        holder = allocatePacketHolder(0);
        if (holder == NULL) {
            PltUnlockMutex(&batchedInputMutex);
            return -1;
        }

        // Send each controller on a separate channel specific to motion sensors
        holder->channelId = CTRL_CHANNEL_SENSOR_BASE + controllerNumber;

        holder->packet.controllerMotion.header.size = BE32(sizeof(SS_CONTROLLER_MOTION_PACKET) - sizeof(uint32_t));
        holder->packet.controllerMotion.header.magic = LE32(SS_CONTROLLER_MOTION_MAGIC);
        holder->packet.controllerMotion.controllerNumber = controllerNumber;
        holder->packet.controllerMotion.motionType = motionType;
        memset(holder->packet.controllerMotion.zero, 0, sizeof(holder->packet.controllerMotion.zero));

        // Remaining fields are set in the input thread based on the latest currentGamepadSensorState values

        err = LbqOfferQueueItem(&packetQueue, holder, &holder->entry);
        if (err == LBQ_SUCCESS) {
            currentGamepadSensorState[controllerNumber][motionType - 1].dirty = true;
        }
        else {
            LC_ASSERT(err == LBQ_BOUND_EXCEEDED);
            Limelog("Input queue reached maximum size limit\n");
            freePacketHolder(holder);
        }
    }
    else {
        // There's already a packet holder queued to send this event
        err = 0;
    }

    PltUnlockMutex(&batchedInputMutex);

    return err;
}

int LiSendControllerBatteryEvent(uint8_t controllerNumber, uint8_t batteryState, uint8_t batteryPercentage) {
    PPACKET_HOLDER holder;
    int err;

    if (!initialized) {
        return -2;
    }

    // This is a protocol extension only supported with Sunshine
    if (!IS_SUNSHINE()) {
        return LI_ERR_UNSUPPORTED;
    }

    // Sunshine supports up to 16 controllers
    controllerNumber %= MAX_GAMEPADS;

    holder = allocatePacketHolder(0);
    if (holder == NULL) {
        return -1;
    }

    // Send each controller on a separate channel
    holder->channelId = CTRL_CHANNEL_GAMEPAD_BASE + controllerNumber;
    holder->enetPacketFlags = ENET_PACKET_FLAG_RELIABLE;

    holder->packet.controllerBattery.header.size = BE32(sizeof(SS_CONTROLLER_BATTERY_PACKET) - sizeof(uint32_t));
    holder->packet.controllerBattery.header.magic = LE32(SS_CONTROLLER_BATTERY_MAGIC);
    holder->packet.controllerBattery.controllerNumber = controllerNumber;
    holder->packet.controllerBattery.batteryState = batteryState;
    holder->packet.controllerBattery.batteryPercentage = batteryPercentage;
    memset(holder->packet.controllerBattery.zero, 0, sizeof(holder->packet.controllerBattery.zero));

    err = LbqOfferQueueItem(&packetQueue, holder, &holder->entry);
    if (err != LBQ_SUCCESS) {
        LC_ASSERT(err == LBQ_BOUND_EXCEEDED);
        Limelog("Input queue reached maximum size limit\n");
        freePacketHolder(holder);
    }

    return err;
}
