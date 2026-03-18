function file_sink(pipeIn)
% FILE_SINK - On-the-fly disk writing with streaming file reconstruction

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

    config = parse_block_config(block_config);
    
    % BATCH PROCESSING PARAMETERS
    PACKET_SIZE = config.inputPacketSizes(1);
    BATCH_SIZE = config.inputBatchSizes(1);
    LENGTH_BYTES = calculate_length_bytes(BATCH_SIZE);
    BUFFER_SIZE = LENGTH_BYTES + (PACKET_SIZE * BATCH_SIZE);
    
    % Packet structure: 1500 bytes data + 1 byte error flag
    DATA_SIZE = PACKET_SIZE - 1;
    
    % FILE PROTOCOL CONSTANTS
    START_FLAG = uint8([0xAA, 0x55, 0xAA, 0x55]);
    END_FLAG = uint8([0x55, 0xAA, 0x55, 0xAA]);
    FILENAME_LENGTH = 256;
    REPETITIONS = 10;
    
    fprintf('\n========================================\n');
    fprintf('FILE SINK - Streaming On-The-Fly Writes\n');
    fprintf('========================================\n');
    fprintf('Packet Size:  %d bytes (%d data + 1 error flag)\n', PACKET_SIZE, DATA_SIZE);
    fprintf('Batch Size:   %d packets\n', BATCH_SIZE);
    fprintf('Mode:         Write to disk immediately, detect boundaries\n');
    fprintf('========================================\n');

    % Get instance-specific MATLAB port from environment
    matlabPortStr = getenv('MATLAB_PORT');
    if ~isempty(matlabPortStr)
        matlabPort = str2double(matlabPortStr);
    else
        matlabPort = config.socketPort;
    end

    socketObj = matlab_socket_client(config.socketHost, matlabPort, 10);

    if isempty(socketObj)
        error('Failed to connect to socket server. Make sure Electron is running!');
    end

    send_socket_message(socketObj, 'BLOCK_INIT', config.blockId, config.name, '');

    try
        if ~exist(config.outputDirectory, 'dir')
            mkdir(config.outputDirectory);
        end

        send_socket_message(socketObj, 'BLOCK_READY', config.blockId, config.name, '');

        batchCount = 0;
        packetCount = 0;
        totalErrorCount = 0;
        totalBytes = 0;
        startTime  = tic;
        lastTime   = 0;
        lastBytes  = 0;
        
        % Streaming file reconstruction state
        streamBuffer = [];           % Small buffer for file boundary detection
        currentFile = struct();      % Current file being written
        currentFile.active = false;
        currentFile.fid = -1;
        currentFile.name = '';
        currentFile.expectedSize = 0;
        currentFile.writtenBytes = 0;
        currentFile.errorCount = 0;
        currentFile.tempPath = '';
        
        filesReceived = 0;
        
        % Initialize error report file
        reportPath = fullfile(config.outputDirectory, 'error_report.txt');
        initialize_error_report(reportPath);
        
        fprintf('Output directory: %s\n', config.outputDirectory);
        fprintf('Error report:     %s\n', reportPath);
        fprintf('Streaming mode active - writing to disk on-the-fly...\n\n');

        while true
            try
                % Read batch from pipe
                buffer = pipeline_mex('read', pipeIn, BUFFER_SIZE);
                
            catch ME
                fprintf('\n========================================\n');
                fprintf('PIPELINE CLOSED\n');
                fprintf('========================================\n');
                
                % Close any open file
                if currentFile.active && currentFile.fid ~= -1
                    fclose(currentFile.fid);
                    finalize_file(currentFile, reportPath);
                    filesReceived = filesReceived + 1;
                end

                % Print final summary
                fprintf('\n========================================\n');
                fprintf('FINAL SUMMARY\n');
                fprintf('========================================\n');
                fprintf('Files received: %d\n', filesReceived);
                fprintf('Total batches:  %d\n', batchCount);
                fprintf('Total packets:  %d\n', packetCount);
                fprintf('Total errors:   %d\n', totalErrorCount);
                fprintf('Error rate:     %.4f%%\n', 100.0 * totalErrorCount / max(packetCount, 1));
                fprintf('Total data:     %.2f MB\n', totalBytes/1e6);
                fprintf('========================================\n');

                update_final_summary(reportPath, filesReceived, batchCount, packetCount, totalErrorCount, totalBytes);
                fprintf('\nFinal error report saved to: %s\n', reportPath);

                send_socket_message(socketObj, 'BLOCK_STOPPED', config.blockId, config.name, '');
                clear socketObj;
                return;
            end

            % Parse length header
            actualCount = 0;
            for i = 1:LENGTH_BYTES
                byteVal = uint8(int32(buffer(i)) + 128);
                actualCount = actualCount + double(byteVal) * (256^(i-1));
            end
            
            if actualCount == 0
                fprintf('Received EOF signal (0 packets)\n');
                break;
            end
            
            % Extract batch data
            batchData = buffer(LENGTH_BYTES + 1 : end);
            
            batchCount = batchCount + 1;
            packetCount = packetCount + actualCount;

            % ===== OPTIMIZED VECTORIZED PACKET PROCESSING =====
            tic_process = tic;
            
            % Pre-allocate output arrays
            newDataSize = actualCount * DATA_SIZE;
            newData = zeros(newDataSize, 1, 'uint8');
            newErrors = zeros(newDataSize, 1, 'int8');
            
            % Process all packets
            for pktIdx = 1:actualCount
                offset = (pktIdx - 1) * PACKET_SIZE;
                packet = batchData(offset + 1 : offset + PACKET_SIZE);
                
                % Extract data and error flag
                data = packet(1:DATA_SIZE);
                errorFlag = packet(PACKET_SIZE);
                
                % Convert int8 to uint8
                dataBytes = uint8(int32(data) + 128);
                
                % Track errors
                if errorFlag ~= 0
                    totalErrorCount = totalErrorCount + 1;
                end
                
                % Write to pre-allocated arrays
                outOffset = (pktIdx - 1) * DATA_SIZE;
                newData(outOffset + 1 : outOffset + DATA_SIZE) = dataBytes;
                newErrors(outOffset + 1 : outOffset + DATA_SIZE) = repmat(errorFlag, DATA_SIZE, 1);
            end
            
            process_time = toc(tic_process);
            totalBytes = totalBytes + (actualCount * DATA_SIZE);
            
            % ===== STREAMING FILE WRITE WITH BOUNDARY DETECTION =====
            tic_stream = tic;
            
            % Add new data to stream buffer
            streamBuffer = [streamBuffer; newData];
            
            % Process stream buffer for file boundaries
            while true
                % Not currently writing a file - look for START flag
                if ~currentFile.active
                    startPos = find_pattern(streamBuffer, START_FLAG);
                    
                    if isempty(startPos)
                        % No START found - keep last 4KB in buffer (in case START is split)
                        if length(streamBuffer) > 4096
                            streamBuffer = streamBuffer(end-4095:end);
                        end
                        break;
                    end
                    
                    % Found START - parse metadata
                    metadataSize = 4 + (FILENAME_LENGTH * REPETITIONS) + (8 * REPETITIONS);
                    
                    if length(streamBuffer) < startPos + metadataSize - 1
                        % Not enough data for full metadata yet
                        break;
                    end
                    
                    % Extract filename
                    nameStart = startPos + 4;
                    fileNames = cell(REPETITIONS, 1);
                    for i = 1:REPETITIONS
                        nameBytes = streamBuffer(nameStart + (i-1)*FILENAME_LENGTH : nameStart + i*FILENAME_LENGTH - 1);
                        nullPos = find(nameBytes == 0, 1);
                        if ~isempty(nullPos)
                            nameBytes = nameBytes(1:nullPos-1);
                        end
                        fileNames{i} = char(nameBytes');
                    end
                    fileName = majority_vote_string(fileNames);
                    
                    % Extract file size
                    sizeStart = nameStart + (FILENAME_LENGTH * REPETITIONS);
                    fileSizes = zeros(REPETITIONS, 1);
                    for i = 1:REPETITIONS
                        sizeBytes = streamBuffer(sizeStart + (i-1)*8 : sizeStart + i*8 - 1);
                        fileSizes(i) = typecast(uint8(sizeBytes), 'uint64');
                    end
                    fileSize = mode(fileSizes);
                    
                    % Open file for streaming write
                    tempPath = fullfile(config.outputDirectory, [fileName '.part']);
                    finalPath = fullfile(config.outputDirectory, fileName);
                    
                    fid = fopen(tempPath, 'wb');
                    if fid == -1
                        error('Cannot create file: %s', tempPath);
                    end
                    
                    fprintf('Started streaming: %s (%.2f MB)\n', fileName, fileSize/1e6);
                    
                    % Initialize current file state
                    currentFile.active = true;
                    currentFile.fid = fid;
                    currentFile.name = fileName;
                    currentFile.tempPath = tempPath;
                    currentFile.finalPath = finalPath;
                    currentFile.expectedSize = fileSize;
                    currentFile.writtenBytes = 0;
                    currentFile.errorCount = 0;
                    currentFile.startTime = tic;
                    
                    % Remove metadata from buffer
                    dataStart = startPos + metadataSize;
                    streamBuffer = streamBuffer(dataStart:end);
                    
                else
                    % Currently writing a file - write data to disk
                    remainingBytes = currentFile.expectedSize - currentFile.writtenBytes;
                    
                    if remainingBytes <= 0
                        % File complete - look for END flag
                        if length(streamBuffer) >= 4
                            endFlag = streamBuffer(1:4);
                            if isequal(endFlag, END_FLAG')
                                % Valid END flag found
                                streamBuffer = streamBuffer(5:end);
                                
                                % Close and finalize file
                                fclose(currentFile.fid);
                                
                                % Rename from .part to final name
                                movefile(currentFile.tempPath, currentFile.finalPath);
                                
                                writeTime = toc(currentFile.startTime);
                                throughput = (currentFile.expectedSize / 1e6) / writeTime;
                                
                                fprintf('Completed: %s (%.2f MB in %.2fs = %.2f MB/s)', ...
                                        currentFile.name, currentFile.expectedSize/1e6, ...
                                        writeTime, throughput);
                                
                                if currentFile.errorCount > 0
                                    errorRate = 100.0 * currentFile.errorCount / currentFile.expectedSize;
                                    fprintf(' - ⚠ %d errors (%.4f%%)\n', currentFile.errorCount, errorRate);
                                else
                                    fprintf(' - ✓ Clean\n');
                                end
                                
                                % Update error report
                                report = struct();
                                report.name = currentFile.name;
                                report.size = currentFile.expectedSize;
                                report.packets = ceil(currentFile.expectedSize / 1500);
                                report.errors = currentFile.errorCount;
                                report.errorRate = 100.0 * currentFile.errorCount / max(currentFile.expectedSize, 1);
                                report.timestamp = datestr(now);
                                append_file_to_report(reportPath, report);
                                
                                filesReceived = filesReceived + 1;
                                
                                % Reset for next file
                                currentFile.active = false;
                                currentFile.fid = -1;
                                
                            else
                                fprintf('WARNING: END flag mismatch for %s\n', currentFile.name);
                                fclose(currentFile.fid);
                                currentFile.active = false;
                                streamBuffer = streamBuffer(5:end);
                            end
                        else
                            % Wait for more data
                            break;
                        end
                    else
                        % Write available data to file
                        bytesToWrite = min(remainingBytes, length(streamBuffer));
                        
                        if bytesToWrite > 0
                            dataToWrite = streamBuffer(1:bytesToWrite);
                            
                            % Write to disk IMMEDIATELY
                            fwrite(currentFile.fid, dataToWrite, 'uint8');
                            
                            currentFile.writtenBytes = currentFile.writtenBytes + bytesToWrite;
                            
                            % Remove written data from buffer
                            streamBuffer = streamBuffer(bytesToWrite + 1 : end);
                            
                            % Show progress for large files
                            progress = 100.0 * currentFile.writtenBytes / currentFile.expectedSize;
                            if mod(floor(progress), 10) == 0 && mod(floor(progress), 10) ~= floor(100.0 * (currentFile.writtenBytes - bytesToWrite) / currentFile.expectedSize)
                                fprintf('  Progress: %s - %.0f%% (%.2f MB / %.2f MB)\n', ...
                                        currentFile.name, progress, ...
                                        currentFile.writtenBytes/1e6, currentFile.expectedSize/1e6);
                            end
                        else
                            % No more data in buffer
                            break;
                        end
                    end
                end
            end
            
            stream_time = toc(tic_stream);

            % Calculate metrics
            currentTime = toc(startTime);
            elapsed = currentTime - lastTime;
            if elapsed > 0
                instantGbps = ((totalBytes - lastBytes) * 8 / 1e9) / elapsed;
            else
                instantGbps = 0;
            end
            lastTime  = currentTime;
            lastBytes = totalBytes;

            metrics         = struct();
            metrics.frames  = batchCount;
            metrics.gbps    = instantGbps;
            metrics.totalGB = totalBytes / 1e9;
            send_socket_message(socketObj, 'BLOCK_METRICS', config.blockId, config.name, metrics);
            
            % Progress update
            if mod(batchCount, 10) == 0
                fprintf('[Batch %d] %d pkts, %d files, %.2f MB, proc: %.3fs, stream: %.3fs, buf: %.1f KB\n', ...
                        batchCount, packetCount, filesReceived, totalBytes/1e6, ...
                        process_time, stream_time, length(streamBuffer)/1024);
            end
        end

    catch ME
        % Close any open file on error
        if currentFile.active && currentFile.fid ~= -1
            fclose(currentFile.fid);
        end
        
        send_socket_message(socketObj, 'BLOCK_ERROR', config.blockId, config.name, ME.message);
        clear socketObj;
        rethrow(ME);
    end
end

function initialize_error_report(filepath)
    fid = fopen(filepath, 'w');
    if fid == -1
        fprintf('ERROR: Cannot create error report file\n');
        return;
    end
    
    fprintf(fid, '========================================\n');
    fprintf(fid, 'FILE TRANSMISSION ERROR REPORT\n');
    fprintf(fid, '========================================\n');
    fprintf(fid, 'Started: %s\n', datestr(now));
    fprintf(fid, 'Mode: STREAMING (on-the-fly writes)\n\n');
    fprintf(fid, 'FILES RECEIVED:\n');
    fprintf(fid, '========================================\n\n');
    
    fclose(fid);
end

function append_file_to_report(filepath, report)
    fid = fopen(filepath, 'a');
    if fid == -1
        fprintf('ERROR: Cannot append to error report file\n');
        return;
    end
    
    fprintf(fid, '[%s]\n', report.timestamp);
    fprintf(fid, 'File: %s\n', report.name);
    fprintf(fid, '  Size:         %d bytes (%.2f MB)\n', report.size, report.size/1e6);
    fprintf(fid, '  Packets:      %d\n', report.packets);
    fprintf(fid, '  Errors:       %d\n', report.errors);
    fprintf(fid, '  Error Rate:   %.4f%%\n', report.errorRate);
    if report.errors > 0
        fprintf(fid, '  Status:       CORRUPTED ⚠\n');
    else
        fprintf(fid, '  Status:       CLEAN ✓\n');
    end
    fprintf(fid, '\n');
    
    fclose(fid);
end

function update_final_summary(filepath, totalFiles, batches, packets, errors, totalBytes)
    fid = fopen(filepath, 'a');
    if fid == -1
        fprintf('ERROR: Cannot update error report file\n');
        return;
    end
    
    fprintf(fid, '========================================\n');
    fprintf(fid, 'FINAL SUMMARY\n');
    fprintf(fid, '========================================\n');
    fprintf(fid, 'Completed: %s\n\n', datestr(now));
    fprintf(fid, 'Files received: %d\n', totalFiles);
    fprintf(fid, 'Total batches:  %d\n', batches);
    fprintf(fid, 'Total packets:  %d\n', packets);
    fprintf(fid, 'Total errors:   %d\n', errors);
    fprintf(fid, 'Error rate:     %.4f%%\n', 100.0 * errors / max(packets, 1));
    fprintf(fid, 'Total data:     %.2f MB\n', totalBytes/1e6);
    fprintf(fid, '========================================\n');
    
    fclose(fid);
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

function lengthBytes = calculate_length_bytes(maxCount)
    if maxCount <= 255
        lengthBytes = 1;
    elseif maxCount <= 65535
        lengthBytes = 2;
    elseif maxCount <= 16777215
        lengthBytes = 3;
    else
        lengthBytes = 4;
    end
end

function config = parse_block_config(block_config)
    config = block_config;
    blockIdStr = getenv('BLOCK_ID');
    if isempty(blockIdStr)
        config.blockId = 0;
    else
        config.blockId = str2double(blockIdStr);
    end
    
    if ~isfield(config, 'inputPacketSizes')
        error('inputPacketSizes required for batch processing');
    end
    if ~isfield(config, 'inputBatchSizes')
        error('inputBatchSizes required for batch processing');
    end
    
    % Convert to arrays if single values
    if ~iscell(config.inputPacketSizes) && ~ismatrix(config.inputPacketSizes)
        config.inputPacketSizes = [config.inputPacketSizes];
    end
    if ~iscell(config.inputBatchSizes) && ~ismatrix(config.inputBatchSizes)
        config.inputBatchSizes = [config.inputBatchSizes];
    end
end