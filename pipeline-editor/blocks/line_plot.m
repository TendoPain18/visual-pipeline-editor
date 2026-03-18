function line_plot(pipeIn1, pipeIn2)
% LINE_PLOT - Real-time line plot with protocol
%
% @BlockConfig
% name: LinePlot
% inputs: 2
% outputs: 0
% inputSize: [8, 8]
% outputSize: 0
% LTR: true
% startWithAll: true
% graphType: line
% maxPoints: 500
% xLabel: Time (samples)
% yLabel: Speed (rad/s)
% title: Speed Control System
% description: Real-time line plot for speed tracking
% @EndBlockConfig

    config = parse_block_config();
    
    send_protocol_message('BLOCK_INIT', config.blockId, config.name, '');
    
    fprintf('========================================\n');
    fprintf('%s - Starting\n', config.name);
    fprintf('========================================\n');
    fprintf('Pipe In 1:   %s (Reference)\n', pipeIn1);
    fprintf('Pipe In 2:   %s (Measured)\n', pipeIn2);
    fprintf('Graph Type:  %s\n', config.graphType);
    fprintf('Max Points:  %d\n', config.maxPoints);
    fprintf('========================================\n\n');
    
    try
        send_protocol_message('BLOCK_READY', config.blockId, config.name, '');
        
        frameCount = 0;
        totalBytes = 0;
        startTime = tic;
        lastTime = 0;
        lastBytes = 0;
        
        while true
            % Read from both input pipes (8 bytes = double)
            input1 = pipeline_mex('read', pipeIn1, 8);
            input2 = pipeline_mex('read', pipeIn2, 8);
            
            % Extract values
            refSpeed = typecast(input1, 'double');
            measSpeed = typecast(input2, 'double');
            
            frameCount = frameCount + 1;
            totalBytes = totalBytes + length(input1) + length(input2);
            
            % Calculate instantaneous throughput
            currentTime = toc(startTime);
            elapsed = currentTime - lastTime;
            if elapsed > 0
                instantGbps = ((totalBytes - lastBytes) * 8 / 1e9) / elapsed;
            else
                instantGbps = 0;
            end
            lastTime = currentTime;
            lastBytes = totalBytes;
            
            % Send TWO graph data points (reference and measured)
            % Reference speed (series 1)
            graphData1 = struct('x', frameCount, 'y', refSpeed);
            send_protocol_message('BLOCK_GRAPH', config.blockId, config.name, graphData1);
            
            % Measured speed (series 2) - send with slight offset to distinguish
            graphData2 = struct('x', frameCount + 0.1, 'y', measSpeed);
            send_protocol_message('BLOCK_GRAPH', config.blockId, config.name, graphData2);
            
            % Send metrics
            metrics = struct();
            metrics.frames = frameCount;
            metrics.gbps = instantGbps;
            
            send_protocol_message('BLOCK_METRICS', config.blockId, config.name, metrics);
            
            if mod(frameCount, 100) == 0
                fprintf('Sample %d: Ref=%.2f, Meas=%.2f rad/s\n', frameCount, refSpeed, measSpeed);
            end
        end
        
    catch ME
        fprintf('\n\n%s: Stopped - %d frames processed\n', config.name, frameCount);
        send_protocol_message('BLOCK_ERROR', config.blockId, config.name, ME.message);
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
