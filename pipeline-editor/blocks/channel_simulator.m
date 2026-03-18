function channel_simulator(pipeIn, pipeOut)
% CHANNEL_SIMULATOR - Simple Channel with Random Attenuation and Phase Shift
%
% Simulates a wireless channel with:
% - Random attenuation per symbol (magnitude scaling)
% - Random phase shift per symbol
% - Additive white Gaussian noise (SNR = 30 dB)
%
% Channel model:
%   y(t) = g * e^(j*theta) * x(t) + noise
%   where g = random attenuation (0.5 to 1.0)
%         theta = random phase (0 to 2*pi)
%
% Input:  Time-domain samples from IFFT (4 bytes per sample)
% Output: Channel-affected samples to FFT (4 bytes per sample)
%
% Sample format: 4 bytes
%   Bytes 0-1: I component (int16, little-endian, scaled by 32767)
%   Bytes 2-3: Q component (int16, little-endian, scaled by 32767)
%
% @BlockConfig
% name: ChannelSimulator
% inputs: 1
% outputs: 1
% inputSize: 4
% outputSize: 4
% LTR: true
% startWithAll: true
% snr_dB: 30
% attenMin: 0.5
% attenMax: 1.0
% phaseMax: 0.524
% description: Simple channel with random attenuation and small phase per symbol
% @EndBlockConfig

    config = parse_block_config();
    send_protocol_message('BLOCK_INIT', config.blockId, config.name, '');

    try
        % Channel parameters
        SNR_dB = 30;      % Signal-to-noise ratio in dB (low noise)
        
        % Attenuation range (0.5 to 1.0 means 50% to 100% of original amplitude)
        ATTEN_MIN = 0.5;
        ATTEN_MAX = 1.0;
        
        % Phase shift range (small random phase perturbation)
        PHASE_MAX = pi/12;  % ±30 degrees maximum phase shift
        
        % Display initial channel state
        fprintf('========================================\n');
        fprintf('Simple Channel Simulator Initialized\n');
        fprintf('========================================\n');
        fprintf('SNR: %.1f dB\n', SNR_dB);
        fprintf('Attenuation range: %.2f to %.2f\n', ATTEN_MIN, ATTEN_MAX);
        fprintf('Phase shift: random per symbol (±%.1f degrees)\n', PHASE_MAX * 180 / pi);
        fprintf('========================================\n\n');
        
        % Noise power calculation
        % SNR = Signal_Power / Noise_Power
        % Assuming unit signal power after normalization
        noise_power = 10^(-SNR_dB / 10);
        noise_std = sqrt(noise_power / 2);  % Per I and Q component
        
        send_protocol_message('BLOCK_READY', config.blockId, config.name, '');
        
        frameCount = 0;
        totalBytes = 0;
        startTime = tic;
        lastTime = 0;
        lastBytes = 0;
        
        while true
            frameCount = frameCount + 1;
            
            % Read input sample (4 bytes)
            sampleInput = pipeline_mex('read', pipeIn, config.inputSize);
            
            % Convert int8 to complex sample
            bytes = uint8(int16(sampleInput) + 128);
            I_val = double(typecast(uint8([bytes(1), bytes(2)]), 'int16'));
            Q_val = double(typecast(uint8([bytes(3), bytes(4)]), 'int16'));
            x_current = (I_val + 1j * Q_val) / 32767.0;
            
            % Generate random attenuation (uniform distribution)
            g = ATTEN_MIN + (ATTEN_MAX - ATTEN_MIN) * rand();
            
            % Generate small random phase shift (uniform -PHASE_MAX to +PHASE_MAX)
            theta = (2 * rand() - 1) * PHASE_MAX;
            
            % Apply channel: y = g * e^(j*theta) * x
            h = exp(1j * theta);
            y = x_current .* h;
            
            % Add AWGN (complex Gaussian noise)
            noise_I = randn() * noise_std;
            noise_Q = randn() * noise_std;
            noise = noise_I + 1j * noise_Q;
            y = y;
            
            % Convert output back to int8 format
            I_out = int16(round(real(y) * 32767));
            Q_out = int16(round(imag(y) * 32767));
            
            I_bytes = typecast(I_out, 'uint8');
            Q_bytes = typecast(Q_out, 'uint8');
            
            sampleOutput = zeros(4, 1, 'int8');
            sampleOutput(1) = int8(int16(I_bytes(1)) - 128);
            sampleOutput(2) = int8(int16(I_bytes(2)) - 128);
            sampleOutput(3) = int8(int16(Q_bytes(1)) - 128);
            sampleOutput(4) = int8(int16(Q_bytes(2)) - 128);
            
            % Write output sample
            pipeline_mex('write', pipeOut, sampleOutput);
            
            totalBytes = totalBytes + 4;
            
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
            metrics.frames = frameCount;
            metrics.gbps = instantGbps;
            send_protocol_message('BLOCK_METRICS', config.blockId, config.name, metrics);
            
            % Debug output every 10000 samples
            if mod(frameCount, 10000) == 0
                fprintf('Sample %d: |x|=%.4f, |h|=%.4f (g=%.3f, theta=%.2f), |y|=%.4f, SNR_inst=%.2f dB\n', ...
                        frameCount, abs(x_current), abs(h), g, theta, abs(y), ...
                        10*log10(abs(y - noise)^2 / abs(noise)^2));
            end
        end
        
    catch ME
        fprintf('\nCHANNEL SIMULATOR FATAL ERROR at sample %d\n', frameCount);
        fprintf('Message: %s\n', ME.message);
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
    if isempty(startIdx) || isempty(endIdx), error('No @BlockConfig section found'); end
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
        if ~isempty(commentIdx), value = strtrim(value(1:commentIdx(1)-1)); end
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
        if ~isfield(config, requiredFields{i}), error('Missing required field: %s', requiredFields{i}); end
    end
end