/*
 * IEEE 802.11a Full Transceiver Pipeline Simulation
 * 
 * Pipeline:
 *   1. Generate 15000-byte string, split into 10 chunks of 1500 bytes
 *   2. CRC-32 encode each chunk (1500 -> 1504 bytes)
 *   3. PPDU encapsulate (1504 -> 1515 bytes PPDU frame)
 *   4. Scramble (SIGNAL bytes pass-through, DATA scrambled)
 *   5. QAM map to I/Q samples (BPSK / QPSK / 16-QAM / 64-QAM — user selects)
 *   6. Plot TX constellation
 *   7. IFFT (OFDM modulation)
 *   8. Plot time-domain "constellation" (I vs Q after IFFT)
 *   9. FFT (OFDM demodulation) — recover subcarrier symbols
 *  10. Plot RX constellation (should match TX constellation)
 *  11. Reverse pipeline: QAM demap -> descramble -> PPDU decap -> CRC decode
 *  12. Print recovered string
 *
 * Compile:
 *   g++ -O2 -std=c++17 wifi_pipeline_sim.cpp -o wifi_pipeline_sim -lm
 *
 * Run:
 *   ./wifi_pipeline_sim [modulation]
 *   modulation: bpsk | qpsk | 16qam | 64qam  (default: qpsk)
 */

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>
#include <complex>
#include <algorithm>
#include <cassert>

// ============================================================
// Configuration
// ============================================================
static const int CHUNK_SIZE      = 1500;   // bytes per chunk
static const int NUM_CHUNKS      = 10;
static const int TOTAL_DATA_SIZE = CHUNK_SIZE * NUM_CHUNKS;  // 15000 bytes

// OFDM parameters (IEEE 802.11a)
static const int FFT_SIZE        = 64;
static const int NUM_DATA_SC     = 48;
static const int NUM_PILOT_SC    = 4;

using Cplx = std::complex<double>;

// ============================================================
// Modulation enum
// ============================================================
enum ModType { BPSK, QPSK, QAM16, QAM64 };

static int bitsPerSymbol(ModType m) {
    switch(m) { case BPSK: return 1; case QPSK: return 2;
                case QAM16: return 4; case QAM64: return 6; }
    return 1;
}
static const char* modName(ModType m) {
    switch(m) { case BPSK: return "BPSK"; case QPSK: return "QPSK";
                case QAM16: return "16-QAM"; case QAM64: return "64-QAM"; }
    return "?";
}

// ============================================================
// CRC-32 (IEEE 802.3 / ITU-T V.42 — same polynomial as provided code)
// ============================================================
static uint32_t CRC_TABLE[256];

static void buildCrcTable() {
    uint32_t poly = 0xEDB88320;
    for (int i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++)
            crc = (crc & 1) ? ((crc >> 1) ^ poly) : (crc >> 1);
        CRC_TABLE[i] = crc;
    }
}

static uint32_t calcCRC32(const uint8_t* data, int len) {
    uint32_t crc = 0xFFFFFFFF;
    for (int i = 0; i < len; i++)
        crc = (crc >> 8) ^ CRC_TABLE[(crc & 0xFF) ^ data[i]];
    return crc ^ 0xFFFFFFFF;
}

// CRC encode: returns 1504 bytes (1500 data + 4 CRC)
static std::vector<uint8_t> crcEncode(const uint8_t* data, int len) {
    std::vector<uint8_t> out(len + 4);
    memcpy(out.data(), data, len);
    uint32_t crc = calcCRC32(data, len);
    memcpy(out.data() + len, &crc, 4);
    return out;
}

// CRC decode: returns {1500 bytes, error_flag}
static std::pair<std::vector<uint8_t>, bool> crcDecode(const uint8_t* data, int len) {
    int dataLen = len - 4;
    uint32_t calc = calcCRC32(data, dataLen);
    uint32_t recv;
    memcpy(&recv, data + dataLen, 4);
    bool err = (calc != recv);
    return {std::vector<uint8_t>(data, data + dataLen), err};
}

