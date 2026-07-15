/* RECEIVER — Sliding Window XOR FEC.
 *
 * Wire format (163 bytes):
 *   [1 byte type] [2 bytes big-endian seq_16] [160 bytes payload]
 *
 * type byte layout:
 *   Bits 7-4: current_fec_mode (0 = No FEC, 1 = XOR 3+1, 2 = XOR 2+1, 3 = XOR 1+1 / Duplicate)
 *   Bits 3-0: packet_type (0 = Data Packet, 1 = Sliding Parity Packet)
 *
 * Ports:
 *   bind 47002  <- media from sender
 *   send 47020  -> harness player
 *   send 47003  -> feedback to sender
 */
#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define PKT_SIZE        163
#define PLAYED_SIZE   65536
#define MAX_GAPS        512
#define RX_BUF_SIZE    2048

static uint64_t now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL;
}

static unsigned char played[PLAYED_SIZE];
static unsigned char rx_direct[PLAYED_SIZE];

static int is_played(uint32_t seq) {
    uint32_t idx = seq / 8;
    if (idx >= PLAYED_SIZE) return 1;
    return (played[idx] & (1u << (seq % 8))) != 0;
}

static void set_played(uint32_t seq) {
    uint32_t idx = seq / 8;
    if (idx < PLAYED_SIZE)
        played[idx] |= (1u << (seq % 8));
}

static int is_rx_direct(uint32_t seq) {
    uint32_t idx = seq / 8;
    if (idx >= PLAYED_SIZE) return 1;
    return (rx_direct[idx] & (1u << (seq % 8))) != 0;
}

static void set_rx_direct(uint32_t seq) {
    uint32_t idx = seq / 8;
    if (idx < PLAYED_SIZE)
        rx_direct[idx] |= (1u << (seq % 8));
}

static uint32_t recon32(uint16_t s16, uint32_t max32) {
    uint32_t epoch = max32 & 0xFFFF0000u;
    int diff = (int)s16 - (int)(uint16_t)(max32 & 0xFFFF);
    if (diff < -32768) return epoch + 0x10000u + s16;
    if (diff >  32768 && epoch >= 0x10000u) return epoch - 0x10000u + s16;
    return epoch + s16;
}

/* Gap table */
struct Gap {
    uint32_t seq;
    uint64_t created_ms;
    uint64_t first_nack_ms;
    uint64_t last_nack_ms;
    int      active;
} gaps[MAX_GAPS];

static uint64_t grace_ms = 0;
static uint64_t retry_ms = 25;
static uint64_t expire_ms = 200;

static void gap_insert(uint32_t seq, uint64_t now) {
    for (int k = 0; k < MAX_GAPS; k++)
        if (gaps[k].active && gaps[k].seq == seq) return;

    for (int k = 0; k < MAX_GAPS; k++) {
        if (!gaps[k].active) {
            gaps[k].seq          = seq;
            gaps[k].created_ms   = now;
            gaps[k].first_nack_ms = 0;
            gaps[k].last_nack_ms  = 0;
            gaps[k].active       = 1;
            return;
        }
    }

    uint64_t oldest = UINT64_MAX;
    int ok = 0;
    for (int k = 0; k < MAX_GAPS; k++) {
        if (gaps[k].active && gaps[k].created_ms < oldest) {
            oldest = gaps[k].created_ms;
            ok = k;
        }
    }
    gaps[ok].seq           = seq;
    gaps[ok].created_ms    = now;
    gaps[ok].first_nack_ms = 0;
    gaps[ok].last_nack_ms  = 0;
    gaps[ok].active        = 1;
}

static void gap_close(uint32_t seq) {
    for (int k = 0; k < MAX_GAPS; k++)
        if (gaps[k].active && gaps[k].seq == seq)
            gaps[k].active = 0;
}

/* Sliding Window FEC Storage */
static unsigned char rx_payload[RX_BUF_SIZE][160];
static unsigned char parity_payload[RX_BUF_SIZE][160];
static uint8_t parity_active[RX_BUF_SIZE];
static uint32_t parity_seq[RX_BUF_SIZE];

static void rx_store(uint32_t seq, const unsigned char *pl) {
    unsigned idx = seq % RX_BUF_SIZE;
    memcpy(rx_payload[idx], pl, 160);
}

static int rx_get(int32_t seq, unsigned char *out) {
    if (seq < 0) {
        memset(out, 0, 160);
        return 1;
    }
    if (is_played((uint32_t)seq)) {
        unsigned idx = (uint32_t)seq % RX_BUF_SIZE;
        memcpy(out, rx_payload[idx], 160);
        return 1;
    }
    return 0;
}

