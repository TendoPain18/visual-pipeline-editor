import React, { useEffect, useRef, useState } from 'react';
import { X } from 'lucide-react';

export const GraphWindow = ({ blockId, blockName, graphType, data, onClose, maxPoints = 15000 }) => {
  const canvasRef = useRef(null);
  const [position, setPosition] = useState({ x: 100, y: 100 });
  const [isDragging, setIsDragging] = useState(false);
  const [dragOffset, setDragOffset] = useState({ x: 0, y: 0 });
  
  // Rolling buffer for on-the-fly plotting
  const pointBufferRef = useRef([]);
  const bufferIndexRef = useRef(0);

  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas) return;

    const ctx = canvas.getContext('2d');
    const width = canvas.width;
    const height = canvas.height;

    // Clear canvas
    ctx.clearRect(0, 0, width, height);

    // Draw background
    ctx.fillStyle = '#ffffff';
    ctx.fillRect(0, 0, width, height);

    // Draw grid
    ctx.strokeStyle = '#f0f0f0';
    ctx.lineWidth = 1;
    for (let x = 0; x <= width; x += 40) {
      ctx.beginPath();
      ctx.moveTo(x, 0);
      ctx.lineTo(x, height);
      ctx.stroke();
    }
    for (let y = 0; y <= height; y += 40) {
      ctx.beginPath();
      ctx.moveTo(0, y);
      ctx.lineTo(width, y);
      ctx.stroke();
    }

    if (!data.xData || data.xData.length === 0) {
      ctx.fillStyle = '#666';
      ctx.font = '16px sans-serif';
      ctx.textAlign = 'center';
      ctx.fillText('Waiting for data...', width / 2, height / 2);
      return;
    }

    // Calculate plot area
    const padding = { left: 60, right: 40, top: 40, bottom: 60 };
    const plotWidth = width - padding.left - padding.right;
    const plotHeight = height - padding.top - padding.bottom;

    // Different rendering for scatter (constellation) vs line plots
    if (graphType === 'scatter') {
      // CONSTELLATION DIAGRAM - centered axes at (0,0)
      // ON-THE-FLY PLOTTING with rolling window
      
      // Add new points to rolling buffer
      for (let i = 0; i < data.xData.length; i++) {
        const newPoint = { x: data.xData[i], y: data.yData[i] };
        
        if (pointBufferRef.current.length < maxPoints) {
          // Buffer not full - just append
          pointBufferRef.current.push(newPoint);
        } else {
          // Buffer full - replace oldest (circular buffer)
          pointBufferRef.current[bufferIndexRef.current] = newPoint;
          bufferIndexRef.current = (bufferIndexRef.current + 1) % maxPoints;
        }
      }
      
      // Use buffered points for rendering
      const points = pointBufferRef.current;
      
      if (points.length === 0) {
        ctx.fillStyle = '#666';
        ctx.font = '16px sans-serif';
        ctx.textAlign = 'center';
        ctx.fillText('Accumulating points...', width / 2, height / 2);
        return;
      }
      
      // Find max absolute value for symmetric axes
      const maxAbsX = Math.max(...points.map(p => Math.abs(p.x)));
      const maxAbsY = Math.max(...points.map(p => Math.abs(p.y)));
      const maxAbs = Math.max(maxAbsX, maxAbsY) || 1;
      
      // Add 10% padding
      const axisRange = maxAbs * 1.5;
      
      // Center of plot area
      const centerX = padding.left + plotWidth / 2;
      const centerY = padding.top + plotHeight / 2;
      
      // Scale factor: pixels per unit
      const scale = Math.min(plotWidth, plotHeight) / (2 * axisRange);
      
      const xScale = (x) => centerX + x * scale;
      const yScale = (y) => centerY - y * scale;
      
      // Draw grid lines
      ctx.strokeStyle = '#e0e0e0';
      ctx.lineWidth = 1;
      const gridStep = axisRange / 5;
      
      for (let i = -5; i <= 5; i++) {
        const val = i * gridStep;
        const screenX = xScale(val);
        const screenY = yScale(val);
        
        // Vertical grid lines
        ctx.beginPath();
        ctx.moveTo(screenX, padding.top);
        ctx.lineTo(screenX, height - padding.bottom);
        ctx.stroke();
        
        // Horizontal grid lines
        ctx.beginPath();
        ctx.moveTo(padding.left, screenY);
        ctx.lineTo(width - padding.right, screenY);
        ctx.stroke();
      }
      
      // Draw CENTER axes (I and Q)
      ctx.strokeStyle = '#333';
      ctx.lineWidth = 2;
      
      // Horizontal axis (I-axis)
      ctx.beginPath();
      ctx.moveTo(padding.left, centerY);
      ctx.lineTo(width - padding.right, centerY);
      ctx.stroke();
      
      // Vertical axis (Q-axis)
      ctx.beginPath();
      ctx.moveTo(centerX, padding.top);
      ctx.lineTo(centerX, height - padding.bottom);
      ctx.stroke();
      
      // Draw axis labels
      ctx.fillStyle = '#333';
      ctx.font = '12px sans-serif';
      ctx.textAlign = 'center';
      
      // I-axis label (bottom)
      ctx.fillText('In-phase (I)', width / 2, height - 10);
      
      // Q-axis label (left side, rotated)
      ctx.save();
      ctx.translate(15, height / 2);
      ctx.rotate(-Math.PI / 2);
      ctx.fillText('Quadrature (Q)', 0, 0);
      ctx.restore();
      
      // Draw tick marks and values
      ctx.font = '10px sans-serif';
      
      for (let i = -5; i <= 5; i++) {
        const val = i * gridStep;
        const screenX = xScale(val);
        const screenY = yScale(val);
        
        // Skip center ticks (would overlap with axes)
        if (i !== 0) {
          // X-axis ticks
          ctx.strokeStyle = '#333';
          ctx.beginPath();
          ctx.moveTo(screenX, centerY - 5);
          ctx.lineTo(screenX, centerY + 5);
          ctx.stroke();
          
          ctx.fillStyle = '#333';
          ctx.textAlign = 'center';
          ctx.fillText(val.toFixed(1), screenX, centerY + 18);
          
          // Y-axis ticks
          ctx.strokeStyle = '#333';
          ctx.beginPath();
          ctx.moveTo(centerX - 5, screenY);
          ctx.lineTo(centerX + 5, screenY);
          ctx.stroke();
          
          ctx.fillStyle = '#333';
          ctx.textAlign = 'right';
          ctx.fillText(val.toFixed(1), centerX - 8, screenY + 4);
        }
      }
      
      // Draw origin marker
      ctx.fillStyle = '#333';
      ctx.beginPath();
      ctx.arc(centerX, centerY, 3, 0, Math.PI * 2);
      ctx.fill();
      
      // Draw constellation points from rolling buffer
      ctx.fillStyle = 'rgba(59, 130, 246, 0.6)';
      points.forEach(point => {
        const screenX = xScale(point.x);
        const screenY = yScale(point.y);
        
        ctx.beginPath();
        ctx.arc(screenX, screenY, 2, 0, Math.PI * 2);
        ctx.fill();
      });
      
      // Draw info text
      ctx.fillStyle = '#666';
      ctx.font = '11px sans-serif';
      ctx.textAlign = 'right';
      ctx.fillText(`Points: ${points.length}/${maxPoints}`, width - 10, 25);
      
    } else if (graphType === 'line') {
      // LINE PLOT - traditional axes at bottom/left
      
      // Find data ranges
      const xMin = Math.min(...data.xData);
      const xMax = Math.max(...data.xData);
      const yMin = Math.min(...data.yData);
      const yMax = Math.max(...data.yData);

      // Add padding to ranges
      const xRange = xMax - xMin || 1;
      const yRange = yMax - yMin || 1;
      const xPadding = xRange * 0.1;
      const yPadding = yRange * 0.1;

      const xScale = (x) => padding.left + ((x - (xMin - xPadding)) / (xRange + 2 * xPadding)) * plotWidth;
      const yScale = (y) => height - padding.bottom - ((y - (yMin - yPadding)) / (yRange + 2 * yPadding)) * plotHeight;

      // Draw axes
      ctx.strokeStyle = '#333';
      ctx.lineWidth = 2;
      ctx.beginPath();
      ctx.moveTo(padding.left, height - padding.bottom);
      ctx.lineTo(width - padding.right, height - padding.bottom);
      ctx.stroke();

      ctx.beginPath();
      ctx.moveTo(padding.left, padding.top);
      ctx.lineTo(padding.left, height - padding.bottom);
      ctx.stroke();

      // Draw axis labels
      ctx.fillStyle = '#333';
      ctx.font = '12px sans-serif';
      ctx.textAlign = 'center';
      
      // X-axis label
      ctx.fillText('Time (samples)', width / 2, height - 10);
      
      // Y-axis label
      ctx.save();
      ctx.translate(15, height / 2);
      ctx.rotate(-Math.PI / 2);
      ctx.fillText('Speed (rad/s)', 0, 0);
      ctx.restore();

      // Draw tick marks and values
      ctx.font = '10px sans-serif';
      const numTicks = 5;
      
      // X-axis ticks
      for (let i = 0; i <= numTicks; i++) {
        const x = xMin - xPadding + ((xRange + 2 * xPadding) * i) / numTicks;
        const screenX = xScale(x);
        
        ctx.strokeStyle = '#333';
        ctx.beginPath();
        ctx.moveTo(screenX, height - padding.bottom);
        ctx.lineTo(screenX, height - padding.bottom + 5);
        ctx.stroke();
        
        ctx.fillStyle = '#333';
        ctx.textAlign = 'center';
        ctx.fillText(x.toFixed(0), screenX, height - padding.bottom + 18);
      }

      // Y-axis ticks
      for (let i = 0; i <= numTicks; i++) {
        const y = yMin - yPadding + ((yRange + 2 * yPadding) * i) / numTicks;
        const screenY = yScale(y);
        
        ctx.strokeStyle = '#333';
        ctx.beginPath();
        ctx.moveTo(padding.left - 5, screenY);
        ctx.lineTo(padding.left, screenY);
        ctx.stroke();
        
        ctx.fillStyle = '#333';
        ctx.textAlign = 'right';
        ctx.fillText(y.toFixed(1), padding.left - 8, screenY + 4);
      }
      
      // Draw line plot with TWO series (reference and measured)
      const combined = data.xData.map((x, i) => ({ x, y: data.yData[i] }));
      
      // Separate into reference and measured based on x value pattern
      const refData = [];
      const measData = [];
      
      combined.forEach(point => {
        if (Math.abs(point.x - Math.round(point.x)) < 0.01) {
          refData.push(point);
        } else {
          measData.push({ x: Math.round(point.x - 0.1), y: point.y });
        }
      });
      
      // Draw reference speed (blue line)
      if (refData.length > 0) {
        ctx.strokeStyle = '#3b82f6';
        ctx.lineWidth = 2;
        ctx.beginPath();
        
        refData.forEach((point, i) => {
          const screenX = xScale(point.x);
          const screenY = yScale(point.y);
          
          if (i === 0) {
            ctx.moveTo(screenX, screenY);
          } else {
            ctx.lineTo(screenX, screenY);
          }
        });
        ctx.stroke();
      }
      
      // Draw measured speed (green line)
      if (measData.length > 0) {
        ctx.strokeStyle = '#10b981';
        ctx.lineWidth = 2;
        ctx.setLineDash([5, 3]);
        ctx.beginPath();
        
        measData.forEach((point, i) => {
          const screenX = xScale(point.x);
          const screenY = yScale(point.y);
          
          if (i === 0) {
            ctx.moveTo(screenX, screenY);
          } else {
            ctx.lineTo(screenX, screenY);
          }
        });
        ctx.stroke();
        ctx.setLineDash([]);
      }
      
      // Draw legend
      ctx.font = '11px sans-serif';
      ctx.textAlign = 'left';
      
      ctx.fillStyle = '#3b82f6';
      ctx.fillRect(width - 150, 20, 20, 3);
      ctx.fillStyle = '#333';
      ctx.fillText('Reference Speed', width - 125, 25);
      
      ctx.strokeStyle = '#10b981';
      ctx.lineWidth = 3;
      ctx.setLineDash([5, 3]);
      ctx.beginPath();
      ctx.moveTo(width - 150, 40);
      ctx.lineTo(width - 130, 40);
      ctx.stroke();
      ctx.setLineDash([]);
      ctx.fillStyle = '#333';
      ctx.fillText('Measured Speed', width - 125, 43);
      
      // Draw info text
      ctx.fillStyle = '#666';
      ctx.font = '11px sans-serif';
      ctx.textAlign = 'right';
      ctx.fillText(`Points: ${data.xData.length}`, width - 10, height - padding.bottom + 35);
    }

  }, [data, graphType, maxPoints]);

  const handleMouseDown = (e) => {
    if (e.target.closest('.graph-close-button')) return;
    setIsDragging(true);
    setDragOffset({
      x: e.clientX - position.x,
      y: e.clientY - position.y
    });
  };

  const handleMouseMove = (e) => {
    if (isDragging) {
      setPosition({
        x: e.clientX - dragOffset.x,
        y: e.clientY - dragOffset.y
      });
    }
  };

  const handleMouseUp = () => {
    setIsDragging(false);
  };

  useEffect(() => {
    if (isDragging) {
      window.addEventListener('mousemove', handleMouseMove);
      window.addEventListener('mouseup', handleMouseUp);
      return () => {
        window.removeEventListener('mousemove', handleMouseMove);
        window.removeEventListener('mouseup', handleMouseUp);
      };
    }
  }, [isDragging, dragOffset]);

  return (
    <div
      style={{
        position: 'fixed',
        left: position.x,
        top: position.y,
        zIndex: 1000,
        background: 'white',
        borderRadius: '8px',
        boxShadow: '0 4px 20px rgba(0,0,0,0.3)',
        cursor: isDragging ? 'grabbing' : 'grab'
      }}
      onMouseDown={handleMouseDown}
    >
      <div className="flex items-center justify-between p-2 bg-gradient-to-r from-purple-600 to-blue-600 rounded-t-lg">
        <h3 className="text-white font-semibold text-sm">
          📊 {blockName} - {graphType}
        </h3>
        <button
          className="graph-close-button text-white hover:text-gray-200 p-1"
          onClick={onClose}
          onMouseDown={(e) => e.stopPropagation()}
        >
          <X size={16} />
        </button>
      </div>
      <div className="p-2">
        <canvas
          ref={canvasRef}
          width={600}
          height={400}
          style={{ border: '1px solid #ddd', borderRadius: '4px' }}
        />
      </div>
    </div>
  );
};