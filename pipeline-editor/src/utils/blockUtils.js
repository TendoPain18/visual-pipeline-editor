export const GRID_SIZE = 20;
export const BLOCK_WIDTH = 140;
export const BLOCK_HEIGHT = 80;

/**
 * Parse a MATLAB block_config struct literal from file content.
 * Extracts configuration from struct(...) definition.
 */
const parseMatlabStruct = (fileContent) => {
  // Match block_config = struct( ... );  — spans multiple lines
  const structMatch = fileContent.match(
    /block_config\s*=\s*struct\s*\(([\s\S]*?)\)\s*;/
  );
  if (!structMatch) return null;

  const body = structMatch[1];

  // Strip MATLAB line-continuation ellipses and newlines so we have one flat string
  const flat = body.replace(/\.\.\.\s*\n/g, ' ').replace(/\n/g, ' ');

  const config = {};

  // Tokenise key-value pairs.
  // Each pair looks like:  'key', <value>
  // where <value> is one of:
  //   'string'  |  [n, n, ...]  |  number  |  true  |  false
  const pairRe = /'(\w+)'\s*,\s*('([^']*)'|\[([^\]]*)\]|(true|false)|([^\s,][^,]*))/g;
  let m;
  while ((m = pairRe.exec(flat)) !== null) {
    const key = m[1];
    const rawVal = m[2];

    if (rawVal.startsWith("'")) {
      // String
      config[key] = m[3];
    } else if (rawVal.startsWith('[')) {
      // Numeric array  [1500, 1504]
      try {
        config[key] = m[4].split(',').map(v => Number(v.trim()));
      } catch {
        config[key] = rawVal;
      }
    } else if (rawVal === 'true') {
      config[key] = true;
    } else if (rawVal === 'false') {
      config[key] = false;
    } else {
      // Number (decimal or hex)
      const num = rawVal.trim().startsWith('0x')
        ? parseInt(rawVal.trim(), 16)
        : Number(rawVal.trim());
      config[key] = isNaN(num) ? rawVal.trim() : num;
    }
  }

  return Object.keys(config).length > 0 ? config : null;
};

/**
 * Calculate the number of bytes needed to represent a count
 * (e.g., if batchSize is 10, we need 1 byte; if 300, we need 2 bytes)
 */
const calculateLengthBytes = (maxCount) => {
  if (maxCount <= 255) return 1;           // uint8
  if (maxCount <= 65535) return 2;         // uint16
  if (maxCount <= 16777215) return 3;      // uint24
  return 4;                                 // uint32
};

/**
 * Calculate total buffer size including length header and all packets
 */
const calculateBufferSize = (packetSize, batchSize) => {
  const lengthBytes = calculateLengthBytes(batchSize);
  return lengthBytes + (packetSize * batchSize);
};

/**
 * Parse a generic block that uses run_generic_block()
 */
