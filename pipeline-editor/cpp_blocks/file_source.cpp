#include "core/run_generic_block.h"
#include <vector>
#include <string>
#include <fstream>
#include <filesystem>
#include <cstring>
#include <algorithm>

namespace fs = std::filesystem;

const uint8_t START_FLAG[] = {0xAA, 0x55, 0xAA, 0x55};
const uint8_t END_FLAG[]   = {0x55, 0xAA, 0x55, 0xAA};
const int FILENAME_LENGTH  = 256;
const int REPETITIONS      = 10;

// -----------------------------------------------------------------------
// Tuning constants
// -----------------------------------------------------------------------
static constexpr int PACKET_SIZE      = 1500;
static constexpr int BATCH_SIZE       = 64;
static constexpr int BATCH_BYTES      = PACKET_SIZE * BATCH_SIZE;    // 96 000 B
static constexpr int RING_BATCHES     = 16;
static constexpr int RING_CAP         = RING_BATCHES * BATCH_BYTES;  // 1 536 000 B
static constexpr int DISK_CHUNK       = 3 * BATCH_BYTES;             // 288 000 B
static constexpr int REFILL_WHEN_FREE = DISK_CHUNK;

// -----------------------------------------------------------------------
// Ring buffer — heap-backed to avoid stack overflow
// -----------------------------------------------------------------------
struct RingBuf {
    std::vector<int8_t> data;
    int head = 0;
    int tail = 0;
    int used = 0;

    void init(int capacity) { data.assign(capacity, 0); }

    int  cap()   const { return (int)data.size(); }
    int  avail() const { return used; }
    int  free()  const { return cap() - used; }
    bool empty() const { return used == 0; }

    int write(const int8_t *src, int len) {
        len = std::min(len, free());
        if (len <= 0) return 0;
        int first = std::min(len, cap() - tail);
        memcpy(data.data() + tail, src, first);
        if (len > first) memcpy(data.data(), src + first, len - first);
        tail  = (tail + len) % cap();
        used += len;
        return len;
    }

    int peek(int8_t *dst, int len) const {
        len = std::min(len, used);
        if (len <= 0) return 0;
        int first = std::min(len, cap() - head);
        memcpy(dst, data.data() + head, first);
        if (len > first) memcpy(dst + first, data.data() + head + first - cap(), len - first);
        return len;
    }

    void consume(int len) {
        head  = (head + len) % cap();
        used -= len;
    }
};

// -----------------------------------------------------------------------
// State machine
// -----------------------------------------------------------------------
enum class FileState {
    IDLE,
    SEND_HEADER,
    SEND_DATA,
    SEND_FOOTER,
    DONE
};

struct FileSourceData {
    std::vector<std::string> filePaths;
    int                      currentFileIdx = 0;
    std::string              sourceDirectory;

    FileState     fileState            = FileState::IDLE;
    std::ifstream currentFileStream;
    uint64_t      currentFileSize      = 0;
    uint64_t      currentFileBytesRead = 0;
    std::string   currentFileName;

    RingBuf             ring;
    std::vector<int8_t> diskBuf;
};

// -----------------------------------------------------------------------
// Header builder
// -----------------------------------------------------------------------
static std::vector<int8_t> build_header(const std::string &fileName, uint64_t fileSize) {
    std::vector<int8_t> hdr;
    hdr.reserve(4 + FILENAME_LENGTH * REPETITIONS + 8 * REPETITIONS);

    for (int i = 0; i < 4; i++)
        hdr.push_back((int8_t)((int32_t)START_FLAG[i] - 128));

    for (int r = 0; r < REPETITIONS; r++) {
        uint8_t nameBytes[FILENAME_LENGTH] = {};
        int nameLen = std::min((int)fileName.size(), FILENAME_LENGTH);
        memcpy(nameBytes, fileName.c_str(), nameLen);
        for (int j = 0; j < FILENAME_LENGTH; j++)
            hdr.push_back((int8_t)((int32_t)nameBytes[j] - 128));
    }

    for (int r = 0; r < REPETITIONS; r++) {
        const uint8_t *sb = (const uint8_t *)&fileSize;
        for (int j = 0; j < 8; j++)
            hdr.push_back((int8_t)((int32_t)sb[j] - 128));
    }

    return hdr;
}

