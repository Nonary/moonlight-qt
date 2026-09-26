#include "Limelight-internal.h"
#include "RtpVideoQueue.h"
#include "rs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EXPECT(condition, description) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s (line %d)\n", description, __LINE__); \
        failures++; \
    } \
} while (0)

#define PACKET_SIZE 64
#define DATA_PACKETS 4
#define DEADLINE_US 1000
#define PYROWAVE_FORMAT 0x0F0000

int AppVersionQuad[4] = {7, 1, 431, -1};
STREAM_CONFIGURATION StreamConfig = {0};
CONNECTION_LISTENER_CALLBACKS ListenerCallbacks = {0};
int NegotiatedVideoFormat = PYROWAVE_FORMAT;

static uint64_t fakeNowUs = 1000000;
static int failures;
static unsigned submittedFrames[8];
static unsigned submittedLost[8];
static unsigned submittedPackets[8];
static unsigned lostNotifications;
static unsigned fecPercent;

uint64_t PltGetMicroseconds(void) { return ++fakeNowUs; }
void connectionSawFrame(uint32_t frame) { (void)frame; }
void connectionSendFrameFecStatus(PSS_FRAME_FEC_STATUS status) { (void)status; }
void notifyFrameLost(unsigned frame, bool speculative) {
    (void)frame;
    (void)speculative;
    lostNotifications++;
}
void queueRtpPacket(PRTPV_QUEUE_ENTRY entry) {
    unsigned frame = LE32(((PNV_VIDEO_PACKET)((char*)entry->packet + sizeof(RTP_PACKET) + 4))->frameIndex);
    if (frame < 8) {
        submittedFrames[frame]++;
        submittedLost[frame] += entry->isLost;
        submittedPackets[frame]++;
    }
    free(entry->packet);
}

// These tests use zero-parity FEC blocks. Entering the Reed-Solomon path would
// mean the scenario is malformed for this harness and should fail immediately.
void reed_solomon_init(void) { }
reed_solomon* reed_solomon_new(int dataShards, int parityShards) {
    (void)dataShards; (void)parityShards;
    fprintf(stderr, "unexpected Reed-Solomon recovery\n");
    abort();
}
void reed_solomon_release(reed_solomon* rs) { (void)rs; }
int reed_solomon_decode(reed_solomon* rs, uint8_t** shards, uint8_t* marks, int count, int size) {
    (void)rs; (void)shards; (void)marks; (void)count; (void)size;
    fprintf(stderr, "unexpected Reed-Solomon decode\n");
    abort();
}

static void resetObservations(void) {
    memset(submittedFrames, 0, sizeof(submittedFrames));
    memset(submittedLost, 0, sizeof(submittedLost));
    memset(submittedPackets, 0, sizeof(submittedPackets));
    lostNotifications = 0;
}

static void beginQueue(RTP_VIDEO_QUEUE* queue, int videoFormat) {
    memset(queue, 0, sizeof(*queue));
    fecPercent = 0;
    NegotiatedVideoFormat = videoFormat;
    RtpvInitializeQueue(queue);
    resetObservations();
}

