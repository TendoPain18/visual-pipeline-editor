#include "core/run_generic_block.h"
#include <cstring>
#include <cmath>
#include <libgen.h>

static char g_rateConfigPath[4096];

struct RateParams {
    int      R1_R4[4];
    uint8_t  value;
    int      NDBPS;
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
        default: return {{0,0,0,0},  0,   0};
    }
}

static bool isValidRate(int rate) {
    int valid[] = {6, 9, 12, 18, 24, 36, 48, 54};
    for (int r : valid) if (rate == r) return true;
    return false;
}

// -----------------------------------------------------------------------
// FIX: computeFrameBits now includes the 24-bit SIGNAL field.
//
// 802.11a PPDU DATA field layout fed into the convolutional encoder:
//   SIGNAL  : 24 bits  (rate + length + parity, always R=1/2)
//   SERVICE : 16 bits
//   PSDU    : 8 * PSDU_bytes bits
//   TAIL    :  6 bits
//   PAD     :  pad_bits  (to fill last OFDM symbol)
//
// n_sym is computed over (SERVICE + PSDU + TAIL), because SIGNAL occupies
// its own dedicated OFDM symbol and is NOT counted in n_sym.
// However lipBits must include SIGNAL so that downstream blocks can slice
// it off correctly as the first 24 bits of the buffer.
//
// Previous (WRONG):  return data_bits + pad_bits;          // missed SIGNAL
// Fixed:             return SIGNAL_BITS + data_bits + pad_bits;
// -----------------------------------------------------------------------
static const uint32_t SIGNAL_BITS = 24;  // always 24 bits for 802.11a SIGNAL field

static uint32_t computeFrameBits(int NDBPS) {
    // data_bits = SERVICE(16) + PSDU(1504*8) + TAIL(6)
    const uint32_t data_bits  = 16 + 1504 * 8 + 6;   // = 12054
    const uint32_t n_sym      = (data_bits + NDBPS - 1) / NDBPS;
    const uint32_t total_bits = n_sym * NDBPS;
    const uint32_t pad_bits   = total_bits - data_bits;

    // lipBits = SIGNAL + padded data field (exact bit count)
    return SIGNAL_BITS + data_bits + pad_bits;
}

static int computeFrameBytes(int NDBPS) {
    uint32_t bits = computeFrameBits(NDBPS);
    return (bits + 7) / 8;
}

static int readRateFromFile(const char* filepath) {
    FILE* f = fopen(filepath, "r");
    if (!f) {
        fprintf(stderr, "[PpduEncapsulate] ERROR: Cannot open rate config file '%s'\n", filepath);
        exit(1);
    }
    int rate = 0, parsed = fscanf(f, "%d", &rate);
    fclose(f);
    if (parsed != 1) {
        fprintf(stderr, "[PpduEncapsulate] ERROR: Could not parse integer\n");
        exit(1);
    }
    if (!isValidRate(rate)) {
        fprintf(stderr, "[PpduEncapsulate] ERROR: Invalid rate %d Mbps\n", rate);
        exit(1);
    }
    return rate;
}

static const char* rateNameFromVal(int dataRate) {
    switch (dataRate) {
        case  6: return "6 Mbps";
        case  9: return "9 Mbps";
        case 12: return "12 Mbps";
        case 18: return "18 Mbps";
        case 24: return "24 Mbps";
        case 36: return "36 Mbps";
        case 48: return "48 Mbps";
        case 54: return "54 Mbps";
        default: return "UNKNOWN";
    }
}

struct PpduEncapsulateData {
    RateParams currentRate;
    int frameCount;
    int dataRate_mbps;
};

PpduEncapsulateData init_ppdu_encapsulate(const BlockConfig& config) {
    PpduEncapsulateData data;
    data.dataRate_mbps = readRateFromFile(g_rateConfigPath);
    data.currentRate   = getRateParams(data.dataRate_mbps);
    data.frameCount    = 0;

    uint32_t frameBits  = computeFrameBits(data.currentRate.NDBPS);
    int      frameBytes = computeFrameBytes(data.currentRate.NDBPS);


    return data;
}

