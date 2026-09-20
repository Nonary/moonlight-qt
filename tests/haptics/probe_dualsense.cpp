// Local physical-output probe. Uses the production waveform renderer.
#include "../../app/streaming/input/dualsensehaptics.cpp"
#include <cmath>
#include <cstdio>
#include <cstring>

static void pump(unsigned ms) {
    const auto end = SDL_GetTicks() + ms;
    while (!SDL_TICKS_PASSED(SDL_GetTicks(), end)) {
        SDL_PumpEvents();
        SDL_Delay(5);
    }
}
int main(int argc, char** argv) {
    setbuf(stdout, nullptr);
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE, "1");
    SDL_SetHint("SDL_JOYSTICK_ENHANCED_REPORTS", "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (SDL_Init(SDL_INIT_GAMECONTROLLER) != 0) { puts(SDL_GetError()); return 1; }
    SDL_GameController* pad = nullptr;
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        auto* candidate = SDL_GameControllerOpen(i);
        if (!candidate) continue;
        printf("Device %d: %s type=%d path=%s serial=%s\n", i,
               SDL_GameControllerName(candidate), SDL_GameControllerGetType(candidate),
               SDL_GameControllerPath(candidate), SDL_GameControllerGetSerial(candidate));
        if (!pad && SDL_GameControllerGetType(candidate) == SDL_CONTROLLER_TYPE_PS5) pad = candidate;
        else SDL_GameControllerClose(candidate);
    }
    if (!pad) { SDL_Quit(); return 2; }
    const char* mode = argc > 1 ? argv[1] : "enumerate";
    if (!strcmp(mode, "rumble")) {
        puts("RUMBLE start: two one-second pulses");
        for (int i = 0; i < 2; ++i) {
            printf("SDL rumble result=%d\n", SDL_GameControllerRumble(pad, 32768, 32768, 1000));
            pump(1100);
            SDL_GameControllerRumble(pad, 0, 0, 0);
            pump(700);
        }
    }
    else if (!strcmp(mode, "trigger")) {
        uint8_t report[47] {};
        report[0] = 0x0c;
        report[10] = report[21] = 0x01;
        report[11] = report[22] = 48;
        report[12] = report[23] = 96;
        puts("TRIGGER start: squeeze both triggers during the next 8 seconds");
        printf("SDL effect result=%d\n", SDL_GameControllerSendEffect(pad, report, sizeof(report)));
        pump(8000);
        memset(report, 0, sizeof(report)); report[0] = 0x0c; report[10] = report[21] = 0x05;
        printf("SDL clear result=%d\n", SDL_GameControllerSendEffect(pad, report, sizeof(report)));
        pump(250);
    }
    else if (!strcmp(mode, "wave")) {
        if (!DualSenseHaptics::attach(0, pad)) { puts("Waveform attach failed"); SDL_GameControllerClose(pad); SDL_Quit(); return 3; }
        puts("WAVE start: 120 Hz left for 2 seconds, then right for 2 seconds");
        auto deadline = Clock::now();
        for (unsigned packet = 0; packet < 800; ++packet) {
            uint8_t samples[960] {};
            for (unsigned f = 0; f < 240; ++f) {
                int16_t tone = int16_t(16000 * sin((packet * 240 + f) * 6.283185307179586 * 120 / 48000));
                MlHapticsWrite16(samples + f * 4 + (packet < 400 ? 0 : 2), uint16_t(tone));
            }
            DualSenseHaptics::receive(0, packet, samples, 240);
            SDL_PumpEvents();
            deadline += std::chrono::milliseconds(5);
            std::this_thread::sleep_until(deadline);
        }
        pump(150);
        DualSenseHaptics::detach(0);
    }
    SDL_GameControllerRumble(pad, 0, 0, 0);
    pump(100);
    SDL_GameControllerClose(pad);
    SDL_Quit();
    puts("DONE");
}