static int addPacket(RTP_VIDEO_QUEUE* queue, unsigned frame, unsigned block,
                     unsigned lastBlock, unsigned index, unsigned baseSequence,
                     unsigned flags, bool recordStart, bool validHeader,
                     unsigned criticalCount) {
    const int dataOffset = sizeof(RTP_PACKET) + 4;
    const int payloadOffset = dataOffset + sizeof(NV_VIDEO_PACKET);
    const int packetLength = StreamConfig.packetSize + dataOffset;
    char* buffer = (char*)calloc(1, packetLength + sizeof(RTPV_QUEUE_ENTRY));
    PRTP_PACKET packet;
    PNV_VIDEO_PACKET nv;
    PRTPV_QUEUE_ENTRY entry;
    unsigned sequence = (baseSequence + index) & 0xFFFF;
    int result;

    if (buffer == NULL) abort();
    packet = (PRTP_PACKET)buffer;
    packet->header = FLAG_EXTENSION;
    packet->sequenceNumber = (uint16_t)sequence;
    packet->timestamp = frame * 900;
    packet->ssrc = 0x12345678;

    nv = (PNV_VIDEO_PACKET)(buffer + dataOffset);
    nv->streamPacketIndex = LE32((block * DATA_PACKETS + index) << 8);
    nv->frameIndex = LE32(frame);
    nv->flags = (uint8_t)flags;
    nv->extraFlags = recordStart ? NV_VIDEO_PACKET_EXTRA_FLAG_PYROWAVE_RECORD_START : 0;
    nv->multiFecBlocks = (uint8_t)((block << 4) | (lastBlock << 6));
    nv->fecInfo = LE32((DATA_PACKETS << 22) | (fecPercent << 4) | (index << 12));

    if (index == 0) {
        uint8_t* payload = (uint8_t*)buffer + payloadOffset;
        if (validHeader) {
            payload[0] = 0x01;
            payload[6] = (uint8_t)criticalCount;
            payload[7] = (uint8_t)(criticalCount >> 8);
        } else {
            payload[0] = 0x02;
            payload[6] = (uint8_t)criticalCount;
            payload[7] = (uint8_t)(criticalCount >> 8);
        }
    }

    entry = (PRTPV_QUEUE_ENTRY)(buffer + packetLength);
    memset(entry, 0, sizeof(*entry));
    entry->packet = packet;
    result = RtpvAddPacket(queue, packet, packetLength, entry);
    if (result == RTPF_RET_REJECTED) {
        free(buffer);
    }
    return result;
}

static void addFullFrame(RTP_VIDEO_QUEUE* queue, unsigned frame, unsigned baseSequence,
                         unsigned block, unsigned lastBlock, bool recordStart,
                         unsigned criticalCount) {
    unsigned i;
    for (i = 0; i < DATA_PACKETS; i++) {
        unsigned flags = FLAG_CONTAINS_PIC_DATA;
        if (i == 0) flags |= FLAG_SOF;
        if (i == DATA_PACKETS - 1) flags |= FLAG_EOF;
        addPacket(queue, frame, block, lastBlock, i, baseSequence, flags,
                  recordStart && i == 0, true, criticalCount);
    }
}

static void testCodecControls(void) {
    RTP_VIDEO_QUEUE queue;
    const int formats[] = {VIDEO_FORMAT_H265_MAIN10, VIDEO_FORMAT_AV1_MAIN10};
    unsigned i;
    for (i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
        beginQueue(&queue, formats[i]);
        addPacket(&queue, 1, 0, 0, 0, 100, FLAG_SOF | FLAG_CONTAINS_PIC_DATA, false, false, 0);
        addPacket(&queue, 1, 0, 0, 2, 100, FLAG_CONTAINS_PIC_DATA, false, false, 0);
        addPacket(&queue, 1, 0, 0, 3, 100, FLAG_EOF | FLAG_CONTAINS_PIC_DATA, false, false, 0);
        EXPECT(RtpvGetPendingFrameDeadlineUs(&queue) == 0, "HEVC/AV1 have no PyroWave expiry deadline");
        EXPECT(!RtpvExpirePendingFrame(&queue, fakeNowUs + 5000), "HEVC/AV1 expiry does not flush partial frame");
        EXPECT(submittedPackets[1] == 0, "HEVC/AV1 partial frame is not submitted");
        addPacket(&queue, 2, 0, 0, 0, 104, FLAG_SOF | FLAG_CONTAINS_PIC_DATA, false, false, 0);
        EXPECT(submittedPackets[1] == 0, "HEVC/AV1 partial frame is dropped at successor");
        RtpvCleanupQueue(&queue);
    }
}

static void testIntactFrameImmediate(void) {
    RTP_VIDEO_QUEUE queue;
    beginQueue(&queue, PYROWAVE_FORMAT);
    addFullFrame(&queue, 1, 100, 0, 0, true, 2);
    EXPECT(submittedPackets[1] == DATA_PACKETS, "intact PyroWave frame submits immediately");
    EXPECT(RtpvGetPendingFrameDeadlineUs(&queue) == 0, "intact frame leaves no deadline");
    RtpvCleanupQueue(&queue);
}