const parseGenericBlock = (fileContent, fileName) => {
  const config = parseMatlabStruct(fileContent);

  if (!config) {
    throw new Error(
      'No block_config struct found.\n\n' +
      'Add a block_config = struct(...); definition to your MATLAB file.\n\n' +
      'Example:\n' +
      "block_config = struct( ...\n" +
      "    'name',              'MyBlock', ...\n" +
      "    'inputs',            1, ...\n" +
      "    'outputs',           1, ...\n" +
      "    'inputPacketSizes',  1500, ...\n" +
      "    'inputBatchSizes',   10, ...\n" +
      "    'outputPacketSizes', 1500, ...\n" +
      "    'outputBatchSizes',  10, ...\n" +
      "    'LTR',               true, ...\n" +
      "    'startWithAll',      true ...\n" +
      ");"
    );
  }

  const numInputs  = typeof config.inputs  === 'number' ? config.inputs  : 1;
  const numOutputs = typeof config.outputs === 'number' ? config.outputs : 1;

  // Parse packet sizes
  let inputPacketSizes = config.inputPacketSizes;
  let outputPacketSizes = config.outputPacketSizes;
  
  // Parse batch sizes
  let inputBatchSizes = config.inputBatchSizes;
  let outputBatchSizes = config.outputBatchSizes;
  
  // Ensure arrays
  inputPacketSizes = Array.isArray(inputPacketSizes) ? inputPacketSizes : [inputPacketSizes || 0];
  inputBatchSizes = Array.isArray(inputBatchSizes) ? inputBatchSizes : [inputBatchSizes || 1];
  outputPacketSizes = Array.isArray(outputPacketSizes) ? outputPacketSizes : [outputPacketSizes || 0];
  outputBatchSizes = Array.isArray(outputBatchSizes) ? outputBatchSizes : [outputBatchSizes || 1];
  
  // Validate array lengths match port counts
  if (numInputs > 0 && inputPacketSizes.length !== numInputs) {
    throw new Error(`inputPacketSizes array length (${inputPacketSizes.length}) must match inputs (${numInputs})`);
  }
  if (numInputs > 0 && inputBatchSizes.length !== numInputs) {
    throw new Error(`inputBatchSizes array length (${inputBatchSizes.length}) must match inputs (${numInputs})`);
  }
  if (numOutputs > 0 && outputPacketSizes.length !== numOutputs) {
    throw new Error(`outputPacketSizes array length (${outputPacketSizes.length}) must match outputs (${numOutputs})`);
  }
  if (numOutputs > 0 && outputBatchSizes.length !== numOutputs) {
    throw new Error(`outputBatchSizes array length (${outputBatchSizes.length}) must match outputs (${numOutputs})`);
  }
  
  // Calculate buffer sizes (with length header)
  const inputBufferSizes = inputPacketSizes.map((packetSize, i) => 
    calculateBufferSize(packetSize, inputBatchSizes[i])
  );
  
  const outputBufferSizes = outputPacketSizes.map((packetSize, i) => 
    calculateBufferSize(packetSize, outputBatchSizes[i])
  );
  
  // Calculate length header sizes
  const inputLengthBytes = inputBatchSizes.map(calculateLengthBytes);
  const outputLengthBytes = outputBatchSizes.map(calculateLengthBytes);
  
  // Total sizes for display
  const totalInputSize = inputBufferSizes.reduce((a, b) => a + b, 0);
  const totalOutputSize = outputBufferSizes.reduce((a, b) => a + b, 0);

  // Size relation (for display)
  let sizeRelation;
  if (totalInputSize === 0) {
    sizeRelation = { type: 'source', description: 'Data source' };
  } else if (totalOutputSize === 0) {
    sizeRelation = { type: 'sink', description: 'Data sink' };
  } else if (totalOutputSize === totalInputSize) {
    sizeRelation = { type: 'same', description: 'Same size' };
  } else if (totalOutputSize > totalInputSize) {
    const ratio = totalOutputSize / totalInputSize;
    sizeRelation = {
      type: 'multiply',
      factor: ratio,
      description: `×${ratio.toFixed(2)} (${config.description || 'increases size'})`
    };
  } else {
    const ratio = totalInputSize / totalOutputSize;
    sizeRelation = {
      type: 'divide',
      factor: ratio,
      description: `÷${ratio.toFixed(2)} (${config.description || 'decreases size'})`
    };
  }

  const ltr          = config.LTR          !== undefined ? config.LTR          : true;
  const startWithAll = config.startWithAll !== undefined ? config.startWithAll : false;
  const isGraph      = config.graphType    !== undefined;

  return {
    name:         config.name || fileName.replace('.m', ''),
    fileName,
    code:         fileContent,
    inputs:       numInputs,
    outputs:      numOutputs,
    
    // BATCH PROCESSING FIELDS
    inputPacketSizes,
    inputBatchSizes,
    outputPacketSizes,
    outputBatchSizes,
    inputBufferSizes,
    outputBufferSizes,
    inputLengthBytes,
    outputLengthBytes,
    
    // LEGACY FIELDS (for display)
    inputSize: totalInputSize,
    outputSize: totalOutputSize,
    inputSizes: inputBufferSizes,
    outputSizes: outputBufferSizes,
    
    description:  config.description || 'No description',
    config,
    color:        isGraph ? '#9333ea' : generateColor(config.name || fileName.replace('.m', '')),
    portPositions: calculatePortPositions(numInputs, numOutputs, ltr, BLOCK_WIDTH, BLOCK_HEIGHT, GRID_SIZE),
    sizeRelation,
    ltr,
    startWithAll,
    isGraph,
    graphType:    config.graphType || null
  };
};

/**
 * Main block parser - detects and parses generic blocks
 */
export const parseMatlabBlock = (fileContent, fileName) => {
  // Check if this is a generic block (looks for run_generic_block call)
  const isGenericBlock = fileContent.includes('run_generic_block');
  
  if (isGenericBlock) {
    return parseGenericBlock(fileContent, fileName);
  } else {
    throw new Error(
      'Only generic blocks are supported.\n\n' +
      'Please use the generic block framework with run_generic_block().\n\n' +
      'Example:\n' +
      'function my_block(pipeIn, pipeOut)\n' +
      '    block_config = struct(...);\n' +
      '    process_fn = @(input, count, data, cfg) your_processing(input, count, data, cfg);\n' +
      '    run_generic_block(pipeIn, pipeOut, block_config, process_fn);\n' +
      'end'
    );
  }
};

