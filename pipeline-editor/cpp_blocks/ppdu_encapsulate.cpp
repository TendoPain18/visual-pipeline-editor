#include "core/run_generic_block.h"
#include <cstring>
#include <cmath>
#include <libgen.h>  // for dirname()

// ============================================================
// IEEE 802.11a PPDU Encapsulator
//
// Input  0: PSDU from CRC-encode            (1504 bytes/pkt, fixed)
//
// Output 0: Full PPDU frame (DATA pipe)     (1515 bytes/pkt, max)
//             Layout: SIGNAL(3) + SERVICE(2) + PSDU(1504) + TAIL_PAD
//             Only the first length_in_pipe bytes are meaningful.
//
// Output 1: RATE / LIP companion pipe       (3 bytes/pkt)
//             Byte 0 : rate_value  (uint8)
//             Byte 1 : lip_low    (uint8)   -- length_in_pipe low byte
//             Byte 2 : lip_high   (uint8)   -- length_in_pipe high byte
//             length_in_pipe = actual frame bytes written to DATA pipe
//
// Pipe sizes use the MAXIMUM possible value across all 802.11a rates.
// DATA pipe is always 1515 bytes/pkt (worst case: 12-54 Mbps with 42-bit pad).
// Downstream blocks use length_in_pipe to ignore trailing padding bytes.
//
// Rate mapping (IEEE 802.11a):
//   6 Mbps  -> rate_val=13, R=1/2, NDBPS=24  -> frame = 1512 bytes
//   9 Mbps  -> rate_val=15, R=1/2, NDBPS=36  -> frame = 1511 bytes
//  12 Mbps  -> rate_val= 5, R=2/3, NDBPS=48  -> frame = 1515 bytes  <-- MAX
//  18 Mbps  -> rate_val= 7, R=2/3, NDBPS=72  -> frame = 1515 bytes
//  24 Mbps  -> rate_val= 9, R=3/4, NDBPS=96  -> frame = 1515 bytes
//  36 Mbps  -> rate_val=11, R=3/4, NDBPS=144 -> frame = 1515 bytes
//  48 Mbps  -> rate_val= 1, R=2/3, NDBPS=192 -> frame = 1515 bytes
//  54 Mbps  -> rate_val= 3, R=3/4, NDBPS=216 -> frame = 1515 bytes
// ============================================================

// Global path to rate config file — built from argv[0] in main()
static char g_rateConfigPath[4096];

struct RateParams {
    int      R1_R4[4];   // rate bits for SIGNAL field
    uint8_t  value;      // rate_value byte
    int      NDBPS;      // data bits per OFDM symbol
};

static RateParams getRateParams(int dataRate) {
    switch (dataRate) {
        case  6: return {{1,0,1,1}, 13,  24};
        case  9: return {{1,1,1,1}, 15,  36};
        case 12: return {{1,0,1,0},  5,  48};
        case 18: return {{1,1,1,0},  7,  72};
        case 24: return {{1,0,0,1},  9,  96};
        case 36: return {{1,1,0,1}, 11, 144};
        case 48: return {{1,0,0,0},  1, 192};
        case 54: return {{1,1,0,0},  3, 216};
        default: return {{0,0,0,0},  0,   0}; // sentinel: invalid
    }
}

static bool isValidRate(int rate) {
    int valid[] = {6, 9, 12, 18, 24, 36, 48, 54};
    for (int r : valid) if (rate == r) return true;
    return false;
}

// Compute the actual frame byte count for a given rate (NDBPS).
// Frame layout: SIGNAL(3) + SERVICE(2) + PSDU(1504) + TAIL_PAD
// data_bits = SERVICE_bits(16) + PSDU_bits(12032) + TAIL_bits(6) = 12054
// n_sym = ceil(12054 / NDBPS)
// pad_bits = n_sym*NDBPS - 12054
// tail_pad_bytes = ceil((6 + pad_bits) / 8)
// frame_bytes = 3 + 2 + 1504 + tail_pad_bytes
static int computeFrameBytes(int NDBPS) {
    const int data_bits      = 16 + 1504 * 8 + 6; // 12054
    const int n_sym          = (data_bits + NDBPS - 1) / NDBPS;
    const int total_bits     = n_sym * NDBPS;
    const int pad_bits       = total_bits - data_bits;
    const int tail_pad_bits  = 6 + pad_bits;
    const int tail_pad_bytes = (tail_pad_bits + 7) / 8;
    return 3 + 2 + 1504 + tail_pad_bytes;
}

