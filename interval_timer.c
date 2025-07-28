#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <fcntl.h>
#include <sys/select.h>
#include <math.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/extensions/dpms.h>
#include <X11/extensions/Xrandr.h>
#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>
#include <alsa/asoundlib.h>

#define MAX_INTERVALS 100
#define MAX_LABEL_LENGTH 50
#define SOUND_DEVICE "default"

typedef struct {
    char label[MAX_LABEL_LENGTH];
    int duration;  // in seconds
} Interval;

typedef struct {
    Interval intervals[MAX_INTERVALS];
    int count;
} IntervalSet;

// Global variables
IntervalSet interval_set;
int current_interval = 0;
int time_remaining = 0;
Display *display = NULL;
Window window;
cairo_surface_t *surface = NULL;
cairo_t *cr = NULL;
snd_pcm_t *audio_handle = NULL;
int running = 1;
int screen_width, screen_height;

// Function prototypes
void setup_x11_window();
void cleanup_x11();
void prevent_screen_sleep();
void allow_screen_sleep();
void setup_audio();
void cleanup_audio();
void reset_audio();
void play_beep();
void draw_timer(int minutes, int seconds, const char *label);
void draw_completion_message(const char *label);
void flash_screen();
void load_intervals(const char *filename);
void signal_handler(int sig);
int check_x11_keypress();

int main(int argc, char *argv[]) {
    if (argc != 2) {
        printf("Usage: %s <interval_file>\n", argv[0]);
        printf("Interval file format:\n");
        printf("label duration_seconds\n");
        printf("Example:\n");
        printf("Warmup 300\n");
        printf("Sprint 30\n");
        printf("Rest 60\n");
        return 1;
    }

    // Set up signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Load intervals from file
    load_intervals(argv[1]);

    if (interval_set.count == 0) {
        printf("No intervals loaded. Check your interval file.\n");
        return 1;
    }

    // Initialize X11 display and window
    setup_x11_window();
    if (!display) {
        printf("Error: Cannot open X11 display\n");
        return 1;
    }

    // Set up audio
    setup_audio();

    // Prevent screen sleep
    prevent_screen_sleep();

    // Main timer loop
    while (running && current_interval < interval_set.count) {
        Interval *interval = &interval_set.intervals[current_interval];
        time_remaining = interval->duration;

        while (time_remaining > 0 && running) {
            int minutes = time_remaining / 60;
            int seconds = time_remaining % 60;

            draw_timer(minutes, seconds, interval->label);

            sleep(1);
            time_remaining--;

            // Check for key press to skip interval
            int key = check_x11_keypress();
            if (key == 'q' || key == 'Q' || key == 27) { // Q, q, or Escape
                running = 0;
                break;
            } else if (key == 's' || key == 'S') {
                break; // Skip to next interval
            }
        }

        if (running) {
            // Interval finished - flash and beep
            flash_screen();
            reset_audio(); // Reset audio before playing
            play_beep();
            
            // Show completion message briefly
            draw_completion_message(interval->label);
            
            // Wait 2 seconds before automatically continuing
            sleep(2);
            
            current_interval++;
        }
    }

    // Cleanup
    allow_screen_sleep();
    cleanup_x11();
    cleanup_audio();

    printf("\nInterval training completed!\n");
    return 0;
}

void setup_x11_window() {
    display = XOpenDisplay(NULL);
    if (!display) return;

    int screen = DefaultScreen(display);
    Window root = DefaultRootWindow(display);
    
    // Get screen dimensions
    XRRScreenResources *resources = XRRGetScreenResources(display, root);
    if (resources && resources->noutput > 0) {
        XRROutputInfo *output_info = XRRGetOutputInfo(display, resources, resources->outputs[0]);
        if (output_info && output_info->crtc != None) {
            XRRCrtcInfo *crtc_info = XRRGetCrtcInfo(display, resources, output_info->crtc);
            if (crtc_info) {
                screen_width = crtc_info->width;
                screen_height = crtc_info->height;
                XRRFreeCrtcInfo(crtc_info);
            }
            XRRFreeOutputInfo(output_info);
        }
        XRRFreeScreenResources(resources);
    } else {
        screen_width = DisplayWidth(display, screen);
        screen_height = DisplayHeight(display, screen);
    }

    // Create window
    XVisualInfo vinfo;
    XMatchVisualInfo(display, screen, 32, TrueColor, &vinfo);
    
    XSetWindowAttributes attr;
    attr.colormap = XCreateColormap(display, root, vinfo.visual, AllocNone);
    attr.border_pixel = 0;
    attr.background_pixel = 0;
    attr.override_redirect = True; // Make it fullscreen
    
    window = XCreateWindow(display, root, 0, 0, screen_width, screen_height,
                          0, vinfo.depth, InputOutput, vinfo.visual,
                          CWColormap | CWBorderPixel | CWBackPixel | CWOverrideRedirect, &attr);
    
    // Set window properties
    XSetWindowAttributes wattr;
    wattr.override_redirect = False; // Allow window manager control
    XChangeWindowAttributes(display, window, CWOverrideRedirect, &wattr);
    
    // Set up event mask for keyboard and mouse events
    XSelectInput(display, window, KeyPressMask | KeyReleaseMask | ExposureMask | StructureNotifyMask);
    
    // Set window manager hints
    XWMHints wm_hints;
    wm_hints.flags = InputHint | StateHint;
    wm_hints.input = True;
    wm_hints.initial_state = NormalState;
    XSetWMHints(display, window, &wm_hints);
    
    // Set window name
    XStoreName(display, window, "Interval Timer");
    
    // Set fullscreen
    Atom wm_state = XInternAtom(display, "_NET_WM_STATE", False);
    Atom fullscreen = XInternAtom(display, "_NET_WM_STATE_FULLSCREEN", False);
    XChangeProperty(display, window, wm_state, XA_ATOM, 32, PropModeReplace, (unsigned char *)&fullscreen, 1);
    
    // Map window
    XMapWindow(display, window);
    XFlush(display);
    
    // Give the window manager time to process the window
    usleep(100000); // 100ms
    
    // Create Cairo surface
    surface = cairo_xlib_surface_create(display, window, vinfo.visual, screen_width, screen_height);
    cr = cairo_create(surface);
}

