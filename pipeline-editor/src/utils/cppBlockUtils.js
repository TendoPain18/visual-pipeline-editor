import { generateColor, calculatePortPositions, BLOCK_WIDTH, BLOCK_HEIGHT, GRID_SIZE } from './blockUtils';

/**
 * Parse C++ / CUDA block configuration from source code.
 * Looks for BlockConfig struct initialization.
 * Supports both .cpp and .cu file extensions.
 *
 * FIX: The original regex `[^}]*` stopped at the first `}` inside the struct body,
 * which happened to be the closing brace of the first array literal (e.g. `{1504}`).
 * This meant ltr, startWithAll, and description were never reached by the parser.
 *
 * The fix extracts the struct body by counting brace depth instead.
 */

/**
 * Extract the content inside the outermost BlockConfig braces,
 * correctly handling nested `{...}` array literals.
 */
const extractStructBody = (fileContent) => {
  const headerMatch = fileContent.match(/BlockConfig\s+\w+\s*=\s*\{/);
  if (!headerMatch) return null;

  const startIdx = headerMatch.index + headerMatch[0].length;

  let depth = 1;
  let i = startIdx;

  while (i < fileContent.length && depth > 0) {
    if (fileContent[i] === '{') depth++;
    else if (fileContent[i] === '}') depth--;
    i++;
  }

  if (depth !== 0) return null;

  return fileContent.slice(startIdx, i - 1);
};

const parseCppStruct = (fileContent) => {
  const body = extractStructBody(fileContent);
  if (!body) return null;

  const config = {};

  // ── String fields ────────────────────────────────────────────────────────────
  const stringFields = ['name', 'description'];
  stringFields.forEach(field => {
    const regex = new RegExp('"([^"]*)"[^\\n]*//[^\\n]*\\b' + field + '\\b', 'i');
    const match = body.match(regex);
    if (match) config[field] = match[1];
  });

  // ── Numeric fields ───────────────────────────────────────────────────────────
  const numericFields = ['inputs', 'outputs'];
  numericFields.forEach(field => {
    const regex = new RegExp('(\\d+)[^\\n]*//[^\\n]*\\b' + field + '\\b', 'i');
    const match = body.match(regex);
    if (match) config[field] = parseInt(match[1]);
  });

  // ── Boolean fields ───────────────────────────────────────────────────────────
  const boolFields = ['ltr', 'startWithAll', 'isGraph'];
  boolFields.forEach(field => {
    const regex = new RegExp('(true|false)[^\\n]*//[^\\n]*\\b' + field + '\\b', 'i');
    const match = body.match(regex);
    if (match) config[field] = match[1].toLowerCase() === 'true';
  });

  // ── Array fields ─────────────────────────────────────────────────────────────
  const arrayFields = [
    'inputPacketSizes',
    'inputBatchSizes',
    'outputPacketSizes',
    'outputBatchSizes',
  ];
  arrayFields.forEach(field => {
    const regex = new RegExp('\\{([^}]*)\\}[^\\n]*//[^\\n]*\\b' + field + '\\b', 'i');
    const match = body.match(regex);
    if (match) {
      config[field] = match[1]
        .split(',')
        .map(v => parseInt(v.trim()))
        .filter(n => !isNaN(n));
    }
  });

  // ── graphType (optional string) ──────────────────────────────────────────────
  const graphTypeMatch = body.match(/"([^"]*)"[^\\n]*\/\/[^\\n]*\bgraphType\b/i);
  if (graphTypeMatch) config.graphType = graphTypeMatch[1];

  return Object.keys(config).length > 0 ? config : null;
};

/**
 * Calculate length bytes needed for batch count
 */
const calculateLengthBytes = (maxCount) => {
  if (maxCount <= 255) return 1;
  if (maxCount <= 65535) return 2;
  if (maxCount <= 16777215) return 3;
  return 4;
};

/**
 * Calculate total buffer size (length header + all packets)
 */
const calculateBufferSize = (packetSize, batchSize) => {
  const lengthBytes = calculateLengthBytes(batchSize);
  return lengthBytes + (packetSize * batchSize);
};

/**
 * Returns true if the file is a CUDA source file (.cu extension).
 */
export const isCudaFile = (fileName) => {
  return fileName && fileName.toLowerCase().endsWith('.cu');
};

/**
 * Strips .cpp or .cu extension and returns just the base name.
 */
