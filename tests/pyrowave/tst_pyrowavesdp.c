#include "Limelight-internal.h"
#include "ControllerSpeakerPcm.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Exercise the production SDP generator without starting a connection.
int AppVersionQuad[4] = {7, 1, 431, -1};
STREAM_CONFIGURATION StreamConfig;
CONNECTION_LISTENER_CALLBACKS ListenerCallbacks;
DECODER_RENDERER_CALLBACKS VideoCallbacks;
AUDIO_RENDERER_CALLBACKS AudioCallbacks;
ConnListenerControllerSpeakerPcm ControllerSpeakerPcmCallback;
int NegotiatedVideoFormat;
struct sockaddr_storage RemoteAddr;
uint16_t RtspPortNumber = 48010;
uint16_t VideoPortNumber = 47998;
uint32_t EncryptionFeaturesSupported;
uint32_t EncryptionFeaturesRequested;
uint32_t EncryptionFeaturesEnabled;
bool AudioEncryptionEnabled;
bool HighQualitySurroundSupported;
bool HighQualitySurroundEnabled;
int AudioPacketDuration;

bool PltSafeStrcpy(char* dest, size_t size, const char* src) {
    if (strlen(src) >= size) return false;
    memcpy(dest, src, strlen(src) + 1);
    return true;
}

void addrToUrlSafeString(struct sockaddr_storage* addr, char* string, size_t size) {
    (void)addr;
    if (!PltSafeStrcpy(string, size, "127.0.0.1")) abort();
}

bool isReferenceFrameInvalidationSupportedByDecoder(void) { return false; }

static int failures;

#define EXPECT(condition, description) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s (line %d)\n", description, __LINE__); \
        failures++; \
    } \
} while (0)

static char* generateSdp(int format, int compression, int linkMbps) {
    int length = 0;
    memset(&StreamConfig, 0, sizeof(StreamConfig));
    StreamConfig.width = 2560;
    StreamConfig.height = 1440;
    StreamConfig.fps = 120;
    StreamConfig.bitrate = 800000;
    StreamConfig.packetSize = 1392;
    StreamConfig.streamingRemotely = STREAM_CFG_LOCAL;
    StreamConfig.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
    StreamConfig.pyrowaveCompression = compression;
    StreamConfig.pyrowaveLinkMbps = linkMbps;
    NegotiatedVideoFormat = format;
    RemoteAddr.ss_family = AF_INET;
    AudioEncryptionEnabled = false;
    EncryptionFeaturesEnabled = 0;

    char* sdp = getSdpPayloadForStreamConfig(14, &length);
    EXPECT(sdp != NULL && length > 0, "SDP generation succeeds");
    return sdp;
}

static void speakerCallback(uint16_t controller, uint64_t owner,
                            uint32_t generation, uint32_t sequence,
                            uint64_t position, uint16_t flags,
                            const uint8_t* pcm, uint16_t frames) {
    (void)controller; (void)owner; (void)generation; (void)sequence;
    (void)position; (void)flags; (void)pcm; (void)frames;
    EXPECT(false, "SDP generation must not invoke the speaker renderer");
}

static void checkSpeakerNegotiation(void) {
    const char* attribute = "a=x-ml-general.featureFlags:";
    const uint32_t baseFlags = ML_FF_FEC_STATUS | ML_FF_SESSION_ID_V1;
    for (int enabled = 0; enabled < 3; enabled++) {
        // Exercise registration and subsequent removal, not just zero startup state.
        ControllerSpeakerPcmCallback = enabled == 1 ? speakerCallback : NULL;
        char* sdp = generateSdp(VIDEO_FORMAT_H264, 0, 1000);
        if (sdp == NULL) continue;
        const char* line = strstr(sdp, attribute);
        uint32_t flags = 0;
        EXPECT(line != NULL && sscanf(line + strlen(attribute), "%" SCNu32, &flags) == 1,
               "Sunshine SDP contains parseable client capabilities");
        EXPECT(flags == (baseFlags | (enabled == 1 ? ML_FF_CONTROLLER_SPEAKER_PCM : 0)),
               "Speaker capability follows renderer registration without changing other flags");
        free(sdp);
    }
    AppVersionQuad[3] = 0;
    ControllerSpeakerPcmCallback = speakerCallback;
    char* sdp = generateSdp(VIDEO_FORMAT_H264, 0, 1000);
    if (sdp != NULL) {
        EXPECT(strstr(sdp, attribute) == NULL,
               "Non-Sunshine hosts do not receive Sunshine speaker capabilities");
        free(sdp);
    }
    AppVersionQuad[3] = -1;
    ControllerSpeakerPcmCallback = NULL;
}

int main(void) {
    const int pyrowaveFormats[] = {
        VIDEO_FORMAT_PYROWAVE, VIDEO_FORMAT_PYROWAVE_444,
        VIDEO_FORMAT_PYROWAVE_HDR10, VIDEO_FORMAT_PYROWAVE_HDR10_444
    };
    const int fallbackFormats[] = {VIDEO_FORMAT_H264, VIDEO_FORMAT_H265, VIDEO_FORMAT_AV1_MAIN8};
    unsigned int i;

    checkSpeakerNegotiation();

    for (i = 0; i < sizeof(pyrowaveFormats) / sizeof(pyrowaveFormats[0]); i++) {
        char* normal = generateSdp(pyrowaveFormats[i], 0, 1000);
        if (normal == NULL) continue;
        EXPECT(strstr(normal, "a=x-ss-video[0].pyrowaveFeatures:1 \r\n") != NULL,
               "Normal PyroWave advertises only record framing");
        EXPECT(strstr(normal, "pyrowaveCompression") == NULL,
               "Normal PyroWave does not request compression transport");
        EXPECT(strstr(normal, "a=x-ss-video[0].pyrowaveLinkMbps:1000 \r\n") != NULL,
               "PyroWave sends the receiver link speed for host pacing");
        free(normal);

        char* compression = generateSdp(pyrowaveFormats[i], 1, 1000);
        if (compression == NULL) continue;
        EXPECT(strstr(compression, "a=x-ss-video[0].pyrowaveFeatures:9 \r\n") != NULL,
               "PyroWave compression advertises record framing and compression support");
        EXPECT(strstr(compression, "a=x-ss-video[0].pyrowaveCompression:1 \r\n") != NULL,
               "PyroWave compression explicitly opts in");
        EXPECT(strstr(compression, "a=x-nv-vqos[0].bitStreamFormat:3 \r\n") != NULL,
               "Compressed keeps the negotiated PyroWave codec");
        free(compression);

        char* unknownLink = generateSdp(pyrowaveFormats[i], 0, 0);
        if (unknownLink == NULL) continue;
        EXPECT(strstr(unknownLink, "pyrowaveLinkMbps") == NULL,
               "An unknown receiver link speed is not advertised");
        free(unknownLink);
    }

    for (i = 0; i < sizeof(fallbackFormats) / sizeof(fallbackFormats[0]); i++) {
        char* fallback = generateSdp(fallbackFormats[i], 1, 1000);
        if (fallback == NULL) continue;
        EXPECT(strstr(fallback, "pyrowaveCompression") == NULL,
               "A non-PyroWave codec never opts into compression transport");
        EXPECT(strstr(fallback, "pyrowaveFeatures") == NULL,
               "A non-PyroWave codec never advertises PyroWave features");
        EXPECT(strstr(fallback, "pyrowaveLinkMbps") == NULL,
               "Other codecs do not send PyroWave pacing attributes");
        free(fallback);
    }

    if (failures != 0) return 1;
    puts("PyroWave compression SDP negotiation checks passed");
    return 0;
}