// -----------------------------------------------------------------------
// fill_ring
// -----------------------------------------------------------------------
static bool fill_ring(FileSourceData &d) {
    while (d.ring.free() >= REFILL_WHEN_FREE && d.fileState != FileState::DONE) {

        switch (d.fileState) {

        case FileState::IDLE: {
            if (d.currentFileIdx >= (int)d.filePaths.size()) {
                d.fileState = FileState::DONE;
                break;
            }
            const std::string &path = d.filePaths[d.currentFileIdx];
            d.currentFileName       = fs::path(path).filename().string();

            d.currentFileStream.open(path, std::ios::binary);
            if (!d.currentFileStream) {
                d.currentFileIdx++;
                break;
            }
            d.currentFileSize      = fs::file_size(path);
            d.currentFileBytesRead = 0;
            d.fileState            = FileState::SEND_HEADER;
            break;
        }

        case FileState::SEND_HEADER: {
            auto hdr = build_header(d.currentFileName, d.currentFileSize);
            d.ring.write(hdr.data(), (int)hdr.size());
            d.fileState = FileState::SEND_DATA;
            break;
        }

        case FileState::SEND_DATA: {
            uint64_t remaining = d.currentFileSize - d.currentFileBytesRead;
            if (remaining == 0) {
                d.currentFileStream.close();
                d.fileState = FileState::SEND_FOOTER;
                break;
            }

            int toRead = (int)std::min((uint64_t)DISK_CHUNK, remaining);
            d.currentFileStream.read((char *)d.diskBuf.data(), toRead);
            int got = (int)d.currentFileStream.gcount();

            for (int i = 0; i < got; i++)
                d.diskBuf[i] = (int8_t)((int32_t)(uint8_t)d.diskBuf[i] - 128);

            d.ring.write(d.diskBuf.data(), got);
            d.currentFileBytesRead += got;

            if (d.currentFileBytesRead >= d.currentFileSize) {
                d.currentFileStream.close();
                d.fileState = FileState::SEND_FOOTER;
            }
            break;
        }

        case FileState::SEND_FOOTER: {
            int8_t ef[4];
            for (int i = 0; i < 4; i++)
                ef[i] = (int8_t)((int32_t)END_FLAG[i] - 128);
            d.ring.write(ef, 4);
            d.currentFileIdx++;
            d.fileState = FileState::IDLE;
            break;
        }

        case FileState::DONE:
            break;
        }
    }

    return !(d.fileState == FileState::DONE && d.ring.empty());
}

// -----------------------------------------------------------------------
// init
// -----------------------------------------------------------------------
FileSourceData init_file_source(const BlockConfig &config) {
    FileSourceData data;
    data.sourceDirectory = "Test_Files";

    data.ring.init(RING_CAP);
    data.diskBuf.resize(DISK_CHUNK);

    if (!fs::exists(data.sourceDirectory)) {
        fs::create_directories(data.sourceDirectory);
        exit(1);
    }

    for (const auto &entry : fs::directory_iterator(data.sourceDirectory))
        if (entry.is_regular_file())
            data.filePaths.push_back(entry.path().string());

    if (data.filePaths.empty())
        exit(1);

    return data;
}

// -----------------------------------------------------------------------
// process
// -----------------------------------------------------------------------
void process_file_source(
    const char ** /*pipeIn*/,
    const char **pipeOut,
    FileSourceData &customData,
    const BlockConfig &config
) {
    PipeIO output(pipeOut[0], config.outputPacketSizes[0], config.outputBatchSizes[0]);

    bool hasData = fill_ring(customData);

    if (!hasData)
        throw std::runtime_error("All files transmitted");

    if (customData.ring.empty()) return;

    int available   = customData.ring.avail();
    int maxPackets  = (available + PACKET_SIZE - 1) / PACKET_SIZE;
    int numPackets  = std::min(BATCH_SIZE, maxPackets);
    int bytesToSend = numPackets * PACKET_SIZE;
    if (bytesToSend > available) bytesToSend = available;

    int8_t *outputBatch = new int8_t[output.getBufferSize()];
    memset(outputBatch, 0, output.getBufferSize());

    int peeked = customData.ring.peek(outputBatch, bytesToSend);
    customData.ring.consume(peeked);

    output.write(outputBatch, numPackets);

    static bool firstBatch = true;
    if (firstBatch && numPackets > 0) {
        firstBatch = false;
        printf("[FileSource] pkt[0] OUTPUT: %d bits\n", PACKET_SIZE * 8);
        fflush(stdout);
    }

    delete[] outputBatch;
}

// -----------------------------------------------------------------------
// main
// -----------------------------------------------------------------------
int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: file_source <pipeOut>\n");
        return 1;
    }

    const char *pipeOut = argv[1];

    BlockConfig config = {
        "FileSource",   // name
        0,              // inputs
        1,              // outputs
        {},             // inputPacketSizes
        {},             // inputBatchSizes
        {1500},         // outputPacketSizes
        {64},           // outputBatchSizes
        true,           // ltr
        false,          // startWithAll
        "Ring-buffer streaming source"
    };

    run_manual_block(nullptr, &pipeOut, config, process_file_source, init_file_source);
    return 0;
}