static void fwd_player(int out_fd, struct sockaddr_in *player,
                       uint32_t seq, const unsigned char *payload) {
    unsigned char buf[164];
    buf[0] = (seq >> 24) & 0xFF;
    buf[1] = (seq >> 16) & 0xFF;
    buf[2] = (seq >>  8) & 0xFF;
    buf[3] =  seq        & 0xFF;
    memcpy(&buf[4], payload, 160);
    sendto(out_fd, buf, 164, 0, (struct sockaddr *)player, sizeof *player);
}

static int W = 4;

static void try_resolve(uint32_t max_seq, int out_fd, struct sockaddr_in *player) {
    // Scan window of active parities around max_seq
    uint32_t start = max_seq > 120 ? max_seq - 120 : 0;
    uint32_t end = max_seq + 10;

    for (int step = 0; step < 6; step++) { // Iterative back-substitution loops
        int resolved_any = 0;
        for (uint32_t j = start; j <= end; j++) {
            unsigned idx = j % RX_BUF_SIZE;
            if (parity_active[idx] && parity_seq[idx] == j) {
                int missing_count = 0;
                int32_t missing_seq = -1;

                for (int offset = 0; offset < W; offset++) {
                    int32_t s = (int32_t)j - offset;
                    if (s >= 0) {
                        if (!is_played((uint32_t)s)) {
                            missing_count++;
                            missing_seq = s;
                        }
                    }
                }

                if (missing_count == 0) {
                    parity_active[idx] = 0;
                } 
                else if (missing_count == 1) {
                    unsigned char reconstructed[160];
                    memcpy(reconstructed, parity_payload[idx], 160);
                    int valid = 1;

                    for (int offset = 0; offset < W; offset++) {
                        int32_t s = (int32_t)j - offset;
                        if (s != missing_seq) {
                            unsigned char temp[160];
                            if (rx_get(s, temp)) {
                                for (int i = 0; i < 160; i++) reconstructed[i] ^= temp[i];
                            } else {
                                valid = 0;
                            }
                        }
                    }

                    if (valid && !is_played((uint32_t)missing_seq)) {
                        rx_store((uint32_t)missing_seq, reconstructed);
                        set_played((uint32_t)missing_seq);
                        gap_close((uint32_t)missing_seq);
                        fwd_player(out_fd, player, (uint32_t)missing_seq, reconstructed);

                        parity_active[idx] = 0;
                        resolved_any = 1;
                    }
                }
            }
        }
        if (!resolved_any) break;
    }
}

static void send_nack(int fd, struct sockaddr_in *dst, uint32_t seq) {
    unsigned char buf[4];
    buf[0] = (seq >> 24) & 0xFF;
    buf[1] = (seq >> 16) & 0xFF;
    buf[2] = (seq >>  8) & 0xFF;
    buf[3] =  seq        & 0xFF;
    sendto(fd, buf, 4, 0, (struct sockaddr *)dst, sizeof *dst);
}

static uint32_t max_seq = 0;
static int seen_any = 0;

static void update_max_seq(uint32_t seq32, uint8_t fec_mode, uint64_t now, int fb_fd, struct sockaddr_in *relay_fb) {
    if (seq32 > max_seq) {
        if (fec_mode != 3) {
            for (uint32_t s = max_seq + 1; s < seq32; s++) {
                if (!is_played(s)) {
                    gap_insert(s, now);
                    if (grace_ms == 0) {
                        send_nack(fb_fd, relay_fb, s);
                        for (int kk = 0; kk < MAX_GAPS; kk++) {
                            if (gaps[kk].active && gaps[kk].seq == s) {
                                gaps[kk].first_nack_ms = now;
                                gaps[kk].last_nack_ms  = now;
                                break;
                            }
                        }
                    }
                }
            }
        }
        max_seq = seq32;
    }
}

