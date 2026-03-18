#include "mex.h"
#include <windows.h>
#include <string.h>

// OPTIMIZED VERSION - Reduce overhead

void mexFunction(int nlhs, mxArray *plhs[], int nrhs, const mxArray *prhs[]) {
    if (nrhs < 2) {
        mexErrMsgTxt("Usage: pipeline_mex('write'/'read', 'PipeName', data/size)");
    }
    
    char cmd[16];
    mxGetString(prhs[0], cmd, sizeof(cmd));
    
    char pipeName[128];
    mxGetString(prhs[1], pipeName, sizeof(pipeName));
    
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
        
        // Use MATLAB's type: int8_T instead of int8_t
        int8_T* data = (int8_T*)mxGetData(dataArray);
        size_t dataSize = mxGetNumberOfElements(dataArray);
        
        // Open shared memory
        HANDLE hMapFile = OpenFileMappingA(FILE_MAP_WRITE, FALSE, pipeName);
        if (!hMapFile) {
            char err[256];
            sprintf(err, "Failed to open %s (Error: %d)", pipeName, GetLastError());
            mexErrMsgTxt(err);
        }
        
        // Map view - CRITICAL: Use FILE_MAP_WRITE for better performance
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
        
        // OPTIMIZED: Direct memory copy (no intermediate buffer)
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
            sprintf(err, "Failed to open %s (Error: %d)", pipeName, GetLastError());
            mexErrMsgTxt(err);
        }
        
        // Map view - CRITICAL: Use FILE_MAP_READ for better performance
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
        
        // OPTIMIZED: Direct memory copy
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