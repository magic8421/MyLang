/* Native implementation for examples/sdl3_window/sdl3_window.my */
#include "sdl3_window.h"
#include <SDL3/SDL.h>

static int g_quit = 0;

int32_t MySdl_init(MySdl* thiz) {
    /* SDL3: SDL_Init returns true on success. */
    return SDL_Init(SDL_INIT_VIDEO) ? 0 : 1;
}

uint64_t MySdl_createWindow(MySdl* thiz, int32_t w, int32_t h) {
    SDL_Window* win = SDL_CreateWindow("Hello from MyLang native", w, h, 0);
    return (uint64_t)win;
}

void MySdl_pump(MySdl* thiz) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_EVENT_QUIT) {
            g_quit = 1;
        }
    }
}

int32_t MySdl_shouldQuit(MySdl* thiz) {
    return g_quit;
}

void MySdl_delay(MySdl* thiz, int32_t ms) {
    SDL_Delay((Uint32)ms);
}

void MySdl_destroyWindow(MySdl* thiz, uint64_t win) {
    SDL_DestroyWindow((SDL_Window*)win);
}

void MySdl_quit(MySdl* thiz) {
    SDL_Quit();
}