void cleanup_x11() {
    if (cr) cairo_destroy(cr);
    if (surface) cairo_surface_destroy(surface);
    if (window) XDestroyWindow(display, window);
    if (display) XCloseDisplay(display);
}

void prevent_screen_sleep() {
    if (display) {
        DPMSDisable(display);
    }
}

void allow_screen_sleep() {
    if (display) {
        DPMSEnable(display);
    }
}

void setup_audio() {
    int err = snd_pcm_open(&audio_handle, SOUND_DEVICE, SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        printf("Warning: Cannot open audio device: %s\n", snd_strerror(err));
        audio_handle = NULL;
        return;
    }

    snd_pcm_hw_params_t *params;
    snd_pcm_hw_params_alloca(&params);
    snd_pcm_hw_params_any(audio_handle, params);
    snd_pcm_hw_params_set_access(audio_handle, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(audio_handle, params, SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_channels(audio_handle, params, 1);
    snd_pcm_hw_params_set_rate(audio_handle, params, 44100, 0);
    snd_pcm_hw_params_set_period_size(audio_handle, params, 1024, 0);

    err = snd_pcm_hw_params(audio_handle, params);
    if (err < 0) {
        printf("Warning: Cannot set audio parameters: %s\n", snd_strerror(err));
        snd_pcm_close(audio_handle);
        audio_handle = NULL;
        return;
    }
    
    // Prepare the audio device
    err = snd_pcm_prepare(audio_handle);
    if (err < 0) {
        printf("Warning: Cannot prepare audio device: %s\n", snd_strerror(err));
        snd_pcm_close(audio_handle);
        audio_handle = NULL;
    }
}

void cleanup_audio() {
    if (audio_handle) {
        snd_pcm_close(audio_handle);
        audio_handle = NULL;
    }
}

void reset_audio() {
    if (audio_handle) {
        snd_pcm_drop(audio_handle);
        snd_pcm_prepare(audio_handle);
    }
}

void play_beep() {
    if (!audio_handle) return;

    // Generate a simple beep sound
    short buffer[4410]; // 0.1 second at 44.1kHz
    for (int i = 0; i < 4410; i++) {
        buffer[i] = (short)(sin(2.0 * M_PI * 800.0 * i / 44100.0) * 8000);
    }

    snd_pcm_sframes_t frames = snd_pcm_writei(audio_handle, buffer, 4410);
    if (frames < 0) {
        // Try to recover from error
        if (snd_pcm_recover(audio_handle, frames, 0) >= 0) {
            // Retry the write
            frames = snd_pcm_writei(audio_handle, buffer, 4410);
        }
    }
    
    if (frames >= 0) {
        snd_pcm_drain(audio_handle);
    }
}

void draw_timer(int minutes, int seconds, const char *label) {
    if (!cr) return;

    // Clear background
    cairo_set_source_rgb(cr, 0.0, 0.0, 0.0); // Black background
    cairo_paint(cr);

    // Set up text properties
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0); // White text

    // Draw title
    cairo_set_font_size(cr, 48);
    const char *title = "INTERVAL TIMER";
    cairo_text_extents_t extents;
    cairo_text_extents(cr, title, &extents);
    cairo_move_to(cr, (screen_width - extents.width) / 2, 100);
    cairo_show_text(cr, title);

    // Draw interval label
    cairo_set_font_size(cr, 36);
    cairo_text_extents(cr, label, &extents);
    cairo_move_to(cr, (screen_width - extents.width) / 2, 200);
    cairo_show_text(cr, label);

    // Draw timer
    char time_str[10];
    snprintf(time_str, sizeof(time_str), "%02d:%02d", minutes, seconds);
    cairo_set_font_size(cr, 120);
    cairo_text_extents(cr, time_str, &extents);
    cairo_move_to(cr, (screen_width - extents.width) / 2, (screen_height + extents.height) / 2);
    cairo_show_text(cr, time_str);

    // Draw instructions
    cairo_set_font_size(cr, 24);
    const char *instructions[] = {
        "Press 'S' to skip interval",
        "Press 'Q' or 'ESC' to quit",
        "Intervals continue automatically"
    };
    
    for (int i = 0; i < 2; i++) {
        cairo_text_extents(cr, instructions[i], &extents);
        cairo_move_to(cr, (screen_width - extents.width) / 2, screen_height - 150 + i * 40);
        cairo_show_text(cr, instructions[i]);
    }

    // Update display
    cairo_surface_flush(surface);
    XFlush(display);
}

void draw_completion_message(const char *label) {
    if (!cr) return;

    // Flash effect
    for (int i = 0; i < 3; i++) {
        // White flash
        cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
        cairo_paint(cr);
        cairo_surface_flush(surface);
        XFlush(display);
        usleep(200000);

        // Red flash
        cairo_set_source_rgb(cr, 1.0, 0.0, 0.0);
        cairo_paint(cr);
        cairo_surface_flush(surface);
        XFlush(display);
        usleep(200000);
    }

    // Draw completion message
    cairo_set_source_rgb(cr, 0.0, 0.0, 0.0); // Black background
    cairo_paint(cr);

    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0); // White text

    // Draw completion text
    cairo_set_font_size(cr, 48);
    const char *completion_text = "INTERVAL COMPLETE!";
    cairo_text_extents_t extents;
    cairo_text_extents(cr, completion_text, &extents);
    cairo_move_to(cr, (screen_width - extents.width) / 2, screen_height / 2 - 100);
    cairo_show_text(cr, completion_text);

    // Draw interval name
    cairo_set_font_size(cr, 36);
    cairo_text_extents(cr, label, &extents);
    cairo_move_to(cr, (screen_width - extents.width) / 2, screen_height / 2);
    cairo_show_text(cr, label);

    // Draw continue instruction
    cairo_set_font_size(cr, 24);
    const char *continue_text = "Continuing automatically in 2 seconds...";
    cairo_text_extents(cr, continue_text, &extents);
    cairo_move_to(cr, (screen_width - extents.width) / 2, screen_height / 2 + 100);
    cairo_show_text(cr, continue_text);

    cairo_surface_flush(surface);
    XFlush(display);
}

