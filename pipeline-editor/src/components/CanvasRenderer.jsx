import { useEffect } from 'react';
import { GRID_SIZE, BLOCK_WIDTH, BLOCK_HEIGHT } from '../utils/blockUtils';
import { getPortPosition } from '../utils/canvasUtils';
import { formatThroughput } from '../utils/formatThroughput';

export const CanvasRenderer = ({
  canvasRef,
  blocks,
  connections,
  selectedBlocks,
  selectedConnections,
  currentConnection,
  mousePosition,
  hoveredConnection,
  hoveredWaypoint,
  hoveredBlock,
  hoveredConnectionPoint,
  draggingWaypoint,
  blockProcesses,
  blockMetrics,
  blockStatus,
  selectionBox,
  serverRunning
}) => {
  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas) return;
    const ctx = canvas.getContext('2d');
    
    ctx.clearRect(0, 0, canvas.width, canvas.height);
    
    // Draw grid
    ctx.strokeStyle = 'rgba(200, 200, 200, 0.3)';
    ctx.lineWidth = 1;
    
    for (let x = 0; x <= canvas.width; x += GRID_SIZE) {
      ctx.beginPath();
      ctx.moveTo(x, 0);
      ctx.lineTo(x, canvas.height);
      ctx.stroke();
    }
    
    for (let y = 0; y <= canvas.height; y += GRID_SIZE) {
      ctx.beginPath();
      ctx.moveTo(0, y);
      ctx.lineTo(canvas.width, y);
      ctx.stroke();
    }
    
    // Draw connections
    connections.forEach(conn => {
      const fromBlock = blocks.find(b => b.id === conn.fromBlock);
      const toBlock = blocks.find(b => b.id === conn.toBlock);
      
      if (fromBlock && toBlock) {
        const startPos = getPortPosition(fromBlock, 'output', conn.fromPort);
        const endPos = getPortPosition(toBlock, 'input', conn.toPort);
        
        const isConnectionHovered = hoveredConnection === conn.id;
        const isConnectionSelected = selectedConnections.find(c => c.id === conn.id);
        
        ctx.strokeStyle = isConnectionSelected ? '#f59e0b' : (isConnectionHovered ? '#3b82f6' : '#64748b');
        ctx.lineWidth = isConnectionSelected ? 5 : (isConnectionHovered ? 4 : 3);
        ctx.beginPath();
        
        const points = [startPos];
        if (conn.waypoints) points.push(...conn.waypoints);
        points.push(endPos);
        
        ctx.moveTo(points[0].x, points[0].y);
        
        if (points.length === 2) {
          ctx.lineTo(points[1].x, points[1].y);
        } else {
          const cornerRadius = 10;
          
          for (let i = 0; i < points.length - 1; i++) {
            const current = points[i];
            const next = points[i + 1];
            const hasNextNext = i < points.length - 2;
            
            if (!hasNextNext) {
              ctx.lineTo(next.x, next.y);
            } else {
              const nextNext = points[i + 2];
              
              const dx1 = next.x - current.x;
              const dy1 = next.y - current.y;
              const dist1 = Math.sqrt(dx1 * dx1 + dy1 * dy1);
              
              const dx2 = nextNext.x - next.x;
              const dy2 = nextNext.y - next.y;
              const dist2 = Math.sqrt(dx2 * dx2 + dy2 * dy2);
              
              const actualRadius = Math.min(cornerRadius, dist1 / 2, dist2 / 2);
              
              const ratio1 = (dist1 - actualRadius) / dist1;
              const beforeCornerX = current.x + dx1 * ratio1;
              const beforeCornerY = current.y + dy1 * ratio1;
              
              const ratio2 = actualRadius / dist2;
              const afterCornerX = next.x + dx2 * ratio2;
              const afterCornerY = next.y + dy2 * ratio2;
              
              ctx.lineTo(beforeCornerX, beforeCornerY);
              ctx.quadraticCurveTo(next.x, next.y, afterCornerX, afterCornerY);
            }
          }
        }
        
        ctx.stroke();
        
        const lastPoint = points[points.length - 2] || startPos;
        const angle = Math.atan2(endPos.y - lastPoint.y, endPos.x - lastPoint.x);
        const arrowLength = 12;
        
        ctx.beginPath();
        ctx.moveTo(endPos.x, endPos.y);
        ctx.lineTo(
          endPos.x - arrowLength * Math.cos(angle - Math.PI / 6),
          endPos.y - arrowLength * Math.sin(angle - Math.PI / 6)
        );
        ctx.moveTo(endPos.x, endPos.y);
        ctx.lineTo(
          endPos.x - arrowLength * Math.cos(angle + Math.PI / 6),
          endPos.y - arrowLength * Math.sin(angle + Math.PI / 6)
        );
        ctx.stroke();
        
        if (conn.waypoints && conn.waypoints.length > 0) {
          conn.waypoints.forEach((wp, idx) => {
            const isThisWaypointHovered = hoveredWaypoint?.connectionId === conn.id && 
                                         hoveredWaypoint?.waypointIndex === idx;
            const isThisWaypointDragging = draggingWaypoint?.connectionId === conn.id && 
                                          draggingWaypoint?.waypointIndex === idx;
            
            if (isThisWaypointHovered || isThisWaypointDragging || isConnectionHovered || isConnectionSelected) {
              if (isThisWaypointHovered || isThisWaypointDragging) {
                ctx.fillStyle = 'rgba(59, 130, 246, 0.3)';
                ctx.beginPath();
                ctx.arc(wp.x, wp.y, 12, 0, Math.PI * 2);
                ctx.fill();
              }
              
              ctx.fillStyle = isThisWaypointDragging ? '#3b82f6' : (isThisWaypointHovered ? '#60a5fa' : '#94a3b8');
              ctx.beginPath();
              ctx.arc(wp.x, wp.y, 8, 0, Math.PI * 2);
              ctx.fill();
              
              ctx.fillStyle = '#ffffff';
              ctx.beginPath();
              ctx.arc(wp.x, wp.y, 4, 0, Math.PI * 2);
              ctx.fill();
            }
          });
        }
      }
    });
    
    // Draw current connection
    if (currentConnection) {
      const fromBlock = blocks.find(b => b.id === currentConnection.fromBlock);
      if (fromBlock && mousePosition) {
        const startPos = getPortPosition(fromBlock, 'output', currentConnection.fromPort);
        
        ctx.strokeStyle = '#3b82f6';
        ctx.lineWidth = 3;
        ctx.setLineDash([5, 5]);
        ctx.beginPath();
        ctx.moveTo(startPos.x, startPos.y);
        
        currentConnection.waypoints.forEach(wp => {
          ctx.lineTo(wp.x, wp.y);
        });
        
        ctx.lineTo(mousePosition.x, mousePosition.y);
        ctx.stroke();
        ctx.setLineDash([]);
        
        currentConnection.waypoints.forEach(wp => {
          ctx.fillStyle = '#3b82f6';
          ctx.beginPath();
          ctx.arc(wp.x, wp.y, 8, 0, Math.PI * 2);
          ctx.fill();
          
          ctx.fillStyle = '#ffffff';
          ctx.beginPath();
          ctx.arc(wp.x, wp.y, 4, 0, Math.PI * 2);
          ctx.fill();
        });
      }
    }
    
    // Draw selection box
    if (selectionBox) {
      ctx.strokeStyle = '#3b82f6';
      ctx.lineWidth = 2;
      ctx.setLineDash([5, 5]);
      ctx.strokeRect(selectionBox.x, selectionBox.y, selectionBox.width, selectionBox.height);
      ctx.fillStyle = 'rgba(59, 130, 246, 0.1)';
      ctx.fillRect(selectionBox.x, selectionBox.y, selectionBox.width, selectionBox.height);
      ctx.setLineDash([]);
    }
    
    // Draw blocks
    const sortedBlocks = [...blocks].sort((a, b) => (a.zIndex || 0) - (b.zIndex || 0));
    
    sortedBlocks.forEach(block => {
      const radius = 12;
      const status = blockStatus[block.id];
      const isRunning = status === 'ready';
      
      const isSelected = selectedBlocks.find(b => b.id === block.id);
      const isHovered = hoveredBlock?.id === block.id;
      
      // Draw block background
      ctx.fillStyle = block.color;
      ctx.beginPath();
      ctx.roundRect(block.x, block.y, BLOCK_WIDTH, BLOCK_HEIGHT, radius);
      ctx.fill();
      
      // Draw block border with status color
      let borderColor = '#1e293b';
      if (isSelected) {
        borderColor = '#fbbf24';
      } else if (status === 'ready') {
        borderColor = '#10b981';
      } else if (status === 'initializing' || status === 'starting') {
        borderColor = '#f59e0b';
      } else if (status === 'error') {
        borderColor = '#ef4444';
      }
      
      ctx.strokeStyle = borderColor;
      ctx.lineWidth = isSelected ? 3 : 2;
      ctx.stroke();
      
      // Draw status indicator
      if (status) {
        let indicatorColor;
        if (status === 'ready') indicatorColor = '#10b981';
        else if (status === 'initializing' || status === 'starting') indicatorColor = '#f59e0b';
        else if (status === 'error') indicatorColor = '#ef4444';
        else if (status === 'stopping') indicatorColor = '#6b7280';
        
        if (indicatorColor) {
          ctx.fillStyle = indicatorColor;
          ctx.beginPath();
          ctx.arc(block.x + BLOCK_WIDTH - 10, block.y + 10, 5, 0, Math.PI * 2);
          ctx.fill();
        }
      }
      
      // Draw graph indicator
      if (block.isGraph) {
        ctx.fillStyle = 'rgba(255, 255, 255, 0.9)';
        ctx.beginPath();
        ctx.arc(block.x + 10, block.y + 10, 8, 0, Math.PI * 2);
        ctx.fill();
        
        ctx.strokeStyle = '#9333ea';
        ctx.lineWidth = 2;
        ctx.beginPath();
        ctx.moveTo(block.x + 6, block.y + 13);
        ctx.lineTo(block.x + 8, block.y + 11);
        ctx.lineTo(block.x + 10, block.y + 9);
        ctx.lineTo(block.x + 12, block.y + 7);
        ctx.lineTo(block.x + 14, block.y + 10);
        ctx.stroke();
      }
      
      // Draw block text
      const metrics = blockMetrics[block.id];
      
      if (metrics) {
        // Calculate bps from gbps
        const bps = metrics.gbps * 1e9;
        const throughputStr = formatThroughput(bps);
        
        const lines = [];
        lines.push({ text: block.name, font: 'bold 14px sans-serif', color: '#ffffff' });
        
        if (metrics.totalFrames) {
          lines.push({ text: `F:${metrics.frames}/${metrics.totalFrames}`, font: 'bold 10px sans-serif', color: '#ffff00' });
        } else {
          lines.push({ text: `F:${metrics.frames}`, font: 'bold 10px sans-serif', color: '#ffff00' });
        }
        
        lines.push({ text: throughputStr, font: 'bold 10px sans-serif', color: '#ffff00' });
        
        if (metrics.totalGB !== undefined) {
          lines.push({ text: `T:${metrics.totalGB.toFixed(2)} GB`, font: 'bold 10px sans-serif', color: '#ffff00' });
        }
        
        const lineHeight = 14;
        const totalHeight = lines.length * lineHeight;
        const startY = block.y + (BLOCK_HEIGHT - totalHeight) / 2 + lineHeight / 2;
        
        lines.forEach((line, i) => {
          ctx.fillStyle = line.color;
          ctx.font = line.font;
          ctx.textAlign = 'center';
          ctx.textBaseline = 'middle';
          ctx.fillText(line.text, block.x + BLOCK_WIDTH / 2, startY + i * lineHeight);
        });
      } else {
        const centerY = block.y + BLOCK_HEIGHT / 2;
        
        ctx.fillStyle = '#ffffff';
        ctx.font = 'bold 14px sans-serif';
        ctx.textAlign = 'center';
        ctx.textBaseline = 'middle';
        ctx.fillText(block.name, block.x + BLOCK_WIDTH / 2, centerY - 8);
        
        ctx.font = '11px sans-serif';
        const infoText = block.isGraph ? `📊 ${block.graphType}` : `In: ${block.inputs} | Out: ${block.outputs}`;
        ctx.fillText(infoText, block.x + BLOCK_WIDTH / 2, centerY + 8);
      }
      
      // Draw start/stop buttons (top-left corner, bigger, with padding)
      if (isHovered && serverRunning) {
        const buttonSize = 20; // Bigger button
        const padding = 6; // Padding from corner
        const buttonX = block.x + padding;
        const buttonY = block.y + padding;
        
        if (isRunning) {
          // Stop button
          ctx.fillStyle = 'rgba(239, 68, 68, 0.9)';
          ctx.fillRect(buttonX, buttonY, buttonSize, buttonSize);
          ctx.strokeStyle = '#ffffff';
          ctx.lineWidth = 2;
          ctx.strokeRect(buttonX + 5, buttonY + 5, buttonSize - 10, buttonSize - 10);
        } else {
          // Start button
          ctx.fillStyle = 'rgba(34, 197, 94, 0.9)';
          ctx.beginPath();
          ctx.arc(buttonX + buttonSize/2, buttonY + buttonSize/2, buttonSize/2, 0, Math.PI * 2);
          ctx.fill();
          
          ctx.fillStyle = '#ffffff';
          ctx.beginPath();
          ctx.moveTo(buttonX + buttonSize/2 - 4, buttonY + buttonSize/2 - 5);
          ctx.lineTo(buttonX + buttonSize/2 - 4, buttonY + buttonSize/2 + 5);
          ctx.lineTo(buttonX + buttonSize/2 + 6, buttonY + buttonSize/2);
          ctx.closePath();
          ctx.fill();
        }
      }
      
      // Draw ports
      const drawPort = (x, y, isInput, isHovered) => {
        ctx.fillStyle = isHovered ? '#3b82f6' : (isInput ? '#f59e0b' : '#10b981');
        ctx.beginPath();
        ctx.arc(block.x + x, block.y + y, 6, 0, Math.PI * 2);
        ctx.fill();
        
        ctx.fillStyle = '#ffffff';
        ctx.beginPath();
        ctx.arc(block.x + x, block.y + y, 3, 0, Math.PI * 2);
        ctx.fill();
      };
      
      block.portPositions.inputs.forEach((port, idx) => {
        const isHovered = hoveredConnectionPoint?.blockId === block.id && 
                         hoveredConnectionPoint?.type === 'input' && 
                         hoveredConnectionPoint?.index === idx;
        drawPort(port.x, port.y, true, isHovered);
      });
      
      block.portPositions.outputs.forEach((port, idx) => {
        const isHovered = hoveredConnectionPoint?.blockId === block.id && 
                         hoveredConnectionPoint?.type === 'output' && 
                         hoveredConnectionPoint?.index === idx;
        drawPort(port.x, port.y, false, isHovered);
      });
    });
  }, [
    blocks,
    connections,
    selectedBlocks,
    selectedConnections,
    currentConnection,
    hoveredConnectionPoint,
    blockProcesses,
    blockStatus,
    hoveredWaypoint,
    draggingWaypoint,
    hoveredConnection,
    blockMetrics,
    selectionBox,
    mousePosition,
    hoveredBlock,
    serverRunning
  ]);

  return null;
};