// ============================================================
// PPDU Encapsulate / Decapsulate (simplified from provided code)
// ============================================================
// We use a fixed NDBPS=48 (12 Mbps, QPSK R=2/3) for the PPDU framing
// since we do our own simple QAM modulation below.
// PPDU layout: SIGNAL(3) + SERVICE(2) + PSDU(1504) = 1509 bytes minimum
// We just use 1509 bytes (no tail/pad concern for this sim).
static const int PPDU_HEADER = 5;  // SIGNAL(3) + SERVICE(2)

static std::vector<uint8_t> ppduEncapsulate(const std::vector<uint8_t>& psdu) {
    // psdu = 1504 bytes
    uint16_t LENGTH = (uint16_t)psdu.size();
    // SIGNAL bytes (rate=5 = 12Mbps QPSK)
    uint8_t rateField = 5 & 0x0F;
    uint8_t parity = 0;
    for (int b = 0; b < 4;  b++) parity ^= (rateField >> b) & 1;
    for (int b = 0; b < 12; b++) parity ^= (LENGTH >> b) & 1;

    uint8_t sig1 = rateField
                 | (((LENGTH >> 0) & 1) << 5)
                 | (((LENGTH >> 1) & 1) << 6)
                 | (((LENGTH >> 2) & 1) << 7);
    uint8_t sig2 = (uint8_t)((LENGTH >> 3) & 0xFF);
    uint8_t sig3 = (uint8_t)(((LENGTH >> 11) & 1) | (parity << 1));

    std::vector<uint8_t> frame;
    frame.push_back(sig1);
    frame.push_back(sig2);
    frame.push_back(sig3);
    frame.push_back(0);  // SERVICE[0] — seed stored here by scrambler
    frame.push_back(0);  // SERVICE[1]
    frame.insert(frame.end(), psdu.begin(), psdu.end());
    return frame;  // 1509 bytes
}

static std::vector<uint8_t> ppduDecapsulate(const std::vector<uint8_t>& frame) {
    // Skip SIGNAL(3) + SERVICE(2) = 5 bytes -> PSDU
    if ((int)frame.size() <= PPDU_HEADER) return {};
    return std::vector<uint8_t>(frame.begin() + PPDU_HEADER, frame.end());
}

// ============================================================
// Scrambler / Descrambler (IEEE 802.11a, S(x) = x^7 + x^4 + 1)
// ============================================================
static uint8_t SCRAM_LUT[128][2000];  // lut[seed][byte_index]

static void buildScramblerLUT() {
    for (int seed = 1; seed <= 127; seed++) {
        uint8_t st[7];
        for (int b = 0; b < 7; b++) st[b] = (seed >> (6 - b)) & 1;
        for (int bi = 0; bi < 2000; bi++) {
            uint8_t sb = 0;
            for (int bit = 0; bit < 8; bit++) {
                sb |= (st[6] << bit);
                uint8_t nb = st[6] ^ st[3];
                for (int s = 6; s > 0; s--) st[s] = st[s - 1];
                st[0] = nb;
            }
            SCRAM_LUT[seed][bi] = sb;
        }
    }
}

// Scramble: bytes [0..2] = SIGNAL -> unchanged, bytes [3..] = scramble
// seed stored in SERVICE byte [3] bits [6:0]
static std::vector<uint8_t> scramble(const std::vector<uint8_t>& frame, int seed) {
    std::vector<uint8_t> out = frame;
    const uint8_t* seq = SCRAM_LUT[seed];
    for (int j = 3; j < (int)frame.size(); j++)
        out[j] = frame[j] ^ seq[j];
    // Store seed in SERVICE byte 0 bits [6:0]
    out[3] = (out[3] & 0x80) | (uint8_t)(seed & 0x7F);
    return out;
}