void process_ppdu_encapsulate(
    const char** pipeIn,
    const char** pipeOut,
    PpduEncapsulateData& customData,
    const BlockConfig& config
) {
    PipeIO input  (pipeIn[0],  config.inputPacketSizes[0],  config.inputBatchSizes[0]);
    PipeIO outData(pipeOut[0], config.outputPacketSizes[0], config.outputBatchSizes[0]);
    PipeIO outRate(pipeOut[1], config.outputPacketSizes[1], config.outputBatchSizes[1]);

    int8_t* inputBatch = new int8_t[input.getBufferSize()];
    int8_t* dataBatch  = new int8_t[outData.getBufferSize()];
    int8_t* rateBatch  = new int8_t[outRate.getBufferSize()];

    int actualCount = input.read(inputBatch);
    memset(dataBatch, 0, outData.getBufferSize());
    memset(rateBatch, 0, outRate.getBufferSize());

    const int inPktSize  = config.inputPacketSizes[0];   // 1504
    const int outDataSize = config.outputPacketSizes[0]; // 1515 (buffer)
    const int outRateSize = config.outputPacketSizes[1]; // 5

    // lipBits now correctly includes the 24-bit SIGNAL field
    uint32_t lipBits = computeFrameBits(customData.currentRate.NDBPS);
    const RateParams& rp = customData.currentRate;

    const bool isFirstBatch = (customData.frameCount == 0);



    for (int i = 0; i < actualCount; i++) {
        const bool dbg = isFirstBatch && (i == 0);

        const int inOff   = i * inPktSize;
        const int dataOff = i * outDataSize;
        const int rateOff = i * outRateSize;

        // Convert signed input to unsigned PSDU bytes
        uint8_t psdu[1504];
        for (int j = 0; j < inPktSize; j++)
            psdu[j] = (uint8_t)((int32_t)inputBatch[inOff + j] + 128);

        // -------------------------------------------------------------------
        // Build SIGNAL field (24 bits = 3 bytes)
        //   Bits  3:0  = RATE    (R1..R4)
        //   Bit   4    = Reserved (0)
        //   Bits 16:5  = LENGTH  (bytes)
        //   Bit  17    = Parity  (even parity over bits 0-16)
        //   Bits 23:18 = TAIL    (0)
        // Packed into 3 bytes as per 802.11a spec:
        //   sig1[7:0] = RATE[3:0] | LENGTH[2:0] << 5
        //   sig2[7:0] = LENGTH[10:3]
        //   sig3[7:0] = LENGTH[11] | PARITY << 1
        // -------------------------------------------------------------------
        uint16_t LENGTH     = (uint16_t)inPktSize;   // 1504
        uint8_t  rate_field = (uint8_t)(rp.R1_R4[0]
                            | (rp.R1_R4[1] << 1)
                            | (rp.R1_R4[2] << 2)
                            | (rp.R1_R4[3] << 3));
        uint8_t parity = 0;
        for (int b = 0; b < 4;  b++) parity ^= (rp.R1_R4[b] & 1);
        for (int b = 0; b < 12; b++) parity ^= (LENGTH >> b) & 1;

        uint8_t sig1 = rate_field
                     | (uint8_t)(((LENGTH >>  0) & 1) << 5)
                     | (uint8_t)(((LENGTH >>  1) & 1) << 6)
                     | (uint8_t)(((LENGTH >>  2) & 1) << 7);
        uint8_t sig2 = (uint8_t)((LENGTH >> 3) & 0xFF);
        uint8_t sig3 = (uint8_t)(((LENGTH >> 11) & 1) | (parity << 1));

        // -------------------------------------------------------------------
        // Write packet: SIGNAL(3) + SERVICE(2) + PSDU(1504)
        // -------------------------------------------------------------------
        uint8_t* dst = (uint8_t*)(dataBatch + dataOff);
        int idx = 0;
        dst[idx++] = sig1;   // SIGNAL byte 0
        dst[idx++] = sig2;   // SIGNAL byte 1
        dst[idx++] = sig3;   // SIGNAL byte 2
        dst[idx++] = 0x00;   // SERVICE byte 0  (zeros, scrambler seeds later)
        dst[idx++] = 0x00;   // SERVICE byte 1
        memcpy(dst + idx, psdu, inPktSize);

        // Convert to int8 representation used on pipes
        for (int j = 0; j < outDataSize; j++)
            dataBatch[dataOff + j] = (int8_t)((int32_t)dst[j] - 128);

        // -------------------------------------------------------------------
        // RATE pipe: rate_val(1B) + lipBits uint32 LE (4B)
        // lipBits is the EXACT total bit count the downstream encoder needs
        // -------------------------------------------------------------------
        rateBatch[rateOff + 0] = (int8_t)((int32_t)rp.value           - 128);
        rateBatch[rateOff + 1] = (int8_t)((int32_t)((lipBits      ) & 0xFF) - 128);
        rateBatch[rateOff + 2] = (int8_t)((int32_t)((lipBits >>  8) & 0xFF) - 128);
        rateBatch[rateOff + 3] = (int8_t)((int32_t)((lipBits >> 16) & 0xFF) - 128);
        rateBatch[rateOff + 4] = (int8_t)((int32_t)((lipBits >> 24) & 0xFF) - 128);

        if (dbg) {
            uint32_t signalBitsIn  = 12032; // PSDU 1504*8 input bits
            uint32_t signalBitsOut = 24;
            uint32_t dataBitsOut   = lipBits - 24;
            printf("[PpduEncapsulate] pkt[0] INPUT : %u bits (PSDU)\n", signalBitsIn);
            printf("[PpduEncapsulate] pkt[0] OUTPUT: signal=%u  data=%u  total=%u bits\n",
                   signalBitsOut, dataBitsOut, lipBits);
            fflush(stdout);
        }
    }



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

    char exePath[4096];
    strncpy(exePath, argv[0], sizeof(exePath) - 1);
    exePath[sizeof(exePath) - 1] = '\0';
    char* exeDir = dirname(exePath);
    snprintf(g_rateConfigPath, sizeof(g_rateConfigPath), "%s/rate_config.txt", exeDir);

    const char* pipeIn     = argv[1];
    const char* pipeOuts[] = {argv[2], argv[3]};

    BlockConfig config = {
        "PpduEncapsulate",
        1,                   // inputs
        2,                   // outputs
        {1504},              // inputPacketSizes  [PSDU]
        {64},                // inputBatchSizes
        {1515, 5},           // outputPacketSizes [DATA_buf, RATE/lipBits_LE]
        {64, 64},            // outputBatchSizes
        true,                // ltr
        true,                // startWithAll
        "IEEE 802.11a PPDU encapsulation - lipBits = SIGNAL(24) + DATA + PAD (exact bits)"
    };

    run_manual_block(&pipeIn, pipeOuts, config, process_ppdu_encapsulate, init_ppdu_encapsulate);
    return 0;
}