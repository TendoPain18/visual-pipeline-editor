function crc_encode(pipeIn, pipeOut)
% CRC_ENCODE - ITU-T CRC-32 encoder using generic block framework

    % ========== USER CONFIGURATION ==========
    block_config = struct( ...
        'name',              'CrcEncode', ...
        'inputs',            1, ...
        'outputs',           1, ...
        'inputPacketSizes',  1500, ...
        'inputBatchSizes',   44740, ...
        'outputPacketSizes', 1504, ...
        'outputBatchSizes',  44740, ...
        'LTR',               true, ...
        'startWithAll',      true, ...
        'socketHost',        'localhost', ...
        'socketPort',        9001, ...
        'description',       'CRC-32 encoder (ITU-T V.42) - batch processing' ...
    );
    
    % ========== CUSTOM INITIALIZATION ==========
    init_fn = @(config) build_crc32_table();
    
    % ========== PROCESSING FUNCTION ==========
    process_fn = @(inputBatch, actualCount, crcTable, config) ...
        process_crc_encode(inputBatch, actualCount, crcTable, config);
    
    % ========== RUN GENERIC BLOCK ==========
    run_generic_block(pipeIn, pipeOut, block_config, process_fn, init_fn);
end

function [outputBatch, actualOutputCount] = process_crc_encode(inputBatch, actualCount, crcTable, config)
    INPUT_PACKET_SIZE = config.inputPacketSizes(1);
    OUTPUT_PACKET_SIZE = config.outputPacketSizes(1);
    OUTPUT_BATCH_SIZE = config.outputBatchSizes(1);
    
    % Pre-allocate output batch
    outputBatch = zeros(OUTPUT_BATCH_SIZE * OUTPUT_PACKET_SIZE, 1, 'int8');
    
    % Process each packet: add CRC
    for pktIdx = 1:actualCount
        % Extract input packet
        inputOffset = (pktIdx - 1) * INPUT_PACKET_SIZE;
        inputPacket = inputBatch(inputOffset + 1 : inputOffset + INPUT_PACKET_SIZE);
        
        % Calculate CRC on the int8 data by treating as uint8
        dataAsUint8 = typecast(inputPacket, 'uint8');
        crc32 = calculate_crc32(dataAsUint8, crcTable);
        
        % Create output packet (data + CRC)
        outputOffset = (pktIdx - 1) * OUTPUT_PACKET_SIZE;
        outputBatch(outputOffset + 1 : outputOffset + INPUT_PACKET_SIZE) = inputPacket;
        
        % Append CRC as 4 bytes (little-endian)
        crcBytes = typecast(uint32(crc32), 'uint8');
        crcInt8 = typecast(crcBytes, 'int8');
        outputBatch(outputOffset + INPUT_PACKET_SIZE + 1 : outputOffset + OUTPUT_PACKET_SIZE) = crcInt8;
    end
    
    actualOutputCount = actualCount;
end

function crcTable = build_crc32_table()
    poly = uint32(0xEDB88320);
    crcTable = zeros(256, 1, 'uint32');
    for i = 0:255
        crc = uint32(i);
        for j = 0:7
            if bitand(crc, uint32(1))
                crc = bitxor(bitshift(crc, -1), poly);
            else
                crc = bitshift(crc, -1);
            end
        end
        crcTable(i + 1) = crc;
    end
end

function crc = calculate_crc32(data, crcTable)
    crc = uint32(0xFFFFFFFF);
    for i = 1:length(data)
        tableIdx = bitxor(bitand(crc, uint32(0xFF)), uint32(data(i))) + 1;
        crc = bitxor(bitshift(crc, -8), crcTable(tableIdx));
    end
    crc = bitxor(crc, uint32(0xFFFFFFFF));
end