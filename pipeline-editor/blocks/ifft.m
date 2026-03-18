function ifft_block(pipeInOfdmFreq, pipeOutOfdmTime)
% IFFT_BLOCK - Performs 64-point IFFT on OFDM symbols with cyclic prefix
%
% Input 1: OFDM frequency-domain symbols (256 bytes = 64 complex subcarriers)
% Output 1: OFDM time-domain samples (4 bytes per sample, sent one-by-one)
%
% Each complex number format: 4 bytes
%   Bytes 0-1: I component (int16, little-endian, scaled by 32767)
%   Bytes 2-3: Q component (int16, little-endian, scaled by 32767)
%
% Operation:
% 1. Read 64 complex subcarriers (frequency domain)
% 2. Perform 64-point IFFT
% 3. Add cyclic prefix: copy last 16 samples to beginning
% 4. Output 80 samples ONE BY ONE (16 CP + 64 data)
%
% Cyclic Prefix (CP): 25% = 16 samples
%   IEEE 802.11a uses T_GI = 0.8 μs guard interval (16 samples at 20 MHz)
%
% Note: MATLAB IFFT uses subcarrier ordering [0, 1, 2, ..., 31, -32, -31, ..., -1]
%       Input uses ordering [-32, -31, ..., -1, 0, 1, ..., 31]
%       So we need to perform fftshift before IFFT
%
% @BlockConfig
% name: IFFT
% inputs: 1
% outputs: 1
% inputSize: 256
% outputSize: 4
% LTR: true
% startWithAll: true
% description: 64-point IFFT with 25% cyclic prefix, outputs samples one-by-one
% @EndBlockConfig

    config = parse_block_config();
    send_protocol_message('BLOCK_INIT', config.blockId, config.name, '');

    try
        send_protocol_message('BLOCK_READY', config.blockId, config.name, '');
        
        frameCount = 0;
        totalSymbols = 0;
        startTime = tic;
        lastTime = 0;
        lastSymbols = 0;
        
        while true
            frameCount = frameCount + 1;
            
            % Read OFDM symbol (64 complex subcarriers in frequency domain)
            ofdmSymbolInput = pipeline_mex('read', pipeInOfdmFreq, config.inputSize);
            
            % Extract 64 complex subcarriers
            subcarriers = zeros(1, 64);
            for i = 1:64
                baseIdx = (i - 1) * 4;
                
                % Convert int8 to uint8
                bytes = uint8(int16(ofdmSymbolInput(baseIdx+1:baseIdx+4)) + 128);
                
                % Reconstruct int16 values using typecast
                I_val = double(typecast(uint8([bytes(1), bytes(2)]), 'int16'));
                Q_val = double(typecast(uint8([bytes(3), bytes(4)]), 'int16'));
                
                % Convert back to complex (descale from int16 range)
                subcarriers(i) = (I_val + 1i * Q_val) / 32767.0;
            end
            
            % DEBUG: Print on first frame
            if frameCount == 1
                fprintf('DEBUG IFFT Frame 1:\n');
                fprintf('  Input subcarrier format: [-32...-1, 0, 1...31] (indices 1-64)\n');
                fprintf('  First subcarrier (idx=-32): %.4f + %.4fi\n', real(subcarriers(1)), imag(subcarriers(1)));
                fprintf('  DC subcarrier (idx=0): %.4f + %.4fi\n', real(subcarriers(33)), imag(subcarriers(33)));
                fprintf('  Last subcarrier (idx=31): %.4f + %.4fi\n', real(subcarriers(64)), imag(subcarriers(64)));
            end
            
            % Reorder subcarriers for MATLAB IFFT
            % Input order: [-32, -31, ..., -1, 0, 1, ..., 31] (indices 1-64)
            % MATLAB IFFT expects: [0, 1, ..., 31, -32, -31, ..., -1]
            % This is accomplished by ifftshift
            subcarriers_reordered = ifftshift(subcarriers);
            
            % Perform 64-point IFFT
            timeDomain = ifft(subcarriers_reordered, 64);
            
            % Add cyclic prefix: copy last 16 samples to beginning
            % CP_length = 16 (25% of 64)
            % Output: [samples(49:64), samples(1:64)] = 80 samples total
            timeDomainWithCP = [timeDomain(49:64), timeDomain];
            
            % DEBUG: Print on first frame
            if frameCount == 1
                fprintf('  After IFFT (64 samples):\n');
                fprintf('  First time sample: %.4f + %.4fi\n', real(timeDomain(1)), imag(timeDomain(1)));
                fprintf('  Last time sample: %.4f + %.4fi\n', real(timeDomain(64)), imag(timeDomain(64)));
                fprintf('  Peak magnitude: %.4f\n', max(abs(timeDomain)));
                fprintf('  Average power: %.4f\n', mean(abs(timeDomain).^2));
                fprintf('  After adding CP (80 samples):\n');
                fprintf('  CP first sample (copy of sample 49): %.4f + %.4fi\n', real(timeDomainWithCP(1)), imag(timeDomainWithCP(1)));
                fprintf('  CP last sample (copy of sample 64): %.4f + %.4fi\n', real(timeDomainWithCP(16)), imag(timeDomainWithCP(16)));
                fprintf('  Sending 80 samples one-by-one...\n');
            end
            
            % Send each sample one-by-one (80 samples total)
            for i = 1:80
                sample = timeDomainWithCP(i);
                
                % Scale to int16 range
                I_val = int16(round(real(sample) * 32767));
                Q_val = int16(round(imag(sample) * 32767));
                
                % Convert to bytes using typecast (little-endian)
                I_bytes = typecast(I_val, 'uint8');
                Q_bytes = typecast(Q_val, 'uint8');
                
                % Pack as int8 range [-128, 127]
                sampleOutput = zeros(4, 1, 'int8');
                sampleOutput(1) = int8(int16(I_bytes(1)) - 128);
                sampleOutput(2) = int8(int16(I_bytes(2)) - 128);
                sampleOutput(3) = int8(int16(Q_bytes(1)) - 128);
                sampleOutput(4) = int8(int16(Q_bytes(2)) - 128);
                
                % Write single sample
                pipeline_mex('write', pipeOutOfdmTime, sampleOutput);
                totalSymbols = totalSymbols + 1;
            end
            
            % Metrics
            currentTime = toc(startTime);
            elapsed = currentTime - lastTime;
            if elapsed > 0
                instantRate = (totalSymbols - lastSymbols) / elapsed;
            else
                instantRate = 0;
            end
            lastTime = currentTime;
            lastSymbols = totalSymbols;
            
            metrics = struct();
            metrics.frames = frameCount;
            metrics.gbps = instantRate / 1e6;
            send_protocol_message('BLOCK_METRICS', config.blockId, config.name, metrics);
        end
        
    catch ME
        fprintf('\nIFFT BLOCK FATAL ERROR in frame %d\n', frameCount);
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