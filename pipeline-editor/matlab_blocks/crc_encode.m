function crc_encode(pipeIn, pipeOut)
% CRC_ENCODE  ITU-T CRC-32 encoder — manual I/O version.

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

    run_generic_block({pipeIn}, {pipeOut}, block_config, @process_fn, @init_fn);
end

%% ─── Init ───────────────────────────────────────────────────────────────────
function crcTable = init_fn(~)
    crcTable = buildCrcTable();
    fprintf('[CrcEncode] CRC-32 table built\n');
end

%% ─── Process ────────────────────────────────────────────────────────────────
function process_fn(pipeIn, pipeOut, crcTable, ~)
    inp = pipeIn{1};
    out = pipeOut{1};

    IN_PKT  = inp.getPacketSize();   % 1500
    OUT_PKT = out.getPacketSize();   % 1504

    % ── Read ──────────────────────────────────────────────────────────────────
    [inputBatch, actualCount] = inp.read();

    % ── Process ───────────────────────────────────────────────────────────────
    outputBatch = zeros(out.getBatchSize() * OUT_PKT, 1, 'int8');

    for i = 1:actualCount
        inOff  = (i-1) * IN_PKT;
        outOff = (i-1) * OUT_PKT;

        pkt = inputBatch(inOff+1 : inOff+IN_PKT);

        % Copy data
        outputBatch(outOff+1 : outOff+IN_PKT) = pkt;

        % Calculate and append CRC
        dataU8 = uint8(int32(pkt) + 128);
        crc    = calcCrc32(dataU8, crcTable);

        crcBytes = typecast(uint32(crc), 'uint8');
        crcInt8  = int8(int32(crcBytes) - 128);
        outputBatch(outOff+IN_PKT+1 : outOff+OUT_PKT) = crcInt8(:);
    end

    % ── Write ─────────────────────────────────────────────────────────────────
    out.write(outputBatch, actualCount);
end

%% ─── CRC helpers ─────────────────────────────────────────────────────────────
function table = buildCrcTable()
    poly  = uint32(0xEDB88320);
    table = zeros(256,1,'uint32');
    for i = 0:255
        crc = uint32(i);
        for j = 0:7
            if bitand(crc,uint32(1))
                crc = bitxor(bitshift(crc,-1), poly);
            else
                crc = bitshift(crc,-1);
            end
        end
        table(i+1) = crc;
    end
end

function crc = calcCrc32(data, table)
    crc = uint32(0xFFFFFFFF);
    for i = 1:numel(data)
        idx = bitxor(bitand(crc,uint32(255)), uint32(data(i))) + 1;
        crc = bitxor(bitshift(crc,-8), table(idx));
    end
    crc = bitxor(crc, uint32(0xFFFFFFFF));
end
