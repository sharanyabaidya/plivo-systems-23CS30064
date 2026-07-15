/* SENDER — Sliding Window XOR FEC.
 *
 * Wire format (163 bytes per packet):
 *   [1 byte type] [2 bytes big-endian seq_16] [160 bytes payload]
 *
 * type byte layout:
 *   Bits 7-4: current_fec_mode (0 = No FEC, 1 = XOR 3+1 (N=4), 2 = XOR 2+1 (N=2), 3 = XOR 1+1 (N=1))
 *   Bits 3-0: packet_type (0 = Data Packet, 1 = Sliding Parity Packet)
 *
 * Ports:
 *   bind 47010  <- harness source
 *   send 47001  -> relay uplink
 *   bind 47004  <- NACK / Feedback reports from receiver
 */
#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <stdlib.h>

#define PKT_SIZE     163
#define HISTORY_SIZE 2048

/* State variables */
static int current_fec_mode = 2; // Default to Mode 2 (N=2)
static double smoothed_loss = 0.03;
static double smoothed_jitter = 5.0;
static double smoothed_rtt = 20.0;
static int playout_delay = 100;
static int consecutive_low_loss_windows = 0;

static uint64_t total_raw_received = 0;
static uint64_t total_sent_bytes = 0;

static uint64_t now_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

struct HistEntry {
    uint32_t seq;
    unsigned char payload[160];
    int active;
} history[HISTORY_SIZE];

static void hist_put(uint32_t seq, const unsigned char *pl) {
    unsigned idx = seq % HISTORY_SIZE;
    history[idx].seq = seq;
    memcpy(history[idx].payload, pl, 160);
    history[idx].active = 1;
}

static int hist_get(uint32_t seq, unsigned char *out) {
    unsigned idx = seq % HISTORY_SIZE;
    if (history[idx].active && history[idx].seq == seq) {
        memcpy(out, history[idx].payload, 160);
        return 1;
    }
    return 0;
}

static void send_pkt(int fd, struct sockaddr_in *dst,
                     uint8_t type, uint32_t seq,
                     const unsigned char *payload) {
    // Priority-based bandwidth ceiling clamp
    if (total_raw_received > 0) {
        double ratio = (double)(total_sent_bytes + PKT_SIZE) / (double)total_raw_received;
        if (type == 1) { // Proactive duplicate / FEC
            if (ratio > 1.90) return;
        }
        if (type == 3) { // NACK retransmission
            if (ratio > 1.98) return;
        }
    }

    unsigned char buf[PKT_SIZE];
    uint8_t wire_type = (type == 3) ? 0 : type;
    buf[0] = (current_fec_mode << 4) | (wire_type & 0x0F);
    buf[1] = (seq >> 8) & 0xFF;
    buf[2] =  seq       & 0xFF;
    memcpy(&buf[3], payload, 160);
    
    sendto(fd, buf, PKT_SIZE, 0, (struct sockaddr *)dst, sizeof *dst);
    total_sent_bytes += PKT_SIZE;
}

static int W = 4;

static void process_feedback_report(const unsigned char *buf, ssize_t len) {
    if (len < 14) return;
    double loss = (double)buf[1] / 255.0;
    double max_burst = (double)buf[2];
    double jitter = (double)((buf[3] << 8) | buf[4]);

    // RTT calculation
    uint64_t echo_ts = ((uint64_t)buf[5] << 56) |
                       ((uint64_t)buf[6] << 48) |
                       ((uint64_t)buf[7] << 40) |
                       ((uint64_t)buf[8] << 32) |
                       ((uint64_t)buf[9] << 24) |
                       ((uint64_t)buf[10] << 16) |
                       ((uint64_t)buf[11] << 8) |
                       ((uint64_t)buf[12]);
    uint64_t now = now_us();
    if (now >= echo_ts) {
        double rtt = (double)(now - echo_ts) / 1000.0;
        if (rtt > 0 && rtt < 1000) {
            smoothed_rtt = 0.8 * smoothed_rtt + 0.2 * rtt;
        }
    }

    smoothed_loss = 0.7 * smoothed_loss + 0.3 * loss;
    smoothed_jitter = 0.7 * smoothed_jitter + 0.3 * jitter;

    // Limit target mode to XOR 2+1 (Mode 2) if RTT is small enough compared to playout_delay
    // to allow NACK-based recovery. This saves bandwidth headroom for NACK retransmissions.
    int max_allowed_mode = 3;
    if (smoothed_rtt < (double)playout_delay * 0.7) {
        max_allowed_mode = 2;
    }

    int target_mode = current_fec_mode;
    if (smoothed_loss > 0.08 || max_burst >= 3.0) {
        target_mode = 3; // Mode 3: N=1 (Duplicate/Redundant)
    } else if (smoothed_loss > 0.03 || smoothed_jitter > 15.0) {
        target_mode = 2; // Mode 2: N=2 (XOR 2+1 equivalent)
    } else if (smoothed_loss > 0.005) {
        target_mode = 1; // Mode 1: N=4 (XOR 4+1 equivalent)
    } else {
        target_mode = 0; // Mode 0: No FEC
    }

    if (target_mode > max_allowed_mode) {
        target_mode = max_allowed_mode;
    }

    // Never drop below Mode 2 on tight playout delay profile (W == 2) to maintain minimum required protection
    if (W == 2 && target_mode < 2) {
        target_mode = 2;
    }

    if (target_mode > current_fec_mode) {
        current_fec_mode = target_mode;
        consecutive_low_loss_windows = 0;
    } else if (target_mode < current_fec_mode) {
        consecutive_low_loss_windows++;
        if (consecutive_low_loss_windows >= 3) {
            current_fec_mode = target_mode;
            consecutive_low_loss_windows = 0;
        }
    } else {
        consecutive_low_loss_windows = 0;
    }
}

