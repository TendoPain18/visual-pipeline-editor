import React, { useEffect, useRef } from 'react';
import { X } from 'lucide-react';

export const GraphWindow = ({ blockId, blockName, graphType, data, onClose }) => {
  const canvasRef = useRef(null);
  const [position, setPosition] = React.useState({ x: 100, y: 100 });
  const [isDragging, setIsDragging] = React.useState(false);
  const [dragOffset, setDragOffset] = React.useState({ x: 0, y: 0 });

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
    ctx.fillText('X Value', width / 2, height - 10);
    
    // Y-axis label
    ctx.save();
    ctx.translate(15, height / 2);
    ctx.rotate(-Math.PI / 2);
    ctx.fillText('Y Value', 0, 0);
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
      ctx.fillText(x.toFixed(1), screenX, height - padding.bottom + 18);
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

    // Draw data points
    if (graphType === 'scatter') {
      ctx.fillStyle = '#3b82f6';
      data.xData.forEach((x, i) => {
        const y = data.yData[i];
        const screenX = xScale(x);
        const screenY = yScale(y);
        
        ctx.beginPath();
        ctx.arc(screenX, screenY, 4, 0, Math.PI * 2);
        ctx.fill();
      });
    } else if (graphType === 'line') {
      ctx.strokeStyle = '#3b82f6';
      ctx.lineWidth = 2;
      ctx.beginPath();
      
      data.xData.forEach((x, i) => {
        const y = data.yData[i];
        const screenX = xScale(x);
        const screenY = yScale(y);
        
        if (i === 0) {
          ctx.moveTo(screenX, screenY);
        } else {
          ctx.lineTo(screenX, screenY);
        }
      });
      ctx.stroke();

      // Also draw points
      ctx.fillStyle = '#3b82f6';
      data.xData.forEach((x, i) => {
        const y = data.yData[i];
        const screenX = xScale(x);
        const screenY = yScale(y);
        
        ctx.beginPath();
        ctx.arc(screenX, screenY, 3, 0, Math.PI * 2);
        ctx.fill();
      });
    }

    // Draw info text
    ctx.fillStyle = '#666';
    ctx.font = '11px sans-serif';
    ctx.textAlign = 'right';
    ctx.fillText(`Points: ${data.xData.length}`, width - 10, height - padding.bottom + 35);

  }, [data, graphType]);

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
          width={500}
          height={400}
          style={{ border: '1px solid #ddd', borderRadius: '4px' }}
        />
      </div>
    </div>
  );
};