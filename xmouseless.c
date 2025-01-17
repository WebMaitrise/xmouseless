/*
 * xmouseless
*/
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/XKBlib.h>
#include <X11/extensions/XTest.h>

#define LENGTH(X) (sizeof X / sizeof X[0])

typedef struct
{
    KeySym keysym;
    float x;
    float y;
} MoveBinding;

typedef struct
{
    KeySym keysym;
    unsigned int button;
} ClickBinding;

typedef struct
{
    KeySym keysym;
    float x;
    float y;
} ScrollBinding;

typedef struct
{
    KeySym keysym;
    unsigned int speed;
} SpeedBinding;

typedef struct
{
    KeySym keysym;
    char *command;
} ShellBinding;

/* load configuration */
#include "config.h"

Display *dpy;
Window root;
pthread_t movethread;

struct mode
{
    Bool active;
    Bool secondary_role;
} mode_default = {False, False};

typedef struct mode Mode;

struct movement
{
    float x;
    float y;
    float speed_x;
    float speed_y;
};

typedef struct movement Movement;

static unsigned int speed;
static Mode arrow_mode, mouse_mode;
static Movement mouseinfo, scrollinfo;
static Bool control = False;
static Bool shift = False;

void get_pointer();
void move_relative(float x, float y);
void click(unsigned int button, Bool is_press);
void click_full(unsigned int button);
void scroll(float x, float y);
void handle_key(KeyCode keycode, Bool is_press);
void init_x();
void close_x();
void listen_key(KeySym key);
void stop_listen_key(KeySym key);
void tap(KeySym key);
void define_secondary_role(KeySym pressed_key, KeySym key, Bool is_press, Mode *mode);
void define_modifier(KeySym pressed_key, KeySym key, Bool is_press, Bool *modifier);

void get_pointer()
{
    int x, y;
    int di;
    unsigned int dui;
    Window dummy;
    XQueryPointer(dpy, root, &dummy, &dummy, &x, &y, &di, &di, &dui);
    mouseinfo.x = x;
    mouseinfo.y = y;
}

void move_relative(float x, float y)
{
    mouseinfo.x += x;
    mouseinfo.y += y;
    XWarpPointer(dpy, None, root, 0, 0, 0, 0,
                 (int)mouseinfo.x, (int)mouseinfo.y);
    XFlush(dpy);
}

void click(unsigned int button, Bool is_press)
{
    XTestFakeButtonEvent(dpy, button, is_press, CurrentTime);
    XFlush(dpy);
}

void click_full(unsigned int button)
{
    XTestFakeButtonEvent(dpy, button, 1, CurrentTime);
    XTestFakeButtonEvent(dpy, button, 0, CurrentTime);
    XFlush(dpy);
}

void scroll(float x, float y)
{
    scrollinfo.x += x;
    scrollinfo.y += y;
    while (scrollinfo.y <= -0.51)
    {
        scrollinfo.y += 1;
        click_full(4);
    }
    while (scrollinfo.y >= 0.51)
    {
        scrollinfo.y -= 1;
        click_full(5);
    }
    while (scrollinfo.x <= -0.51)
    {
        scrollinfo.x += 1;
        click_full(6);
    }
    while (scrollinfo.x >= 0.51)
    {
        scrollinfo.x -= 1;
        click_full(7);
    }
}

void init_x()
{
    int screen;

    /* initialize support for concurrent threads */
    XInitThreads();

    dpy = XOpenDisplay((char *)0);
    screen = DefaultScreen(dpy);
    root = RootWindow(dpy, screen);

    /* turn auto key repeat off */
    XAutoRepeatOff(dpy);

    listen_key(XK_d);
    listen_key(XK_space);
    listen_key(XK_End);
}

void listen_key(KeySym key)
{
    KeyCode key_code = XKeysymToKeycode(dpy, key);

    XGrabKey(dpy, key_code, 0, root, True, GrabModeAsync, GrabModeAsync);
}

void stop_listen_key(KeySym key)
{
    KeyCode key_code = XKeysymToKeycode(dpy, key);

    XUngrabKey(dpy, key_code, 0, root);
}

