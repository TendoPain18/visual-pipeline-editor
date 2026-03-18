function file_source(pipeOut)
% FILE_SOURCE - Single-pass file transmission
%
% @BlockConfig
% name: FileSource
% inputs: 0
% outputs: 1
% inputSize: 0
% outputSize: 1500
% LTR: true
% startWithAll: false
% sourceDirectory: C:\Users\amrga\Downloads\final\pipeline-editor\Test_Files
% loopMode: false
% description: File source block - sends all files once then exits
% @EndBlockConfig

    config = parse_block_config();
    PACKET_SIZE = 1500;
    
    send_protocol_message('BLOCK_INIT', config.blockId, config.name, '');

    try
        if ~exist(config.sourceDirectory, 'dir')
            mkdir(config.sourceDirectory);
            fprintf('Created source directory. Add files and restart.\n');
            send_protocol_message('BLOCK_ERROR', config.blockId, config.name, 'No source directory');
            return;
        end
        
        send_protocol_message('BLOCK_READY', config.blockId, config.name, '');
        
        packetCount = 0;
        totalBytes = 0;
        startTime = tic;
        lastTime = 0;
        lastBytes = 0;
        
        % Get file list
        fileList = dir(fullfile(config.sourceDirectory, '*.*'));
        fileList = fileList(~[fileList.isdir]);
        
        if isempty(fileList)
            fprintf('No files found in source directory.\n');
            send_protocol_message('BLOCK_ERROR', config.blockId, config.name, 'No files to send');
            return;
        end
        
        fprintf('Found %d file(s) to send\n\n', length(fileList));
        
        % Process each file once
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
            
            % Send START marker
            startPacket = create_start_marker();
            pipeline_mex('write', pipeOut, startPacket);
            packetCount = packetCount + 1;
            totalBytes = totalBytes + PACKET_SIZE;
            
            % Send header
            headerPacket = create_file_header(fileName, actualFileSize);
            pipeline_mex('write', pipeOut, headerPacket);
            packetCount = packetCount + 1;
            totalBytes = totalBytes + PACKET_SIZE;
            
            % Send data packets
            numPackets = ceil(length(fileData) / PACKET_SIZE);
            for pktIdx = 1:numPackets
                startIdx = (pktIdx - 1) * PACKET_SIZE + 1;
                endIdx = min(pktIdx * PACKET_SIZE, length(fileData));
                
                packet = zeros(PACKET_SIZE, 1, 'int8');
                dataLen = endIdx - startIdx + 1;
                packet(1:dataLen) = int8(int32(fileData(startIdx:endIdx)) - 128);
                
                pipeline_mex('write', pipeOut, packet);
                
                packetCount = packetCount + 1;
                totalBytes = totalBytes + PACKET_SIZE;
                
                % Metrics
                currentTime = toc(startTime);
                elapsed = currentTime - lastTime;
                if elapsed > 0
                    instantGbps = ((totalBytes - lastBytes) * 8 / 1e9) / elapsed;
                else
                    instantGbps = 0;
                end
                lastTime = currentTime;
                lastBytes = totalBytes;
                
                metrics = struct();
                metrics.frames = packetCount;
                metrics.gbps = instantGbps;
                metrics.totalGB = totalBytes / 1e9;
                send_protocol_message('BLOCK_METRICS', config.blockId, config.name, metrics);
            end
            
            % Send END marker after each file
            endPacket = create_end_marker();
            pipeline_mex('write', pipeOut, endPacket);
            packetCount = packetCount + 1;
            totalBytes = totalBytes + PACKET_SIZE;
            
            % Update metrics after END marker
            metrics = struct();
            metrics.frames = packetCount;
            metrics.gbps = instantGbps;
            metrics.totalGB = totalBytes / 1e9;
            send_protocol_message('BLOCK_METRICS', config.blockId, config.name, metrics);
            
            fprintf('  Sent: %d packets (%.2f KB)\n', numPackets, actualFileSize/1024);
        end
        
        fprintf('\n========================================\n');
        fprintf('TRANSMISSION COMPLETE\n');
        fprintf('========================================\n');
        fprintf('Total files sent: %d\n', length(fileList));
        fprintf('Total packets:    %d\n', packetCount);
        fprintf('Total data:       %.2f MB\n', totalBytes/1e6);
        fprintf('========================================\n');
        
        % Wait 5 seconds before closing
        fprintf('\nWaiting 5 seconds before exit...\n');
        pause(5);
        fprintf('Exiting.\n');
        
    catch ME
        send_protocol_message('BLOCK_ERROR', config.blockId, config.name, ME.message);
        rethrow(ME);
    end
end

function headerPacket = create_file_header(fileName, fileSize)
    PACKET_SIZE = 1500;
    headerPacket = zeros(PACKET_SIZE, 1, 'int8');
    headerPacket(1:4) = int8(int32([0x46, 0x49, 0x4C, 0x45]) - 128);
    fileNameBytes = uint8(fileName);
    nameLen = length(fileNameBytes);
    headerPacket(5) = int8(int32(bitand(nameLen, 0xFF)) - 128);
    headerPacket(6) = int8(int32(bitshift(nameLen, -8)) - 128);
    for i = 1:nameLen
        headerPacket(6 + i) = int8(int32(fileNameBytes(i)) - 128);
    end
    sizePos = 7 + nameLen;
    fileSize64 = uint64(fileSize);
    for i = 0:7
        headerPacket(sizePos + i) = int8(int32(bitand(bitshift(fileSize64, -8*i), uint64(0xFF))) - 128);
    end
end

function startPacket = create_start_marker()
    PACKET_SIZE = 1500;
    startPacket = zeros(PACKET_SIZE, 1, 'int8');
    % START marker: 0x53 0x54 0x41 0x52 ("STAR")
    startPacket(1:4) = int8(int32([0x53, 0x54, 0x41, 0x52]) - 128);
end

function endPacket = create_end_marker()
    PACKET_SIZE = 1500;
    endPacket = zeros(PACKET_SIZE, 1, 'int8');
    % END marker: 0x45 0x4E 0x44 0x00 ("END\0")
    endPacket(1:4) = int8(int32([0x45, 0x4E, 0x44, 0x00]) - 128);
end

function config = parse_block_config()
    filePath = mfilename('fullpath');
    fid = fopen([filePath '.m'], 'r');
    if fid == -1, error('Cannot open configuration file'); end
    content = fread(fid, '*char')';
    fclose(fid);
    startMarker = '@BlockConfig';
    endMarker = '@EndBlockConfig';
    startIdx = strfind(content, startMarker);
    endIdx = strfind(content, endMarker);
    if isempty(startIdx) || isempty(endIdx)
        error('No @BlockConfig section found');
    end
    configStart = startIdx(1) + length(startMarker);
    configEnd = endIdx(1) - 1;
    configText = content(configStart:configEnd);
    config = struct();
    lines = strsplit(configText, newline);
    for i = 1:length(lines)
        line = strtrim(lines{i});
        if isempty(line), continue; end
        if line(1) == '%', line = strtrim(line(2:end)); end
        if isempty(line), continue; end
        colonIdx = strfind(line, ':');
        if isempty(colonIdx), continue; end
        key = strtrim(line(1:colonIdx(1)-1));
        value = strtrim(line(colonIdx(1)+1:end));
        commentIdx = strfind(value, '%');
        if ~isempty(commentIdx)
            value = strtrim(value(1:commentIdx(1)-1));
        end
        if isempty(key), continue; end
        try
            numValue = eval(value);
            config.(key) = numValue;
        catch
            config.(key) = value;
        end
    end
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