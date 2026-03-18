#include "mex.h"
#include <windows.h>
#include <string.h>
#include <stdlib.h>

// PRODUCTION VERSION - Instance-aware pipe names
//
// USAGE
// ─────
// Read (blocking / infinite wait — original behaviour):
//   data = pipeline_mex('read', 'PipeName', sizeBytes)
//
// Read with timeout (returns empty int8 array on timeout):
//   data = pipeline_mex('read', 'PipeName', sizeBytes, timeoutMs)
//   e.g. data = pipeline_mex('read', 'P1', 9001, 5000)   % 5-second timeout
//        if isempty(data), disp('timeout'); end
//
// Write (blocking — waits for consumer to drain previous batch):
//   pipeline_mex('write', 'PipeName', int8Data)
//
// Write with overwrite (does NOT wait for consumer — best-effort / real-time):
//   pipeline_mex('write', 'PipeName', int8Data, 'overwrite')
//   e.g. pipeline_mex('write', 'P2', myData, 'overwrite')

void mexFunction(int nlhs, mxArray *plhs[],
                 int nrhs, const mxArray *prhs[])
{
    if (nrhs < 2) {
        mexErrMsgTxt(
            "Usage:\n"
            "  pipeline_mex('read',  'PipeName', sizeBytes [, timeoutMs])\n"
            "  pipeline_mex('write', 'PipeName', int8Data  [, 'overwrite'])");
    }

    char cmd[16];
    mxGetString(prhs[0], cmd, sizeof(cmd));

    char shortPipeName[128];
    mxGetString(prhs[1], shortPipeName, sizeof(shortPipeName));

    // ── Build full instance-scoped pipe name ──────────────────────────────────
    char pipeName[256];
    char *instanceId = getenv("INSTANCE_ID");
    if (instanceId && strlen(instanceId) > 0) {
        sprintf(pipeName, "Instance_%s_%s", instanceId, shortPipeName);
    } else {
        strcpy(pipeName, shortPipeName);
    }

    // ── Event names ───────────────────────────────────────────────────────────
    char readyName[256], emptyName[256];
    sprintf(readyName, "%s_Ready", pipeName);
    sprintf(emptyName, "%s_Empty", pipeName);

    // =========================================================================
    // READ
    // =========================================================================
    if (strcmp(cmd, "read") == 0) {

        if (nrhs < 3) {
            mexErrMsgTxt("read requires a size argument: pipeline_mex('read', 'PipeName', sizeBytes)");
        }

        size_t readSize = (size_t)mxGetScalar(prhs[2]);

        // Optional 4th argument: timeout in milliseconds (default = INFINITE)
        DWORD timeoutMs = INFINITE;
        if (nrhs >= 4) {
            double t = mxGetScalar(prhs[3]);
            if (t >= 0) {
                timeoutMs = (DWORD)t;
            }
        }

        // Open shared memory
        HANDLE hMapFile = OpenFileMappingA(FILE_MAP_READ, FALSE, pipeName);
        if (!hMapFile) {
            char err[256];
            sprintf(err, "Failed to open shared memory '%s' (Error: %lu)", pipeName, GetLastError());
            mexErrMsgTxt(err);
        }

        void *pBuf = MapViewOfFile(hMapFile, FILE_MAP_READ, 0, 0, readSize);
        if (!pBuf) {
            CloseHandle(hMapFile);
            mexErrMsgTxt("MapViewOfFile failed");
        }

        HANDLE hReady = OpenEventA(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, readyName);
        HANDLE hEmpty = OpenEventA(EVENT_MODIFY_STATE,               FALSE, emptyName);

        if (!hReady || !hEmpty) {
            UnmapViewOfFile(pBuf);
            CloseHandle(hMapFile);
            if (hReady) CloseHandle(hReady);
            if (hEmpty) CloseHandle(hEmpty);
            mexErrMsgTxt("Failed to open pipe events");
        }

        // ── Wait for data ─────────────────────────────────────────────────────
        DWORD waitResult = WaitForSingleObject(hReady, timeoutMs);

        if (waitResult == WAIT_TIMEOUT) {
            // Return empty int8 array so caller can detect the timeout
            UnmapViewOfFile(pBuf);
            CloseHandle(hMapFile);
            CloseHandle(hReady);
            CloseHandle(hEmpty);
            plhs[0] = mxCreateNumericMatrix(0, 0, mxINT8_CLASS, mxREAL);
            return;
        }

        if (waitResult != WAIT_OBJECT_0) {
            UnmapViewOfFile(pBuf);
            CloseHandle(hMapFile);
            CloseHandle(hReady);
            CloseHandle(hEmpty);
            mexErrMsgTxt("WaitForSingleObject failed (WAIT_FAILED)");
        }

        // ── Copy data out ─────────────────────────────────────────────────────
        plhs[0] = mxCreateNumericMatrix(readSize, 1, mxINT8_CLASS, mxREAL);
        int8_T *outData = (int8_T *)mxGetData(plhs[0]);
        memcpy(outData, pBuf, readSize);

        SetEvent(hEmpty);   // signal consumer has finished reading

        UnmapViewOfFile(pBuf);
        CloseHandle(hMapFile);
        CloseHandle(hReady);
        CloseHandle(hEmpty);

    // =========================================================================
    // WRITE
    // =========================================================================
    } else if (strcmp(cmd, "write") == 0) {

        if (nrhs < 3) {
            mexErrMsgTxt("write requires a data argument: pipeline_mex('write', 'PipeName', int8Data)");
        }

        const mxArray *dataArray = prhs[2];
        if (!mxIsInt8(dataArray)) {
            mexErrMsgTxt("Data must be int8");
        }

        int8_T *data     = (int8_T *)mxGetData(dataArray);
        size_t  dataSize = mxGetNumberOfElements(dataArray);

        // Optional 4th argument: 'overwrite'
        // When set, we do NOT wait for the consumer to drain the previous batch;
        // we immediately overwrite the buffer with new data.
        bool overwrite = false;
        if (nrhs >= 4) {
            char opt[32] = {0};
            mxGetString(prhs[3], opt, sizeof(opt));
            overwrite = (strcmp(opt, "overwrite") == 0);
        }

        // Open shared memory
        HANDLE hMapFile = OpenFileMappingA(FILE_MAP_WRITE, FALSE, pipeName);
        if (!hMapFile) {
            char err[256];
            sprintf(err, "Failed to open shared memory '%s' (Error: %lu)", pipeName, GetLastError());
            mexErrMsgTxt(err);
        }

        void *pBuf = MapViewOfFile(hMapFile, FILE_MAP_WRITE, 0, 0, dataSize);
        if (!pBuf) {
            CloseHandle(hMapFile);
            mexErrMsgTxt("MapViewOfFile failed");
        }

        HANDLE hEmpty = OpenEventA(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, emptyName);
        HANDLE hReady = OpenEventA(EVENT_MODIFY_STATE,               FALSE, readyName);

        if (!hEmpty || !hReady) {
            UnmapViewOfFile(pBuf);
            CloseHandle(hMapFile);
            if (hEmpty) CloseHandle(hEmpty);
            if (hReady) CloseHandle(hReady);
            mexErrMsgTxt("Failed to open pipe events");
        }

        if (!overwrite) {
            // ── Default: wait until consumer has read previous data ───────────
            DWORD waitResult = WaitForSingleObject(hEmpty, INFINITE);
            if (waitResult != WAIT_OBJECT_0) {
                UnmapViewOfFile(pBuf);
                CloseHandle(hMapFile);
                CloseHandle(hEmpty);
                CloseHandle(hReady);
                mexErrMsgTxt("Wait for empty failed");
            }
        } else {
            // ── Overwrite mode: discard any stale "data ready" signal ─────────
            // Reset Ready so any consumer that hasn't read yet will receive our
            // fresh data instead of the old batch.
            ResetEvent(hReady);
        }

        // ── Write the payload ─────────────────────────────────────────────────
        memcpy(pBuf, data, dataSize);
        SetEvent(hReady);   // notify consumer that new data is ready

        UnmapViewOfFile(pBuf);
        CloseHandle(hMapFile);
        CloseHandle(hEmpty);
        CloseHandle(hReady);

    } else {
        mexErrMsgTxt("Unknown command. Use 'read' or 'write'");
    }
}