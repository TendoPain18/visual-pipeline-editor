// Protocol message parser
export const parseProtocolMessage = (rawMessage) => {
  if (!rawMessage.includes('[PROTOCOL_V1]')) return null;
  const regex = /\[PROTOCOL_V1\]\[([^\]]+)\]\[([^\]]+)\]\[([^\]]+)\]\[([^\]]*)\]/;
  const match = rawMessage.match(regex);
  if (!match) return null;
  const [, timestamp, blockIdStr, msgType, data] = match;
  return { timestamp, blockId: parseInt(blockIdStr), msgType, data, raw: rawMessage };
};

export const parseMetricsData = (dataStr) => {
  const parts = dataStr.split('|');
  if (parts.length === 2) {
    return { frames: parseInt(parts[0]), gbps: parseFloat(parts[1]) / 1e9 };
  } else if (parts.length === 3) {
    const thirdValue = parseFloat(parts[2]);
    if (parts[2].includes('.') && thirdValue < 1000) {
      return { frames: parseInt(parts[0]), gbps: parseFloat(parts[1]) / 1e9, totalGB: parseFloat(parts[2]) };
    } else {
      return { frames: parseInt(parts[0]), totalFrames: parseInt(parts[1]), gbps: parseFloat(parts[2]) / 1e9 };
    }
  } else if (parts.length === 4) {
    return { frames: parseInt(parts[0]), totalFrames: parseInt(parts[1]), gbps: parseFloat(parts[2]) / 1e9, totalGB: parseFloat(parts[3]) };
  }
  return null;
};

export const parseGraphData = (dataStr) => {
  const parts = dataStr.split(',');
  if (parts.length === 2) return { x: parseFloat(parts[0]), y: parseFloat(parts[1]) };
  return null;
};
