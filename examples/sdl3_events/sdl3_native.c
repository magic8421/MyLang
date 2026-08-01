/* Native implementation for examples/sdl3_events/sdl3_events.my

   This file shows how to bridge an SDL3 C callback (SDL_AddEventWatch)
   back into MyLang by calling interface methods through the generated
   vtable.  Listeners are retained when added and released when removed
   so the MyLang objects stay alive as long as the native side holds them.
*/
#include "sdl3_events.h"
#include <SDL3/SDL.h>
#ifdef _WIN32
#include <windows.h>
#endif

#define MAX_LISTENERS 16

static int g_quit = 0;

static struct {
    ltk_EventListener listener;
    int active;
} g_listeners[MAX_LISTENERS];

static SRWLOCK g_lock = SRWLOCK_INIT;
static int g_watch_registered = 0;

#define EVENT_ENTRY(x) case x: pstr = #x; break
String* ltk_Application_eventTypeToString(ltk_Application* thiz, uint32_t type){
    const char* pstr = NULL;
    switch (type)
    {
        EVENT_ENTRY(SDL_EVENT_QUIT);
        EVENT_ENTRY(SDL_EVENT_TERMINATING);
        EVENT_ENTRY(SDL_EVENT_LOW_MEMORY);
        EVENT_ENTRY(SDL_EVENT_WILL_ENTER_BACKGROUND);
        EVENT_ENTRY(SDL_EVENT_DID_ENTER_BACKGROUND);
        EVENT_ENTRY(SDL_EVENT_WILL_ENTER_FOREGROUND);
        EVENT_ENTRY(SDL_EVENT_DID_ENTER_FOREGROUND);
        EVENT_ENTRY(SDL_EVENT_LOCALE_CHANGED);
        EVENT_ENTRY(SDL_EVENT_SYSTEM_THEME_CHANGED);
        EVENT_ENTRY(SDL_EVENT_DISPLAY_ORIENTATION);
        EVENT_ENTRY(SDL_EVENT_DISPLAY_ADDED);
        EVENT_ENTRY(SDL_EVENT_DISPLAY_REMOVED);
        EVENT_ENTRY(SDL_EVENT_DISPLAY_MOVED);
        EVENT_ENTRY(SDL_EVENT_DISPLAY_DESKTOP_MODE_CHANGED);
        EVENT_ENTRY(SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED);
        EVENT_ENTRY(SDL_EVENT_DISPLAY_CONTENT_SCALE_CHANGED);
        EVENT_ENTRY(SDL_EVENT_DISPLAY_USABLE_BOUNDS_CHANGED);
        EVENT_ENTRY(SDL_EVENT_WINDOW_SHOWN);
        EVENT_ENTRY(SDL_EVENT_WINDOW_HIDDEN);
        EVENT_ENTRY(SDL_EVENT_WINDOW_EXPOSED);
        EVENT_ENTRY(SDL_EVENT_WINDOW_MOVED);
        EVENT_ENTRY(SDL_EVENT_WINDOW_RESIZED);
        EVENT_ENTRY(SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED);
        EVENT_ENTRY(SDL_EVENT_WINDOW_METAL_VIEW_RESIZED);
        EVENT_ENTRY(SDL_EVENT_WINDOW_MINIMIZED);
        EVENT_ENTRY(SDL_EVENT_WINDOW_MAXIMIZED);
        EVENT_ENTRY(SDL_EVENT_WINDOW_RESTORED);
        EVENT_ENTRY(SDL_EVENT_WINDOW_MOUSE_ENTER);
        EVENT_ENTRY(SDL_EVENT_WINDOW_MOUSE_LEAVE);
        EVENT_ENTRY(SDL_EVENT_WINDOW_FOCUS_GAINED);
        EVENT_ENTRY(SDL_EVENT_WINDOW_FOCUS_LOST);
        EVENT_ENTRY(SDL_EVENT_WINDOW_CLOSE_REQUESTED);
        EVENT_ENTRY(SDL_EVENT_WINDOW_HIT_TEST);
        EVENT_ENTRY(SDL_EVENT_WINDOW_ICCPROF_CHANGED);
        EVENT_ENTRY(SDL_EVENT_WINDOW_DISPLAY_CHANGED);
        EVENT_ENTRY(SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED);
        EVENT_ENTRY(SDL_EVENT_WINDOW_SAFE_AREA_CHANGED);
        EVENT_ENTRY(SDL_EVENT_WINDOW_OCCLUDED);
        EVENT_ENTRY(SDL_EVENT_WINDOW_ENTER_FULLSCREEN);
        EVENT_ENTRY(SDL_EVENT_WINDOW_LEAVE_FULLSCREEN);
        EVENT_ENTRY(SDL_EVENT_WINDOW_DESTROYED);
        EVENT_ENTRY(SDL_EVENT_WINDOW_HDR_STATE_CHANGED);
        EVENT_ENTRY(SDL_EVENT_KEY_DOWN);
        EVENT_ENTRY(SDL_EVENT_KEY_UP);
        EVENT_ENTRY(SDL_EVENT_TEXT_EDITING);
        EVENT_ENTRY(SDL_EVENT_TEXT_INPUT);
        EVENT_ENTRY(SDL_EVENT_KEYMAP_CHANGED);
        EVENT_ENTRY(SDL_EVENT_KEYBOARD_ADDED);
        EVENT_ENTRY(SDL_EVENT_KEYBOARD_REMOVED);
        EVENT_ENTRY(SDL_EVENT_TEXT_EDITING_CANDIDATES);
        EVENT_ENTRY(SDL_EVENT_SCREEN_KEYBOARD_SHOWN);
        EVENT_ENTRY(SDL_EVENT_SCREEN_KEYBOARD_HIDDEN);
        EVENT_ENTRY(SDL_EVENT_MOUSE_MOTION);
        EVENT_ENTRY(SDL_EVENT_MOUSE_BUTTON_DOWN);
        EVENT_ENTRY(SDL_EVENT_MOUSE_BUTTON_UP);
        EVENT_ENTRY(SDL_EVENT_MOUSE_WHEEL);
        EVENT_ENTRY(SDL_EVENT_MOUSE_ADDED);
        EVENT_ENTRY(SDL_EVENT_MOUSE_REMOVED);
        EVENT_ENTRY(SDL_EVENT_JOYSTICK_AXIS_MOTION);
        EVENT_ENTRY(SDL_EVENT_JOYSTICK_BALL_MOTION);
        EVENT_ENTRY(SDL_EVENT_JOYSTICK_HAT_MOTION);
        EVENT_ENTRY(SDL_EVENT_JOYSTICK_BUTTON_DOWN);
        EVENT_ENTRY(SDL_EVENT_JOYSTICK_BUTTON_UP);
        EVENT_ENTRY(SDL_EVENT_JOYSTICK_ADDED);
        EVENT_ENTRY(SDL_EVENT_JOYSTICK_REMOVED);
        EVENT_ENTRY(SDL_EVENT_JOYSTICK_BATTERY_UPDATED);
        EVENT_ENTRY(SDL_EVENT_JOYSTICK_UPDATE_COMPLETE);
        EVENT_ENTRY(SDL_EVENT_GAMEPAD_AXIS_MOTION);
        EVENT_ENTRY(SDL_EVENT_GAMEPAD_BUTTON_DOWN);
        EVENT_ENTRY(SDL_EVENT_GAMEPAD_BUTTON_UP);
        EVENT_ENTRY(SDL_EVENT_GAMEPAD_ADDED);
        EVENT_ENTRY(SDL_EVENT_GAMEPAD_REMOVED);
        EVENT_ENTRY(SDL_EVENT_GAMEPAD_REMAPPED);
        EVENT_ENTRY(SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN);
        EVENT_ENTRY(SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION);
        EVENT_ENTRY(SDL_EVENT_GAMEPAD_TOUCHPAD_UP);
        EVENT_ENTRY(SDL_EVENT_GAMEPAD_SENSOR_UPDATE);
        EVENT_ENTRY(SDL_EVENT_GAMEPAD_UPDATE_COMPLETE);
        EVENT_ENTRY(SDL_EVENT_GAMEPAD_STEAM_HANDLE_UPDATED);
        EVENT_ENTRY(SDL_EVENT_FINGER_DOWN);
        EVENT_ENTRY(SDL_EVENT_FINGER_UP);
        EVENT_ENTRY(SDL_EVENT_FINGER_MOTION);
        EVENT_ENTRY(SDL_EVENT_FINGER_CANCELED);
        EVENT_ENTRY(SDL_EVENT_PINCH_BEGIN);
        EVENT_ENTRY(SDL_EVENT_PINCH_UPDATE);
        EVENT_ENTRY(SDL_EVENT_PINCH_END);
        EVENT_ENTRY(SDL_EVENT_CLIPBOARD_UPDATE);
        EVENT_ENTRY(SDL_EVENT_DROP_FILE);
        EVENT_ENTRY(SDL_EVENT_DROP_TEXT);
        EVENT_ENTRY(SDL_EVENT_DROP_BEGIN);
        EVENT_ENTRY(SDL_EVENT_DROP_COMPLETE);
        EVENT_ENTRY(SDL_EVENT_DROP_POSITION);
        EVENT_ENTRY(SDL_EVENT_AUDIO_DEVICE_ADDED);
        EVENT_ENTRY(SDL_EVENT_AUDIO_DEVICE_REMOVED);
        EVENT_ENTRY(SDL_EVENT_AUDIO_DEVICE_FORMAT_CHANGED);
        EVENT_ENTRY(SDL_EVENT_SENSOR_UPDATE);
        EVENT_ENTRY(SDL_EVENT_PEN_PROXIMITY_IN);
        EVENT_ENTRY(SDL_EVENT_PEN_PROXIMITY_OUT);
        EVENT_ENTRY(SDL_EVENT_PEN_DOWN);
        EVENT_ENTRY(SDL_EVENT_PEN_UP);
        EVENT_ENTRY(SDL_EVENT_PEN_BUTTON_DOWN);
        EVENT_ENTRY(SDL_EVENT_PEN_BUTTON_UP);
        EVENT_ENTRY(SDL_EVENT_PEN_MOTION);
        EVENT_ENTRY(SDL_EVENT_PEN_AXIS);
        EVENT_ENTRY(SDL_EVENT_CAMERA_DEVICE_ADDED);
        EVENT_ENTRY(SDL_EVENT_CAMERA_DEVICE_REMOVED);
        EVENT_ENTRY(SDL_EVENT_CAMERA_DEVICE_APPROVED);
        EVENT_ENTRY(SDL_EVENT_CAMERA_DEVICE_DENIED);
        EVENT_ENTRY(SDL_EVENT_RENDER_TARGETS_RESET);
        EVENT_ENTRY(SDL_EVENT_RENDER_DEVICE_RESET);
        EVENT_ENTRY(SDL_EVENT_RENDER_DEVICE_LOST);
        EVENT_ENTRY(SDL_EVENT_PRIVATE0);
        EVENT_ENTRY(SDL_EVENT_PRIVATE1);
        EVENT_ENTRY(SDL_EVENT_PRIVATE2);
        EVENT_ENTRY(SDL_EVENT_PRIVATE3);
        EVENT_ENTRY(SDL_EVENT_POLL_SENTINEL);
        EVENT_ENTRY(SDL_EVENT_USER);
        EVENT_ENTRY(SDL_EVENT_LAST);
        EVENT_ENTRY(SDL_EVENT_ENUM_PADDING);

    default:
        pstr = "Unknown type";
    }

    String* str = mylang_string_new(MYLANG_TID_String, pstr);
    return str;
}
#undef EVENT_ENTRY