static void testOptionalHoleExpiresWithoutSuccessor(void) {
    RTP_VIDEO_QUEUE queue;
    beginQueue(&queue, PYROWAVE_FORMAT);
    addPacket(&queue, 1, 0, 0, 0, 100, FLAG_SOF | FLAG_CONTAINS_PIC_DATA, true, true, 2);
    addPacket(&queue, 1, 0, 0, 1, 100, FLAG_CONTAINS_PIC_DATA, false, false, 0);
    addPacket(&queue, 1, 0, 0, 3, 100, FLAG_EOF | FLAG_CONTAINS_PIC_DATA, false, false, 0);
    uint64_t deadline = RtpvGetPendingFrameDeadlineUs(&queue);
    EXPECT(deadline != 0, "partial final block arms silence deadline");
    EXPECT(!RtpvExpirePendingFrame(&queue, deadline - 1), "partial frame is retained before deadline");
    EXPECT(submittedPackets[1] == 0, "frame waits before expiry");
    EXPECT(RtpvExpirePendingFrame(&queue, deadline), "partial frame expires at deadline");
    EXPECT(submittedPackets[1] == DATA_PACKETS, "expiry submits all real and synthesized packets");
    EXPECT(submittedLost[1] == 1, "expiry marks only optional hole as lost");
    EXPECT(RtpvGetPendingFrameDeadlineUs(&queue) == 0, "expiry clears deadline");
    EXPECT(addPacket(&queue, 1, 0, 0, 2, 100, FLAG_CONTAINS_PIC_DATA, false, false, 0) == RTPF_RET_REJECTED,
           "late packet from expired frame is rejected");
    EXPECT(submittedPackets[1] == DATA_PACKETS, "late packet cannot change already submitted frame");
    RtpvCleanupQueue(&queue);
}

static void testMissingEofCanExpire(void) {
    RTP_VIDEO_QUEUE queue;
    beginQueue(&queue, PYROWAVE_FORMAT);
    addPacket(&queue, 1, 0, 0, 0, 100, FLAG_SOF | FLAG_CONTAINS_PIC_DATA, true, true, 1);
    addPacket(&queue, 1, 0, 0, 1, 100, FLAG_CONTAINS_PIC_DATA, false, false, 0);
    addPacket(&queue, 1, 0, 0, 2, 100, FLAG_CONTAINS_PIC_DATA, false, false, 0);
    uint64_t deadline = RtpvGetPendingFrameDeadlineUs(&queue);
    EXPECT(deadline != 0, "missing EOF still arms deadline with complete critical prefix");
    EXPECT(RtpvExpirePendingFrame(&queue, deadline), "missing EOF frame expires without successor");
    EXPECT(submittedPackets[1] == DATA_PACKETS, "missing EOF expiry submits padded frame");
    EXPECT(submittedLost[1] == 1, "missing EOF is synthesized as optional loss");
    RtpvCleanupQueue(&queue);
}

static void testProgressExtendsDeadline(void) {
    RTP_VIDEO_QUEUE queue;
    beginQueue(&queue, PYROWAVE_FORMAT);
    addPacket(&queue, 1, 0, 0, 0, 100, FLAG_SOF | FLAG_CONTAINS_PIC_DATA, true, true, 2);
    addPacket(&queue, 1, 0, 0, 1, 100, FLAG_CONTAINS_PIC_DATA, false, false, 0);
    uint64_t first = RtpvGetPendingFrameDeadlineUs(&queue);
    fakeNowUs += 100;
    EXPECT(addPacket(&queue, 1, 0, 0, 1, 100, FLAG_CONTAINS_PIC_DATA, false, false, 0) == RTPF_RET_REJECTED,
           "duplicate packet is rejected");
    EXPECT(RtpvGetPendingFrameDeadlineUs(&queue) == first, "duplicate packet cannot prolong the deadline");
    fakeNowUs += 400;
    addPacket(&queue, 1, 0, 0, 2, 100, FLAG_CONTAINS_PIC_DATA, false, false, 0);
    uint64_t second = RtpvGetPendingFrameDeadlineUs(&queue);
    EXPECT(second > first, "new unique packet reschedules deadline");
    EXPECT(!RtpvExpirePendingFrame(&queue, first), "old deadline cannot flush after progress");
    EXPECT(RtpvExpirePendingFrame(&queue, second), "new deadline flushes the still-missing optional suffix");
    EXPECT(submittedLost[1] == 1, "only absent optional suffix packet is synthesized at current deadline");
    RtpvCleanupQueue(&queue);
}

