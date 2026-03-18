import React, { useState, useRef, useEffect } from 'react';
import { Toolbar } from './components/Toolbar';
import { Sidebar } from './components/Sidebar';
import { CanvasRenderer } from './components/CanvasRenderer';
import { CodeEditorModal } from './components/CodeEditorModal';
import { GraphWindow } from './components/GraphWindow';
import { useHistory } from './hooks/useHistory';
import { useProcessManager } from './hooks/useProcessManager';
import { useCanvasInteraction } from './hooks/useCanvasInteraction';
import { useKeyboardShortcuts } from './hooks/useKeyboardShortcuts';
import { useFileOperations } from './hooks/useFileOperations';
import { useServerOperations } from './hooks/useServerOperations';
import { useCanvasHandlers } from './hooks/useCanvasHandlers';
import { GRID_SIZE, parseMatlabBlock, calculatePortPositions, BLOCK_WIDTH, BLOCK_HEIGHT } from './utils/blockUtils';

const PipelineEditor = () => {
  const [sidebarMode, setSidebarMode] = useState(null);
  const [selectedBlocks, setSelectedBlocks] = useState([]);
  const [selectedConnections, setSelectedConnections] = useState([]);
  const [isEditingCode, setIsEditingCode] = useState(false);
  const [serverRunning, setServerRunning] = useState(false);
  const [executionLog, setExecutionLog] = useState([]);
  const [projectDir, setProjectDir] = useState('');
  const [clipboard, setClipboard] = useState(null);
  const [isStartingServer, setIsStartingServer] = useState(false);
  const [graphWindows, setGraphWindows] = useState({});
  
  const canvasRef = useRef(null);

  const addLog = (type, message) => {
    const timestamp = new Date().toLocaleTimeString();
    console.log(`[${timestamp}] [${type}] ${message}`);
    setExecutionLog(prev => [...prev, { type, message, time: timestamp }]);
  };

  const {
    blocks,
    setBlocks,
    connections,
    setConnections,
    updateBlocksWithHistory,
    updateConnectionsWithHistory,
    updateBothWithHistory,
    undo,
    redo,
    saveToHistory
  } = useHistory();

  const {
    blockProcesses,
    setBlockProcesses,
    blockMetrics,
    setBlockMetrics,
    graphData,
    setGraphData,
    blockStatus // NEW: Block status from protocol
  } = useProcessManager(addLog);

  const {
    draggingBlock,
    setDraggingBlock,
    dragOffset,
    setDragOffset,
    currentConnection,
    setCurrentConnection,
    mousePosition,
    setMousePosition,
    hoveredConnectionPoint,
    setHoveredConnectionPoint,
    draggingWaypoint,
    setDraggingWaypoint,
    hoveredWaypoint,
    setHoveredWaypoint,
    hoveredConnection,
    setHoveredConnection,
    hoveredBlock,
    setHoveredBlock,
    selectionBox,
    setSelectionBox,
    isSelecting,
    setIsSelecting,
    selectionStart,
    setSelectionStart,
    snapToGrid
  } = useCanvasInteraction(GRID_SIZE);

  const {
    handleFileUpload,
    handleSaveDiagram,
    handleLoadDiagram
  } = useFileOperations({
    blocks,
    connections,
    projectDir,
    addLog,
    updateBothWithHistory,
    updateBlocksWithHistory,
    snapToGrid
  });

  const {
    handleStartServer,
    handleStart,
    handleStop,
    handleTerminate,
    handleStartBlock,
    handleStopBlock
  } = useServerOperations({
    blocks,
    connections,
    projectDir,
    setServerRunning,
    setBlockProcesses,
    setIsStartingServer,
    addLog
  });

  const {
    handleCanvasClick,
    handleMouseMove,
    handleMouseUp,
    handleRightClick,
    handleDoubleClickCanvas,
    handleCanvasMouseDown
  } = useCanvasHandlers({
    blocks,
    setBlocks,
    connections,
    setConnections,
    currentConnection,
    setCurrentConnection,
    mousePosition,
    setMousePosition,
    selectedBlocks,
    setSelectedBlocks,
    selectedConnections,
    setSelectedConnections,
    draggingBlock,
    setDraggingBlock,
    dragOffset,
    setDragOffset,
    draggingWaypoint,
    setDraggingWaypoint,
    isSelecting,
    setIsSelecting,
    selectionStart,
    setSelectionStart,
    selectionBox,
    setSelectionBox,
    setHoveredConnectionPoint,
    setHoveredWaypoint,
    setHoveredConnection,
    setHoveredBlock,
    serverRunning,
    snapToGrid,
    saveToHistory,
    addLog,
    setSidebarMode,
    canvasRef
  });

  useEffect(() => {
    const getDir = async () => {
      const dir = await window.electronAPI.getAppPath();
      setProjectDir(dir);
    };
    getDir();
  }, []);

  useEffect(() => {
    Object.entries(graphData).forEach(([blockId, data]) => {
      if (!graphWindows[blockId]) {
        const block = blocks.find(b => b.id === parseInt(blockId));
        if (block && block.isGraph) {
          setGraphWindows(prev => ({
            ...prev,
            [blockId]: { blockName: block.name, graphType: block.graphType }
          }));
        }
      }
    });
  }, [graphData, blocks]);

  const handleCopy = () => {
    if (selectedBlocks.length > 0) {
      setClipboard({ type: 'blocks', data: JSON.parse(JSON.stringify(selectedBlocks)) });
      addLog('success', `Copied ${selectedBlocks.length} block(s)`);
    }
  };

  const handlePaste = () => {
    if (clipboard && clipboard.type === 'blocks') {
      const idMapping = {};
      const timestamp = Date.now();
      const newBlocks = clipboard.data.map((block, index) => {
        const newId = timestamp + Math.random() * 10000 + index * 1000;
        idMapping[block.id] = newId;
        return {
          ...JSON.parse(JSON.stringify(block)),
          id: newId,
          x: snapToGrid(block.x + 40),
          y: snapToGrid(block.y + 40),
          zIndex: blocks.length + Object.keys(idMapping).length,
          name: block.name.includes('_copy') ? block.name : `${block.name}_copy${index + 1}`
        };
      });
      
      const allBlocks = [...blocks, ...newBlocks];
      updateBlocksWithHistory(allBlocks);
      setSelectedBlocks(newBlocks);
      setSidebarMode('config');
      addLog('success', `Pasted ${newBlocks.length} block(s) with unique IDs: ${newBlocks.map(b => b.id).join(', ')}`);
    }
  };

  const handleDelete = () => {
    if (serverRunning && selectedConnections.length > 0) {
      addLog('warning', 'Cannot delete connections while server is running');
      return;
    }
    
    if (selectedBlocks.length > 0) {
      const selectedIds = selectedBlocks.map(b => b.id);
      const newBlocks = blocks.filter(b => !selectedIds.includes(b.id));
      const newConnections = connections.filter(c => 
        !selectedIds.includes(c.fromBlock) && !selectedIds.includes(c.toBlock)
      );
      updateBothWithHistory(newBlocks, newConnections);
      addLog('info', `Deleted ${selectedBlocks.length} block(s)`);
      setSelectedBlocks([]);
      setSidebarMode(null);
    } else if (selectedConnections.length > 0) {
      const selectedIds = selectedConnections.map(c => c.id);
      const newConnections = connections.filter(c => !selectedIds.includes(c.id));
      updateConnectionsWithHistory(newConnections);
      addLog('info', `Deleted ${selectedConnections.length} connection(s)`);
      setSelectedConnections([]);
    }
  };

  const handleUndo = () => {
    if (undo()) {
      setSelectedBlocks([]);
      setSelectedConnections([]);
      addLog('info', 'Undo');
    }
  };

  const handleRedo = () => {
    if (redo()) {
      setSelectedBlocks([]);
      setSelectedConnections([]);
      addLog('info', 'Redo');
    }
  };

  const handleEditBlockCode = (block) => {
    setSelectedBlocks([block]);
    setIsEditingCode(true);
  };

  const handleSaveBlockCode = (updatedCode) => {
    const block = selectedBlocks[0];
    const reparsedData = parseMatlabBlock(updatedCode, block.fileName);
    const newPortPositions = calculatePortPositions(
      reparsedData.inputs, 
      reparsedData.outputs, 
      reparsedData.ltr,
      BLOCK_WIDTH,
      BLOCK_HEIGHT,
      GRID_SIZE
    );
    
    const updatedBlock = { 
      ...block, 
      code: updatedCode,
      inputs: reparsedData.inputs,
      outputs: reparsedData.outputs,
      config: reparsedData.config,
      sizeRelation: reparsedData.sizeRelation,
      inputSize: reparsedData.inputSize,
      outputSize: reparsedData.outputSize,
      portPositions: newPortPositions,
      ltr: reparsedData.ltr,
      startWithAll: reparsedData.startWithAll,
      isGraph: reparsedData.isGraph,
      graphType: reparsedData.graphType
    };
    
    const removedConnections = connections.filter(conn => {
      if (conn.fromBlock === block.id && conn.fromPort >= reparsedData.outputs) {
        return true;
      }
      if (conn.toBlock === block.id && conn.toPort >= reparsedData.inputs) {
        return true;
      }
      return false;
    });
    
    const newBlocks = blocks.map(b => b.id === block.id ? updatedBlock : b);
    const newConnections = connections.filter(conn => !removedConnections.includes(conn));
    
    if (removedConnections.length > 0) {
      addLog('warning', `Removed ${removedConnections.length} connection(s) due to port changes`);
      updateBothWithHistory(newBlocks, newConnections);
    } else {
      updateBlocksWithHistory(newBlocks);
    }
    
    setSelectedBlocks([updatedBlock]);
    addLog('success', `Updated code for block ${block.name}`);
    setIsEditingCode(false);
  };

  const handleCanvasClickWithButtons = (e) => {
    if (serverRunning && hoveredBlock) {
      const canvas = canvasRef.current;
      const rect = canvas.getBoundingClientRect();
      const x = e.clientX - rect.left;
      const y = e.clientY - rect.top;
      
      const buttonSize = 20; // Updated to match new button size
      const padding = 6; // Updated to match new padding
      const buttonX = hoveredBlock.x + padding;
      const buttonY = hoveredBlock.y + padding;
      
      if (x >= buttonX && x <= buttonX + buttonSize && y >= buttonY && y <= buttonY + buttonSize) {
        const isRunning = blockStatus[hoveredBlock.id] === 'ready';
        if (isRunning) {
          handleStopBlock(hoveredBlock, blockProcesses, setBlockMetrics);
        } else {
          handleStartBlock(hoveredBlock, blockProcesses);
        }
        e.stopPropagation();
        return;
      }
    }
    
    handleCanvasClick(e);
  };


  useKeyboardShortcuts({
    onUndo: handleUndo,
    onRedo: handleRedo,
    onCopy: handleCopy,
    onPaste: handlePaste,
    onDelete: handleDelete,
    isEditingCode,
    currentConnection,
    setCurrentConnection,
    setMousePosition,
    addLog
  });

  return (
    <div className="flex h-screen bg-gray-100">
      <Sidebar
        sidebarMode={sidebarMode}
        selectedBlocks={selectedBlocks}
        blocks={blocks}
        connections={connections}
        serverRunning={serverRunning}
        blockProcesses={blockProcesses}
        blockStatus={blockStatus} // NEW: Pass block status
        executionLog={executionLog}
        onEditCode={handleEditBlockCode}
        onDelete={handleDelete}
        onBlockColorChange={(block, color) => {
          const updated = { ...block, color };
          setBlocks(blocks.map(b => b.id === block.id ? updated : b));
          setSelectedBlocks([updated]);
        }}
        onKillProcess={async (pid, name) => {
          const result = await window.electronAPI.killProcess(pid);
          if (result.success) {
            addLog('info', `Killed process ${name} (PID: ${pid})`);
          }
        }}
        onClearLog={() => setExecutionLog([])}
      />
      
      <div className="flex-1 flex flex-col">
        <Toolbar
          onStartServer={handleStartServer}
          onStart={() => handleStart()}
          onStop={() => handleStop(blockProcesses, setBlockMetrics)}
          onTerminate={() => handleTerminate(setBlockMetrics)}
          onImport={async () => {
            const newBlocks = await handleFileUpload();
            if (newBlocks && newBlocks.length > 0) {
              setSelectedBlocks(newBlocks);
              setSidebarMode('config');
            }
          }}
          onLoad={handleLoadDiagram}
          onSave={handleSaveDiagram}
          serverRunning={serverRunning}
          isStartingServer={isStartingServer}
          hasProcesses={Object.keys(blockProcesses).length > 0}
        />
        
        <div className="flex-1 overflow-auto bg-gray-50">
          <canvas 
            ref={canvasRef} 
            width={2000} 
            height={1500}
            className={currentConnection ? "cursor-crosshair" : (hoveredWaypoint || draggingWaypoint ? "cursor-move" : "cursor-default")}
            onClick={handleCanvasClickWithButtons}
            onMouseDown={handleCanvasMouseDown}
            onMouseMove={handleMouseMove}
            onMouseUp={handleMouseUp}
            onMouseLeave={handleMouseUp}
            onContextMenu={handleRightClick}
            onDoubleClick={(e) => {
              const result = handleDoubleClickCanvas(e);
              if (result.type === 'block') {
                handleEditBlockCode(result.block);
              }
            }}
          />
          <CanvasRenderer
            canvasRef={canvasRef}
            blocks={blocks}
            connections={connections}
            selectedBlocks={selectedBlocks}
            selectedConnections={selectedConnections}
            currentConnection={currentConnection}
            mousePosition={mousePosition}
            hoveredConnection={hoveredConnection}
            hoveredWaypoint={hoveredWaypoint}
            hoveredBlock={hoveredBlock}
            hoveredConnectionPoint={hoveredConnectionPoint}
            draggingWaypoint={draggingWaypoint}
            blockProcesses={blockProcesses}
            blockMetrics={blockMetrics}
            blockStatus={blockStatus} // NEW: Pass block status
            selectionBox={selectionBox}
            serverRunning={serverRunning}
          />
        </div>
      </div>

      {Object.entries(graphWindows).map(([blockId, windowInfo]) => (
        <GraphWindow
          key={blockId}
          blockId={blockId}
          blockName={windowInfo.blockName}
          graphType={windowInfo.graphType}
          data={graphData[blockId] || { xData: [], yData: [] }}
          onClose={() => {
            setGraphWindows(prev => {
              const newWindows = { ...prev };
              delete newWindows[blockId];
              return newWindows;
            });
          }}
        />
      ))}

      {isEditingCode && selectedBlocks.length === 1 && (
        <CodeEditorModal
          block={selectedBlocks[0]}
          onSave={handleSaveBlockCode}
          onClose={() => setIsEditingCode(false)}
        />
      )}
    </div>
  );
};

export default PipelineEditor;