void flash_screen() {
    if (!cr) return;
    
    for (int i = 0; i < 3; i++) {
        // White flash
        cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
        cairo_paint(cr);
        cairo_surface_flush(surface);
        XFlush(display);
        usleep(200000);
        
        // Red flash
        cairo_set_source_rgb(cr, 1.0, 0.0, 0.0);
        cairo_paint(cr);
        cairo_surface_flush(surface);
        XFlush(display);
        usleep(200000);
    }
}

void load_intervals(const char *filename) {
    FILE *file = fopen(filename, "r");
    if (!file) {
        printf("Error: Cannot open file %s\n", filename);
        return;
    }

    interval_set.count = 0;
    char line[256];
    
    while (fgets(line, sizeof(line), file) && interval_set.count < MAX_INTERVALS) {
        char label[MAX_LABEL_LENGTH];
        int duration;
        
        if (sscanf(line, "%s %d", label, &duration) == 2) {
            strncpy(interval_set.intervals[interval_set.count].label, label, MAX_LABEL_LENGTH - 1);
            interval_set.intervals[interval_set.count].label[MAX_LABEL_LENGTH - 1] = '\0';
            interval_set.intervals[interval_set.count].duration = duration;
            interval_set.count++;
        }
    }
    
    fclose(file);
}

void signal_handler(int sig) {
    (void)sig; // Suppress unused parameter warning
    running = 0;
}

int check_x11_keypress() {
    if (!display) return 0;
    
    XEvent event;
    while (XPending(display)) {
        XNextEvent(display, &event);
        
        switch (event.type) {
            case KeyPress: {
                KeySym keysym;
                char key[32];
                int len = XLookupString(&event.xkey, key, sizeof(key), &keysym, NULL);
                
                if (len > 0) {
                    return key[0];
                } else if (keysym == XK_Escape) {
                    return 27; // Escape key
                }
                break;
            }
            case ClientMessage: {
                // Handle window close events
                Atom wm_delete = XInternAtom(display, "WM_DELETE_WINDOW", True);
                if (event.xclient.message_type == wm_delete) {
                    return 'q'; // Treat window close as quit
                }
                break;
            }
            case ConfigureNotify:
                // Handle window resize events
                break;
            case Expose:
                // Handle window expose events
                break;
        }
    }
    
    return 0; // No key pressed
} 