static std::vector<uint8_t> descramble(const std::vector<uint8_t>& frame) {
    uint8_t seed = frame[3] & 0x7F;
    if (seed == 0) seed = 1;
    std::vector<uint8_t> out = frame;
    const uint8_t* seq = SCRAM_LUT[seed];
    for (int j = 3; j < (int)frame.size(); j++)
        out[j] = frame[j] ^ seq[j];
    // restore SERVICE byte 0 seed field to 0 (as it was before scrambling)
    // The seed itself was scrambled, so we need to recover original SERVICE[0]
    // Original SERVICE[0] = 0, and we stored seed in it, so after XOR with seq[3]:
    // Actually the scrambler stores the seed AFTER scrambling, so descramble just XORs
    // and the seed is embedded. Let's just clear bits [6:0] of SERVICE[0].
    out[3] = out[3] & 0x80;
    return out;
}

// ============================================================
// QAM Constellation Maps (IEEE 802.11a Gray-coded)
// ============================================================
static Cplx BPSK_MAP[2];
static Cplx QPSK_MAP[4];
static Cplx QAM16_MAP[16];
static Cplx QAM64_MAP[64];

static void buildConstellations() {
    // BPSK
    BPSK_MAP[0] = {-1.0, 0.0};
    BPSK_MAP[1] = { 1.0, 0.0};

    // QPSK: k = 1/sqrt(2)
    double k2 = 1.0 / sqrt(2.0);
    QPSK_MAP[0] = {-k2, -k2};
    QPSK_MAP[1] = {-k2,  k2};
    QPSK_MAP[2] = { k2, -k2};
    QPSK_MAP[3] = { k2,  k2};

    // 16-QAM: k = 1/sqrt(10)
    {
        double k = 1.0 / sqrt(10.0);
        static const int lvl[4] = {-3, -1, 3, 1};
        for (int b = 0; b < 16; b++) {
            int b0b1 = (b >> 2) & 0x3;
            int b2b3 = b & 0x3;
            QAM16_MAP[b] = {k * lvl[b0b1], k * lvl[b2b3]};
        }
    }

    // 64-QAM: k = 1/sqrt(42)
    {
        double k = 1.0 / sqrt(42.0);
        static const int lvl[8] = {-7, -5, -1, -3, 7, 5, 1, 3};
        for (int b = 0; b < 64; b++) {
            int b012 = (b >> 3) & 0x7;
            int b345 = b & 0x7;
            QAM64_MAP[b] = {k * lvl[b012], k * lvl[b345]};
        }
    }
}

static Cplx mapBits(const uint8_t* bits, ModType m) {
    int bps = bitsPerSymbol(m);
    int idx = 0;
    for (int b = 0; b < bps; b++) idx = (idx << 1) | (bits[b] & 1);
    switch (m) {
        case BPSK:  return BPSK_MAP[idx & 1];
        case QPSK:  return QPSK_MAP[idx & 3];
        case QAM16: return QAM16_MAP[idx & 15];
        case QAM64: return QAM64_MAP[idx & 63];
    }
    return {0, 0};
}

// Demapping: find nearest constellation point
static int demapBits(Cplx sym, ModType m) {
    int sz = 1 << bitsPerSymbol(m);
    const Cplx* map = (m == BPSK) ? BPSK_MAP :
                      (m == QPSK) ? QPSK_MAP :
                      (m == QAM16) ? QAM16_MAP : QAM64_MAP;
    int best = 0;
    double bestD = 1e18;
    for (int i = 0; i < sz; i++) {
        double d = std::norm(sym - map[i]);
        if (d < bestD) { bestD = d; best = i; }
    }
    return best;
}