static void testReorderedRecoveryBeforeDeadline(void) {
    RTP_VIDEO_QUEUE queue;
    beginQueue(&queue, PYROWAVE_FORMAT);
    addPacket(&queue, 1, 0, 0, 0, 100, FLAG_SOF | FLAG_CONTAINS_PIC_DATA, true, true, 2);
    addPacket(&queue, 1, 0, 0, 2, 100, FLAG_CONTAINS_PIC_DATA, false, false, 0);
    uint64_t oldDeadline = RtpvGetPendingFrameDeadlineUs(&queue);
    fakeNowUs += 300;
    EXPECT(addPacket(&queue, 1, 0, 0, 1, 100, FLAG_CONTAINS_PIC_DATA, false, false, 0) == RTPF_RET_QUEUED,
           "reordered missing packet accepted before expiry");
    EXPECT(RtpvGetPendingFrameDeadlineUs(&queue) > oldDeadline, "reordered recovery reschedules silence timer");
    EXPECT(RtpvExpirePendingFrame(&queue, oldDeadline) == false, "old deadline does not expire recovered block");
    EXPECT(submittedPackets[1] == 0, "recovered critical payload remains buffered while EOF is absent");
    RtpvCleanupQueue(&queue);
}

static void testStaleDeadlineAfterCleanupAndWrap(void) {
    RTP_VIDEO_QUEUE queue;
    beginQueue(&queue, PYROWAVE_FORMAT);
    queue.nextContiguousSequenceNumber = 65534;
    addPacket(&queue, 1, 0, 0, 0, 65534, FLAG_SOF | FLAG_CONTAINS_PIC_DATA, true, true, 1);
    addPacket(&queue, 1, 0, 0, 2, 65534, FLAG_CONTAINS_PIC_DATA, false, false, 0);
    uint64_t staleDeadline = RtpvGetPendingFrameDeadlineUs(&queue);
    EXPECT(staleDeadline != 0, "wrap case arms deadline");
    addPacket(&queue, 2, 0, 0, 0, 2, FLAG_SOF | FLAG_CONTAINS_PIC_DATA, true, true, 1);
    EXPECT(RtpvGetPendingFrameDeadlineUs(&queue) > staleDeadline, "frame transition replaces old deadline with successor deadline");
    EXPECT(!RtpvExpirePendingFrame(&queue, staleDeadline), "stale deadline cannot flush successor frame");
    EXPECT(submittedPackets[1] == DATA_PACKETS, "previous partial frame finalized by successor");
    EXPECT(submittedPackets[2] == 0, "successor packet remains queued across sequence wrap");
    RtpvCleanupQueue(&queue);
    EXPECT(RtpvGetPendingFrameDeadlineUs(&queue) == 0, "cleanup clears the pending deadline");
    EXPECT(!RtpvExpirePendingFrame(&queue, staleDeadline + 10000), "cleanup cannot leave an expirable frame");
}

