#ifndef RUN_GENERIC_BLOCK_H
#define RUN_GENERIC_BLOCK_H

#include "cpp_socket_client.h"
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string>
#include <vector>
#include <deque>

struct BlockConfig {
    const char* name;
    int inputs;
    int outputs;
    std::vector<int> inputPacketSizes;
    std::vector<int> inputBatchSizes;
    std::vector<int> outputPacketSizes;
    std::vector<int> outputBatchSizes;
    bool ltr;
    bool startWithAll;
    const char* description;
};

// ============================================================================
// PIPE READ / WRITE OPTIONS
// ============================================================================

/**
 * Read options passed to readBatch() / PipeIO::read().
 *
 * timeoutMs:
 *   PIPE_WAIT_INFINITE (default) – block forever until data arrives (old behaviour).
 *   Any positive value            – wait at most N milliseconds.
 *                                   On timeout, returns PIPE_RESULT_TIMEOUT (-1).
 *
 * Example:
 *   PipeReadOptions opts;
 *   opts.timeoutMs = 5000;   // wait up to 5 s
 *   int n = input.read(buf, opts);
 *   if (n == PIPE_RESULT_TIMEOUT) { ... }
 */
static const DWORD PIPE_WAIT_INFINITE = INFINITE;

struct PipeReadOptions {
    DWORD timeoutMs = PIPE_WAIT_INFINITE;   // INFINITE or milliseconds
};

/** Returned by read() when the wait times out (timeoutMs is not INFINITE). */
static const int PIPE_RESULT_TIMEOUT = -1;

/**
 * Write options passed to writeBatch() / PipeIO::write().
 *
 * overwrite:
 *   false (default) – wait until the consumer has read the previous batch
 *                     before writing (old behaviour, no data loss).
 *   true            – do NOT wait; immediately overwrite whatever is in the
 *                     shared buffer, then signal "ready" so the consumer sees
 *                     the latest data. Use for real-time / best-effort streams
 *                     where dropping stale frames is acceptable.
 *
 * Example:
 *   PipeWriteOptions opts;
 *   opts.overwrite = true;
 *   output.write(buf, count, opts);
 */
struct PipeWriteOptions {
    bool overwrite = false;   // true = overwrite without waiting for consumer
};

// ============================================================================
// HELPERS
// ============================================================================

inline int calculateLengthBytes(int maxCount) {
    if (maxCount <= 255)      return 1;
    if (maxCount <= 65535)    return 2;
    if (maxCount <= 16777215) return 3;
    return 4;
}

inline int calculateBufferSize(int packetSize, int batchSize) {
    int lengthBytes = calculateLengthBytes(batchSize);
    return lengthBytes + (packetSize * batchSize);
}

// ============================================================================
// LOW-LEVEL readBatch
// ============================================================================

/**
 * Returns the number of packets read, or PIPE_RESULT_TIMEOUT on timeout.
 */
inline int readBatch(const char*        pipeName,
                     int8_t*            batchData,
                     int                lengthBytes,
                     int                totalSize,
                     const PipeReadOptions& opts = PipeReadOptions{})
{
    std::string instanceId  = getEnvString("INSTANCE_ID", "");
    std::string fullPipeName = "Instance_" + instanceId + "_" + std::string(pipeName);

    char readyName[256], emptyName[256];
    sprintf(readyName, "%s_Ready", fullPipeName.c_str());
    sprintf(emptyName, "%s_Empty", fullPipeName.c_str());

    HANDLE hMapFile = OpenFileMappingA(FILE_MAP_READ, FALSE, fullPipeName.c_str());
    if (!hMapFile) {
        fprintf(stderr, "[PIPE] Failed to open %s: %lu\n", fullPipeName.c_str(), GetLastError());
        return 0;
    }

    void* pBuf = MapViewOfFile(hMapFile, FILE_MAP_READ, 0, 0, totalSize);
    if (!pBuf) {
        CloseHandle(hMapFile);
        return 0;
    }

    HANDLE hReady = OpenEventA(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, readyName);
    HANDLE hEmpty = OpenEventA(EVENT_MODIFY_STATE,               FALSE, emptyName);

    // ── Wait for data (configurable timeout) ─────────────────────────────────
    DWORD waitResult = WaitForSingleObject(hReady, opts.timeoutMs);

    int actualCount = 0;

    if (waitResult == WAIT_OBJECT_0) {
        // Data is available – copy it out
        uint8_t* buffer = (uint8_t*)pBuf;
        for (int i = 0; i < lengthBytes; i++) {
            actualCount |= buffer[i] << (i * 8);
        }
        memcpy(batchData, buffer + lengthBytes, totalSize - lengthBytes);
        SetEvent(hEmpty);
    } else {
        // WAIT_TIMEOUT or WAIT_FAILED
        actualCount = PIPE_RESULT_TIMEOUT;
    }

    UnmapViewOfFile(pBuf);
    CloseHandle(hMapFile);
    if (hReady) CloseHandle(hReady);
    if (hEmpty) CloseHandle(hEmpty);

    return actualCount;
}

