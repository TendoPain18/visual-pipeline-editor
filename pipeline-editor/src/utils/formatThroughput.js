// Add this utility function to format throughput with proper units
export const formatThroughput = (bps) => {
  if (bps < 1000) {
    return `${bps.toFixed(0)} bps`;
  } else if (bps < 1000000) {
    const kbps = bps / 1000;
    return `${kbps.toFixed(1)} Kbps`;
  } else if (bps < 1000000000) {
    const mbps = bps / 1000000;
    return `${mbps.toFixed(1)} Mbps`;
  } else {
    const gbps = bps / 1000000000;
    return `${gbps.toFixed(1)} Gbps`;
  }
};