void tap(KeySym key)
{
    KeyCode code = XKeysymToKeycode(dpy, key);

    XTestFakeKeyEvent(dpy, code, True, CurrentTime);
    XTestFakeKeyEvent(dpy, code, False, CurrentTime);
}

void define_secondary_role(KeySym pressed_key, KeySym key, Bool is_press, Mode *mode)
{
    if ((*mode).active && pressed_key != key)
        (*mode).secondary_role = True;

    if (pressed_key == key)
    {
        if (!is_press)
        {
            if ((*mode).active && !(*mode).secondary_role)
            {
                stop_listen_key(key);
                tap(key);
                listen_key(key);
            }
        }

        (*mode).active = is_press;
        (*mode).secondary_role = False;

        if ((*mode).active)
        {
            listen_key(XK_Control_L);
            listen_key(XK_Shift_L);
        }
        else
        {
            control = False;
            shift = False;
            stop_listen_key(XK_Control_L);
            stop_listen_key(XK_Shift_L);
        }
    }
}

void define_modifier(KeySym pressed_key, KeySym key, Bool is_press, Bool *modifier)
{
    if (pressed_key == key)
        *modifier = is_press;
}

void close_x(int exit_status)
{
    /* turn auto repeat on again */
    XAutoRepeatOn(dpy);
    XUngrabKey(dpy, AnyKey, AnyModifier, root);
    XCloseDisplay(dpy);
    exit(exit_status);
}

void *move_forever(void *val)
{
    /* this function is executed in a seperate thread */
    while (1)
    {
        if (!mouse_mode.active)
        {
            mouseinfo.speed_x = 0;
            mouseinfo.speed_y = 0;

            scrollinfo.speed_x = 0;
            scrollinfo.speed_y = 0;
        }

        /* move mouse? */
        if (mouseinfo.speed_x != 0 || mouseinfo.speed_y != 0)
        {
            move_relative((float)mouseinfo.speed_x * speed / move_rate,
                          (float)mouseinfo.speed_y * speed / move_rate);
        }
        /* scroll? */
        if (scrollinfo.speed_x != 0 || scrollinfo.speed_y != 0)
        {
            scroll((float)scrollinfo.speed_x / move_rate,
                   (float)scrollinfo.speed_y / move_rate);
        }
        usleep(1000000 / move_rate);
    }
}