export const generateColor = (name) => {
  const colors = ['#3b82f6', '#8b5cf6', '#ec4899', '#f59e0b', '#6366f1'];
  let hash = 0;
  for (let i = 0; i < name.length; i++) {
    hash = name.charCodeAt(i) + ((hash << 5) - hash);
  }
  return colors[Math.abs(hash) % colors.length];
};

export const calculatePortPositions = (numInputs, numOutputs, ltr, BLOCK_WIDTH, BLOCK_HEIGHT, GRID_SIZE) => {
  const inputPorts  = [];
  const outputPorts = [];

  const calculatePorts = (numPorts) => {
    const ports   = [];
    const centerY = BLOCK_HEIGHT / 2;

    if (numPorts === 1) {
      ports.push(centerY);
    } else if (numPorts === 2) {
      ports.push(centerY - GRID_SIZE);
      ports.push(centerY + GRID_SIZE);
    } else if (numPorts === 3) {
      ports.push(centerY - GRID_SIZE);
      ports.push(centerY);
      ports.push(centerY + GRID_SIZE);
    } else {
      const spacing     = GRID_SIZE;
      const totalHeight = (numPorts - 1) * spacing;
      const startY      = centerY - totalHeight / 2;
      for (let i = 0; i < numPorts; i++) {
        ports.push(startY + i * spacing);
      }
    }
    return ports;
  };

  const inputYPositions  = calculatePorts(numInputs);
  const outputYPositions = calculatePorts(numOutputs);

  const inputX  = ltr ? 0         : BLOCK_WIDTH;
  const outputX = ltr ? BLOCK_WIDTH : 0;

  for (let i = 0; i < numInputs;  i++) inputPorts.push( { x: inputX,  y: inputYPositions[i],  type: 'input',  index: i });
  for (let i = 0; i < numOutputs; i++) outputPorts.push({ x: outputX, y: outputYPositions[i], type: 'output', index: i });

  return { inputs: inputPorts, outputs: outputPorts };
};

export const topologicalSort = (blocks, connections) => {
  const sources = blocks.filter(b => b.inputs === 0);
  if (sources.length === 0) throw new Error('No source blocks found');

  console.log(`[TopologicalSort] Found ${sources.length} source block(s):`, sources.map(s => s.name));

  const feedbackConnections = new Set();
  const forwardMap = new Map();
  blocks.forEach(block => forwardMap.set(block.id, []));
  connections.forEach(conn => {
    if (forwardMap.has(conn.fromBlock))
      forwardMap.get(conn.fromBlock).push({ blockId: conn.toBlock, connId: conn.id });
  });

  const visited  = new Set();
  const recStack = new Set();

  const detectCycle = (nodeId) => {
    visited.add(nodeId);
    recStack.add(nodeId);
    for (const { blockId: neighborId, connId } of (forwardMap.get(nodeId) || [])) {
      if (!visited.has(neighborId)) {
        if (detectCycle(neighborId)) return true;
      } else if (recStack.has(neighborId)) {
        feedbackConnections.add(connId);
        return true;
      }
    }
    recStack.delete(nodeId);
    return false;
  };

  sources.forEach(source => { if (!visited.has(source.id)) detectCycle(source.id); });

  const adjacencyMap = new Map();
  blocks.forEach(block => adjacencyMap.set(block.id, []));
  connections.forEach(conn => {
    if (!feedbackConnections.has(conn.id) && adjacencyMap.has(conn.fromBlock))
      adjacencyMap.get(conn.fromBlock).push(conn.toBlock);
  });

  const sorted       = [...sources];
  const visitedNodes = new Set(sources.map(s => s.id));
  let   currentLevel = [...sources];

  while (currentLevel.length > 0) {
    const nextLevel = [];
    for (const currentBlock of currentLevel) {
      for (const targetId of (adjacencyMap.get(currentBlock.id) || [])) {
        if (!visitedNodes.has(targetId)) {
          const inputConns = connections.filter(c => c.toBlock === targetId && !feedbackConnections.has(c.id));
          if (inputConns.every(c => visitedNodes.has(c.fromBlock))) {
            const targetBlock = blocks.find(b => b.id === targetId);
            visitedNodes.add(targetId);
            sorted.push(targetBlock);
            nextLevel.push(targetBlock);
          }
        }
      }
    }
    currentLevel = nextLevel;
  }

  if (sorted.length !== blocks.length) {
    blocks.filter(b => !visitedNodes.has(b.id)).forEach(block => sorted.push(block));
  }

  console.log('[TopologicalSort] Final startup order:', sorted.map(b => b.name));
  return sorted;
};