// ============================================================================
// LOW-LEVEL writeBatch
// ============================================================================

inline void writeBatch(const char*         pipeName,
                       int8_t*             batchData,
                       int                 actualCount,
                       int                 lengthBytes,
                       int                 totalSize,
                       const PipeWriteOptions& opts = PipeWriteOptions{})
{
    std::string instanceId   = getEnvString("INSTANCE_ID", "");
    std::string fullPipeName = "Instance_" + instanceId + "_" + std::string(pipeName);

    char readyName[256], emptyName[256];
    sprintf(readyName, "%s_Ready", fullPipeName.c_str());
    sprintf(emptyName, "%s_Empty", fullPipeName.c_str());

    HANDLE hMapFile = OpenFileMappingA(FILE_MAP_WRITE, FALSE, fullPipeName.c_str());
    if (!hMapFile) {
        fprintf(stderr, "[PIPE] Failed to open %s: %lu\n", fullPipeName.c_str(), GetLastError());
        return;
    }

    void* pBuf = MapViewOfFile(hMapFile, FILE_MAP_WRITE, 0, 0, totalSize);
    if (!pBuf) {
        CloseHandle(hMapFile);
        return;
    }

    HANDLE hEmpty = OpenEventA(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, emptyName);
    HANDLE hReady = OpenEventA(EVENT_MODIFY_STATE,               FALSE, readyName);

    if (!opts.overwrite) {
        // ── Default: wait until consumer has finished reading ─────────────────
        WaitForSingleObject(hEmpty, INFINITE);
    } else {
        // ── Overwrite mode: check if consumer is still reading ────────────────
        // If the Ready event is already set, a previous write was not yet read.
        // We reset it (cancel the old "data ready" signal) so our new data wins.
        // We do NOT wait for Empty – we overwrite immediately.
        //
        // ResetEvent on Ready clears any pending "data ready" signal, then we
        // write our fresh data and signal Ready again below.
        // This is safe because:
        //   - If consumer is mid-read it holds the view; our write is coherent
        //     because Windows shared-memory writes are word-atomic on x86/x64.
        //   - The consumer will re-check and get the new batch on its next cycle.
        ResetEvent(hReady);   // discard stale "data ready" notification
    }

    // ── Write the payload ─────────────────────────────────────────────────────
    uint8_t* buffer = (uint8_t*)pBuf;
    for (int i = 0; i < lengthBytes; i++) {
        buffer[i] = (actualCount >> (i * 8)) & 0xFF;
    }
    memcpy(buffer + lengthBytes, batchData, totalSize - lengthBytes);

    SetEvent(hReady);   // notify consumer that fresh data is available

    UnmapViewOfFile(pBuf);
    CloseHandle(hMapFile);
    if (hEmpty) CloseHandle(hEmpty);
    if (hReady) CloseHandle(hReady);
}

// ============================================================================
// PipeIO HELPER CLASS
// ============================================================================

class PipeIO {
private:
    const char* pipeName;
    int         lengthBytes;
    int         packetSize;
    int         batchSize;
    int         bufferSize;

public:
    PipeIO(const char* pipe, int pktSize, int batchSz)
        : pipeName(pipe), packetSize(pktSize), batchSize(batchSz)
    {
        lengthBytes = calculateLengthBytes(batchSz);
        bufferSize  = lengthBytes + (pktSize * batchSz);
    }

    // ── Read ─────────────────────────────────────────────────────────────────

    /**
     * Blocking read (default – waits forever).
     * Returns the number of packets read.
     */
    int read(int8_t* buffer) {
        return readBatch(pipeName, buffer, lengthBytes, bufferSize);
    }

    /**
     * Read with explicit options (timeout, etc.).
     * Returns the number of packets read, or PIPE_RESULT_TIMEOUT (-1).
     *
     * Usage:
     *   PipeReadOptions opts;
     *   opts.timeoutMs = 2000;          // 2 s timeout
     *   int n = io.read(buf, opts);
     *   if (n == PIPE_RESULT_TIMEOUT) { ... handle no data ... }
     */
    int read(int8_t* buffer, const PipeReadOptions& opts) {
        return readBatch(pipeName, buffer, lengthBytes, bufferSize, opts);
    }

