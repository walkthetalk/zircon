// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#define _DARWIN_C_SOURCE

#include <inttypes.h>
#include <libgen.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <zircon/boot/netboot.h>

#include "bootserver.h"

#define ANSI_RED "\x1b[31m"
#define ANSI_GREEN "\x1b[32m"
#define ANSI_YELLOW "\x1b[33m"
#define ANSI_BLUE "\x1b[34m"
#define ANSI_MAGENTA "\x1b[35m"
#define ANSI_CYAN "\x1b[36m"
#define ANSI_RESET "\x1b[0m"
#define ANSI_CLEARLINE "\33[2K\r"

#define MAX_FVM_IMAGES 4

#define ANSI(name) (use_color == false || is_redirected) ? "" : ANSI_##name

#define log(args...)                                                      \
    do {                                                                  \
        char logline[1024];                                               \
        snprintf(logline, sizeof(logline), args);                         \
        fprintf(stderr, "%s [%s] %s\n", date_string(), appname, logline); \
    } while (false)

char* appname;
int64_t us_between_packets = DEFAULT_US_BETWEEN_PACKETS;

static bool use_tftp = true;
static bool use_color = true;
static size_t total_file_size;
static bool file_info_printed;
static int progress_reported;
static int packets_sent;
static char filename_in_flight[PATH_MAX];
static struct timeval start_time, end_time;
static bool is_redirected;
static const char spinner[] = {'|', '/', '-', '\\'};

