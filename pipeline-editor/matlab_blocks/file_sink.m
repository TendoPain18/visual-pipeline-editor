function file_sink(pipeIn)
% FILE_SINK - On-the-fly disk writing using generic block framework

    % ========== USER CONFIGURATION ==========
    block_config = struct( ...
        'name',             'FileSink', ...
        'inputs',           1, ...
        'outputs',          0, ...
        'inputPacketSizes', 1501, ...
        'inputBatchSizes',  44740, ...
        'LTR',              false, ...
        'startWithAll',     true, ...
        'socketHost',       'localhost', ...
        'socketPort',       9001, ...
        'outputDirectory',  'C:\Users\amrga\Downloads\final\pipeline-editor\Output_Files', ...
        'description',      'Streaming file sink with on-the-fly disk writes' ...
    );
    
    % ========== CUSTOM INITIALIZATION ==========
    init_fn = @(config) initialize_file_sink(config);
    
    % ========== PROCESSING FUNCTION ==========
    process_fn = @(inputBatch, actualCount, initData, config) ...
        process_file_sink_persistent(inputBatch, actualCount, initData, config);
    
    % ========== RUN GENERIC BLOCK ==========
    run_generic_block(pipeIn, [], block_config, process_fn, init_fn);
end

function customData = initialize_file_sink(config)
    % FILE PROTOCOL CONSTANTS
    START_FLAG = uint8([0xAA, 0x55, 0xAA, 0x55]);
    END_FLAG = uint8([0x55, 0xAA, 0x55, 0xAA]);
    FILENAME_LENGTH = 256;
    REPETITIONS = 10;
    
    if ~exist(config.outputDirectory, 'dir')
        mkdir(config.outputDirectory);
    end
    
    % Initialize error report
    reportPath = fullfile(config.outputDirectory, 'error_report.txt');
    fid = fopen(reportPath, 'w');
    if fid == -1
        error('Cannot create error report file: %s', reportPath);
    end
    fprintf(fid, '========================================\n');
    fprintf(fid, 'FILE TRANSMISSION ERROR REPORT\n');
    fprintf(fid, '========================================\n');
    fprintf(fid, 'Started: %s\n', datestr(now));
    fprintf(fid, 'Mode: STREAMING (on-the-fly writes)\n\n');
    fprintf(fid, 'FILES RECEIVED:\n');
    fprintf(fid, '========================================\n\n');
    fclose(fid);
    
    customData = struct();
    customData.streamBuffer = [];
    customData.currentFile = struct('active', false, 'fid', -1);
    customData.filesReceived = 0;
    customData.totalErrorCount = 0;
    customData.outputDirectory = config.outputDirectory;
    customData.reportPath = reportPath;
    customData.START_FLAG = START_FLAG;
    customData.END_FLAG = END_FLAG;
    customData.FILENAME_LENGTH = FILENAME_LENGTH;
    customData.REPETITIONS = REPETITIONS;
    customData.lastProgressPercent = -1;
    
    fprintf('Output directory: %s\n', config.outputDirectory);
    fprintf('Error report:     %s\n', reportPath);
    fprintf('Streaming mode active - writing to disk on-the-fly...\n\n');
end