int main(void) {
    const char *e = getenv("DELAY_MS");
    uint64_t d = e ? (uint64_t)strtoul(e, NULL, 10) : 100;
    retry_ms = d / 4 < 20 ? 20 : d / 4;
    expire_ms = d;
    if (d <= 60) W = 2;

    int in_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in in_addr = {0};
    in_addr.sin_family      = AF_INET;
    in_addr.sin_port        = htons(47002);
    in_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (bind(in_fd, (struct sockaddr *)&in_addr, sizeof in_addr) < 0) {
        perror("bind 47002"); return 1;
    }

    int fb_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in relay_fb = {0};
    relay_fb.sin_family      = AF_INET;
    relay_fb.sin_port        = htons(47003);
    relay_fb.sin_addr.s_addr = inet_addr("127.0.0.1");

    int out_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in player = {0};
    player.sin_family      = AF_INET;
    player.sin_port        = htons(47020);
    player.sin_addr.s_addr = inet_addr("127.0.0.1");

    unsigned char in_buf[2048];

    uint64_t last_report_ms = 0;
    static uint64_t last_arrival_ms = 0;
    static uint32_t last_seq_for_jitter = 0;
    static double jitter_estimate = 0.0;

    for (;;) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(in_fd, &fds);

        struct timeval tv = { 0, 5000 };
        select(in_fd + 1, &fds, NULL, NULL, &tv);
        uint64_t now = now_ms();

        // 1. Send Feedback Report every 500ms
        if (last_report_ms == 0 || now - last_report_ms >= 500) {
            uint32_t start_seq = max_seq > 50 ? max_seq - 50 : 0;
            uint32_t end_seq = max_seq;
            uint32_t span = end_seq - start_seq + 1;
            
            int direct_count = 0;
            int max_burst = 0;
            int current_burst = 0;

            for (uint32_t s = start_seq; s <= end_seq; s++) {
                if (is_rx_direct(s)) {
                    direct_count++;
                }
                
                // Bursts are defined by played packets to reflect actual playout gaps
                if (is_played(s)) {
                    current_burst = 0;
                } else {
                    current_burst++;
                    if (current_burst > max_burst) max_burst = current_burst;
                }
            }

            double loss_rate = 1.0 - ((double)direct_count / (double)span);
            uint8_t loss_q8 = (uint8_t)(loss_rate * 255.0);
            
            unsigned char report[14] = {0};
            report[0] = 2; // Type 2 = Feedback Report
            report[1] = loss_q8;
            report[2] = (uint8_t)max_burst;
            report[3] = ((uint16_t)jitter_estimate >> 8) & 0xFF;
            report[4] = (uint16_t)jitter_estimate & 0xFF;
            
            uint64_t ts_us = now * 1000ULL;
            report[5]  = (ts_us >> 56) & 0xFF;
            report[6]  = (ts_us >> 48) & 0xFF;
            report[7]  = (ts_us >> 40) & 0xFF;
            report[8]  = (ts_us >> 32) & 0xFF;
            report[9]  = (ts_us >> 24) & 0xFF;
            report[10] = (ts_us >> 16) & 0xFF;
            report[11] = (ts_us >> 8)  & 0xFF;
            report[12] = ts_us         & 0xFF;

            sendto(fb_fd, report, 14, 0, (struct sockaddr *)&relay_fb, sizeof relay_fb);
            last_report_ms = now;
        }

        // 2. Service NACK table
        for (int k = 0; k < MAX_GAPS; k++) {
            struct Gap *g = &gaps[k];
            if (!g->active) continue;

            if (now - g->created_ms > expire_ms) { g->active = 0; continue; }
            if (now - g->created_ms < grace_ms) continue;

            if (g->first_nack_ms == 0) {
                send_nack(fb_fd, &relay_fb, g->seq);
                g->first_nack_ms = now;
                g->last_nack_ms  = now;
                continue;
            }

            if (now - g->last_nack_ms >= retry_ms) {
                send_nack(fb_fd, &relay_fb, g->seq);
                g->last_nack_ms = now;
            }
        }

        if (!FD_ISSET(in_fd, &fds)) continue;

        ssize_t n = recvfrom(in_fd, in_buf, sizeof in_buf, 0, NULL, NULL);
        if (n < PKT_SIZE) continue;

        uint8_t  fec_mode = in_buf[0] >> 4;
        uint8_t  type     = in_buf[0] & 0x0F;
        uint16_t s16      = ((uint16_t)in_buf[1] << 8) | in_buf[2];
        unsigned char *payload = &in_buf[3];
        uint32_t seq32;

        if (!seen_any) {
            seq32 = s16;
            max_seq = seq32;
            seen_any = 1;
        } else {
            seq32 = recon32(s16, max_seq);
        }

        // Jitter Estimation
        if (last_arrival_ms > 0 && seq32 > last_seq_for_jitter) {
            double expected = (seq32 - last_seq_for_jitter) * 20.0;
            double actual = (double)(now - last_arrival_ms);
            double diff = actual - expected;
            if (diff < 0) diff = -diff;
            jitter_estimate = jitter_estimate + (diff - jitter_estimate) / 16.0;
        }
        last_arrival_ms = now;
        last_seq_for_jitter = seq32;

        unsigned idx = seq32 % RX_BUF_SIZE;

        if (type == 0) {
            set_rx_direct(seq32);
            gap_close(seq32);
            rx_store(seq32, payload);

            if (is_played(seq32)) {
                try_resolve(max_seq, out_fd, &player);
                continue;
            }
            set_played(seq32);

            update_max_seq(seq32, fec_mode, now, fb_fd, &relay_fb);

            fwd_player(out_fd, &player, seq32, payload);
            try_resolve(max_seq, out_fd, &player);

        } else if (type == 1) {
            parity_active[idx] = 1;
            parity_seq[idx] = seq32;
            memcpy(parity_payload[idx], payload, 160);
            update_max_seq(seq32, fec_mode, now, fb_fd, &relay_fb);
            try_resolve(max_seq, out_fd, &player);
        }
    }
    return 0;
}
