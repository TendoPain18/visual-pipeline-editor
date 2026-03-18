function file_sink(pipeIn)
% FILE_SINK - Continuous file reception (INSTANCE-AWARE)

block_config = struct( ...
    'name',            'FileSink', ...
    'inputs',          1, ...
    'outputs',         0, ...
    'inputSize',       1501, ...
    'outputSize',      0, ...
    'LTR',             false, ...
    'startWithAll',    true, ...
    'socketHost',      'localhost', ...
    'socketPort',      9001, ...
    'outputDirectory', 'C:\Users\amrga\Downloads\final\pipeline-editor\Output_Files', ...
    'reportFile',      'error_report.txt', ...
    'description',     'File sink - receives multiple batches, reports after each batch' ...
);

    config = parse_block_config(block_config);

    % Get instance-specific MATLAB port from environment
    matlabPortStr = getenv('MATLAB_PORT');
    if ~isempty(matlabPortStr)
        matlabPort = str2double(matlabPortStr);
    else
        matlabPort = config.socketPort;
    end

    % SOCKET CONNECTION (REQUIRED)
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

        packetCount = 0;
        totalBytes  = 0;
        startTime   = tic;
        lastTime    = 0;
        lastBytes   = 0;

        currentFile  = struct();
        currentFile.active = false;
        fileStats    = {};
        totalErrors  = 0;

        while true
            try
                packet = pipeline_mex('read', pipeIn, config.inputSize);
            catch ME
                fprintf('\n========================================\n');
                fprintf('PIPELINE CLOSED\n');
                fprintf('========================================\n');

                if currentFile.active && ~isempty(currentFile.data)
                    fprintf('Saving incomplete file: %s\n', currentFile.name);
                    save_file(currentFile, config.outputDirectory);
                    fileStats{end+1} = struct('name', currentFile.name, ...
                                              'size', length(currentFile.data), ...
                                              'expected', currentFile.size, ...
                                              'errors', currentFile.errors, ...
                                              'packets', currentFile.packets, ...
                                              'complete', false);
                    totalErrors = totalErrors + currentFile.errors;
                end

                fprintf('Generating final error report...\n');
                generate_error_report(fileStats, config.outputDirectory, config.reportFile);

                fprintf('\n========================================\n');
                fprintf('RECEPTION SUMMARY\n');
                fprintf('========================================\n');
                fprintf('Total files received: %d\n', length(fileStats));
                fprintf('Total packets:        %d\n', packetCount);
                fprintf('Total errors:         %d\n', totalErrors);
                fprintf('Total data:           %.2f MB\n', totalBytes/1e6);
                fprintf('========================================\n');

                send_socket_message(socketObj, 'BLOCK_STOPPED', config.blockId, config.name, '');
                clear socketObj;
                return;
            end

            data      = packet(1:1500);
            errorFlag = packet(1501);

            packetCount = packetCount + 1;
            totalBytes  = totalBytes + 1500;

            dataBytes = uint8(int32(data) + 128);

            isStart  = (dataBytes(1) == 0x53 && dataBytes(2) == 0x54 && ...
                       dataBytes(3) == 0x41 && dataBytes(4) == 0x52);
            isHeader = (dataBytes(1) == 0x46 && dataBytes(2) == 0x49 && ...
                       dataBytes(3) == 0x4C && dataBytes(4) == 0x45);
            isEnd    = (dataBytes(1) == 0x45 && dataBytes(2) == 0x4E && ...
                       dataBytes(3) == 0x44 && dataBytes(4) == 0x00);

            if isStart
                fprintf('\n--- START marker received ---\n');
                continue;
            end

            if isEnd
                fprintf('--- END marker received ---\n');

                if currentFile.active
                    if length(currentFile.data) >= currentFile.size
                        currentFile.data = currentFile.data(1:currentFile.size);
                        save_file(currentFile, config.outputDirectory);
                        fileStats{end+1} = struct('name', currentFile.name, ...
                                                  'size', currentFile.size, ...
                                                  'expected', currentFile.size, ...
                                                  'errors', currentFile.errors, ...
                                                  'packets', currentFile.packets, ...
                                                  'complete', true);
                        fprintf('  File complete and saved: %s\n', currentFile.name);
                    else
                        save_file(currentFile, config.outputDirectory);
                        fileStats{end+1} = struct('name', currentFile.name, ...
                                                  'size', length(currentFile.data), ...
                                                  'expected', currentFile.size, ...
                                                  'errors', currentFile.errors, ...
                                                  'packets', currentFile.packets, ...
                                                  'complete', false);
                        fprintf('  File incomplete: %s (%d/%d bytes)\n', ...
                                currentFile.name, length(currentFile.data), currentFile.size);
                    end
                    totalErrors = totalErrors + currentFile.errors;
                    generate_error_report(fileStats, config.outputDirectory, config.reportFile);
                    currentFile.active = false;
                end

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
                metrics.frames  = packetCount;
                metrics.gbps    = instantGbps;
                metrics.totalGB = totalBytes / 1e9;
                send_socket_message(socketObj, 'BLOCK_METRICS', config.blockId, config.name, metrics);
                fprintf('Total files received: %d, Total packets: %d\n\n', length(fileStats), packetCount);
                continue;
            end

            if isHeader
                if currentFile.active
                    save_file(currentFile, config.outputDirectory);
                    fileStats{end+1} = struct('name', currentFile.name, ...
                                              'size', currentFile.size, ...
                                              'expected', currentFile.size, ...
                                              'errors', currentFile.errors, ...
                                              'packets', currentFile.packets, ...
                                              'complete', true);
                    totalErrors = totalErrors + currentFile.errors;
                end

                nameLen  = bitor(uint16(dataBytes(5)), bitshift(uint16(dataBytes(6)), 8));
                fileName = char(dataBytes(7:6+nameLen)');

                fileSize = uint64(0);
                for i = 0:7
                    fileSize = bitor(fileSize, bitshift(uint64(dataBytes(7+nameLen+i)), 8*i));
                end

                currentFile.active  = true;
                currentFile.name    = fileName;
                currentFile.size    = double(fileSize);
                currentFile.data    = [];
                currentFile.errors  = 0;
                currentFile.packets = 0;

                if errorFlag ~= 0
                    currentFile.errors = currentFile.errors + 1;
                end
                currentFile.packets = currentFile.packets + 1;

            else
                if currentFile.active
                    currentFile.data    = [currentFile.data; data];
                    currentFile.packets = currentFile.packets + 1;
                    if errorFlag ~= 0
                        currentFile.errors = currentFile.errors + 1;
                    end
                end
            end

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
            metrics.frames  = packetCount;
            metrics.gbps    = instantGbps;
            metrics.totalGB = totalBytes / 1e9;
            send_socket_message(socketObj, 'BLOCK_METRICS', config.blockId, config.name, metrics);
        end

    catch ME
        send_socket_message(socketObj, 'BLOCK_ERROR', config.blockId, config.name, ME.message);
        clear socketObj;
        rethrow(ME);
    end
end

function save_file(fileInfo, outputDir)
    filePath = fullfile(outputDir, fileInfo.name);
    fid = fopen(filePath, 'wb');
    if fid == -1
        fprintf('ERROR: Cannot create: %s\n', filePath);
        return;
    end
    dataBytes = uint8(int32(fileInfo.data) + 128);
    fwrite(fid, dataBytes);
    fclose(fid);
    fprintf('Saved: %s (%.2f KB, %d errors)\n', fileInfo.name, fileInfo.size/1024, fileInfo.errors);
end

function generate_error_report(fileStats, outputDir, reportFile)
    reportPath = fullfile(outputDir, reportFile);
    fid = fopen(reportPath, 'w');
    if fid == -1, return; end

    fprintf(fid, '========================================\n');
    fprintf(fid, 'FILE TRANSMISSION ERROR REPORT\n');
    fprintf(fid, '========================================\n');
    fprintf(fid, 'Generated: %s\n', datestr(now));
    fprintf(fid, 'Total files: %d\n', length(fileStats));
    fprintf(fid, '========================================\n\n');

    totalErrors  = 0;
    totalPackets = 0;
    completeFiles = 0;

    for i = 1:length(fileStats)
        stat      = fileStats{i};
        errorRate = 100.0 * stat.errors / stat.packets;
        totalErrors  = totalErrors  + stat.errors;
        totalPackets = totalPackets + stat.packets;

        if stat.complete
            completeFiles = completeFiles + 1;
            status = 'COMPLETE';
        else
            status = 'INCOMPLETE';
        end

        fprintf(fid, 'File %d: %s [%s]\n', i, stat.name, status);
        fprintf(fid, '  Expected Size: %.2f KB\n', stat.expected/1024);
        fprintf(fid, '  Actual Size:   %.2f KB\n', stat.size/1024);
        fprintf(fid, '  Packets:       %d\n', stat.packets);
        fprintf(fid, '  Errors:        %d\n', stat.errors);
        fprintf(fid, '  Error Rate:    %.2f%%\n', errorRate);
        fprintf(fid, '\n');
    end

    fprintf(fid, '========================================\n');
    fprintf(fid, 'SUMMARY\n');
    fprintf(fid, '========================================\n');
    fprintf(fid, 'Complete files:   %d / %d\n', completeFiles, length(fileStats));
    fprintf(fid, 'Total packets:    %d\n', totalPackets);
    fprintf(fid, 'Total errors:     %d\n', totalErrors);
    if totalPackets > 0
        fprintf(fid, 'Overall error rate: %.2f%%\n', 100.0 * totalErrors / totalPackets);
    end
    fprintf(fid, '========================================\n');
    fclose(fid);
end

function config = parse_block_config(block_config)
    config = block_config;
    blockIdStr = getenv('BLOCK_ID');
    if isempty(blockIdStr)
        config.blockId = 0;
    else
        config.blockId = str2double(blockIdStr);
    end
    requiredFields = {'name', 'inputs', 'outputs', 'inputSize', 'outputSize'};
    for i = 1:length(requiredFields)
        if ~isfield(config, requiredFields{i})
            error('Missing required field: %s', requiredFields{i});
        end
    end
end