function [outputBatch, actualOutputCount] = process_file_sink_persistent(inputBatch, actualCount, initData, config)
    % Use persistent variable to maintain state
    persistent state;
    
    % First call - initialize from initData
    if isempty(state)
        state = initData;
    end
    
    PACKET_SIZE = config.inputPacketSizes(1);
    DATA_SIZE = PACKET_SIZE - 1;  % Last byte is error flag
    
    % ===== OPTIMIZED: Process entire batch at once =====
    if actualCount == 0
        % EOF signal received
        fprintf('Received EOF signal (0 packets)\n');
        
        % Close any open file
        if state.currentFile.active && state.currentFile.fid ~= -1
            fclose(state.currentFile.fid);
            if exist(state.currentFile.tempPath, 'file')
                movefile(state.currentFile.tempPath, state.currentFile.finalPath);
            end
            fprintf('Completed: %s\n', state.currentFile.name);
            state.filesReceived = state.filesReceived + 1;
            state.currentFile.active = false;
        end
        
        fprintf('\n========================================\n');
        fprintf('ALL FILES RECEIVED\n');
        fprintf('========================================\n');
        fprintf('Total files: %d\n', state.filesReceived);
        fprintf('========================================\n');
        
        outputBatch = [];
        actualOutputCount = 0;
        return;
    end
    
    % Extract all data from batch at once (MUCH FASTER)
    batchData = inputBatch(1 : actualCount * PACKET_SIZE);
    
    % Vectorized extraction: reshape to separate packets
    batchMatrix = reshape(batchData, PACKET_SIZE, actualCount);
    dataMatrix = batchMatrix(1:DATA_SIZE, :);  % All data bytes
    errorFlags = batchMatrix(PACKET_SIZE, :);   % All error flags
    
    % Convert to uint8 and flatten
    newData = uint8(int32(dataMatrix(:)) + 128);
    
    % Track errors
    state.totalErrorCount = state.totalErrorCount + sum(errorFlags ~= 0);
    
    % Add new data to stream buffer
    state.streamBuffer = [state.streamBuffer; newData];
    
    % Process stream buffer for file boundaries
    while true
        if ~state.currentFile.active
            % Look for START flag
            startPos = find_pattern(state.streamBuffer, state.START_FLAG);
            
            if isempty(startPos)
                if length(state.streamBuffer) > 4096
                    state.streamBuffer = state.streamBuffer(end-4095:end);
                end
                break;
            end
            
            % Parse metadata
            metadataSize = 4 + (state.FILENAME_LENGTH * state.REPETITIONS) + (8 * state.REPETITIONS);
            
            if length(state.streamBuffer) < startPos + metadataSize - 1
                break;
            end
            
            % Extract filename (majority vote)
            nameStart = startPos + 4;
            fileNames = cell(state.REPETITIONS, 1);
            for i = 1:state.REPETITIONS
                nameBytes = state.streamBuffer(nameStart + (i-1)*state.FILENAME_LENGTH : nameStart + i*state.FILENAME_LENGTH - 1);
                nullPos = find(nameBytes == 0, 1);
                if ~isempty(nullPos)
                    nameBytes = nameBytes(1:nullPos-1);
                end
                fileNames{i} = char(nameBytes');
            end
            fileName = majority_vote_string(fileNames);
            
            % Extract file size (mode)
            sizeStart = nameStart + (state.FILENAME_LENGTH * state.REPETITIONS);
            fileSizes = zeros(state.REPETITIONS, 1);
            for i = 1:state.REPETITIONS
                sizeBytes = state.streamBuffer(sizeStart + (i-1)*8 : sizeStart + i*8 - 1);
                fileSizes(i) = typecast(uint8(sizeBytes), 'uint64');
            end
            fileSize = mode(fileSizes);
            
            % Create safe file paths
            safeFileName = regexprep(fileName, '[\\/:*?"<>|]', '_');  % Remove invalid chars
            tempPath = fullfile(state.outputDirectory, [safeFileName '.part']);
            finalPath = fullfile(state.outputDirectory, safeFileName);
            
            % Open file for writing
            fid = fopen(tempPath, 'wb');
            if fid == -1
                error('Cannot create file: %s', tempPath);
            end
            
            fprintf('Started streaming: %s (%.2f MB)\n', fileName, fileSize/1e6);
            
            % Initialize current file state
            state.currentFile.active = true;
            state.currentFile.fid = fid;
            state.currentFile.name = fileName;
            state.currentFile.tempPath = tempPath;
            state.currentFile.finalPath = finalPath;
            state.currentFile.expectedSize = fileSize;
            state.currentFile.writtenBytes = 0;
            state.currentFile.errorCount = 0;
            state.currentFile.startTime = tic;
            state.lastProgressPercent = -1;
            
            % Remove metadata from buffer
            dataStart = startPos + metadataSize;
            state.streamBuffer = state.streamBuffer(dataStart:end);
            
        else
            % Write data to file
            remainingBytes = state.currentFile.expectedSize - state.currentFile.writtenBytes;
            
            if remainingBytes <= 0
                % File complete - look for END flag
                if length(state.streamBuffer) >= 4
                    endFlag = state.streamBuffer(1:4);
                    if isequal(endFlag, state.END_FLAG')
                        state.streamBuffer = state.streamBuffer(5:end);
                        
                        % Close and finalize file
                        fclose(state.currentFile.fid);
                        movefile(state.currentFile.tempPath, state.currentFile.finalPath);
                        
                        writeTime = toc(state.currentFile.startTime);
                        throughput = (state.currentFile.expectedSize / 1e6) / writeTime;
                        
                        fprintf('Completed: %s (%.2f MB in %.2fs = %.2f MB/s)', ...
                                state.currentFile.name, state.currentFile.expectedSize/1e6, ...
                                writeTime, throughput);
                        
                        if state.currentFile.errorCount > 0
                            errorRate = 100.0 * state.currentFile.errorCount / state.currentFile.expectedSize;
                            fprintf(' - ⚠ %d errors (%.4f%%)\n', state.currentFile.errorCount, errorRate);
                        else
                            fprintf(' - ✓ Clean\n');
                        end
                        
                        % Update error report
                        append_file_to_report(state.reportPath, state.currentFile);
                        
                        state.filesReceived = state.filesReceived + 1;
                        state.currentFile.active = false;
                        state.currentFile.fid = -1;
                        
                    else
                        fprintf('WARNING: END flag mismatch for %s\n', state.currentFile.name);
                        fclose(state.currentFile.fid);
                        state.currentFile.active = false;
                        state.streamBuffer = state.streamBuffer(5:end);
                    end
                else
                    break;
                end
            else
                % Write available data
                bytesToWrite = min(remainingBytes, length(state.streamBuffer));
                
                if bytesToWrite > 0
                    dataToWrite = state.streamBuffer(1:bytesToWrite);
                    fwrite(state.currentFile.fid, dataToWrite, 'uint8');
                    state.currentFile.writtenBytes = state.currentFile.writtenBytes + bytesToWrite;
                    state.streamBuffer = state.streamBuffer(bytesToWrite + 1 : end);
                    
                    % Show progress only at 10% intervals
                    progress = floor(100.0 * state.currentFile.writtenBytes / state.currentFile.expectedSize);
                    if mod(progress, 10) == 0 && progress ~= state.lastProgressPercent
                        fprintf('  Progress: %s - %d%% (%.2f MB / %.2f MB)\n', ...
                                state.currentFile.name, progress, ...
                                state.currentFile.writtenBytes/1e6, state.currentFile.expectedSize/1e6);
                        state.lastProgressPercent = progress;
                    end
                else
                    break;
                end
            end
        end
    end
    
    outputBatch = [];
    actualOutputCount = 0;
end

function pos = find_pattern(data, pattern)
    pos = [];
    if length(data) < length(pattern)
        return;
    end
    
    for i = 1:length(data) - length(pattern) + 1
        if isequal(data(i:i+length(pattern)-1), pattern')
            pos = i;
            return;
        end
    end
end

function result = majority_vote_string(strings)
    if isempty(strings)
        result = '';
        return;
    end
    
    [uniqueStrings, ~, idx] = unique(strings);
    counts = histc(idx, 1:length(uniqueStrings));
    [~, maxIdx] = max(counts);
    result = uniqueStrings{maxIdx};
end

function append_file_to_report(filepath, fileInfo)
    fid = fopen(filepath, 'a');
    if fid == -1
        return;
    end
    
    fprintf(fid, '[%s]\n', datestr(now));
    fprintf(fid, 'File: %s\n', fileInfo.name);
    fprintf(fid, '  Size:         %d bytes (%.2f MB)\n', fileInfo.expectedSize, fileInfo.expectedSize/1e6);
    fprintf(fid, '  Errors:       %d\n', fileInfo.errorCount);
    if fileInfo.errorCount > 0
        errorRate = 100.0 * fileInfo.errorCount / fileInfo.expectedSize;
        fprintf(fid, '  Error Rate:   %.4f%%\n', errorRate);
        fprintf(fid, '  Status:       CORRUPTED ⚠\n');
    else
        fprintf(fid, '  Status:       CLEAN ✓\n');
    end
    fprintf(fid, '\n');
    
    fclose(fid);
end