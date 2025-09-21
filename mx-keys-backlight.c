#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <hidapi/hidapi.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>

// Default receiver VID/PID; can be overridden by environment variables
#define DEFAULT_RECEIVER_VID 0x046D
#define DEFAULT_RECEIVER_PID 0xC52B

// HID++ report IDs
#define HIDPP_SHORT_REPORT_ID 0x10
#define HIDPP_LONG_REPORT_ID  0x11

// HID++ 2.0 constants
#define HIDPP_FEATURE_BACKLIGHT2 0x1982

// Short vs Long message sizes
#define HIDPP_SHORT_PAYLOAD_SIZE 6   // bytes after report id and device index
#define HIDPP_LONG_PAYLOAD_SIZE  18  // bytes after report id and device index

// (no-op endian helpers needed)

static void flush_input(hid_device *dev) {
    uint8_t tmp[64];
    hid_set_nonblocking(dev, 1);
    while (hid_read(dev, tmp, sizeof(tmp)) > 0) { /* discard */ }
    hid_set_nonblocking(dev, 0);
}

// Read receiver VID/PID from environment, falling back to defaults.
// Accepts decimal (e.g., 1133) or hex (e.g., 0x046D, 046D) strings.
static void get_receiver_ids(uint16_t *vid_out, uint16_t *pid_out) {
    const char *vid_env = getenv("MX_KEYS_RECEIVER_VID");
    const char *pid_env = getenv("MX_KEYS_RECEIVER_PID");

    uint16_t vid = DEFAULT_RECEIVER_VID;
    uint16_t pid = DEFAULT_RECEIVER_PID;

    if (vid_env && *vid_env) {
        unsigned long v = strtoul(vid_env, NULL, 0);
        if (v <= 0xFFFFUL) vid = (uint16_t)v;
    }
    if (pid_env && *pid_env) {
        unsigned long p = strtoul(pid_env, NULL, 0);
        if (p <= 0xFFFFUL) pid = (uint16_t)p;
    }

    *vid_out = vid;
    *pid_out = pid;
}

// Send a HID++ long request and wait for the matching reply for the dev_index and request_id
static int send_request(hid_device *dev, uint8_t dev_index, uint16_t request_id, const uint8_t *payload, size_t payload_len,
                        uint8_t *reply, size_t reply_cap, int timeout_ms) {
    uint8_t buf[1 + 1 + HIDPP_LONG_PAYLOAD_SIZE];
    memset(buf, 0, sizeof(buf));
    buf[0] = HIDPP_LONG_REPORT_ID;
    buf[1] = dev_index;
    buf[2] = (uint8_t)((request_id >> 8) & 0xFF);
    buf[3] = (uint8_t)(request_id & 0xFF);
    size_t n = payload_len > 16 ? 16 : payload_len;
    if (payload && n) memcpy(&buf[4], payload, n);

    flush_input(dev);
    int wr = hid_write(dev, buf, sizeof(buf));
    if (wr < 0) return -1;

    int remaining = timeout_ms;
    int step = 50; // ms per poll
    hid_set_nonblocking(dev, 0);
    while (remaining >= 0) {
        int rr = hid_read_timeout(dev, reply, (size_t)reply_cap, step);
        if (rr <= 0) {
            remaining -= step;
            continue;
        }
        if (rr < 4) continue;
        if (reply[0] != HIDPP_LONG_REPORT_ID) continue;
        if (reply[1] != dev_index) continue; // different device slot
        if (reply[2] != buf[2] || reply[3] != buf[3]) continue; // not our request
        // HID++ 2.0 error reply payload starts with 0xFF
        if (rr >= 5 && reply[4] == 0xFF) return -2;
        return rr;
    }
    return -3; // timeout
}

// Simple cache for selected HID path, slot, feature index
static const char *cache_path_file() {
    static char path[512];
    const char *home = getenv("HOME");
    if (!home) home = ".";
    // Store cache under the app dir, not in bin
    snprintf(path, sizeof(path), "%s/.mx-keys-cli/cache", home);
    return path;
}

