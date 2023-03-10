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
#include <time.h>

bool stop;

#define CANERROR

// Zero-initialized
typedef struct {
    char err[256];
    struct libevdev *penDevs[16];
    int nPenDevs;
    struct pollfd penFds[16];
    struct libevdev *touchDevs[16];
    int nTouchDevs;
} Devices;

bool str_starts_with (const char *str, const char *prefix) {
    return strncmp(str, prefix, strlen(prefix)) == 0;
}

void catch_stop_signal (int signum) {
    stop = true;
    printf("\nReceived signal");
    switch (signum) {
        case SIGINT: printf(" INT"); break;
        case SIGTERM: printf(" TERM"); break;
        default: printf(" %d", signum); break;
    }
    printf("\n");
}

CANERROR bool devices_refresh (Devices *devs);
CANERROR void devices_grab_touch_devs (Devices *devs, bool grab);

CANERROR Devices devices(void) {
    Devices devs = {0};
    devices_refresh(&devs);
    return devs;
}

void devices_cleanup (Devices *devs, bool ungrab) {
    if (ungrab)
        devices_grab_touch_devs(devs, false);
    for (size_t i = 0; i < devs->nPenDevs; i++) {
        close(libevdev_get_fd(devs->penDevs[i]));
        libevdev_free(devs->penDevs[i]);
    }
    for (size_t i = 0; i < devs->nTouchDevs; i++) {
        close(libevdev_get_fd(devs->touchDevs[i]));
        libevdev_free(devs->touchDevs[i]);
    }
}

// Returns true if any devices changed
CANERROR bool devices_refresh (Devices *devs) {
    Devices oldDevs = *devs;

    *devs = (Devices){0};

    const char *dirName = "/dev/input";
    DIR *pDir = NULL;
    struct dirent *pDirEnt = NULL;

    if (!(pDir = opendir(dirName))) {
        snprintf(devs->err, sizeof(devs->err), "failed to open directory \"%s\": %s", dirName, strerror(errno));
        return false;
    }

    bool errOccurred = false;

    while ((pDirEnt = readdir(pDir))) {
        if (pDirEnt->d_type == DT_CHR && str_starts_with(pDirEnt->d_name, "event")) {
            char fileName[256];
            int fd;
            int rc = 1;
            struct libevdev *dev = NULL;
            bool dontFree = false;
            
            snprintf(fileName, sizeof(fileName), "%s/%s", dirName, pDirEnt->d_name);
            fd = open(fileName, O_RDWR | O_NONBLOCK);
            if (fd == -1) {
                snprintf(devs->err, sizeof(devs->err), "failed to open \"%s\": %s", fileName, strerror(errno));
                errOccurred = true;
                break;
            }
            rc = libevdev_new_from_fd(fd, &dev);
            if (rc < 0) {
                snprintf(devs->err, sizeof(devs->err), "failed to open device \"%s\": %s", fileName, strerror(-rc));
                errOccurred = true;
                close(fd);
                break;
            }

            if (libevdev_has_event_code(dev, EV_KEY, BTN_TOOL_PEN) && libevdev_has_event_code(dev, EV_KEY, BTN_TOOL_RUBBER)) {
                if (devs->nPenDevs < 16)
                    devs->penDevs[devs->nPenDevs++] = dev;
                dontFree = true;
            } else if (libevdev_has_event_code(dev, EV_KEY, BTN_TOUCH) && !libevdev_has_property(dev, INPUT_PROP_POINTER)) {
                if (devs->nPenDevs < 16)
                    devs->touchDevs[devs->nTouchDevs++] = dev;
                dontFree = true;
            }
            
            if (!dontFree) {
                close(libevdev_get_fd(dev));
                libevdev_free(dev);
            }
        }
    }

    closedir(pDir);

    if (errOccurred) {
        devices_cleanup(devs, true);
        return false;
    }

    for (size_t i = 0; i < devs->nPenDevs; i++) {
        devs->penFds[i].fd = libevdev_get_fd(devs->penDevs[i]);
        devs->penFds[i].events = POLLIN;
        devs->penFds[i].revents = 0;
    }

    bool changed = false;
    if (oldDevs.nPenDevs != devs->nPenDevs)
        changed = true;
    if (oldDevs.nTouchDevs != devs->nTouchDevs)
        changed = true;
    for (size_t i = 0; !changed && i < oldDevs.nPenDevs; i++) {
        if (strcmp(libevdev_get_name(oldDevs.penDevs[i]), libevdev_get_name(devs->penDevs[i])) != 0)
            changed = true;
    }
    for (size_t i = 0; !changed && i < oldDevs.nTouchDevs; i++) {
        if (strcmp(libevdev_get_name(oldDevs.touchDevs[i]), libevdev_get_name(devs->touchDevs[i])) != 0)
            changed = true;
    }

    if (changed)
        devices_cleanup(&oldDevs, true);
    else {
        devices_cleanup(devs, false);
        *devs = oldDevs;
    }

    return changed;
}

