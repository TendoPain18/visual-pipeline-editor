function random_4bit(pipeOut)
% RANDOM_4BIT - Generates random 4-bit numbers with protocol
%
% @BlockConfig
% name: Random4Bit
% inputs: 0
% outputs: 1
% inputSize: 0
% outputSize: 8
% LTR: true
% startWithAll: true
% minValue: 0
% maxValue: 15
% totalDataGB: 5
% description: Generates random 4-bit numbers (0-15)
% @EndBlockConfig

    config = parse_block_config();
    
    % Send BLOCK_INIT
    send_protocol_message('BLOCK_INIT', config.blockId, config.name, '');
    
    fprintf('========================================\n');
    fprintf('%s - Starting\n', config.name);
    fprintf('========================================\n');
    fprintf('Pipe Out:    %s\n', pipeOut);
    fprintf('Range:       %d to %d\n', config.minValue, config.maxValue);
    fprintf('Total Data:  %.1f GB\n', config.totalDataGB);
    fprintf('========================================\n\n');
    
    try
        % Send BLOCK_READY
        send_protocol_message('BLOCK_READY', config.blockId, config.name, '');
        
        frameSize = config.outputSize;
        totalFrames = ceil((config.totalDataGB * 1e9) / frameSize);
        frameCount = 0;
        totalBytes = 0;
        startTime = tic;
        lastTime = 0;
        lastBytes = 0;
        
        fprintf('Will generate %d frames to reach %.1f GB\n\n', totalFrames, config.totalDataGB);
        
        for i = 1:totalFrames
            % Generate random 4-bit number
            randomValue = randi([config.minValue, config.maxValue], 1);
            
            % Convert to int8 array
            outputData = int8(zeros(8, 1));
            outputData(1) = int8(randomValue);
            
            % Write to pipe (blocks until pipe is ready)
            pipeline_mex('write', pipeOut, outputData);
            
            frameCount = frameCount + 1;
            totalBytes = totalBytes + length(outputData);
            
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