static int save_cache(const char *hid_path, int slot, int feat_idx) {
    const char *p = cache_path_file();
    // Ensure directory exists
    char dir[512];
    strncpy(dir, p, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    char *slash = strrchr(dir, '/');
    if (slash) { *slash = '\0'; mkdir(dir, 0700); }
    FILE *f = fopen(p, "w");
    if (!f) {
        fprintf(stderr, "Failed to write cache %s: %s\n", p, strerror(errno));
        return -1;
    }
    fprintf(f, "%s\n%d\n%d\n", hid_path ? hid_path : "", slot, feat_idx);
    fclose(f);
    return 0;
}

static int load_cache(char *hid_path_out, size_t cap, int *slot_out, int *feat_idx_out) {
    const char *p = cache_path_file();
    FILE *f = fopen(p, "r");
    if (!f) return -1;
    char line[1024];
    if (!fgets(line, sizeof(line), f)) { fclose(f); return -1; }
    size_t len = strcspn(line, "\r\n");
    line[len] = '\0';
    strncpy(hid_path_out, line, cap - 1);
    hid_path_out[cap - 1] = '\0';
    if (!fgets(line, sizeof(line), f)) { fclose(f); return -1; }
    *slot_out = atoi(line);
    if (!fgets(line, sizeof(line), f)) { fclose(f); return -1; }
    *feat_idx_out = atoi(line);
    fclose(f);
    return 0;
}

static void clear_cache(void) {
    const char *p = cache_path_file();
    unlink(p);
}

// Query FEATURE_SET to resolve the feature index for BACKLIGHT2 at a given device index.
// Returns feature index (0..n) or -1 on failure.
static int resolve_feature_index(hid_device *dev, uint8_t dev_index, uint16_t feature_id) {
    uint8_t payload[2] = { (uint8_t)((feature_id >> 8) & 0xFF), (uint8_t)(feature_id & 0xFF) };
    uint16_t request_id = (uint16_t)(0x0000 | 0x000F); // ROOT.getFeature with SWID 0xF
    uint8_t reply[64];
    int rr = send_request(dev, dev_index, request_id, payload, sizeof(payload), reply, sizeof(reply), 800);
    if (rr >= 7) {
        int idx = reply[4];
        if (idx > 0) return idx;
    }
    return -1;
}

// Perform BACKLIGHT2 0x00 read: returns 12-byte structure in resp (enabled, options, supported, effects(2), level, dho(2), dhi(2), dpow(2))
static int backlight2_read(hid_device *dev, uint8_t dev_index, int feat_idx, uint8_t *resp /*>=12*/) {
    uint16_t request_id = (uint16_t)(((uint16_t)feat_idx << 8) | 0x00);
    request_id = (uint16_t)((request_id & 0xFFF0) | 0x0F);
    uint8_t reply[64];
    int rr = send_request(dev, dev_index, request_id, NULL, 0, reply, sizeof(reply), 1000);
    if (rr < 4 + 12) return -1;
    memcpy(resp, &reply[4], 12);
    return 0;
}

// Perform BACKLIGHT2 0x10 write with the 10-byte payload
static int backlight2_write(hid_device *dev, uint8_t dev_index, int feat_idx,
    uint8_t enabled, uint8_t options, uint8_t effect, uint8_t level, uint16_t dho, uint16_t dhi, uint16_t dpow) {
    uint8_t payload[10];
    payload[0] = enabled;           // 0x00/0x01 (non-0xFF indicates enabled)
    payload[1] = options;           // lower 3 bits preserved; mode in bits 3..4
    payload[2] = effect;            // 0xFF = no change
    payload[3] = level;             // only used if mode==manual
    payload[4] = (uint8_t)(dho & 0xFF);
    payload[5] = (uint8_t)((dho >> 8) & 0xFF);
    payload[6] = (uint8_t)(dhi & 0xFF);
    payload[7] = (uint8_t)((dhi >> 8) & 0xFF);
    payload[8] = (uint8_t)(dpow & 0xFF);
    payload[9] = (uint8_t)((dpow >> 8) & 0xFF);

    uint16_t request_id = (uint16_t)(((uint16_t)feat_idx << 8) | 0x10);
    request_id = (uint16_t)((request_id & 0xFFF0) | 0x0F);
    uint8_t reply[64];
    int rr = send_request(dev, dev_index, request_id, payload, sizeof(payload), reply, sizeof(reply), 1000);
    return (rr >= 0) ? 0 : -2;
}

// Get BACKLIGHT2 level range via 0x20; returns max level (exclusive upper bound), or -1
static int backlight2_get_level_max(hid_device *dev, uint8_t dev_index, int feat_idx) {
    uint16_t request_id = (uint16_t)(((uint16_t)feat_idx << 8) | 0x20);
    request_id = (uint16_t)((request_id & 0xFFF0) | 0x0F);
    uint8_t reply[64];
    int rr = send_request(dev, dev_index, request_id, NULL, 0, reply, sizeof(reply), 1000);
    if (rr >= 5) return reply[4];
    return -1;
}

// Apply ON: enable backlight in basic/auto mode and verify by reading back
static int apply_on(hid_device *dev, uint8_t slot, int feat_idx) {
    uint8_t state[12];
    if (backlight2_read(dev, slot, feat_idx, state) != 0) return -1;

    uint8_t enabled = state[0];
    uint8_t options = state[1];
    uint16_t dho = (uint16_t)(state[6] | (state[7] << 8));
    uint16_t dhi = (uint16_t)(state[8] | (state[9] << 8));
    uint16_t dpow = (uint16_t)(state[10] | (state[11] << 8));

    int max = backlight2_get_level_max(dev, slot, feat_idx);
    int level = (max > 0 ? max - 1 : 0x0F);

    (void)enabled; // not used beyond verification, but kept for clarity
    uint8_t mode = 0x00; // Basic/auto enabled
    options = (uint8_t)((options & 0x07) | (mode << 3));
    enabled = 0x01;
    if (backlight2_write(dev, slot, feat_idx, enabled, options, 0xFF, (uint8_t)level, dho, dhi, dpow) != 0) return -1;

    uint8_t verify[12];
    if (backlight2_read(dev, slot, feat_idx, verify) != 0) return -1;
    return verify[0] ? 0 : -1; // enabled flag must be non-zero
}

// Apply OFF: disable backlight and verify by reading back
static int apply_off(hid_device *dev, uint8_t slot, int feat_idx) {
    uint8_t state[12];
    if (backlight2_read(dev, slot, feat_idx, state) != 0) return -1;

    uint8_t enabled = state[0];
    uint8_t options = state[1];
    uint16_t dho = (uint16_t)(state[6] | (state[7] << 8));
    uint16_t dhi = (uint16_t)(state[8] | (state[9] << 8));
    uint16_t dpow = (uint16_t)(state[10] | (state[11] << 8));

    (void)enabled; // previous value unused
    if (backlight2_write(dev, slot, feat_idx, 0x00, options, 0xFF, 0x00, dho, dhi, dpow) != 0) return -1;

    uint8_t verify[12];
    if (backlight2_read(dev, slot, feat_idx, verify) != 0) return -1;
    return verify[0] == 0 ? 0 : -1; // enabled flag must be zero
}

// Apply FORCE-ON: send OFF then immediately ON within the same session to refresh timer
static int apply_force_on(hid_device *dev, uint8_t slot, int feat_idx) {
    uint8_t state[12];
    if (backlight2_read(dev, slot, feat_idx, state) != 0) return -1;

    uint8_t options = state[1];
    uint16_t dho = (uint16_t)(state[6] | (state[7] << 8));
    uint16_t dhi = (uint16_t)(state[8] | (state[9] << 8));
    uint16_t dpow = (uint16_t)(state[10] | (state[11] << 8));

    int max = backlight2_get_level_max(dev, slot, feat_idx);
    int level = (max > 0 ? max - 1 : 0x0F);

    // OFF (ignore failure)
    (void)backlight2_write(dev, slot, feat_idx, 0x00, options, 0xFF, 0x00, dho, dhi, dpow);

    // ON immediately (basic/auto mode)
    uint8_t mode = 0x00;
    uint8_t on_options = (uint8_t)((options & 0x07) | (mode << 3));
    int wr = backlight2_write(dev, slot, feat_idx, 0x01, on_options, 0xFF, (uint8_t)level, dho, dhi, dpow);
    if (wr != 0) return -1;

    return 0;
}

static void usage(const char *prog) {
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s on\n", prog);
    fprintf(stderr, "  %s off\n", prog);
    fprintf(stderr, "  %s force-on\n", prog);
}

static hid_device *open_receiver_and_resolve(uint16_t vid, uint16_t pid, int *out_slot, int *out_feat_idx, char *out_path, size_t out_path_cap) {
    struct hid_device_info *devs = hid_enumerate(vid, pid);
    struct hid_device_info *cur = devs;
    hid_device *best = NULL;
    *out_slot = -1;
    *out_feat_idx = -1;
    // candidate index not used in quiet build

    while (cur) {
        hid_device *h = hid_open_path(cur->path);
        if (h) {
            // Try to resolve feature on each slot once
            for (uint8_t dev_index = 1; dev_index <= 6; ++dev_index) {
                int idx = resolve_feature_index(h, dev_index, HIDPP_FEATURE_BACKLIGHT2);
                if (idx >= 0) {
                    best = h;
                    *out_slot = dev_index;
                    *out_feat_idx = idx;
                    if (out_path && cur->path) {
                        strncpy(out_path, cur->path, out_path_cap - 1);
                        out_path[out_path_cap - 1] = '\0';
                    }
                    break;
                }
            }
            if (!best) {
                hid_close(h);
            } else {
                break;
            }
        }
        cur = cur->next;
    }
    hid_free_enumeration(devs);
    return best;
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(argv[0]); return 1; }
    const char *cmd = argv[1];

    if (hid_init() != 0) {
        fprintf(stderr, "hid_init failed\n");
        return 1;
    }

    int found_idx = -1;
    int feat_idx = -1;
    char cached_path[1024];
    hid_device *dev = NULL;
    if (load_cache(cached_path, sizeof(cached_path), &found_idx, &feat_idx) == 0) {
        dev = hid_open_path(cached_path);
        if (!dev) {
            clear_cache();
        } else {
            // Verify that this slot actually exposes BACKLIGHT2; adjust if needed
            int resolved = resolve_feature_index(dev, (uint8_t)found_idx, HIDPP_FEATURE_BACKLIGHT2);
            if (resolved >= 0) {
                if (resolved != feat_idx) {
                    feat_idx = resolved;
                    save_cache(cached_path, found_idx, feat_idx);
                }
            } else {
                // Cached slot no longer exposes BACKLIGHT2: clear and re-enumerate later
                hid_close(dev);
                dev = NULL;
                clear_cache();
            }
        }
    }
    char chosen_path[1024] = {0};
    uint16_t used_vid = 0, used_pid = 0;
    get_receiver_ids(&used_vid, &used_pid);
    if (!dev) {
        found_idx = -1;
        feat_idx = -1;
        dev = open_receiver_and_resolve(used_vid, used_pid, &found_idx, &feat_idx, chosen_path, sizeof(chosen_path));
        if (found_idx < 0 || !dev) {
            fprintf(stderr, "Error: could not open receiver %04x:%04x or find BACKLIGHT2.\n", used_vid, used_pid);
            if (dev) hid_close(dev);
            hid_exit();
            return 1;
        }
        if (chosen_path[0]) save_cache(chosen_path, found_idx, feat_idx);
    } else {
        // We have a validated/updated cached target; ensure feature reads
        uint8_t probe[12];
        if (backlight2_read(dev, (uint8_t)found_idx, feat_idx, probe) != 0) {
            hid_close(dev);
            clear_cache();
            dev = open_receiver_and_resolve(used_vid, used_pid, &found_idx, &feat_idx, chosen_path, sizeof(chosen_path));
            if (found_idx < 0 || !dev) {
                fprintf(stderr, "Error: device cache invalid and re-discovery failed.\n");
                if (dev) hid_close(dev);
                hid_exit();
                return 1;
            }
            if (chosen_path[0]) save_cache(chosen_path, found_idx, feat_idx);
        }
    }

    // Determine desired action (support only on/off) with verification
    int rc = 0;
    if (strcmp(cmd, "on") == 0) {
        rc = apply_on(dev, (uint8_t)found_idx, feat_idx);
        if (rc == 0) {
            printf("Backlight enabled.\n");
        }
    } else if (strcmp(cmd, "off") == 0) {
        rc = apply_off(dev, (uint8_t)found_idx, feat_idx);
        if (rc == 0) {
            printf("Backlight disabled.\n");
        }
    } else if (strcmp(cmd, "force-on") == 0) {
        rc = apply_force_on(dev, (uint8_t)found_idx, feat_idx);
        if (rc == 0) {
            printf("Backlight forced on.\n");
        }
    } else {
        usage(argv[0]);
        hid_close(dev);
        hid_exit();
        return 1;
    }

    if (rc != 0) {
        // Command did not verify on cached target. Re-enumerate and retry once.
        clear_cache();
        hid_close(dev);
        found_idx = -1;
        feat_idx = -1;
        dev = open_receiver_and_resolve(used_vid, used_pid, &found_idx, &feat_idx, chosen_path, sizeof(chosen_path));
        if (!dev || found_idx < 0) {
            fprintf(stderr, "Error: target not found after cache invalidation.\n");
            if (dev) hid_close(dev);
            hid_exit();
            return 1;
        }
        if (strcmp(cmd, "on") == 0) rc = apply_on(dev, (uint8_t)found_idx, feat_idx);
        else if (strcmp(cmd, "off") == 0) rc = apply_off(dev, (uint8_t)found_idx, feat_idx);
        else rc = apply_force_on(dev, (uint8_t)found_idx, feat_idx);
        if (rc == 0) {
            if (chosen_path[0]) save_cache(chosen_path, found_idx, feat_idx);
            if (strcmp(cmd, "on") == 0) printf("Backlight enabled.\n");
            else if (strcmp(cmd, "off") == 0) printf("Backlight disabled.\n");
            else printf("Backlight forced on.\n");
        } else {
            fprintf(stderr, "Error: command did not take effect.\n");
            hid_close(dev);
            hid_exit();
            return 1;
        }
    }

    hid_close(dev);
    hid_exit();
    return 0;
}