char* date_string() {
    static char date_buf[80];
    time_t t = time(NULL);
    struct tm tm = *localtime(&t);

    snprintf(date_buf, sizeof(date_buf), "%4d-%02d-%02d %02d:%02d:%02d",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return date_buf;
}

char* sockaddr_str(struct sockaddr_in6* addr) {
    static char buf[128];
    char tmp[INET6_ADDRSTRLEN];
    snprintf(buf, sizeof(buf), "[%s]:%d",
             inet_ntop(AF_INET6, &addr->sin6_addr, tmp, INET6_ADDRSTRLEN),
             ntohs(addr->sin6_port));
    return buf;
}

void initialize_status(const char* name, size_t size) {
    total_file_size = size;
    progress_reported = 0;
    packets_sent = 0;
    snprintf(filename_in_flight, sizeof(filename_in_flight),
             "%s%s%s", ANSI(GREEN), name, ANSI(RESET));
}

void update_status(size_t bytes_so_far) {
    char progress_str[PATH_MAX];
    size_t offset = 0;

#define UPDATE_LOG(args...)                                                     \
    do {                                                                        \
        if (offset < PATH_MAX) {                                                \
            offset += snprintf(progress_str + offset, PATH_MAX - offset, args); \
        }                                                                       \
    } while (false)

    packets_sent++;

    bool is_last_piece = (bytes_so_far == total_file_size);
    if (total_file_size == 0) {
        return;
    }

    if (is_redirected) {
        int percent_sent = (bytes_so_far * 100 / (total_file_size));
        if (percent_sent - progress_reported >= 5) {
            fprintf(stderr, "\t%d%%...", percent_sent);
            progress_reported = percent_sent;
        }
    } else {
        if (packets_sent > 1024 || is_last_piece) {
            packets_sent = 0;
            static int spin = 0;

            size_t divider = (total_file_size > 0) ? total_file_size : 1;
            UPDATE_LOG("[%c] %4.01f%% of ", spinner[(spin++) % 4],
                       100.0 * (float)bytes_so_far / (float)divider);
            if (total_file_size < 1024) {
                UPDATE_LOG(" %3zu.0  B", total_file_size);
            } else if (total_file_size < 1024 * 1024) {
                UPDATE_LOG(" %5.1f KB", (float)total_file_size / 1024.0);
            } else if (total_file_size < 1024 * 1024 * 1024) {
                UPDATE_LOG(" %5.1f MB", (float)total_file_size / 1024.0 / 1024.0);
            } else {
                UPDATE_LOG(" %5.1f GB", (float)total_file_size / 1024.0 / 1024.0 / 1024.0);
            }

            struct timeval now;
            gettimeofday(&now, NULL);
            int64_t sec = (int64_t)(now.tv_sec - start_time.tv_sec);
            int64_t usec = (int64_t)(now.tv_usec - start_time.tv_usec);
            int64_t elapsed_usec = sec * 1000000 + usec;
            float bytes_in_sec;
            bytes_in_sec = (float)bytes_so_far * 1000000 / ((float)elapsed_usec);
            if (bytes_in_sec < 1024) {
                UPDATE_LOG("  %5.1f  B/s", bytes_in_sec);
            } else if (bytes_in_sec < 1024 * 1024) {
                UPDATE_LOG("  %5.1f KB/s", bytes_in_sec / 1024.0);
            } else if (bytes_in_sec < 1024 * 1024 * 1024) {
                UPDATE_LOG("  %5.1f MB/s", bytes_in_sec / 1024.0 / 1024.0);
            } else {
                UPDATE_LOG("  %5.1f GB/s", bytes_in_sec / 1024.0 / 1024.0 / 1024.0);
            }

            if (is_last_piece) {
                UPDATE_LOG(".");
            } else {
                UPDATE_LOG(" ");
            }
            UPDATE_LOG("  %s", filename_in_flight);
            fprintf(stderr, "%s%s", ANSI_CLEARLINE, progress_str);
        }
    }
}

static int xfer(struct sockaddr_in6* addr, const char* local_name, const char* remote_name) {
    int result;
    is_redirected = !isatty(fileno(stdout));
    gettimeofday(&start_time, NULL);
    file_info_printed = false;
    if (use_tftp) {
        bool first = true;
        while ((result = tftp_xfer(addr, local_name, remote_name)) == -EAGAIN) {
            if (first) {
                fprintf(stderr, "Target busy, waiting.");
                first = false;
            } else {
                fprintf(stderr, ".");
            }
            sleep(1);
            gettimeofday(&start_time, NULL);
        }
    } else {
        result = netboot_xfer(addr, local_name, remote_name);
    }
    gettimeofday(&end_time, NULL);
    if (end_time.tv_usec < start_time.tv_usec) {
        end_time.tv_sec -= 1;
        end_time.tv_usec += 1000000;
    }
    fprintf(stderr, "\n");
    return result;
}

void usage(void) {
    fprintf(stderr,
            "usage:   %s [ <option> ]* [<kernel>] [ <ramdisk> ] [ -- [ <kerneloption> ]* ]\n"
            "\n"
            "options:\n"
            "  -1         only boot once, then exit\n"
            "  -a         only boot device with this IPv6 address\n"
            "  -b <sz>    tftp block size (default=%d, ignored with --netboot)\n"
            "  -i <NN>    number of microseconds between packets\n"
            "             set between 50-500 to deal with poor bootloader network stacks (default=%d)\n"
            "             (ignored with --tftp)\n"
            "  -n         only boot device with this nodename\n"
            "  -w <sz>    tftp window size (default=%d, ignored with --netboot)\n"
            "  --board_name <name>      name of the board files are meant for\n"
            "  --boot <file>            use the supplied file as a kernel\n"
            "  --fvm <file>             use the supplied file as a sparse FVM image (up to 4 times)\n"
            "  --bootloader <file>      use the supplied file as a BOOTLOADER image\n"
            "  --zircona <file>         use the supplied file as a ZIRCON-A ZBI\n"
            "  --zirconb <file>         use the supplied file as a ZIRCON-B ZBI\n"
            "  --zirconr <file>         use the supplied file as a ZIRCON-R ZBI\n"
            "  --vbmetaa <file>         use the supplied file as a AVB vbmeta_a image\n"
            "  --vbmetab <file>         use the supplied file as a AVB vbmeta_b image\n"
            "  --vbmetar <file>         use the supplied file as a AVB vbmeta_r image\n"
            "  --authorized-keys <file> use the supplied file as an authorized_keys file\n"
            "  --fail-fast  exit on first error\n"
            "  --netboot    use the netboot protocol\n"
            "  --tftp       use the tftp protocol (default)\n"
            "  --nocolor    disable ANSI color (false)\n",
            appname, DEFAULT_TFTP_BLOCK_SZ, DEFAULT_US_BETWEEN_PACKETS, DEFAULT_TFTP_WIN_SZ);
    exit(1);
}

void drain(int fd) {
    char buf[4096];
    if (fcntl(fd, F_SETFL, O_NONBLOCK) == 0) {
        while (read(fd, buf, sizeof(buf)) > 0)
            ;
        fcntl(fd, F_SETFL, 0);
    }
}

int send_boot_command(struct sockaddr_in6* ra) {
    // Construct message
    nbmsg msg;
    static int cookie = 0;
    msg.magic = NB_MAGIC;
    msg.cookie = cookie++;
    msg.cmd = NB_BOOT;
    msg.arg = 0;

    // Send to NB_SERVER_PORT
    struct sockaddr_in6 target_addr;
    memcpy(&target_addr, ra, sizeof(struct sockaddr_in6));
    target_addr.sin6_port = htons(NB_SERVER_PORT);
    int s = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) {
        log("cannot create socket %d", s);
        return -1;
    }
    ssize_t send_result = sendto(s, &msg, sizeof(msg), 0, (struct sockaddr*)&target_addr,
                                 sizeof(target_addr));
    if (send_result == sizeof(msg)) {
        close(s);
        log("Issued boot command to %s\n\n", sockaddr_str(ra));
        return 0;
    }
    close(s);
    log("failure sending boot command to %s", sockaddr_str(ra));
    return -1;
}