export const stripCppExtension = (fileName) => {
  return fileName.replace(/\.(cpp|cu)$/i, '');
};

/**
 * Main C++ / CUDA block parser.
 * Accepts both .cpp and .cu files.
 */
export const parseCppBlock = (fileContent, fileName) => {
  const config = parseCppStruct(fileContent);

  if (!config) {
    const isCuda = isCudaFile(fileName);
    throw new Error(
      `No BlockConfig struct found.\n\n` +
      `Add a BlockConfig initialization to your ${isCuda ? 'CUDA (.cu)' : 'C++ (.cpp)'} file.\n\n` +
      `Example:\n` +
      `BlockConfig config = {\n` +
      `    "MyBlock",      // name\n` +
      `    1,              // inputs\n` +
      `    1,              // outputs\n` +
      `    {1500},         // inputPacketSizes\n` +
      `    {10},           // inputBatchSizes\n` +
      `    {1500},         // outputPacketSizes\n` +
      `    {10},           // outputBatchSizes\n` +
      `    true,           // ltr\n` +
      `    true,           // startWithAll\n` +
      `    "Description"   // description\n` +
      `};`
    );
  }

  const numInputs  = config.inputs  || 0;
  const numOutputs = config.outputs || 0;

  let inputPacketSizes  = config.inputPacketSizes  || [0];
  let outputPacketSizes = config.outputPacketSizes || [0];
  let inputBatchSizes   = config.inputBatchSizes   || [1];
  let outputBatchSizes  = config.outputBatchSizes  || [1];

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

  const inputBufferSizes = inputPacketSizes.map((packetSize, i) =>
    calculateBufferSize(packetSize, inputBatchSizes[i])
  );
  const outputBufferSizes = outputPacketSizes.map((packetSize, i) =>
    calculateBufferSize(packetSize, outputBatchSizes[i])
  );

  const inputLengthBytes  = inputBatchSizes.map(calculateLengthBytes);
  const outputLengthBytes = outputBatchSizes.map(calculateLengthBytes);

  const totalInputSize  = inputBufferSizes.reduce((a, b) => a + b, 0);
  const totalOutputSize = outputBufferSizes.reduce((a, b) => a + b, 0);

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
      description: `×${ratio.toFixed(2)} (${config.description || 'increases size'})`,
    };
  } else {
    const ratio = totalInputSize / totalOutputSize;
    sizeRelation = {
      type: 'divide',
      factor: ratio,
      description: `÷${ratio.toFixed(2)} (${config.description || 'decreases size'})`,
    };
  }

  const ltr          = config.ltr          !== undefined ? config.ltr          : true;
  const startWithAll = config.startWithAll !== undefined ? config.startWithAll : false;
  const isGraph      = config.graphType    !== undefined;
  const cuda         = isCudaFile(fileName);

  return {
    name:     config.name || stripCppExtension(fileName),
    fileName,
    code:     fileContent,
    language: 'cpp',   // still 'cpp' so the rest of the pipeline works unchanged
    isCuda:   cuda,    // NEW: flag so compile / run logic can pick nvcc vs g++

    inputs:  numInputs,
    outputs: numOutputs,

    inputPacketSizes,
    inputBatchSizes,
    outputPacketSizes,
    outputBatchSizes,
    inputBufferSizes,
    outputBufferSizes,
    inputLengthBytes,
    outputLengthBytes,

    // Legacy
    inputSize:   totalInputSize,
    outputSize:  totalOutputSize,
    inputSizes:  inputBufferSizes,
    outputSizes: outputBufferSizes,

    description: config.description || 'No description',
    config,
    color: isGraph
      ? '#9333ea'
      : cuda
        ? '#22c55e'   // green tint for CUDA blocks so they're visually distinct
        : generateColor(config.name || stripCppExtension(fileName)),
    portPositions: calculatePortPositions(numInputs, numOutputs, ltr, BLOCK_WIDTH, BLOCK_HEIGHT, GRID_SIZE),
    sizeRelation,
    ltr,
    startWithAll,
    isGraph,
    graphType: config.graphType || null,
  };
};

/**
 * Detect if a file is a C++ or CUDA block.
 * Checks for the run_generic_block include in source content.
 */
export const isCppBlock = (fileContent) => {
  return (
    fileContent.includes('#include "core/run_generic_block.h"') ||
    fileContent.includes('run_generic_block')
  );
};