// ============================================================
// QAM Mapper: bytes -> OFDM subcarrier symbols (data SCs only)
// Returns vector of complex symbols (one per data subcarrier across all OFDM symbols)
// ============================================================
// Data subcarrier indices (same as IEEE 802.11a)
static const int DATA_SC[48] = {
    -26,-25,-24,-23,-22,-20,-19,-18,-17,-16,-15,-14,-13,-12,-11,-10,-9,-8,
    -6,-5,-4,-3,-2,-1,1,2,3,4,5,6,8,9,10,11,12,13,14,15,16,17,18,19,20,
    22,23,24,25,26
};
static const int PILOT_SC[4] = {-21, -7, 7, 21};
static const int8_t PILOT_SEQ[127] = {
     1, 1, 1, 1,-1,-1,-1, 1,-1,-1,-1,-1, 1, 1,-1, 1,
    -1,-1, 1, 1,-1, 1, 1,-1, 1, 1, 1, 1, 1, 1,-1, 1,
     1, 1,-1, 1, 1,-1, 1,-1,-1, 1, 1, 1,-1, 1,-1,-1,
    -1, 1,-1, 1,-1,-1, 1,-1,-1, 1, 1, 1, 1, 1,-1,-1,
     1, 1,-1,-1, 1,-1, 1,-1, 1, 1,-1,-1,-1, 1, 1,-1,
    -1,-1,-1, 1,-1,-1, 1,-1, 1, 1, 1, 1,-1, 1,-1, 1,
    -1, 1,-1,-1,-1,-1,-1, 1,-1, 1, 1,-1, 1,-1, 1, 1,
     1,-1,-1, 1,-1,-1,-1,-1,-1,-1,-1
};

struct OFDMFrame {
    // One OFDM symbol = 64 complex subcarriers
    std::vector<Cplx> subcarriers;  // [symbol][subcarrier] flattened, size = nSyms*64
    int nSyms;
};

// bytes -> OFDM frame (full 64-subcarrier layout per symbol)
static OFDMFrame qamMap(const std::vector<uint8_t>& bytes, ModType m) {
    int bps = bitsPerSymbol(m);
    // Convert bytes to bits (MSB first)
    std::vector<uint8_t> bits;
    for (uint8_t b : bytes) {
        for (int i = 7; i >= 0; i--)
            bits.push_back((b >> i) & 1);
    }
    // Number of OFDM symbols needed
    int numSyms = ((int)bits.size() + bps * NUM_DATA_SC - 1) / (bps * NUM_DATA_SC);
    // Pad bits
    bits.resize(numSyms * bps * NUM_DATA_SC, 0);

    OFDMFrame frame;
    frame.nSyms = numSyms;
    frame.subcarriers.resize(numSyms * FFT_SIZE, {0, 0});

    for (int s = 0; s < numSyms; s++) {
        Cplx* sym = frame.subcarriers.data() + s * FFT_SIZE;
        // Map data subcarriers
        for (int sc_i = 0; sc_i < NUM_DATA_SC; sc_i++) {
            int bitStart = (s * NUM_DATA_SC + sc_i) * bps;
            Cplx c = mapBits(bits.data() + bitStart, m);
            // Convert subcarrier index to array index (0..63)
            int sc = DATA_SC[sc_i];
            int arr_idx = (sc < 0) ? (sc + FFT_SIZE) : sc;
            sym[arr_idx] = c;
        }
        // Pilots
        int8_t pn = PILOT_SEQ[s % 127];
        double pilotScaling[] = {1, 1, 1, -1};
        for (int p = 0; p < NUM_PILOT_SC; p++) {
            int sc = PILOT_SC[p];
            int arr_idx = (sc < 0) ? (sc + FFT_SIZE) : sc;
            sym[arr_idx] = Cplx(pn * pilotScaling[p], 0.0);
        }
        // DC and edge guards are zero (already initialized)
    }
    return frame;
}

// QAM demap: extract data subcarriers from OFDM frame and recover bytes
static std::vector<uint8_t> qamDemap(const OFDMFrame& frame, ModType m, int originalBytes) {
    int bps = bitsPerSymbol(m);
    std::vector<uint8_t> bits;

    for (int s = 0; s < frame.nSyms; s++) {
        const Cplx* sym = frame.subcarriers.data() + s * FFT_SIZE;
        for (int sc_i = 0; sc_i < NUM_DATA_SC; sc_i++) {
            int sc = DATA_SC[sc_i];
            int arr_idx = (sc < 0) ? (sc + FFT_SIZE) : sc;
            int idx = demapBits(sym[arr_idx], m);
            // Extract bits MSB first
            for (int b = bps - 1; b >= 0; b--)
                bits.push_back((idx >> b) & 1);
        }
    }

    // Pack bits into bytes
    std::vector<uint8_t> bytes;
    for (int i = 0; i + 7 < (int)bits.size() && (int)bytes.size() < originalBytes; i += 8) {
        uint8_t byte = 0;
        for (int b = 0; b < 8; b++)
            byte = (byte << 1) | bits[i + b];
        bytes.push_back(byte);
    }
    bytes.resize(originalBytes, 0);
    return bytes;
}

