function qam64_constellation(pipeOut1, pipeOut2)
% QAM64_CONSTELLATION - Generates 64-QAM constellation points with noise
%
% @BlockConfig
% name: QAM64Constellation
% inputs: 0
% outputs: 2
% inputSize: 0
% outputSize: [8, 8]
% LTR: true
% startWithAll: true
% totalDataGB: 5
% description: Generates 64-QAM constellation (I and Q with noise)
% @EndBlockConfig

    config = parse_block_config();
    
    % Send BLOCK_INIT
    send_protocol_message('BLOCK_INIT', config.blockId, config.name, '');
    
    fprintf('========================================\n');
    fprintf('%s - Starting\n', config.name);
    fprintf('========================================\n');
    fprintf('Pipe Out 1:  %s (I channel)\n', pipeOut1);
    fprintf('Pipe Out 2:  %s (Q channel)\n', pipeOut2);
    fprintf('Total Data:  %.1f GB\n', config.totalDataGB);
    fprintf('Constellation: 64-QAM with noise\n');
    fprintf('========================================\n\n');
    
    try
        % Send BLOCK_READY
        send_protocol_message('BLOCK_READY', config.blockId, config.name, '');
        
        frameSize = 8; % Each output is 8 bytes
        totalFrames = ceil((config.totalDataGB * 1e9) / (frameSize * 2));
        frameCount = 0;
        totalBytes = 0;
        startTime = tic;
        lastTime = 0;
        lastBytes = 0;
        
        % 64-QAM constellation points: {-3, -1, 1, 3}
        constellation_points = [-3, -1, 1, 3];
        
        fprintf('Will generate %d frames to reach %.1f GB\n\n', totalFrames, config.totalDataGB);
        
        for i = 1:totalFrames
            % Generate I channel: random from {-3, -1, 1, 3} + noise [-0.5, 0.5]
            I_base = constellation_points(randi([1, 4]));
            I_noise = (rand() - 0.5);
            I_value = I_base + I_noise;
            
            % Generate Q channel: random from {-3, -1, 1, 3} + noise [-0.5, 0.5]
            Q_base = constellation_points(randi([1, 4]));
            Q_noise = (rand() - 0.5);
            Q_value = Q_base + Q_noise;
            
            % Convert to int8 arrays (scale and round for transmission)
            % Scale by 10 to preserve decimal precision
            I_scaled = int8(round(I_value * 10));
            Q_scaled = int8(round(Q_value * 10));
            
            outputData1 = int8(zeros(8, 1));
            outputData1(1) = I_scaled;
            
            outputData2 = int8(zeros(8, 1));
            outputData2(1) = Q_scaled;
            
            % Write to pipes (blocks until pipes are ready)
            pipeline_mex('write', pipeOut1, outputData1);
            pipeline_mex('write', pipeOut2, outputData2);
            
            frameCount = frameCount + 1;
            totalBytes = totalBytes + length(outputData1) + length(outputData2);
            
            % Calculate instantaneous throughput (per frame)
            currentTime = toc(startTime);
            elapsed = currentTime - lastTime;
            if elapsed > 0
                instantGbps = ((totalBytes - lastBytes) * 8 / 1e9) / elapsed;
            else
                instantGbps = 0;
            end
            lastTime = currentTime;
            lastBytes = totalBytes;
            
            % Send metrics every frame
            metrics = struct();
            metrics.frames = frameCount;
            metrics.totalFrames = totalFrames;
            metrics.gbps = instantGbps;
            metrics.totalGB = totalBytes / 1e9;
            
            send_protocol_message('BLOCK_METRICS', config.blockId, config.name, metrics);
        end
        
        fprintf('\n\n%s: Complete - %.2f GB transmitted\n', config.name, totalBytes/1e9);
        
        % Send final BLOCK_STOPPED message
        send_protocol_message('BLOCK_STOPPED', config.blockId, config.name, '');
        
        % Exit cleanly
        return;
        
    catch ME
        send_protocol_message('BLOCK_ERROR', config.blockId, config.name, ME.message);
        rethrow(ME);
    end
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