CANERROR int devices_poll (Devices *devs, int timeout_ms) {
    int rc = 1;
    rc = poll(devs->penFds, devs->nPenDevs, timeout_ms);
    if (rc < 0) {
        if (errno == EINTR)
            return 0;
        snprintf(devs->err, sizeof(devs->err), "failed to poll events: %s", strerror(errno));
        return 0;
    }

    for (size_t i = 0; i < devs->nPenDevs; i++) {
        if (devs->penFds[i].revents & POLLERR) {
            snprintf(devs->err, sizeof(devs->err), "failed to poll events: error from device \"%s\"", libevdev_get_name(devs->penDevs[i]));
            return 0;
        }
        if (devs->penFds[i].revents & POLLHUP) {
            snprintf(devs->err, sizeof(devs->err), "failed to poll events: device \"%s\" hung up", libevdev_get_name(devs->penDevs[i]));
            return 0;
        }
    }
    return rc;
}

CANERROR void devices_grab_touch_devs (Devices *devs, bool grab) {
    for (size_t i = 0; i < devs->nTouchDevs; i++) {
        int rc = 1;
        rc = libevdev_grab(devs->touchDevs[i], grab ? LIBEVDEV_GRAB : LIBEVDEV_UNGRAB);
        if (rc < 0) {
            snprintf(devs->err, sizeof(devs->err), "failed to %s device \"%s\": %s",
                grab ? "grab" : "ungrab",
                libevdev_get_name(devs->touchDevs[i]),
                strerror(-rc));
            return;
        }
    }
}

void print_devices (const Devices *devs) {
    printf("Pens: ");
    for (size_t i = 0; i < devs->nPenDevs; i++) {
        if (i != 0)
            printf(", ");
        printf("\"%s\"", libevdev_get_name(devs->penDevs[i]));
    }
    printf("\n");
    printf("Touchscreens: ");
    for (size_t i = 0; i < devs->nTouchDevs; i++) {
        if (i != 0)
            printf(", ");
        printf("\"%s\"", libevdev_get_name(devs->touchDevs[i]));
    }
    printf("\n");
}

int main (int argc, const char **argv) {
    signal(SIGINT, catch_stop_signal);
    signal(SIGTERM, catch_stop_signal);

    bool restart = false;

    while (!stop) {
        if (restart)
            printf("Restarting, press Ctrl+C to stop\n");

        printf("Initializing devices\n");

        restart = false;

        time_t lastRefresh = time(NULL);
        Devices devs = devices();
        if (devs.err[0] != 0) {
            fprintf(stderr, "Error getting devices: %s\n", devs.err);
            restart = true;
            sleep(1);
            continue;
        }

        print_devices(&devs);

        while (!stop && !restart) {
            time_t now = time(NULL);
            if (now - lastRefresh > 4) {
                bool changed = false;
                lastRefresh = now;
                changed = devices_refresh(&devs);
                if (devs.err[0] != 0) {
                    fprintf(stderr, "Error refreshing devices: %s\n", devs.err);
                    restart = true;
                    sleep(1);
                    continue;
                }
                if (changed) {
                    printf("Devices changed\n");
                    print_devices(&devs);
                }
            }

            if (devices_poll(&devs, 20)) {
                for (size_t i = 0; i < devs.nPenDevs; i++) {
                    struct input_event ev;
                    int rc = 1;
                    rc = libevdev_next_event(devs.penDevs[i], LIBEVDEV_READ_FLAG_NORMAL, &ev);
                    if (rc == -EAGAIN)
                        continue; // No events pending
                    else if (rc < 0) {
                        fprintf(stderr, "Error: failed to get next event for device \"%s\": %s", libevdev_get_name(devs.penDevs[i]), strerror(-rc));
                        restart = true;
                        break;
                    }
                    if (ev.type == EV_KEY &&
                        (ev.code == BTN_TOOL_PEN || ev.code == BTN_TOOL_RUBBER) &&
                        (ev.value == 0 || ev.value == 1)) {
                        printf("Touch %s\n", ev.value == 1 ? "disabled" : "re-enabled");
                        devices_grab_touch_devs(&devs, ev.value == 1);
                        if (devs.err[0] != 0) {
                            fprintf(stderr, "Error: %s\n", devs.err);
                            restart = true;
                            break;
                        }
                    }
                }
            } else {
                if (devs.err[0] != 0) {
                    fprintf(stderr, "Error: %s\n", devs.err);
                    restart = true;
                }
            }
        }

        printf("Cleaning up\n");

        devices_cleanup(&devs, true);
    }
}