// Read data rate (Mbps) from a config file (full path).
// File must contain a single integer that is a valid 802.11a rate.
// Exits the process with a descriptive error message on any failure.
static int readRateFromFile(const char* filepath) {
    FILE* f = fopen(filepath, "r");
    if (!f) {
        fprintf(stderr, "[PpduEncapsulate] ERROR: Cannot open rate config file '%s'\n", filepath);
        fprintf(stderr, "[PpduEncapsulate]        Make sure rate_config.txt is in the same directory as the executable.\n");
        fprintf(stderr, "[PpduEncapsulate]        The file must contain one of: 6 9 12 18 24 36 48 54\n");
        exit(1);
    }

    int rate   = 0;
    int parsed = fscanf(f, "%d", &rate);
    fclose(f);

    if (parsed != 1) {
        fprintf(stderr, "[PpduEncapsulate] ERROR: Could not parse an integer from '%s'\n", filepath);
        fprintf(stderr, "[PpduEncapsulate]        The file must contain one of: 6 9 12 18 24 36 48 54\n");
        exit(1);
    }

    if (!isValidRate(rate)) {
        fprintf(stderr, "[PpduEncapsulate] ERROR: Invalid rate %d Mbps read from '%s'\n", rate, filepath);
        fprintf(stderr, "[PpduEncapsulate]        Valid 802.11a rates are: 6 9 12 18 24 36 48 54\n");
        exit(1);
    }

    return rate;
}

struct PpduEncapsulateData {
    RateParams currentRate;
    int        frameCount;
    int        dataRate_mbps;
};

PpduEncapsulateData init_ppdu_encapsulate(const BlockConfig& config) {
    PpduEncapsulateData data;

    data.dataRate_mbps = readRateFromFile(g_rateConfigPath);
    data.currentRate   = getRateParams(data.dataRate_mbps);
    data.frameCount    = 0;

    int frameBytes = computeFrameBytes(data.currentRate.NDBPS);
    printf("[PpduEncapsulate] Rate config file  : %s\n", g_rateConfigPath);
    printf("[PpduEncapsulate] Rate               : %d Mbps (rate_val=%d)\n",
           data.dataRate_mbps, data.currentRate.value);
    printf("[PpduEncapsulate] NDBPS              : %d\n", data.currentRate.NDBPS);
    printf("[PpduEncapsulate] Frame bytes        : %d  (length_in_pipe per packet)\n", frameBytes);
    printf("[PpduEncapsulate] DATA pipe size     : 1515 bytes/pkt  (max across all rates)\n");
    printf("[PpduEncapsulate] RATE pipe          : rate_val(1B) + lip_lo(1B) + lip_hi(1B)\n");
    printf("[PpduEncapsulate] Ready\n");

    return data;
}

