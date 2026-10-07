// Entry point: service init, SDL window/renderer, then App::run().
#include <SDL.h>
#include <SDL_image.h>
#include <SDL_ttf.h>

#include <cstdio>
#include <cstdlib>

#include "app/app.hpp"
#include "app/platform.hpp"

#ifdef __SWITCH__
#include <switch.h>
#endif

int main(int, char**) {
    platform::init();
    net::globalInit();

    int w = 1280, h = 720;
#ifdef __SWITCH__
    if (appletGetOperationMode() == AppletOperationMode_Console) {
        w = 1920;
        h = 1080;
    }
#else
    if (getenv("AI_SWITCH_DOCKED")) {  // desktop preview of the docked 1080p layout
        w = 1920;
        h = 1080;
    }
#endif
    if (SDL_Init(SDL_INIT_VIDEO) < 0 || TTF_Init() < 0) {
        std::printf("SDL init failed: %s\n", SDL_GetError());
        return 1;
    }
    IMG_Init(IMG_INIT_JPG);  // screenshot thumbnails
    SDL_StopTextInput();  // we use swkbd directly; SDL text input would also swallow touch
    // RESIZABLE lets the SDL Switch port switch between 720p and 1080p on dock changes.
    SDL_Window* win = SDL_CreateWindow("Claude (Unofficial)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h,
                                       SDL_WINDOW_RESIZABLE);
    SDL_Renderer* ren =
        win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC) : nullptr;
    if (!ren) {
        std::printf("window/renderer failed: %s\n", SDL_GetError());
        return 1;
    }

    {
        App app(win, ren);
        if (app.init())
            app.run();
        else
            std::printf("init failed (fonts missing from romfs?)\n");
    }  // App (and its worker thread) shut down before SDL and sockets

    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    IMG_Quit();
    TTF_Quit();
    SDL_Quit();
    net::globalCleanup();
    platform::shutdown();
    return 0;
}
