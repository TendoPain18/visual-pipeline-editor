function file_source(pipeOut)
% FILE_SOURCE - Single-pass file transmission (INSTANCE-AWARE)

block_config = struct( ...
    'name',            'FileSource', ...
    'inputs',          0, ...
    'outputs',         1, ...
    'inputSize',       0, ...
    'outputSize',      1500, ...
    'LTR',             true, ...
    'startWithAll',    false, ...
    'socketHost',      'localhost', ...
    'socketPort',      9001, ...
    'sourceDirectory', 'C:\Users\amrga\Downloads\final\pipeline-editor\Test_Files', ...
    'loopMode',        false, ...
    'description',     'File source block - sends all files once then exits' ...
);

    config     = parse_block_config(block_config);
    PACKET_SIZE = 1500;

    fprintf('\n========================================\n');
    fprintf('FILE SOURCE - Instance-Aware Socket Mode\n');
    fprintf('========================================\n');

    % Get instance-specific MATLAB port from environment
    matlabPortStr = getenv('MATLAB_PORT');
    if ~isempty(matlabPortStr)
        matlabPort = str2double(matlabPortStr);
        fprintf('Instance MATLAB Port: %d\n', matlabPort);
    else
        matlabPort = config.socketPort;
        fprintf('Warning: Using default port: %d\n', matlabPort);
    end

    fprintf('Connecting to socket %s:%d...\n', config.socketHost, matlabPort);

    socketObj = matlab_socket_client(config.socketHost, matlabPort, 10);

    if isempty(socketObj)
        error('Failed to connect to socket server. Make sure Electron is running!');
    end

    fprintf('? Socket connected\n');
    fprintf('========================================\n\n');

    send_socket_message(socketObj, 'BLOCK_INIT', config.blockId, config.name, '');

    try
        if ~exist(config.sourceDirectory, 'dir')
            mkdir(config.sourceDirectory);
            fprintf('Created source directory. Add files and restart.\n');
            send_socket_message(socketObj, 'BLOCK_ERROR', config.blockId, config.name, 'No source directory');
            clear socketObj;
            return;
        end

        send_socket_message(socketObj, 'BLOCK_READY', config.blockId, config.name, '');

        packetCount = 0;
        totalBytes  = 0;
        startTime   = tic;
        lastTime    = 0;
        lastBytes   = 0;
        instantGbps = 0;

        fileList = dir(fullfile(config.sourceDirectory, '*.*'));
        fileList = fileList(~[fileList.isdir]);

        if isempty(fileList)
            fprintf('No files found in source directory.\n');
            send_socket_message(socketObj, 'BLOCK_ERROR', config.blockId, config.name, 'No files to send');
            clear socketObj;
            return;
        end

        fprintf('Found %d file(s) to send\n\n', length(fileList));

        for fileIdx = 1:length(fileList)
            fileName = fileList(fileIdx).name;
            filePath = fullfile(config.sourceDirectory, fileName);

            fprintf('Sending: %s\n', fileName);

            fid = fopen(filePath, 'rb');
            if fid == -1
                fprintf('ERROR: Cannot open: %s\n', fileName);
                continue;
            end
            fileData = fread(fid, '*uint8');
            fclose(fid);

            actualFileSize = length(fileData);

            % START marker
            startPacket = create_start_marker(PACKET_SIZE);
            pipeline_mex('write', pipeOut, startPacket);
            packetCount = packetCount + 1;
            totalBytes  = totalBytes + PACKET_SIZE;

            % Header
            headerPacket = create_file_header(fileName, actualFileSize, PACKET_SIZE);
            pipeline_mex('write', pipeOut, headerPacket);
            packetCount = packetCount + 1;
            totalBytes  = totalBytes + PACKET_SIZE;

            % Data packets
            numPackets = ceil(length(fileData) / PACKET_SIZE);
            for pktIdx = 1:numPackets
                startIdx = (pktIdx - 1) * PACKET_SIZE + 1;
                endIdx   = min(pktIdx * PACKET_SIZE, length(fileData));

                packet  = zeros(PACKET_SIZE, 1, 'int8');
                dataLen = endIdx - startIdx + 1;
                packet(1:dataLen) = int8(int32(fileData(startIdx:endIdx)) - 128);

                pipeline_mex('write', pipeOut, packet);

                packetCount = packetCount + 1;
                totalBytes  = totalBytes + PACKET_SIZE;

                currentTime = toc(startTime);
                elapsed = currentTime - lastTime;
                if elapsed > 0
                    instantGbps = ((totalBytes - lastBytes) * 8 / 1e9) / elapsed;
                end
                lastTime  = currentTime;
                lastBytes = totalBytes;

                metrics        = struct();
                metrics.frames = packetCount;
                metrics.gbps   = instantGbps;
                metrics.totalGB = totalBytes / 1e9;
                send_socket_message(socketObj, 'BLOCK_METRICS', config.blockId, config.name, metrics);
            end

            % END marker
            endPacket = create_end_marker(PACKET_SIZE);
            pipeline_mex('write', pipeOut, endPacket);
            packetCount = packetCount + 1;
            totalBytes  = totalBytes + PACKET_SIZE;

            metrics        = struct();
            metrics.frames = packetCount;
            metrics.gbps   = instantGbps;
            metrics.totalGB = totalBytes / 1e9;
            send_socket_message(socketObj, 'BLOCK_METRICS', config.blockId, config.name, metrics);

            fprintf('  Sent: %d packets (%.2f KB)\n', numPackets, actualFileSize/1024);
        end

        fprintf('\n========================================\n');
        fprintf('TRANSMISSION COMPLETE\n');
        fprintf('========================================\n');
        fprintf('Total files sent: %d\n', length(fileList));
        fprintf('Total packets:    %d\n', packetCount);
        fprintf('Total data:       %.2f MB\n', totalBytes/1e6);
        fprintf('========================================\n');

        finalMetrics        = struct();
        finalMetrics.frames = packetCount;
        finalMetrics.gbps   = 0;
        finalMetrics.totalGB = totalBytes / 1e9;
        send_socket_message(socketObj, 'BLOCK_METRICS', config.blockId, config.name, finalMetrics);

        fprintf('\nWaiting 5 seconds before exit...\n');
        pause(5);
        fprintf('Exiting.\n');

        send_socket_message(socketObj, 'BLOCK_STOPPED', config.blockId, config.name, '');
        clear socketObj;

    catch ME
        send_socket_message(socketObj, 'BLOCK_ERROR', config.blockId, config.name, ME.message);
        clear socketObj;
        rethrow(ME);
    end