int32_t ltk_Application_init(ltk_Application* thiz) {
    (void)thiz;
#ifdef _WIN32
    /* MyLang strings are UTF-8 bytes end to end (source literals and SDL's
       event.text.text alike); switch the console code page once here instead
       of transcoding at every boundary. */
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
    return SDL_Init(SDL_INIT_VIDEO) ? 0 : 1;
}

ltk_Window* ltk_Application_createWindow(ltk_Application* thiz, int32_t w, int32_t h) {
    (void)thiz;
    SDL_Window* handle = SDL_CreateWindow("MyLang SDL3 events", w, h, 0);
    ltk_Window *win = mylang_new_object(sizeof(ltk_Window), MYLANG_TID_ltk_Window, _mylang_dtor_ltk_Window);
    win->handle = (int64_t)handle;
    win->window_id = SDL_GetWindowID(handle);
    SDL_StartTextInput(handle);
    mylang_array_push(&thiz->windows, sizeof(void*), 2, &win);
    return win;
}

void ltk_Application_pump(ltk_Application* thiz) {
    (void)thiz;
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_EVENT_QUIT:
                g_quit = 1;
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                {
                    ltk_MouseEvent ev = { 0 };
                    ev.window_id = event.button.windowID;
                    ev.button = event.button.button;
                    ev.x = event.button.x;
                    ev.y = event.button.y;
                    ltk_Application_onMouseEvent(thiz, &ev);
                }
                break;
            case SDL_EVENT_MOUSE_WHEEL:
                {
                    ltk_MouseEvent ev = { 0 };
                    ev.window_id = event.wheel.windowID;
                    ev.wheel_x = event.wheel.integer_x;
                    ev.wheel_y = event.wheel.integer_y;
                    ev.x = event.wheel.x;
                    ev.y = event.wheel.y;
                    ltk_Application_onMouseEvent(thiz, &ev);
                }
                break;
            case SDL_EVENT_KEY_DOWN:
                {
                    ltk_KeyEvent ev = {0};
                    ev.window_id = event.key.windowID;
                    ev.key = event.key.key;
                    ltk_Application_onKeyEvent(thiz, &ev);
                }
                break;
            case SDL_EVENT_TEXT_INPUT:
                {
                    ltk_TextInputEvent ev = {0};
                    ev.window_id = event.text.windowID;
                    ev.text = mylang_string_new(MYLANG_TID_String, event.text.text);
                    ltk_Application_onTextInput(thiz, &ev);
                }
                break;
            default:
                break;
        }
    }
}

int32_t ltk_Application_shouldQuit(ltk_Application* thiz) {
    (void)thiz;
    return g_quit;
}

void ltk_Application_delay(ltk_Application* thiz, int32_t ms) {
    (void)thiz;
    SDL_Delay((Uint32)ms);
}

void ltk_Application_destroyWindow(ltk_Application* thiz, ltk_Window* win) {
    (void)thiz;
    SDL_DestroyWindow((SDL_Window *)win->handle);
}

void ltk_Application_quit(ltk_Application* thiz) {
    (void)thiz;
    SDL_Quit();
}