    // ── Write ────────────────────────────────────────────────────────────────

    /**
     * Blocking write (default – waits for consumer to drain previous batch).
     */
    void write(int8_t* buffer, int actualCount) {
        writeBatch(pipeName, buffer, actualCount, lengthBytes, bufferSize);
    }

    /**
     * Write with explicit options (overwrite mode, etc.).
     *
     * Usage (overwrite / best-effort):
     *   PipeWriteOptions opts;
     *   opts.overwrite = true;
     *   io.write(buf, count, opts);
     */
    void write(int8_t* buffer, int actualCount, const PipeWriteOptions& opts) {
        writeBatch(pipeName, buffer, actualCount, lengthBytes, bufferSize, opts);
    }

    // ── Accessors ────────────────────────────────────────────────────────────
    int         getBufferSize()  const { return bufferSize;  }
    int         getPacketSize()  const { return packetSize;  }
    int         getBatchSize()   const { return batchSize;   }
    int         getLengthBytes() const { return lengthBytes; }
    const char* getName()        const { return pipeName;    }
};

// ============================================================================
// MANUAL I/O BLOCK RUNNER
// ============================================================================

template<typename CustomData>
void run_manual_block(
    const char** pipeIn,
    const char** pipeOut,
    const BlockConfig& config,
    void (*process_fn)(const char**, const char**, CustomData&, const BlockConfig&),
    CustomData (*init_fn)(const BlockConfig&) = nullptr,
    int metricsWindow = 5          // number of recent rates to average before sending
) {
    int blockId = getEnvInt("BLOCK_ID", 0);
    int cppPort = getEnvInt("CPP_PORT",  9002);

    CppSocketClient socket("127.0.0.1", cppPort);
    if (!socket.connect(10)) {
        fprintf(stderr, "[BLOCK] Failed to connect to socket server\n");
        return;
    }

    DWORD pid = GetCurrentProcessId();
    socket.sendInit(blockId, config.name, pid);

    bool isSource = (config.inputs  == 0);
    bool isSink   = (config.outputs == 0);

    CustomData customData;
    if (init_fn) {
        printf("Running custom initialization...\n");
        customData = init_fn(config);
    }

    socket.sendReady(blockId, config.name);

    int     iterationCount = 0;
    double  totalBytes     = 0;
    double  cumulativeBytes = 0;    // all-time bytes for the avg Gbps fallback

    // Rolling window of recent Gbps samples (capped at metricsWindow entries)
    std::deque<double> rateWindow;

    LARGE_INTEGER freq, startTime, lastTime;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&startTime);
    lastTime = startTime;

    try {
        while (true) {
            process_fn(pipeIn, pipeOut, customData, config);
            iterationCount++;

            double batchBytes = 0.0;
            if (!isSink && config.outputs > 0) {
                batchBytes = calculateBufferSize(config.outputPacketSizes[0], config.outputBatchSizes[0]);
            } else if (!isSource && config.inputs > 0) {
                batchBytes = calculateBufferSize(config.inputPacketSizes[0], config.inputBatchSizes[0]);
            }
            totalBytes     += batchBytes;
            cumulativeBytes += batchBytes;

            LARGE_INTEGER currentTime;
            QueryPerformanceCounter(&currentTime);
            double elapsed = (double)(currentTime.QuadPart - lastTime.QuadPart) / freq.QuadPart;

            if (elapsed > 0) {
                // Compute the instantaneous rate for this interval
                double instantGbps = ((totalBytes * 8.0) / 1e9) / elapsed;

                // Push into the rolling window, evict oldest if full
                rateWindow.push_back(instantGbps);
                if ((int)rateWindow.size() > metricsWindow)
                    rateWindow.pop_front();

                // Average over whatever samples we have so far
                double sumGbps = 0.0;
                for (double r : rateWindow) sumGbps += r;
                double avgGbps = sumGbps / (double)rateWindow.size();

                socket.sendMetrics(blockId, config.name, iterationCount, avgGbps, cumulativeBytes / 1e9);

                totalBytes = 0;
                lastTime   = currentTime;
            }

            if (iterationCount % 100 == 0) {
                // (optional console log placeholder)
            }
        }
    } catch (...) {
        socket.sendStopped(blockId, config.name);
    }
}

#endif // RUN_GENERIC_BLOCK_H