static void testUnknownHeadersAndCriticalLossDoNotFlush(void) {
    RTP_VIDEO_QUEUE queue;
    uint64_t deadline;
    beginQueue(&queue, PYROWAVE_FORMAT);
    addPacket(&queue, 1, 0, 0, 0, 100, FLAG_SOF | FLAG_CONTAINS_PIC_DATA, true, false, 2);
    addPacket(&queue, 1, 0, 0, 3, 100, FLAG_EOF | FLAG_CONTAINS_PIC_DATA, false, false, 0);
    deadline = RtpvGetPendingFrameDeadlineUs(&queue);
    EXPECT(deadline != 0, "unknown header still allows timer tracking");
    EXPECT(!RtpvExpirePendingFrame(&queue, deadline), "invalid short header blocks early flush");
    EXPECT(submittedPackets[1] == 0, "unknown critical count is not presented");
    RtpvCleanupQueue(&queue);

    beginQueue(&queue, PYROWAVE_FORMAT);
    addPacket(&queue, 1, 0, 0, 0, 100, FLAG_SOF | FLAG_CONTAINS_PIC_DATA, true, true, 0);
    addPacket(&queue, 1, 0, 0, 3, 100, FLAG_EOF | FLAG_CONTAINS_PIC_DATA, false, false, 0);
    deadline = RtpvGetPendingFrameDeadlineUs(&queue);
    EXPECT(deadline != 0, "zero critical count still arms candidate deadline");
    EXPECT(!RtpvExpirePendingFrame(&queue, deadline), "zero critical count is not enough evidence to flush");
    EXPECT(submittedPackets[1] == 0, "unknown zero critical prefix is not presented");
    RtpvCleanupQueue(&queue);

    beginQueue(&queue, PYROWAVE_FORMAT);
    addPacket(&queue, 1, 0, 0, 0, 100, FLAG_SOF | FLAG_CONTAINS_PIC_DATA, false, true, 2);
    addPacket(&queue, 1, 0, 0, 3, 100, FLAG_EOF | FLAG_CONTAINS_PIC_DATA, false, false, 0);
    deadline = RtpvGetPendingFrameDeadlineUs(&queue);
    EXPECT(deadline != 0, "record header without record-start flag still tracks deadline");
    EXPECT(!RtpvExpirePendingFrame(&queue, deadline), "missing record-start flag blocks expiry flush");
    EXPECT(submittedPackets[1] == 0, "unmarked record header is not trusted as critical metadata");
    RtpvCleanupQueue(&queue);

    beginQueue(&queue, PYROWAVE_FORMAT);
    addPacket(&queue, 1, 0, 0, 0, 100, FLAG_SOF | FLAG_CONTAINS_PIC_DATA, true, true, 3);
    addPacket(&queue, 1, 0, 0, 3, 100, FLAG_EOF | FLAG_CONTAINS_PIC_DATA, false, false, 0);
    deadline = RtpvGetPendingFrameDeadlineUs(&queue);
    EXPECT(deadline != 0, "critical-loss case arms candidate deadline");
    EXPECT(!RtpvExpirePendingFrame(&queue, deadline), "missing critical data blocks expiry flush");
    EXPECT(submittedPackets[1] == 0, "critical loss leaves frame buffered for normal successor handling");
    RtpvCleanupQueue(&queue);

    beginQueue(&queue, PYROWAVE_FORMAT);
    addPacket(&queue, 1, 0, 0, 0, 100, FLAG_SOF | FLAG_CONTAINS_PIC_DATA, true, true, 5);
    addPacket(&queue, 1, 0, 0, 3, 100, FLAG_EOF | FLAG_CONTAINS_PIC_DATA, false, false, 0);
    deadline = RtpvGetPendingFrameDeadlineUs(&queue);
    EXPECT(deadline != 0, "oversized critical prefix still arms candidate deadline");
    EXPECT(!RtpvExpirePendingFrame(&queue, deadline), "critical prefix beyond block bounds is rejected");
    EXPECT(submittedPackets[1] == 0, "oversized critical prefix is not presented");
    RtpvCleanupQueue(&queue);

    beginQueue(&queue, PYROWAVE_FORMAT);
    fecPercent = 1; // 4 data packets plus one parity packet
    addPacket(&queue, 1, 0, 0, 0, 100, FLAG_SOF | FLAG_CONTAINS_PIC_DATA, true, true, 1);
    addPacket(&queue, 1, 0, 0, 2, 100, FLAG_CONTAINS_PIC_DATA, false, false, 0);
    addPacket(&queue, 1, 0, 0, 3, 100, FLAG_EOF | FLAG_CONTAINS_PIC_DATA, false, false, 0);
    EXPECT(RtpvGetPendingFrameDeadlineUs(&queue) == 0, "parity-protected block has no silence deadline");
    EXPECT(!RtpvExpirePendingFrame(&queue, fakeNowUs + 5000), "parity-protected block cannot be silence-flushed");
    EXPECT(submittedPackets[1] == 0, "parity-protected block remains queued for FEC recovery");
    RtpvCleanupQueue(&queue);
}

