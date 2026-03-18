function send_protocol_message(msgType, blockId, blockName, data)
% SEND_PROTOCOL_MESSAGE - Send standardized protocol message
    timestamp = datestr(now, 'HH:MM:SS.FFF');
    switch msgType
        case 'BLOCK_INIT'
            dataStr = blockName;
        case 'BLOCK_READY'
            dataStr = blockName;
        case 'BLOCK_ERROR'
            dataStr = data;
        case 'BLOCK_METRICS'
            if isfield(data, 'totalFrames') && isfield(data, 'totalGB')
                dataStr = sprintf('%d|%d|%.0f|%.2f', data.frames, data.totalFrames, data.gbps * 1e9, data.totalGB);
            elseif isfield(data, 'totalFrames')
                dataStr = sprintf('%d|%d|%.0f', data.frames, data.totalFrames, data.gbps * 1e9);
            elseif isfield(data, 'totalGB')
                dataStr = sprintf('%d|%.0f|%.2f', data.frames, data.gbps * 1e9, data.totalGB);
            else
                dataStr = sprintf('%d|%.0f', data.frames, data.gbps * 1e9);
            end
        case 'BLOCK_GRAPH'
            dataStr = sprintf('%.2f,%.2f', data.x, data.y);
        case 'BLOCK_STOPPING'
            dataStr = blockName;
        case 'BLOCK_STOPPED'
            dataStr = blockName;
        otherwise
            error('Unknown message type: %s', msgType);
    end
    msg = sprintf('[PROTOCOL_V1][%s][%d][%s][%s]', timestamp, blockId, msgType, dataStr);
    fprintf('%s\n', msg);
end