int main(void) {
    int in_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in in_addr = {0};
    in_addr.sin_family      = AF_INET;
    in_addr.sin_port        = htons(47010);
    in_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (bind(in_fd, (struct sockaddr *)&in_addr, sizeof in_addr) < 0) {
        perror("bind 47010"); return 1;
    }

    int fb_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in fb_addr = {0};
    fb_addr.sin_family      = AF_INET;
    fb_addr.sin_port        = htons(47004);
    fb_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (bind(fb_fd, (struct sockaddr *)&fb_addr, sizeof fb_addr) < 0) {
        perror("bind 47004"); return 1;
    }

    int out_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in relay = {0};
    relay.sin_family      = AF_INET;
    relay.sin_port        = htons(47001);
    relay.sin_addr.s_addr = inet_addr("127.0.0.1");

    unsigned char in_buf[2048];
    unsigned char fb_buf[64];

    // Determine window size dynamically: W=2 for low playout delays to fit in deadline
    const char *env_delay = getenv("DELAY_MS");
    W = 4;
    if (env_delay) {
        int d = atoi(env_delay);
        playout_delay = d;
        if (d <= 60) W = 2;
    }

    int max_fd = in_fd > fb_fd ? in_fd : fb_fd;

    for (;;) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(in_fd, &fds);
        FD_SET(fb_fd, &fds);
        select(max_fd + 1, &fds, NULL, NULL, NULL);

        /* 1. New frame from harness source */
        if (FD_ISSET(in_fd, &fds)) {
            ssize_t n = recvfrom(in_fd, in_buf, sizeof in_buf, 0, NULL, NULL);
            if (n >= 164) {
                uint32_t seq = ((uint32_t)in_buf[0] << 24) |
                               ((uint32_t)in_buf[1] << 16) |
                               ((uint32_t)in_buf[2] <<  8) |
                               ((uint32_t)in_buf[3]);
                unsigned char *payload = &in_buf[4];

                total_raw_received += 160;

                hist_put(seq, payload);
                send_pkt(out_fd, &relay, 0, seq, payload);

                // Sliding Window FEC Generation (Adaptive Window Size W)
                if (current_fec_mode > 0) {
                    int N = (current_fec_mode == 1) ? 4 : ((current_fec_mode == 2) ? 2 : 1);
                    if (seq % N == 0) {
                        unsigned char parity[160];
                        memset(parity, 0, 160);
                        // XOR the last W frames [seq-W+1, seq]
                        for (int offset = 0; offset < W; offset++) {
                            if (seq >= offset) {
                                unsigned char temp[160];
                                if (hist_get(seq - offset, temp)) {
                                    for (int i = 0; i < 160; i++) parity[i] ^= temp[i];
                                }
                            }
                        }
                        send_pkt(out_fd, &relay, 1, seq, parity);
                    }
                }
            }
        }

        /* 2. NACK / Feedback report from receiver */
        if (FD_ISSET(fb_fd, &fds)) {
            ssize_t n = recvfrom(fb_fd, fb_buf, sizeof fb_buf, 0, NULL, NULL);
            if (n == 4) {
                uint32_t nack_seq = ((uint32_t)fb_buf[0] << 24) |
                                    ((uint32_t)fb_buf[1] << 16) |
                                    ((uint32_t)fb_buf[2] <<  8) |
                                    ((uint32_t)fb_buf[3]);
                unsigned char retx[160];
                if (hist_get(nack_seq, retx)) {
                    send_pkt(out_fd, &relay, 3, nack_seq, retx);
                }
            } else if (n >= 14 && fb_buf[0] == 2) {
                process_feedback_report(fb_buf, n);
            }
        }
    }
    return 0;
}