static void testCriticalCompleteAndOptionalMissingMultiblock(void) {
    RTP_VIDEO_QUEUE queue;
    beginQueue(&queue, PYROWAVE_FORMAT);
    // Block 0 arrives intact; block 1 has a two-packet critical prefix. The
    // critical prefix is real while the final optional packet is missing.
    for (unsigned i = 0; i < DATA_PACKETS; i++) {
        unsigned flags = FLAG_CONTAINS_PIC_DATA | (i == 0 ? FLAG_SOF : 0) |
                         (i == DATA_PACKETS - 1 ? FLAG_EOF : 0);
        addPacket(&queue, 1, 0, 1, i, 100, flags, i == 0, true, i == 0 ? 6 : 0);
    }
    addPacket(&queue, 1, 1, 1, 0, 104, FLAG_SOF | FLAG_CONTAINS_PIC_DATA, false, false, 0);
    addPacket(&queue, 1, 1, 1, 1, 104, FLAG_CONTAINS_PIC_DATA, false, false, 0);
    addPacket(&queue, 1, 1, 1, 2, 104, FLAG_CONTAINS_PIC_DATA, false, false, 0);
    uint64_t deadline = RtpvGetPendingFrameDeadlineUs(&queue);
    EXPECT(deadline != 0, "optional missing multiblock suffix arms deadline");
    EXPECT(RtpvExpirePendingFrame(&queue, deadline), "multiblock frame flushes after validating combined critical prefix");
    EXPECT(submittedPackets[1] == 2 * DATA_PACKETS, "multiblock expiry emits both blocks");
    EXPECT(submittedLost[1] == 1, "multiblock expiry fills only optional suffix");
    RtpvCleanupQueue(&queue);
}

static void testMissingCriticalFromEarlierBlockBlocksExpiry(void) {
    RTP_VIDEO_QUEUE queue;
    beginQueue(&queue, PYROWAVE_FORMAT);
    // Block 0 is staged with a synthesized hole in its critical prefix.
    // Block 1 then has only optional loss. The missing marker in the completed
    // list must still prevent early presentation of the whole frame.
    addPacket(&queue, 1, 0, 1, 0, 100, FLAG_SOF | FLAG_CONTAINS_PIC_DATA, true, true, 6);
    addPacket(&queue, 1, 0, 1, 2, 100, FLAG_CONTAINS_PIC_DATA, false, false, 0);
    addPacket(&queue, 1, 0, 1, 3, 100, FLAG_EOF | FLAG_CONTAINS_PIC_DATA, false, false, 0);
    addPacket(&queue, 1, 1, 1, 0, 104, FLAG_SOF | FLAG_CONTAINS_PIC_DATA, false, false, 0);
    addPacket(&queue, 1, 1, 1, 1, 104, FLAG_CONTAINS_PIC_DATA, false, false, 0);
    addPacket(&queue, 1, 1, 1, 2, 104, FLAG_CONTAINS_PIC_DATA, false, false, 0);
    uint64_t deadline = RtpvGetPendingFrameDeadlineUs(&queue);
    EXPECT(deadline != 0, "last block with critical loss in earlier block still arms timer");
    EXPECT(!RtpvExpirePendingFrame(&queue, deadline), "earlier critical hole blocks multiblock expiry");
    EXPECT(submittedPackets[1] == 0, "frame with earlier critical loss remains unsubmitted");
    RtpvCleanupQueue(&queue);
}

int main(void) {
    StreamConfig.packetSize = PACKET_SIZE;
    testCodecControls();
    testIntactFrameImmediate();
    testOptionalHoleExpiresWithoutSuccessor();
    testMissingEofCanExpire();
    testProgressExtendsDeadline();
    testReorderedRecoveryBeforeDeadline();
    testStaleDeadlineAfterCleanupAndWrap();
    testUnknownHeadersAndCriticalLossDoNotFlush();
    testCriticalCompleteAndOptionalMissingMultiblock();
    testMissingCriticalFromEarlierBlockBlocksExpiry();
    if (failures != 0) {
        fprintf(stderr, "%d queue test(s) failed\n", failures);
        return 1;
    }
    puts("PASS: RtpVideoQueue PyroWave expiry tests");
    return 0;
}





