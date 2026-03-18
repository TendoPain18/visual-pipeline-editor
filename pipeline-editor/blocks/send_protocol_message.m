function send_protocol_message(msgType, blockId, blockName, data)
% SEND_PROTOCOL_MESSAGE - Send standardized protocol message
%
% Usage:
%   send_protocol_message('BLOCK_INIT', blockId, blockName, '')
%   send_protocol_message('BLOCK_READY', blockId, blockName, '')
%   send_protocol_message('BLOCK_ERROR', blockId, blockName, errorMsg)
%   send_protocol_message('BLOCK_METRICS', blockId, blockName, metricsStruct)
%   send_protocol_message('BLOCK_GRAPH', blockId, blockName, graphStruct)
%   send_protocol_message('BLOCK_STOPPING', blockId, blockName, '')
%   send_protocol_message('BLOCK_STOPPED', blockId, blockName, '')

    timestamp = datestr(now, 'HH:MM:SS.FFF');
    
    % Format data based on message type
    switch msgType
        case 'BLOCK_INIT'
            dataStr = blockName;
        case 'BLOCK_READY'
            dataStr = blockName;
        case 'BLOCK_ERROR'
            dataStr = data;
        case 'BLOCK_METRICS'
            % data should be struct with: frames, gbps, totalGB (optional), totalFrames (optional)
            % Convert gbps to bps for better precision with small values
            bps = data.gbps * 1e9;
            
            if isfield(data, 'totalFrames') && isfield(data, 'totalGB')
                dataStr = sprintf('%d|%d|%.0f|%.2f', data.frames, data.totalFrames, bps, data.totalGB);
            elseif isfield(data, 'totalFrames')
                dataStr = sprintf('%d|%d|%.0f', data.frames, data.totalFrames, bps);
            elseif isfield(data, 'totalGB')
                dataStr = sprintf('%d|%.0f|%.2f', data.frames, bps, data.totalGB);
            else
                dataStr = sprintf('%d|%.0f', data.frames, bps);
            end
        case 'BLOCK_GRAPH'
            % data should be struct with: x, y
            dataStr = sprintf('%.2f,%.2f', data.x, data.y);
        case 'BLOCK_STOPPING'
            dataStr = blockName;
        case 'BLOCK_STOPPED'
            dataStr = blockName;
        otherwise
            error('Unknown message type: %s', msgType);
    end
    
    % Construct protocol message
    msg = sprintf('[PROTOCOL_V1][%s][%d][%s][%s]', timestamp, blockId, msgType, dataStr);
    
    % Output to stdout (captured by Electron)
    fprintf('%s\n', msg);
end