// ============================================================
// FFT / IFFT (Cooley-Tukey, in-place, radix-2)
// ============================================================
static void fft(std::vector<Cplx>& a, bool inverse) {
    int n = a.size();
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (int len = 2; len <= n; len <<= 1) {
        double ang = 2 * M_PI / len * (inverse ? -1 : 1);
        Cplx wlen(cos(ang), sin(ang));
        for (int i = 0; i < n; i += len) {
            Cplx w(1, 0);
            for (int j = 0; j < len / 2; j++) {
                Cplx u = a[i + j], v = a[i + j + len/2] * w;
                a[i + j] = u + v;
                a[i + j + len/2] = u - v;
                w *= wlen;
            }
        }
    }
    if (inverse) for (auto& x : a) x /= n;
}

// IFFT on OFDM frame: freq domain -> time domain (per symbol)
static std::vector<Cplx> ifftFrame(const OFDMFrame& frame) {
    std::vector<Cplx> timeSignal;
    timeSignal.reserve(frame.nSyms * FFT_SIZE);
    for (int s = 0; s < frame.nSyms; s++) {
        std::vector<Cplx> sym(frame.subcarriers.begin() + s * FFT_SIZE,
                               frame.subcarriers.begin() + (s + 1) * FFT_SIZE);
        fft(sym, true);  // IFFT
        timeSignal.insert(timeSignal.end(), sym.begin(), sym.end());
    }
    return timeSignal;
}

// FFT on time signal -> recover OFDM frame
static OFDMFrame fftFrame(const std::vector<Cplx>& timeSignal, int nSyms) {
    OFDMFrame frame;
    frame.nSyms = nSyms;
    frame.subcarriers.resize(nSyms * FFT_SIZE);
    for (int s = 0; s < nSyms; s++) {
        std::vector<Cplx> sym(timeSignal.begin() + s * FFT_SIZE,
                               timeSignal.begin() + (s + 1) * FFT_SIZE);
        fft(sym, false);  // FFT
        for (int i = 0; i < FFT_SIZE; i++)
            frame.subcarriers[s * FFT_SIZE + i] = sym[i];
    }
    return frame;
}

// ============================================================
// ASCII Constellation Plot (terminal art)
// ============================================================
struct PlotConfig {
    int width = 60;
    int height = 30;
    double xmin = -1.6;
    double xmax =  1.6;
    double ymin = -1.6;
    double ymax =  1.6;
};

static void plotConstellation(const std::vector<Cplx>& points,
                               const char* title,
                               const PlotConfig& cfg = PlotConfig()) {
    int W = cfg.width, H = cfg.height;
    std::vector<char> grid(W * H, ' ');

    // Axes
    int cx = (int)((0 - cfg.xmin) / (cfg.xmax - cfg.xmin) * (W - 1));
    int cy = (int)((cfg.ymax - 0) / (cfg.ymax - cfg.ymin) * (H - 1));
    for (int i = 0; i < W; i++) grid[cy * W + i] = '-';
    for (int i = 0; i < H; i++) grid[i * W + cx] = '|';
    if (cy >= 0 && cy < H && cx >= 0 && cx < W) grid[cy * W + cx] = '+';

    // Plot points (deduplicate in grid)
    int plotted = 0;
    for (const auto& p : points) {
        int x = (int)((p.real() - cfg.xmin) / (cfg.xmax - cfg.xmin) * (W - 1));
        int y = (int)((cfg.ymax - p.imag()) / (cfg.ymax - cfg.ymin) * (H - 1));
        if (x >= 0 && x < W && y >= 0 && y < H) {
            char& c = grid[y * W + x];
            if (c == ' ' || c == '-' || c == '|' || c == '+') {
                c = '*';
                plotted++;
            }
        }
    }

    printf("\n┌─ %s (%d points plotted) ", title, plotted);
    for (int i = 0; i < W - 4 - (int)strlen(title) - 20; i++) printf("─");
    printf("┐\n");
    for (int row = 0; row < H; row++) {
        printf("│");
        for (int col = 0; col < W; col++) printf("%c", grid[row * W + col]);
        printf("│\n");
    }
    printf("└");
    for (int i = 0; i < W; i++) printf("─");
    printf("┘\n");
    printf("  x: [%.1f, %.1f]   y: [%.1f, %.1f]\n\n",
           cfg.xmin, cfg.xmax, cfg.ymin, cfg.ymax);
}

