#include "mex.h"
#include <windows.h>
#include <string.h>
#include <stdlib.h>

// PRODUCTION VERSION - Instance-aware pipe names

void mexFunction(int nlhs, mxArray *plhs[], int nrhs, const mxArray *prhs[]) {
    if (nrhs < 2) {
        mexErrMsgTxt("Usage: pipeline_mex('write'/'read', 'PipeName', data/size)");
    }
    
    char cmd[16];
    mxGetString(prhs[0], cmd, sizeof(cmd));
    
    char shortPipeName[128];
    mxGetString(prhs[1], shortPipeName, sizeof(shortPipeName));
    
    // ===== BUILD FULL PIPE NAME WITH INSTANCE ID =====
    char pipeName[256];
    char* instanceId = getenv("INSTANCE_ID");
    
    if (instanceId && strlen(instanceId) > 0) {
        sprintf(pipeName, "Instance_%s_%s", instanceId, shortPipeName);
    } else {
        strcpy(pipeName, shortPipeName);
    }
    // =================================================
    
    // Create event names
    char readyName[256], emptyName[256];
    sprintf(readyName, "%s_Ready", pipeName);
    sprintf(emptyName, "%s_Empty", pipeName);
    
    if (strcmp(cmd, "write") == 0) {
        // WRITE OPERATION
        if (nrhs < 3) {
            mexErrMsgTxt("write requires data argument");
        }
        
        const mxArray* dataArray = prhs[2];
        if (!mxIsInt8(dataArray)) {
            mexErrMsgTxt("Data must be int8");
        }
        
        int8_T* data = (int8_T*)mxGetData(dataArray);
        size_t dataSize = mxGetNumberOfElements(dataArray);
        
        // Open shared memory
        HANDLE hMapFile = OpenFileMappingA(FILE_MAP_WRITE, FALSE, pipeName);
        if (!hMapFile) {
            char err[256];
            sprintf(err, "Failed to open %s (Error: %lu)", pipeName, GetLastError());
            mexErrMsgTxt(err);
        }
        
        // Map view
        void* pBuf = MapViewOfFile(hMapFile, FILE_MAP_WRITE, 0, 0, dataSize);
        if (!pBuf) {
            CloseHandle(hMapFile);
            mexErrMsgTxt("MapViewOfFile failed");
        }
        
        // Open events
        HANDLE hEmpty = OpenEventA(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, emptyName);
        HANDLE hReady = OpenEventA(EVENT_MODIFY_STATE, FALSE, readyName);
        
        if (!hEmpty || !hReady) {
            UnmapViewOfFile(pBuf);
            CloseHandle(hMapFile);
            mexErrMsgTxt("Failed to open events");
        }
        
        // Wait for empty (INFINITE wait - blocks will stay alive)
        DWORD waitResult = WaitForSingleObject(hEmpty, INFINITE);
        if (waitResult != WAIT_OBJECT_0) {
            UnmapViewOfFile(pBuf);
            CloseHandle(hMapFile);
            CloseHandle(hEmpty);
            CloseHandle(hReady);
            mexErrMsgTxt("Wait failed");
        }
        
        // Copy data
        memcpy(pBuf, data, dataSize);
        
        // Signal ready
        SetEvent(hReady);
        
        // Cleanup
        UnmapViewOfFile(pBuf);
        CloseHandle(hMapFile);
        CloseHandle(hEmpty);
        CloseHandle(hReady);
        
    } else if (strcmp(cmd, "read") == 0) {
        // READ OPERATION
        if (nrhs < 3) {
            mexErrMsgTxt("read requires size argument");
        }
        
        size_t readSize = (size_t)mxGetScalar(prhs[2]);
        
        // Open shared memory
        HANDLE hMapFile = OpenFileMappingA(FILE_MAP_READ, FALSE, pipeName);
        if (!hMapFile) {
            char err[256];
            sprintf(err, "Failed to open %s (Error: %lu)", pipeName, GetLastError());
            mexErrMsgTxt(err);
        }
        
        // Map view
        void* pBuf = MapViewOfFile(hMapFile, FILE_MAP_READ, 0, 0, readSize);
        if (!pBuf) {
            CloseHandle(hMapFile);
            mexErrMsgTxt("MapViewOfFile failed");
        }
        
        // Open events
        HANDLE hReady = OpenEventA(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, readyName);
        HANDLE hEmpty = OpenEventA(EVENT_MODIFY_STATE, FALSE, emptyName);
        
        if (!hReady || !hEmpty) {
            UnmapViewOfFile(pBuf);
            CloseHandle(hMapFile);
            mexErrMsgTxt("Failed to open events");
        }
        
        // Wait for ready (INFINITE wait - blocks will stay alive)
        DWORD waitResult = WaitForSingleObject(hReady, INFINITE);
        if (waitResult != WAIT_OBJECT_0) {
            UnmapViewOfFile(pBuf);
            CloseHandle(hMapFile);
            CloseHandle(hReady);
            CloseHandle(hEmpty);
            mexErrMsgTxt("Wait failed");
        }
        
        // Create output array
        plhs[0] = mxCreateNumericMatrix(readSize, 1, mxINT8_CLASS, mxREAL);
        int8_T* outData = (int8_T*)mxGetData(plhs[0]);
        
        // Copy data
        memcpy(outData, pBuf, readSize);
        
        // Signal empty
        SetEvent(hEmpty);
        
        // Cleanup
        UnmapViewOfFile(pBuf);
        CloseHandle(hMapFile);
        CloseHandle(hReady);
        CloseHandle(hEmpty);
        
    } else {
        mexErrMsgTxt("Unknown command. Use 'write' or 'read'");
    }
}