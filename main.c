#include <stdio.h>
#include <sys/types.h>
#include <dirent.h>
#include <libevdev/libevdev.h>
#include <errno.h>
#include <string.h>
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <stdlib.h>
#include <signal.h>

bool stop = false;
struct libevdev *penDevs[16] = {NULL};
int nPenDevs = 0;
struct pollfd penFds[16];
struct libevdev *touchDevs[16] = {NULL};
int nTouchDevs = 0;

bool str_starts_with (const char *str, const char *prefix) {
    return strncmp(str, prefix, strlen(prefix)) == 0;
}

void catch_signal (int signum) {
    stop = true;
}

void populate_pen_and_touch_devs (void) {
    const char *dirName = "/dev/input";
    DIR *pDir = NULL;
    struct dirent *pDirEnt = NULL;

    if (!(pDir = opendir(dirName))) {
        fprintf(stderr, "Failed to open directory /dev/input: %s\n", strerror(errno));
        exit(1);
    }
    
    while ((pDirEnt = readdir(pDir))) {
        if (pDirEnt->d_type == DT_CHR && str_starts_with(pDirEnt->d_name, "event")) {
            char fileName[256];
            int fd;
            int rc = 1;
            struct libevdev *dev = NULL;
            bool dontFree = false;
            
            snprintf(fileName, sizeof(fileName), "%s/%s", dirName, pDirEnt->d_name);
            fd = open(fileName, O_RDWR | O_NONBLOCK);
            rc = libevdev_new_from_fd(fd, &dev);
            if (rc < 0) {
                fprintf(stderr, "Failed to open device %s: %s\n", fileName, strerror(-rc));
                exit(1);
            }

            if (libevdev_has_event_code(dev, EV_KEY, BTN_TOOL_PEN) && libevdev_has_event_code(dev, EV_KEY, BTN_TOOL_RUBBER)) {
                if (nPenDevs < 16)
                    penDevs[nPenDevs++] = dev;
                dontFree = true;
            } else if (libevdev_has_event_code(dev, EV_KEY, BTN_TOUCH) && !libevdev_has_property(dev, INPUT_PROP_POINTER)) {
                if (nPenDevs < 16)
                    touchDevs[nTouchDevs++] = dev;
                dontFree = true;
            }
            
            if (!dontFree) {
                libevdev_free(dev);
                close(libevdev_get_fd(dev));
            }
        }
    }
    
    closedir(pDir);
}

void cleanup_pen_and_touch_devs (void) {
    for (size_t i = 0; i < nPenDevs; i++) {
        libevdev_free(penDevs[i]);
        close(libevdev_get_fd(penDevs[i]));
    }
    for (size_t i = 0; i < nTouchDevs; i++) {
        libevdev_free(touchDevs[i]);
        close(libevdev_get_fd(touchDevs[i]));
    }
}

void enable_disable_touch (bool disable) {
    for (size_t i = 0; i < nTouchDevs; i++) {
        int rc = 1;
        rc = libevdev_grab(touchDevs[i], disable ? LIBEVDEV_GRAB : LIBEVDEV_UNGRAB);
        if (rc < 0) {
            fprintf(stderr, "Failed to %s device %s: %s\n",
                disable ? "grab" : "ungrab",
                libevdev_get_name(touchDevs[i]),
                strerror(-rc));
            exit(1);
        }
    }
}

void populate_pen_poll_fds (void) {
    for (size_t i = 0; i < nPenDevs; i++) {
        penFds[i].fd = libevdev_get_fd(penDevs[i]);
        penFds[i].events = POLLIN;
        penFds[i].revents = 0;
    }
}

bool poll_pen_events (int timeout_ms) {
    int rc = 1;
    rc = poll(penFds, nPenDevs, timeout_ms);
    if (rc < 0) {
        fprintf(stderr, "Failed to poll events: %s\n", strerror(errno));
        return false;
    }
    return rc > 0;
}

int main (int argc, const char **argv) {
    populate_pen_and_touch_devs();
    populate_pen_poll_fds();
    
    printf("Pen devices:\n");
    for (size_t i = 0; i < nPenDevs; i++) {
        printf("  %s\n", libevdev_get_name(penDevs[i]));
    }
    printf("Touch devices:\n");
    for (size_t i = 0; i < nTouchDevs; i++) {
        printf("  %s\n", libevdev_get_name(touchDevs[i]));
    }
    
    signal(SIGINT, catch_signal);
    signal(SIGTERM, catch_signal);
    
    while (!stop) {
        if (poll_pen_events(20)) {
            for (size_t i = 0; i < nPenDevs; i++) {
                if (penFds[i].revents & POLLIN) {
                    struct input_event penEvent;
                    int rc = 1;
                    rc = libevdev_next_event(penDevs[i], LIBEVDEV_READ_FLAG_NORMAL, &penEvent);
                    if (rc < 0) {
                        fprintf(stderr, "Failed to get next event: %s\n", strerror(-rc));
                        return 1;
                    }
                    if (penEvent.type == EV_KEY &&
                        (penEvent.code == BTN_TOOL_PEN || penEvent.code == BTN_TOOL_RUBBER) &&
                        (penEvent.value == 0 || penEvent.value == 1)) {
                        printf("touch %s\n", penEvent.value == 1 ? "disabled" : "re-enabled");
                        enable_disable_touch(penEvent.value == 1);
                    }
                } else if (penFds[i].revents & POLLERR) {
                    fprintf(stderr, "Error polling events from device %s\n", libevdev_get_name(penDevs[i]));
                    return 1;
                }
            }
        }
    }
    
    fprintf(stderr, "Exiting\n");
    
    cleanup_pen_and_touch_devs();
}