void process_ppdu_encapsulate(
    const char** pipeIn,
    const char** pipeOut,
    PpduEncapsulateData& customData,
    const BlockConfig& config
) {
    // in[0]  = PSDU (1504 bytes/pkt, fixed)
    // out[0] = DATA pipe (1515 bytes/pkt, max)  -- FIRST: controls rate measurement
    // out[1] = RATE/LIP pipe (3 bytes/pkt)
    PipeIO input  (pipeIn[0],  config.inputPacketSizes[0],  config.inputBatchSizes[0]);
    PipeIO outData(pipeOut[0], config.outputPacketSizes[0], config.outputBatchSizes[0]);
    PipeIO outRate(pipeOut[1], config.outputPacketSizes[1], config.outputBatchSizes[1]);

    int8_t* inputBatch = new int8_t[input.getBufferSize()];
    int8_t* dataBatch  = new int8_t[outData.getBufferSize()];
    int8_t* rateBatch  = new int8_t[outRate.getBufferSize()];

    // STEP 1: Read PSDU batch
    int actualCount = input.read(inputBatch);

    memset(dataBatch, 0, outData.getBufferSize());
    memset(rateBatch, 0, outRate.getBufferSize());

    const int inPktSize   = config.inputPacketSizes[0];   // 1504
    const int outDataSize = config.outputPacketSizes[0];  // 1515
    const int outRateSize = config.outputPacketSizes[1];  // 3

    // Compute length_in_pipe for this batch's rate (same for all packets in batch)
    int frameBytes = computeFrameBytes(customData.currentRate.NDBPS);
    // frameBytes <= 1515 always; rest of the 1515-byte packet is zero-padded

    // Precompute SIGNAL bytes from the rate parameters (same for all packets)
    const RateParams& rp = customData.currentRate;

    // STEP 2: Build each packet
    for (int i = 0; i < actualCount; i++) {
        const int inOff   = i * inPktSize;
        const int dataOff = i * outDataSize;
        const int rateOff = i * outRateSize;

        // Convert PSDU to uint8
        uint8_t psdu[1504];
        for (int j = 0; j < inPktSize; j++) {
            psdu[j] = (uint8_t)((int32_t)inputBatch[inOff + j] + 128);
        }

        uint16_t LENGTH = (uint16_t)inPktSize; // 1504

        // Build SIGNAL field (3 bytes)
        uint8_t rate_field = (uint8_t)(rp.R1_R4[0] | (rp.R1_R4[1] << 1) |
                                       (rp.R1_R4[2] << 2) | (rp.R1_R4[3] << 3));
        // Parity over RATE bits + LENGTH bits
        uint8_t parity = 0;
        for (int b = 0; b < 4;  b++) parity ^= (rp.R1_R4[b] & 1);
        for (int b = 0; b < 12; b++) parity ^= (LENGTH >> b) & 1;

        // Signal byte layout:
        //   byte1[3:0] = RATE
        //   byte1[4]   = reserved (0)
        //   byte1[7:5] = LENGTH[2:0]
        //   byte2[7:0] = LENGTH[10:3]
        //   byte3[0]   = LENGTH[11]
        //   byte3[1]   = PARITY
        //   byte3[7:2] = TAIL (0)
        uint8_t sig1 = rate_field |
                       (((LENGTH >> 0) & 1) << 5) |
                       (((LENGTH >> 1) & 1) << 6) |
                       (((LENGTH >> 2) & 1) << 7);
        uint8_t sig2 = (uint8_t)((LENGTH >> 3) & 0xFF);
        uint8_t sig3 = (uint8_t)(((LENGTH >> 11) & 1) | (parity << 1));

        // SERVICE bytes (all zero = scrambler seed placeholder, SERVICE[0])
        uint8_t svc1 = 0;
        uint8_t svc2 = 0;

        // Assemble frame into output buffer (zero-initialised to handle tail/pad)
        uint8_t* dst = (uint8_t*)(dataBatch + dataOff);
        // (buffer already zeroed by memset above)
        int idx = 0;
        dst[idx++] = sig1;
        dst[idx++] = sig2;
        dst[idx++] = sig3;
        dst[idx++] = svc1;
        dst[idx++] = svc2;
        memcpy(dst + idx, psdu, inPktSize);
        idx += inPktSize;
        // Tail + PAD bytes are already 0 from the memset
        // (idx now points to where tail/pad bytes start; frame is complete)

        // Convert uint8 → int8 for the data output
        for (int j = 0; j < outDataSize; j++) {
            dataBatch[dataOff + j] = (int8_t)((int32_t)dst[j] - 128);
        }

        // Build RATE/LIP packet
        rateBatch[rateOff + 0] = (int8_t)((int32_t)rp.value                    - 128);
        rateBatch[rateOff + 1] = (int8_t)((int32_t)(frameBytes & 0xFF)         - 128);
        rateBatch[rateOff + 2] = (int8_t)((int32_t)((frameBytes >> 8) & 0xFF)  - 128);
    }

    // STEP 3: Send DATA first (controls rate measurement), then RATE/LIP
    outData.write(dataBatch, actualCount);
    outRate.write(rateBatch, actualCount);

    customData.frameCount += actualCount;

    delete[] inputBatch;
    delete[] dataBatch;
    delete[] rateBatch;
}

int main(int argc, char* argv[]) {
    if (argc < 4) {
        fprintf(stderr, "Usage: ppdu_encapsulate <pipeIn> <pipeOutData> <pipeOutRate>\n");
        return 1;
    }

    // Build path to rate_config.txt from the executable's own directory (argv[0])
    char exePath[4096];
    strncpy(exePath, argv[0], sizeof(exePath) - 1);
    exePath[sizeof(exePath) - 1] = '\0';
    char* exeDir = dirname(exePath);  // extracts directory portion of argv[0]
    snprintf(g_rateConfigPath, sizeof(g_rateConfigPath), "%s/rate_config.txt", exeDir);

    const char* pipeIn     = argv[1];
    const char* pipeOuts[] = {argv[2], argv[3]};

    // DATA pipe is always max size (1515) regardless of rate.
    // length_in_pipe in the RATE pipe tells downstream how many bytes are real.
    BlockConfig config = {
        "PpduEncapsulate",      // name
        1,                      // inputs
        2,                      // outputs
        {1504},                 // inputPacketSizes
        {6000},                 // inputBatchSizes
        {1515, 3},              // outputPacketSizes [DATA_max, RATE+LIP]
        {6000, 6000},           // outputBatchSizes
        true,                   // ltr
        true,                   // startWithAll
        "IEEE 802.11a PPDU encapsulation - dynamic lip per rate, max-size DATA pipe"
    };

    run_manual_block(&pipeIn, pipeOuts, config, process_ppdu_encapsulate, init_ppdu_encapsulate);
    return 0;
}