int send_reboot_command(struct sockaddr_in6* ra) {
    // Construct message
    nbmsg msg;
    static int cookie = 0;
    msg.magic = NB_MAGIC;
    msg.cookie = cookie++;
    msg.cmd = NB_REBOOT;
    msg.arg = 0;

    // Send to NB_SERVER_PORT
    struct sockaddr_in6 target_addr;
    memcpy(&target_addr, ra, sizeof(struct sockaddr_in6));
    target_addr.sin6_port = htons(NB_SERVER_PORT);
    int s = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) {
        log("cannot create socket %d", s);
        return -1;
    }
    ssize_t send_result = sendto(s, &msg, sizeof(msg), 0, (struct sockaddr*)&target_addr,
                                 sizeof(target_addr));
    if (send_result == sizeof(msg)) {
        close(s);
        log("Issued reboot command to %s\n\n", sockaddr_str(ra));
        return 0;
    }
    close(s);
    log("failure sending reboot command to %s", sockaddr_str(ra));
    return -1;
}

int main(int argc, char** argv) {
    bool fail_fast = false;
    struct in6_addr allowed_addr;
    struct sockaddr_in6 addr;
    char tmp[INET6_ADDRSTRLEN];
    char cmdline[4096];
    char* cmdnext = cmdline;
    char* nodename = NULL;
    int s = 1;
    size_t num_fvms = 0;
    char board_name_template[] = "/tmp/board_name.XXXXXX";
    const char* board_name = NULL;
    const char* board_name_file = NULL;
    const char* bootloader_image = NULL;
    const char* zircona_image = NULL;
    const char* zirconb_image = NULL;
    const char* zirconr_image = NULL;
    const char* vbmetaa_image = NULL;
    const char* vbmetab_image = NULL;
    const char* vbmetar_image = NULL;
    const char* authorized_keys = NULL;
    const char* fvm_images[MAX_FVM_IMAGES] = {NULL, NULL, NULL, NULL};
    const char* kernel_fn = NULL;
    const char* ramdisk_fn = NULL;
    int once = 0;
    int status;

    memset(&allowed_addr, 0, sizeof(allowed_addr));
    cmdline[0] = 0;
    if ((appname = strrchr(argv[0], '/')) != NULL) {
        appname++;
    } else {
        appname = argv[0];
    }

    while (argc > 1) {
        if (argv[1][0] != '-') {
            if (kernel_fn == NULL) {
                kernel_fn = argv[1];
            } else if (ramdisk_fn == NULL) {
                ramdisk_fn = argv[1];
            } else {
                usage();
            }
        } else if (!strcmp(argv[1], "--fvm")) {
            argc--;
            argv++;
            if (argc <= 1) {
                fprintf(stderr, "'--fvm' option requires an argument (FVM image)\n");
                return -1;
            }
            if (num_fvms == MAX_FVM_IMAGES) {
                fprintf(stderr, "'--fvm' supplied too many times\n");
                return -1;
            }
            fvm_images[num_fvms++] = argv[1];
        } else if (!strcmp(argv[1], "--bootloader")) {
            argc--;
            argv++;
            if (argc <= 1) {
                fprintf(stderr, "'--bootloader' option requires an argument (BOOTLOADER image)\n");
                return -1;
            }
            bootloader_image = argv[1];
        } else if (!strcmp(argv[1], "--zircona")) {
            argc--;
            argv++;
            if (argc <= 1) {
                fprintf(stderr, "'--zircona' option requires an argument (ZIRCON-A image)\n");
                return -1;
            }
            zircona_image = argv[1];
        } else if (!strcmp(argv[1], "--zirconb")) {
            argc--;
            argv++;
            if (argc <= 1) {
                fprintf(stderr, "'--zirconb' option requires an argument (ZIRCON-B image)\n");
                return -1;
            }
            zirconb_image = argv[1];
        } else if (!strcmp(argv[1], "--zirconr")) {
            argc--;
            argv++;
            if (argc <= 1) {
                fprintf(stderr, "'--zirconr' option requires an argument (ZIRCON-R image)\n");
                return -1;
            }
            zirconr_image = argv[1];
        } else if (!strcmp(argv[1], "--vbmetaa")) {
            argc--;
            argv++;
            if (argc <= 1) {
                fprintf(stderr, "'--vbmetaa' option requires an argument (vbmeta_a image)\n");
                return -1;
            }
            vbmetaa_image = argv[1];
        } else if (!strcmp(argv[1], "--vbmetab")) {
            argc--;
            argv++;
            if (argc <= 1) {
                fprintf(stderr, "'--vbmetab' option requires an argument (vbmeta_b image)\n");
                return -1;
            }
            vbmetab_image = argv[1];
        } else if (!strcmp(argv[1], "--vbmetar")) {
            argc--;
            argv++;
            if (argc <= 1) {
                fprintf(stderr, "'--vbmetar' option requires an argument (vbmeta_r image)\n");
                return -1;
            }
            vbmetar_image = argv[1];
        } else if (!strcmp(argv[1], "--authorized-keys")) {
            argc--;
            argv++;
            if (argc <= 1) {
                fprintf(stderr,
                        "'--authorized-keys' option requires an argument (authorized_keys)\n");
                return -1;
            }
            authorized_keys = argv[1];
        } else if (!strcmp(argv[1], "--fail-fast")) {
            fail_fast = true;
        } else if (!strcmp(argv[1], "--boot")) {
            argc--;
            argv++;
            if (argc <= 1) {
                fprintf(stderr,
                        "'--boot' option requires an argument (a kernel image)\n");
                return -1;
            }
            kernel_fn = argv[1];
        } else if (!strcmp(argv[1], "-1")) {
            once = 1;
        } else if (!strcmp(argv[1], "-b")) {
            if (argc <= 1) {
                fprintf(stderr, "'-b' option requires an argument (tftp block size)\n");
                return -1;
            }
            errno = 0;
            static uint16_t block_size;
            block_size = strtoll(argv[2], NULL, 10);
            if (errno != 0 || block_size <= 0) {
                fprintf(stderr, "invalid arg for -b: %s\n", argv[2]);
                return -1;
            }
            tftp_block_size = &block_size;
            argc--;
            argv++;
        } else if (!strcmp(argv[1], "-w")) {
            if (argc <= 1) {
                fprintf(stderr, "'-w' option requires an argument (tftp window size)\n");
                return -1;
            }
            errno = 0;
            static uint16_t window_size;
            window_size = strtoll(argv[2], NULL, 10);
            if (errno != 0 || window_size <= 0) {
                fprintf(stderr, "invalid arg for -w: %s\n", argv[2]);
                return -1;
            }
            tftp_window_size = &window_size;
            argc--;
            argv++;
        } else if (!strcmp(argv[1], "-i")) {
            if (argc <= 1) {
                fprintf(stderr, "'-i' option requires an argument (micros between packets)\n");
                return -1;
            }
            errno = 0;
            us_between_packets = strtoll(argv[2], NULL, 10);
            if (errno != 0 || us_between_packets <= 0) {
                fprintf(stderr, "invalid arg for -i: %s\n", argv[2]);
                return -1;
            }
            fprintf(stderr, "packet spacing set to %" PRId64 " microseconds\n", us_between_packets);
            argc--;
            argv++;
        } else if (!strcmp(argv[1], "-a")) {
            if (argc <= 1) {
                fprintf(stderr, "'-a' option requires a valid ipv6 address\n");
                return -1;
            }
            if (inet_pton(AF_INET6, argv[2], &allowed_addr) != 1) {
                fprintf(stderr, "%s: invalid ipv6 address specified\n", argv[2]);
                return -1;
            }
            argc--;
            argv++;
        } else if (!strcmp(argv[1], "-n")) {
            if (argc <= 1) {
                fprintf(stderr, "'-n' option requires a valid nodename\n");
                return -1;
            }
            nodename = argv[2];
            argc--;
            argv++;
        } else if (!strcmp(argv[1], "--netboot")) {
            use_tftp = false;
        } else if (!strcmp(argv[1], "--tftp")) {
            use_tftp = true;
        } else if (!strcmp(argv[1], "--nocolor")) {
            use_color = false;
        } else if (!strcmp(argv[1], "--board_name")) {
            if (argc <= 1) {
                fprintf(stderr, "'--board_name' option requires a valid board name\n");
                return -1;
            }
            board_name = argv[2];
            argc--;
            argv++;
        } else if (!strcmp(argv[1], "--")) {
            while (argc > 2) {
                size_t len = strlen(argv[2]);
                if (len > (sizeof(cmdline) - 2 - (cmdnext - cmdline))) {
                    fprintf(stderr, "[%s] commandline too large\n", appname);
                    return -1;
                }
                if (cmdnext != cmdline) {
                    *cmdnext++ = ' ';
                }
                memcpy(cmdnext, argv[2], len + 1);
                cmdnext += len;
                argc--;
                argv++;
            }
            break;
        } else {
            usage();
        }
        argc--;
        argv++;
    }
    if (!kernel_fn && !bootloader_image && !zircona_image && !zirconb_image && !zirconr_image &&
        !vbmetaa_image && !vbmetab_image && !fvm_images[0]) {
        usage();
    }
    if (!nodename) {
        nodename = getenv("ZIRCON_NODENAME");
    }
    if (nodename) {
        fprintf(stderr, "[%s] Will only boot nodename '%s'\n", appname, nodename);
    }

    if (board_name) {
        log("Board name set to %s", board_name);
        board_name_file = mktemp(board_name_template);
        int fd = open(board_name_file, O_WRONLY | O_CREAT);
        write(fd, board_name, strlen(board_name));
        close(fd);
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin6_family = AF_INET6;
    addr.sin6_port = htons(NB_ADVERT_PORT);

    s = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) {
        log("cannot create socket %d", s);
        return -1;
    }
    if (bind(s, (void*)&addr, sizeof(addr)) < 0) {
        log("cannot bind to %s %d: %s\nthere may be another bootserver running\n",
            sockaddr_str(&addr),
            errno, strerror(errno));
        close(s);
        return -1;
    }

    log("listening on %s", sockaddr_str(&addr));

    for (;;) {
        struct sockaddr_in6 ra;
        socklen_t rlen;
        char buf[4096];
        nbmsg* msg = (void*)buf;
        rlen = sizeof(ra);
        ssize_t r = recvfrom(s, buf, sizeof(buf) - 1, 0, (void*)&ra, &rlen);
        if (r < 0) {
            log("socket read error %s", strerror(errno));
            close(s);
            return -1;
        }
        if ((size_t)r < sizeof(nbmsg))
            continue;
        if (!IN6_IS_ADDR_LINKLOCAL(&ra.sin6_addr)) {
            log("ignoring non-link-local message");
            continue;
        }
        if (!IN6_IS_ADDR_UNSPECIFIED(&allowed_addr) &&
            !IN6_ARE_ADDR_EQUAL(&allowed_addr, &ra.sin6_addr)) {
            log("ignoring message not from allowed address '%s'",
                inet_ntop(AF_INET6, &allowed_addr, tmp, sizeof(tmp)));
            continue;
        }
        if (msg->magic != NB_MAGIC)
            continue;
        if (msg->cmd != NB_ADVERTISE)
            continue;
        if ((use_tftp && (msg->arg < NB_VERSION_1_3)) ||
            (!use_tftp && (msg->arg < NB_VERSION_1_1))) {
            log("%sIncompatible version 0x%08X of bootloader "
                "detected from %s, please upgrade your bootloader%s",
                ANSI(RED), msg->arg, sockaddr_str(&ra), ANSI(RESET));
            if (once) {
                close(s);
                return -1;
            }
            continue;
        }

        log("Received request from %s", sockaddr_str(&ra));

        // ensure any payload is null-terminated
        buf[r] = 0;

        char* save = NULL;
        char* adv_nodename = NULL;
        const char* adv_version = "unknown";
        for (char* var = strtok_r((char*)msg->data, ";", &save);
             var;
             var = strtok_r(NULL, ";", &save)) {
            if (!strncmp(var, "nodename=", 9)) {
                adv_nodename = var + 9;
            } else if (!strncmp(var, "version=", 8)) {
                adv_version = var + 8;
            }
        }

        if (nodename) {
            if (adv_nodename == NULL) {
                log("ignoring unknown nodename (expecting %s)",
                    nodename);
            } else if (strcmp(adv_nodename, nodename)) {
                log("ignoring nodename %s (expecting %s)",
                    adv_nodename, nodename);
                continue;
            }
        }

        if (strcmp(BOOTLOADER_VERSION, adv_version)) {
            log("%sWARNING: Bootserver version '%s' != remote Zedboot version '%s'."
                " Device will not be serviced. Please upgrade Zedboot.%s",
                ANSI(RED), BOOTLOADER_VERSION, adv_version, ANSI(RESET));
            continue;
        }

        if (adv_nodename) {
          log("Proceeding with nodename %s", adv_nodename);
        }

        log("Transfer starts");
        status = 0;
        // This needs to be first as it validates that the other images are
        // correct.
        if (status == 0 && board_name_file) {
            status = xfer(&ra, board_name_file, NB_BOARD_NAME_FILENAME);
            if (status != 0) {
                log("Invalid board name. Check fx set parameter?");
            }
        }
        if (status == 0 && cmdline[0]) {
            status = xfer(&ra, "(cmdline)", cmdline);
        }
        if (status == 0) {
            if (ramdisk_fn) {
                status = xfer(&ra, ramdisk_fn, NB_RAMDISK_FILENAME);
            }
        }
        for (size_t i = 0; i < num_fvms; i++) {
            if (status == 0 && fvm_images[i]) {
                status = xfer(&ra, fvm_images[i], NB_FVM_FILENAME);
            }
        }
        if (status == 0 && bootloader_image) {
            status = xfer(&ra, bootloader_image, NB_BOOTLOADER_FILENAME);
        }
        if (status == 0 && zircona_image) {
            status = xfer(&ra, zircona_image, NB_ZIRCONA_FILENAME);
        }
        if (status == 0 && zirconb_image) {
            status = xfer(&ra, zirconb_image, NB_ZIRCONB_FILENAME);
        }
        if (status == 0 && zirconr_image) {
            status = xfer(&ra, zirconr_image, NB_ZIRCONR_FILENAME);
        }
        if (status == 0 && vbmetaa_image) {
            status = xfer(&ra, vbmetaa_image, NB_VBMETAA_FILENAME);
        }
        if (status == 0 && vbmetab_image) {
            status = xfer(&ra, vbmetab_image, NB_VBMETAB_FILENAME);
        }
        if (status == 0 && authorized_keys) {
            status = xfer(&ra, authorized_keys, NB_SSHAUTH_FILENAME);
        }
        if (status == 0 && kernel_fn) {
            status = xfer(&ra, kernel_fn, NB_KERNEL_FILENAME);
        }
        if (status == 0) {
            log("Transfer ends successfully.");
            if (kernel_fn) {
                send_boot_command(&ra);
            } else {
                send_reboot_command(&ra);
            }
        } else {
            log("Transfer ends incompletely.");
        }
        if ((status == 0 && once) || (status != 0 && fail_fast)) {
            close(s);
            return status == 0 ? 0 : -1;
        }
        drain(s);
    }

    close(s);
    return 0;
}