void handle_key(KeyCode keycode, Bool is_press)
{
    unsigned int i;
    KeySym keysym;

    keysym = XkbKeycodeToKeysym(dpy, keycode, 0, 0);

    define_modifier(keysym, XK_Control_L, is_press, &control);
    define_modifier(keysym, XK_Shift_L, is_press, &shift);

    define_secondary_role(keysym, XK_d, is_press, &mouse_mode);
    define_secondary_role(keysym, XK_space, is_press, &arrow_mode);

    if (keysym == XK_Control_L)
        printf("Control_L\n");

    if (keysym == XK_Shift_L)
        printf("Shift_L\n");

    if (arrow_mode.active)
    {
        Bool is_arrow = keysym == XK_i || keysym == XK_j || keysym == XK_k || keysym == XK_l;

        if (is_press && is_arrow)
        {
            stop_listen_key(XK_space);

            KeyCode code = XKeysymToKeycode(dpy, XK_space);
            KeyCode shift_key = XKeysymToKeycode(dpy, XK_Shift_L);
            KeyCode control_key = XKeysymToKeycode(dpy, XK_Control_L);

            XTestFakeKeyEvent(dpy, code, False, CurrentTime);

            Bool is_shift = shift;
            Bool is_control = control;

            if (is_shift)
            {
                stop_listen_key(XK_Shift_L);

                XTestFakeKeyEvent(dpy, shift_key, False, CurrentTime);
                XTestFakeKeyEvent(dpy, shift_key, True, CurrentTime);
            }

            if (is_control)
            {
                stop_listen_key(XK_Control_L);

                XTestFakeKeyEvent(dpy, control_key, False, CurrentTime);
                XTestFakeKeyEvent(dpy, control_key, True, CurrentTime);
            }

            if (keysym == XK_i)
                tap(XK_Up);
            if (keysym == XK_j)
                tap(XK_Left);
            if (keysym == XK_k)
                tap(XK_Down);
            if (keysym == XK_l)
                tap(XK_Right);

            XTestFakeKeyEvent(dpy, control_key, False, CurrentTime);
            XTestFakeKeyEvent(dpy, shift_key, False, CurrentTime);

            listen_key(XK_Shift_L);
            listen_key(XK_Control_L);
            listen_key(XK_space);

            XTestFakeKeyEvent(dpy, code, True, CurrentTime);

            if (is_shift)
            {
                XTestFakeKeyEvent(dpy, shift_key, False, CurrentTime);
                listen_key(XK_Shift_L);
                XTestFakeKeyEvent(dpy, shift_key, True, CurrentTime);
            }

            if (is_control)
            {
                XTestFakeKeyEvent(dpy, control_key, False, CurrentTime);
                listen_key(XK_Control_L);
                XTestFakeKeyEvent(dpy, control_key, True, CurrentTime);
            }
        }
    }

    if (mouse_mode.active)
    {
        /* move bindings */
        for (i = 0; i < LENGTH(move_bindings); i++)
        {
            if (move_bindings[i].keysym == keysym)
            {
                int sign = is_press ? 1 : -1;
                mouseinfo.speed_x += sign * move_bindings[i].x;
                mouseinfo.speed_y += sign * move_bindings[i].y;
            }
        }

        /* click bindings */
        for (i = 0; i < LENGTH(click_bindings); i++)
        {
            if (click_bindings[i].keysym == keysym)
            {
                click(click_bindings[i].button, is_press);
                printf("click: %i %i\n", click_bindings[i].button, is_press);
            }
        }

        /* speed bindings */
        for (i = 0; i < LENGTH(speed_bindings); i++)
        {
            if (speed_bindings[i].keysym == keysym)
            {
                speed = is_press ? speed_bindings[i].speed : default_speed;
                printf("speed: %i\n", speed);
            }
        }

        /* scroll bindings */
        for (i = 0; i < LENGTH(scroll_bindings); i++)
        {
            if (scroll_bindings[i].keysym == keysym)
            {
                int sign = is_press ? 1 : -1;
                scrollinfo.speed_x += sign * scroll_bindings[i].x;
                scrollinfo.speed_y += sign * scroll_bindings[i].y;
            }
        }
    }

    /* shell and exit bindings only on key release */
    if (!is_press)
    {
        /* shell bindings */
        // for (i = 0; i < LENGTH(shell_bindings); i++)
        // {
        //     if (shell_bindings[i].keysym == keysym)
        //     {
        //         printf("executing: %s\n", shell_bindings[i].command);
        //         if (fork() == 0)
        //         {
        //             system(shell_bindings[i].command);
        //             exit(EXIT_SUCCESS);
        //         }
        //     }
        // }

        /* exit */
        for (i = 0; i < LENGTH(exit_keys); i++)
        {
            if (exit_keys[i] == keysym)
            {
                close_x(EXIT_SUCCESS);
            }
        }
    }
}

int main()
{
    char keys_return[32];
    int rc;
    int i, j;

    init_x();

    get_pointer();
    mouseinfo.speed_x = 0;
    mouseinfo.speed_y = 0;
    speed = default_speed;

    scrollinfo.x = 0;
    scrollinfo.y = 0;
    scrollinfo.speed_x = 0;
    scrollinfo.speed_y = 0;

    /* start the thread for mouse movement and scrolling */
    rc = pthread_create(&movethread, NULL, &move_forever, NULL);
    if (rc != 0)
    {
        printf("Unable to start thread.\n");
        return EXIT_FAILURE;
    }

    /* get the initial state of all keys */
    XQueryKeymap(dpy, keys_return);
    for (i = 0; i < 32; i++)
    {
        for (j = 0; j < 8; j++)
        {
            if (keys_return[i] & (1 << j))
            {
                handle_key(8 * i + j, 1);
            }
        }
    }

    /* handle key presses and releases */
    while (1)
    {
        XEvent event;
        XNextEvent(dpy, &event);

        switch (event.type)
        {
        case KeyPress:
        case KeyRelease:
            get_pointer();
            handle_key(event.xkey.keycode, event.xkey.type == KeyPress);
            break;
        }
    }
}
