/* Native implementation for examples/sdl3_events/sdl3_events.my

   This file shows how to bridge an SDL3 C callback (SDL_AddEventWatch)
   back into MyLang by calling interface methods through the generated
   vtable.  Listeners are retained when added and released when removed
   so the MyLang objects stay alive as long as the native side holds them.
*/
#include "sdl3_events.h"
#include <SDL3/SDL.h>

#define MAX_LISTENERS 16

static SDL_Window* g_window = NULL;
static int g_quit = 0;

static struct {
    SdlEventListener listener;
    int active;
} g_listeners[MAX_LISTENERS];

static SRWLOCK g_lock = SRWLOCK_INIT;
static int g_watch_registered = 0;

static bool SDLCALL sdl_event_filter(void* userdata, SDL_Event* event) {
    (void)userdata;

    SdlEvent ev;
    ev.type = (int32_t)event->type;
    ev.code = 0;

    switch (event->type) {
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
            ev.code = (int32_t)event->key.key;
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            ev.code = (int32_t)event->button.button;
            break;
        case SDL_EVENT_WINDOW_RESIZED:
            ev.code = (int32_t)event->window.data1;
            break;
        default:
            break;
    }

    if (event->type == SDL_EVENT_QUIT) {
        g_quit = 1;
    }

    AcquireSRWLockShared(&g_lock);
    int i;
    for (i = 0; i < MAX_LISTENERS; i++) {
        if (g_listeners[i].active) {
            SdlEventListener* l = &g_listeners[i].listener;
            l->vtable->onEvent(l->data, ev);
        }
    }
    ReleaseSRWLockShared(&g_lock);

    return true;
}

int32_t SdlApp_init(SdlApp* thiz) {
    (void)thiz;
    return SDL_Init(SDL_INIT_VIDEO) ? 0 : 1;
}

uint64_t SdlApp_createWindow(SdlApp* thiz, int32_t w, int32_t h) {
    (void)thiz;
    g_window = SDL_CreateWindow("MyLang SDL3 events", w, h, 0);
    return (uint64_t)g_window;
}

void SdlApp_addListener(SdlApp* thiz, SdlEventListener l) {
    (void)thiz;

    AcquireSRWLockExclusive(&g_lock);

    int i;
    for (i = 0; i < MAX_LISTENERS; i++) {
        if (!g_listeners[i].active) {
            mylang_retain(l.data);
            g_listeners[i].listener = l;
            g_listeners[i].active = 1;
            break;
        }
    }

    if (!g_watch_registered) {
        SDL_AddEventWatch(sdl_event_filter, NULL);
        g_watch_registered = 1;
    }

    ReleaseSRWLockExclusive(&g_lock);
}

void SdlApp_removeListener(SdlApp* thiz, SdlEventListener l) {
    (void)thiz;

    AcquireSRWLockExclusive(&g_lock);

    int i;
    int remaining = 0;
    for (i = 0; i < MAX_LISTENERS; i++) {
        if (g_listeners[i].active && g_listeners[i].listener.data == l.data) {
            mylang_release(g_listeners[i].listener.data);
            g_listeners[i].active = 0;
        }
        if (g_listeners[i].active) {
            remaining++;
        }
    }

    if (g_watch_registered && remaining == 0) {
        SDL_RemoveEventWatch(sdl_event_filter, NULL);
        g_watch_registered = 0;
    }

    ReleaseSRWLockExclusive(&g_lock);
}

void SdlApp_pump(SdlApp* thiz) {
    (void)thiz;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        /* The registered event watch already dispatched the event to
           listeners.  This loop just drains the queue. */
    }
}

int32_t SdlApp_shouldQuit(SdlApp* thiz) {
    (void)thiz;
    return g_quit;
}

void SdlApp_delay(SdlApp* thiz, int32_t ms) {
    (void)thiz;
    SDL_Delay((Uint32)ms);
}

void SdlApp_destroyWindow(SdlApp* thiz, uint64_t win) {
    (void)thiz;
    (void)win;
    SDL_DestroyWindow(g_window);
    g_window = NULL;
}

void SdlApp_quit(SdlApp* thiz) {
    (void)thiz;
    SDL_Quit();
}