end

function headerPacket = create_file_header(fileName, fileSize, PACKET_SIZE)
    headerPacket = zeros(PACKET_SIZE, 1, 'int8');
    headerPacket(1:4) = int8(int32([0x46, 0x49, 0x4C, 0x45]) - 128);
    fileNameBytes = uint8(fileName);
    nameLen = length(fileNameBytes);
    headerPacket(5) = int8(int32(bitand(nameLen, 0xFF)) - 128);
    headerPacket(6) = int8(int32(bitshift(nameLen, -8)) - 128);
    for i = 1:nameLen
        headerPacket(6 + i) = int8(int32(fileNameBytes(i)) - 128);
    end
    sizePos    = 7 + nameLen;
    fileSize64 = uint64(fileSize);
    for i = 0:7
        headerPacket(sizePos + i) = int8(int32(bitand(bitshift(fileSize64, -8*i), uint64(0xFF))) - 128);
    end
end

function startPacket = create_start_marker(PACKET_SIZE)
    startPacket = zeros(PACKET_SIZE, 1, 'int8');
    startPacket(1:4) = int8(int32([0x53, 0x54, 0x41, 0x52]) - 128);
end

function endPacket = create_end_marker(PACKET_SIZE)
    endPacket = zeros(PACKET_SIZE, 1, 'int8');
    endPacket(1:4) = int8(int32([0x45, 0x4E, 0x44, 0x00]) - 128);
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