// ============================================================
// Extract data subcarrier symbols from OFDM frame for plotting
// ============================================================
static std::vector<Cplx> extractDataSymbols(const OFDMFrame& frame) {
    std::vector<Cplx> syms;
    for (int s = 0; s < frame.nSyms; s++) {
        const Cplx* sym = frame.subcarriers.data() + s * FFT_SIZE;
        for (int sc_i = 0; sc_i < NUM_DATA_SC; sc_i++) {
            int sc = DATA_SC[sc_i];
            int arr_idx = (sc < 0) ? (sc + FFT_SIZE) : sc;
            syms.push_back(sym[arr_idx]);
        }
    }
    return syms;
}

// ============================================================
// Main
// ============================================================
int main(int argc, char* argv[]) {
    // Parse modulation type
    ModType mod = QPSK;
    if (argc >= 2) {
        std::string s = argv[1];
        if      (s == "bpsk")  mod = BPSK;
        else if (s == "qpsk")  mod = QPSK;
        else if (s == "16qam") mod = QAM16;
        else if (s == "64qam") mod = QAM64;
        else {
            fprintf(stderr, "Unknown modulation '%s'. Use: bpsk | qpsk | 16qam | 64qam\n", argv[1]);
            return 1;
        }
    }

    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║      IEEE 802.11a Full Transceiver Pipeline Simulation     ║\n");
    printf("║      Modulation: %-8s                                  ║\n", modName(mod));
    printf("╚══════════════════════════════════════════════════════════╝\n\n");

    // ─── Init ────────────────────────────────────────────────
    buildCrcTable();
    buildScramblerLUT();
    buildConstellations();

    // ─── Step 1: Generate 15000-byte data string ─────────────
    printf("══════════════════════════════════════════════════════════\n");
    printf("  STEP 1: Generate Input Data (%d bytes)\n", TOTAL_DATA_SIZE);
    printf("══════════════════════════════════════════════════════════\n");

    std::string inputStr = "";
    // Build a readable repeating string
    const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 ";
    for (int i = 0; i < TOTAL_DATA_SIZE; i++)
        inputStr += alphabet[i % 63];

    printf("Input string (first 80 chars): \"%.80s...\"\n\n", inputStr.c_str());

    // ─── Step 2-5: TX pipeline per chunk ─────────────────────
    printf("══════════════════════════════════════════════════════════\n");
    printf("  STEP 2-5: CRC → PPDU → Scramble → QAM Map (per chunk)\n");
    printf("══════════════════════════════════════════════════════════\n");

    // We'll accumulate all data subcarrier symbols for plotting
    std::vector<Cplx> allTxSymbols;     // TX constellation points
    std::vector<OFDMFrame> allFrames;   // OFDM frames per chunk
    std::vector<int> frameSizes;        // for demapping: original frame byte sizes

    for (int chunk = 0; chunk < NUM_CHUNKS; chunk++) {
        const uint8_t* rawData = (const uint8_t*)inputStr.data() + chunk * CHUNK_SIZE;

        // CRC encode: 1500 -> 1504 bytes
        auto crcEncoded = crcEncode(rawData, CHUNK_SIZE);

        // PPDU encapsulate: 1504 -> 1509 bytes (SIGNAL+SERVICE+PSDU)
        auto ppduFrame = ppduEncapsulate(crcEncoded);

        // Scramble (SIGNAL[0..2] passthrough, rest scrambled)
        int seed = (chunk * 37 + 127) % 127 + 1;  // seed 1..127
        auto scrambled = scramble(ppduFrame, seed);

        // QAM map to OFDM frame
        OFDMFrame ofdmFrame = qamMap(scrambled, mod);
        frameSizes.push_back((int)scrambled.size());

        // Collect TX symbols for constellation plot
        auto txSyms = extractDataSymbols(ofdmFrame);
        allTxSymbols.insert(allTxSymbols.end(), txSyms.begin(), txSyms.end());

        allFrames.push_back(ofdmFrame);

        if (chunk == 0) {
            printf("  Chunk 0: raw=%d B → CRC=%d B → PPDU=%d B → scrambled=%d B"
                   " → %d OFDM syms × 64 subcarriers\n",
                   CHUNK_SIZE, (int)crcEncoded.size(), (int)ppduFrame.size(),
                   (int)scrambled.size(), ofdmFrame.nSyms);
        }
    }
    printf("  All %d chunks processed.\n\n", NUM_CHUNKS);

    // ─── Step 6: Plot TX Constellation ───────────────────────
    printf("══════════════════════════════════════════════════════════\n");
    printf("  STEP 6: TX Constellation Plot (%s)\n", modName(mod));
    printf("══════════════════════════════════════════════════════════\n");

    {
        PlotConfig cfg;
        if (mod == BPSK)  { cfg.xmin = -1.5; cfg.xmax = 1.5; cfg.ymin = -0.5; cfg.ymax = 0.5; }
        if (mod == QAM64) { cfg.xmin = -1.2; cfg.xmax = 1.2; cfg.ymin = -1.2; cfg.ymax = 1.2; }
        char ttl[64]; snprintf(ttl, 64, "TX %s Constellation", modName(mod));
        plotConstellation(allTxSymbols, ttl, cfg);
    }

    // ─── Step 7: IFFT (OFDM Modulation) ──────────────────────
    printf("══════════════════════════════════════════════════════════\n");
    printf("  STEP 7: IFFT — OFDM Time-Domain Signal\n");
    printf("══════════════════════════════════════════════════════════\n");

    std::vector<std::vector<Cplx>> allTimeDomain;
    std::vector<Cplx> timeForPlot;  // collect some samples for plotting

    for (int chunk = 0; chunk < NUM_CHUNKS; chunk++) {
        auto td = ifftFrame(allFrames[chunk]);
        allTimeDomain.push_back(td);
        // Collect first chunk's time-domain samples for plotting
        if (chunk == 0) timeForPlot = td;
    }

    printf("  IFFT complete. Time-domain samples per chunk: %d\n\n",
           (int)allTimeDomain[0].size());

    // Plot time domain I vs Q (not a constellation per se, but shows the spread)
    printf("══════════════════════════════════════════════════════════\n");
    printf("  STEP 8: Time-Domain I vs Q after IFFT\n");
    printf("══════════════════════════════════════════════════════════\n");
    {
        PlotConfig cfg;
        cfg.xmin = -1.2; cfg.xmax = 1.2; cfg.ymin = -1.2; cfg.ymax = 1.2;
        // IFFT output amplitude depends on FFT size; normalize
        double maxAmp = 0;
        for (auto& c : timeForPlot)
            maxAmp = std::max(maxAmp, std::abs(c));
        std::vector<Cplx> normalised;
        for (auto& c : timeForPlot)
            normalised.push_back(c / (maxAmp > 0 ? maxAmp : 1));
        plotConstellation(normalised, "Time-Domain I/Q after IFFT (chunk 0, normalised)", cfg);
    }

    // ─── Step 9: FFT (OFDM Demodulation) ─────────────────────
    printf("══════════════════════════════════════════════════════════\n");
    printf("  STEP 9: FFT — Recover Frequency-Domain Subcarriers\n");
    printf("══════════════════════════════════════════════════════════\n");

    std::vector<OFDMFrame> rxFrames;
    std::vector<Cplx> allRxSymbols;

    for (int chunk = 0; chunk < NUM_CHUNKS; chunk++) {
        OFDMFrame rxFrame = fftFrame(allTimeDomain[chunk], allFrames[chunk].nSyms);
        rxFrames.push_back(rxFrame);
        auto rxSyms = extractDataSymbols(rxFrame);
        allRxSymbols.insert(allRxSymbols.end(), rxSyms.begin(), rxSyms.end());
    }
    printf("  FFT complete. Subcarriers recovered.\n\n");

    // ─── Step 10: Plot RX Constellation ──────────────────────
    printf("══════════════════════════════════════════════════════════\n");
    printf("  STEP 10: RX Constellation Plot (after FFT)\n");
    printf("══════════════════════════════════════════════════════════\n");
    {
        PlotConfig cfg;
        if (mod == BPSK)  { cfg.xmin = -1.5; cfg.xmax = 1.5; cfg.ymin = -0.5; cfg.ymax = 0.5; }
        if (mod == QAM64) { cfg.xmin = -1.2; cfg.xmax = 1.2; cfg.ymin = -1.2; cfg.ymax = 1.2; }
        char ttl[64]; snprintf(ttl, 64, "RX %s Constellation (post-FFT)", modName(mod));
        plotConstellation(allRxSymbols, ttl, cfg);
    }

    // ─── Step 11: Reverse pipeline ───────────────────────────
    printf("══════════════════════════════════════════════════════════\n");
    printf("  STEP 11: Reverse Pipeline — QAM Demap → Descramble →\n");
    printf("           PPDU Decap → CRC Decode\n");
    printf("══════════════════════════════════════════════════════════\n");

    std::string recoveredStr = "";
    int crcErrors = 0;

    for (int chunk = 0; chunk < NUM_CHUNKS; chunk++) {
        // QAM demap -> recover scrambled PPDU bytes
        auto recovered = qamDemap(rxFrames[chunk], mod, frameSizes[chunk]);

        // Descramble
        auto descrambled = descramble(recovered);

        // PPDU decapsulate -> get PSDU (CRC-encoded, 1504 bytes)
        auto psdu = ppduDecapsulate(descrambled);

        // CRC decode -> get 1500 bytes + error flag
        if ((int)psdu.size() >= 1504) {
            auto [data, err] = crcDecode(psdu.data(), 1504);
            if (err) crcErrors++;
            recoveredStr += std::string((char*)data.data(), data.size());
        } else {
            // Fallback: use whatever we have
            recoveredStr += std::string((char*)psdu.data(),
                                        std::min((int)psdu.size(), CHUNK_SIZE));
            crcErrors++;
        }
    }

    printf("\n  CRC errors: %d / %d chunks\n\n", crcErrors, NUM_CHUNKS);

    // ─── Step 12: Print result ────────────────────────────────
    printf("══════════════════════════════════════════════════════════\n");
    printf("  STEP 12: Recovered String\n");
    printf("══════════════════════════════════════════════════════════\n");

    printf("  First 80 chars of recovered string:\n  \"%.80s...\"\n\n",
           recoveredStr.c_str());

    // Verify
    bool match = (recoveredStr.size() == inputStr.size() &&
                  memcmp(recoveredStr.data(), inputStr.data(), inputStr.size()) == 0);

    printf("══════════════════════════════════════════════════════════\n");
    printf("  RESULT: %s\n", match ? "✓ PERFECT RECOVERY — strings match!" :
                                      "✗ MISMATCH — check pipeline");
    printf("  Modulation : %s (%d bits/symbol)\n", modName(mod), bitsPerSymbol(mod));
    printf("  Chunks     : %d × %d bytes = %d bytes\n", NUM_CHUNKS, CHUNK_SIZE, TOTAL_DATA_SIZE);
    printf("  CRC errors : %d\n", crcErrors);
    printf("══════════════════════════════════════════════════════════\n");

    return match ? 0 : 1;
}