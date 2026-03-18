import { useState } from 'react';
import { getPortAtPosition, getWaypointAtPosition, getConnectionAtPoint, validateConnection } from '../utils/canvasUtils';
import { BLOCK_WIDTH, BLOCK_HEIGHT } from '../utils/blockUtils';

export const useCanvasHandlers = ({
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
}) => {
  const handleCanvasClick = (e) => {
    const canvas = canvasRef.current;
    const rect = canvas.getBoundingClientRect();
    const x = e.clientX - rect.left;
    const y = e.clientY - rect.top;

    const port = getPortAtPosition(x, y, blocks);

    if (port) {
      if (serverRunning) {
        addLog('warning', 'Cannot create connections while server is running');
        return;
      }
      
      if (!currentConnection) {
        if (port.type !== 'output') {
          addLog('warning', 'Connections must start from an output port');
          return;
        }
        
        const existingConnection = connections.find(c => 
          c.fromBlock === port.blockId && c.fromPort === port.index
        );
        
        if (existingConnection) {
          addLog('warning', 'This output already has a connection');
          return;
        }
        
        setCurrentConnection({
          fromBlock: port.blockId,
          fromPort: port.index,
          waypoints: []
        });
        addLog('info', 'Connection started - click to add waypoints, click input to finish');
      } else {
        if (port.type !== 'input') {
          addLog('warning', 'Connections must end at an input port');
          setCurrentConnection(null);
          return;
        }

        if (!validateConnection(currentConnection.fromBlock, currentConnection.fromPort, 
                              port.blockId, port.index, blocks, addLog)) {
          setCurrentConnection(null);
          return;
        }
        
        const exists = connections.some(c => 
          c.fromBlock === currentConnection.fromBlock &&
          c.fromPort === currentConnection.fromPort &&
          c.toBlock === port.blockId &&
          c.toPort === port.index
        );
        
        if (exists) {
          addLog('warning', 'This connection already exists');
          setCurrentConnection(null);
          return;
        }
        
        const newConnections = [...connections, {
          id: Date.now(),
          fromBlock: currentConnection.fromBlock,
          fromPort: currentConnection.fromPort,
          toBlock: port.blockId,
          toPort: port.index,
          waypoints: currentConnection.waypoints
        }];
        
        setConnections(newConnections);
        saveToHistory(blocks, newConnections);
        setCurrentConnection(null);
        setMousePosition(null);
        addLog('success', 'Connection created');
      }
    } else if (currentConnection) {
      const snappedX = snapToGrid(x);
      const snappedY = snapToGrid(y);
      setCurrentConnection({
        ...currentConnection,
        waypoints: [...currentConnection.waypoints, { x: snappedX, y: snappedY }]
      });
      addLog('info', 'Waypoint added');
    } else {
      const connInfo = getConnectionAtPoint(x, y, connections, blocks, snapToGrid);
      if (connInfo) {
        if (e.ctrlKey || e.metaKey) {
          if (selectedConnections.find(c => c.id === connInfo.connection.id)) {
            setSelectedConnections(selectedConnections.filter(c => c.id !== connInfo.connection.id));
          } else {
            setSelectedConnections([...selectedConnections, connInfo.connection]);
          }
        } else {
          setSelectedConnections([connInfo.connection]);
          setSelectedBlocks([]);
          setSidebarMode(null);
        }
      }
    }
  };

  const handleMouseDown = (e, block) => {
    const canvas = canvasRef.current;
    const rect = canvas.getBoundingClientRect();
    const x = e.clientX - rect.left;
    const y = e.clientY - rect.top;
    
    if (currentConnection) {
      return;
    }
    
    const port = getPortAtPosition(x, y, blocks);
    if (port) {
      return;
    }
    
    const maxZIndex = Math.max(...blocks.map(b => b.zIndex || 0), 0);
    const newBlocks = blocks.map(b => 
      b.id === block.id 
        ? { ...b, zIndex: maxZIndex + 1 }
        : b
    );
    setBlocks(newBlocks);
    
    setDraggingBlock(block);
    
    if (e.ctrlKey || e.metaKey) {
      if (selectedBlocks.find(b => b.id === block.id)) {
        setSelectedBlocks(selectedBlocks.filter(b => b.id !== block.id));
      } else {
        setSelectedBlocks([...selectedBlocks, block]);
        setSidebarMode('config');
      }
    } else {
      if (!selectedBlocks.find(b => b.id === block.id)) {
        setSelectedBlocks([block]);
        setSidebarMode('config');
      }
    }
    
    setSelectedConnections([]);
    setDragOffset({ x: x - block.x, y: y - block.y });
  };

  const handleMouseMove = (e) => {
    const canvas = canvasRef.current;
    const rect = canvas.getBoundingClientRect();
    const x = e.clientX - rect.left;
    const y = e.clientY - rect.top;

    setMousePosition({ x, y });

    if (currentConnection) {
      setHoveredConnectionPoint(getPortAtPosition(x, y, blocks));
    } else {
      const waypoint = getWaypointAtPosition(x, y, connections);
      setHoveredWaypoint(waypoint);
      
      const connInfo = getConnectionAtPoint(x, y, connections, blocks, snapToGrid);
      setHoveredConnection(connInfo ? connInfo.connection.id : null);
      
      const sortedBlocks = [...blocks].sort((a, b) => (b.zIndex || 0) - (a.zIndex || 0));
      const foundBlock = sortedBlocks.find(b => 
        x >= b.x && x <= b.x + BLOCK_WIDTH && 
        y >= b.y && y <= b.y + BLOCK_HEIGHT
      );
      setHoveredBlock(foundBlock || null);
    }

    if (isSelecting && selectionStart) {
      setSelectionBox({
        x: Math.min(selectionStart.x, x),
        y: Math.min(selectionStart.y, y),
        width: Math.abs(x - selectionStart.x),
        height: Math.abs(y - selectionStart.y)
      });
      
      const box = {
        x: Math.min(selectionStart.x, x),
        y: Math.min(selectionStart.y, y),
        width: Math.abs(x - selectionStart.x),
        height: Math.abs(y - selectionStart.y)
      };
      
      const blocksInBox = blocks.filter(b => 
        b.x >= box.x && b.x + BLOCK_WIDTH <= box.x + box.width &&
        b.y >= box.y && b.y + BLOCK_HEIGHT <= box.y + box.height
      );
      
      setSelectedBlocks(blocksInBox);
      
      const connectionsInBox = connections.filter(conn => {
        const fromBlock = blocks.find(b => b.id === conn.fromBlock);
        const toBlock = blocks.find(b => b.id === conn.toBlock);
        if (!fromBlock || !toBlock) return false;
        
        const startPos = { x: fromBlock.x + BLOCK_WIDTH, y: fromBlock.y + BLOCK_HEIGHT / 2 };
        const endPos = { x: toBlock.x, y: toBlock.y + BLOCK_HEIGHT / 2 };
        
        return startPos.x >= box.x && startPos.x <= box.x + box.width &&
               startPos.y >= box.y && startPos.y <= box.y + box.height &&
               endPos.x >= box.x && endPos.x <= box.x + box.width &&
               endPos.y >= box.y && endPos.y <= box.y + box.height;
      });
      
      setSelectedConnections(connectionsInBox);
      return;
    }

    if (draggingWaypoint) {
      const snappedX = snapToGrid(x);
      const snappedY = snapToGrid(y);
      
      setConnections(connections.map(conn => {
        if (conn.id === draggingWaypoint.connectionId) {
          const newWaypoints = [...conn.waypoints];
          newWaypoints[draggingWaypoint.waypointIndex] = { x: snappedX, y: snappedY };
          return { ...conn, waypoints: newWaypoints };
        }
        return conn;
      }));
      return;
    }

    if (!draggingBlock) return;
    
    const newX = snapToGrid(x - dragOffset.x);
    const newY = snapToGrid(y - dragOffset.y);
    
    if (selectedBlocks.find(b => b.id === draggingBlock.id)) {
      const selectedIds = selectedBlocks.map(b => b.id);
      const selectedConnectionIds = selectedConnections.map(c => c.id);
      const draggedBlockOldX = draggingBlock.x;
      const draggedBlockOldY = draggingBlock.y;
      const deltaX = newX - draggedBlockOldX;
      const deltaY = newY - draggedBlockOldY;
      
      setBlocks(blocks.map(b => {
        if (selectedIds.includes(b.id)) {
          return {
            ...b,
            x: Math.max(0, b.x + deltaX),
            y: Math.max(0, b.y + deltaY)
          };
        }
        return b;
      }));
      
      setConnections(connections.map(conn => {
        const isSelectedConnection = selectedConnectionIds.includes(conn.id);
        const fromBlockSelected = selectedIds.includes(conn.fromBlock);
        const toBlockSelected = selectedIds.includes(conn.toBlock);
        const bothBlocksSelected = fromBlockSelected && toBlockSelected;
        
        if (bothBlocksSelected && conn.waypoints && conn.waypoints.length > 0) {
          return {
            ...conn,
            waypoints: conn.waypoints.map(wp => ({
              x: wp.x + deltaX,
              y: wp.y + deltaY
            }))
          };
        }
        
        if (isSelectedConnection && conn.waypoints && conn.waypoints.length > 0) {
          return {
            ...conn,
            waypoints: conn.waypoints.map(wp => ({
              x: wp.x + deltaX,
              y: wp.y + deltaY
            }))
          };
        }
        
        return conn;
      }));
      
      setDraggingBlock({ ...draggingBlock, x: newX, y: newY });
    } else {
      setBlocks(blocks.map(b => 
        b.id === draggingBlock.id 
          ? { ...b, x: Math.max(0, newX), y: Math.max(0, newY) }
          : b
      ));
      
      setDraggingBlock({ ...draggingBlock, x: newX, y: newY });
    }
  };

  const handleMouseUp = () => {
    if (draggingBlock) {
      saveToHistory(blocks, connections);
    }
    if (draggingWaypoint) {
      saveToHistory(blocks, connections);
    }
    if (isSelecting) {
      setIsSelecting(false);
      setSelectionBox(null);
      setSelectionStart(null);
    }
    setDraggingBlock(null);
    setDraggingWaypoint(null);
  };

  const handleRightClick = (e) => {
    e.preventDefault();
    
    const canvas = canvasRef.current;
    const rect = canvas.getBoundingClientRect();
    const x = e.clientX - rect.left;
    const y = e.clientY - rect.top;

    if (currentConnection) {
      setCurrentConnection(null);
      setMousePosition(null);
      addLog('info', 'Connection cancelled');
      return;
    }

    const waypoint = getWaypointAtPosition(x, y, connections);
    if (waypoint) {
      const newConnections = connections.map(conn => {
        if (conn.id === waypoint.connectionId) {
          const newWaypoints = conn.waypoints.filter((_, idx) => idx !== waypoint.waypointIndex);
          return { ...conn, waypoints: newWaypoints };
        }
        return conn;
      });
      setConnections(newConnections);
      saveToHistory(blocks, newConnections);
    }
  };

  const handleDoubleClickCanvas = (e) => {
    const canvas = canvasRef.current;
    const rect = canvas.getBoundingClientRect();
    const x = e.clientX - rect.left;
    const y = e.clientY - rect.top;
    
    const sortedBlocks = [...blocks].sort((a, b) => (b.zIndex || 0) - (a.zIndex || 0));
    const clickedBlock = sortedBlocks.find(b => 
      x >= b.x && x <= b.x + BLOCK_WIDTH && 
      y >= b.y && y <= b.y + BLOCK_HEIGHT
    );
    
    if (clickedBlock) {
      return { type: 'block', block: clickedBlock };
    }

    const connInfo = getConnectionAtPoint(x, y, connections, blocks, snapToGrid);
    if (connInfo && !currentConnection) {
      const newConnections = connections.map(conn => {
        if (conn.id === connInfo.connection.id) {
          const newWaypoints = conn.waypoints || [];
          newWaypoints.splice(connInfo.insertAfter, 0, connInfo.position);
          return { ...conn, waypoints: newWaypoints };
        }
        return conn;
      });
      setConnections(newConnections);
      saveToHistory(blocks, newConnections);
      return { type: 'waypoint' };
    }
    
    return { type: 'none' };
  };

  const handleCanvasMouseDown = (e) => {
    const canvas = canvasRef.current;
    const rect = canvas.getBoundingClientRect();
    const x = e.clientX - rect.left;
    const y = e.clientY - rect.top;
    
    const waypoint = getWaypointAtPosition(x, y, connections);
    if (waypoint) {
      setDraggingWaypoint(waypoint);
      e.preventDefault();
      return;
    }
    
    const sortedBlocks = [...blocks].sort((a, b) => (b.zIndex || 0) - (a.zIndex || 0));
    const clickedBlock = sortedBlocks.find(b => 
      x >= b.x && x <= b.x + BLOCK_WIDTH && 
      y >= b.y && y <= b.y + BLOCK_HEIGHT
    );
    
    if (clickedBlock) {
      handleMouseDown(e, clickedBlock);
    } else {
      setIsSelecting(true);
      setSelectionStart({ x, y });
      setSelectionBox({ x, y, width: 0, height: 0 });
      
      if (!e.ctrlKey && !e.metaKey) {
        setSelectedBlocks([]);
        setSelectedConnections([]);
        setSidebarMode(null);
      }
    }
  };

  return {
    handleCanvasClick,
    handleMouseDown,
    handleMouseMove,
    handleMouseUp,
    handleRightClick,
    handleDoubleClickCanvas,
    handleCanvasMouseDown
  };
};
