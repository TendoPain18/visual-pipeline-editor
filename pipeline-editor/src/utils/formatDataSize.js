// Utility function to format data sizes with proper units (B, KB, MB, GB, TB)
export const formatDataSize = (bytes) => {
  if (bytes < 1024) {
    return `${bytes.toFixed(0)} B`;
  } else if (bytes < 1024 * 1024) {
    const kb = bytes / 1024;
    return `${kb.toFixed(2)} KB`;
  } else if (bytes < 1024 * 1024 * 1024) {
    const mb = bytes / (1024 * 1024);
    return `${mb.toFixed(2)} MB`;
  } else if (bytes < 1024 * 1024 * 1024 * 1024) {
    const gb = bytes / (1024 * 1024 * 1024);
    return `${gb.toFixed(2)} GB`;
  } else {
    const tb = bytes / (1024 * 1024 * 1024 * 1024);
    return `${tb.toFixed(2)} TB`;
  }
};