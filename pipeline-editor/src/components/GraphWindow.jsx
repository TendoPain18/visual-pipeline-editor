import React, { useEffect, useRef, useState } from 'react';
import { X } from 'lucide-react';

export const GraphWindow = ({ blockId, blockName, graphType, data, onClose }) => {
  const canvasRef = useRef(null);
  const [position, setPosition] = useState({ x: 100, y: 100 });
  const [isDragging, setIsDragging] = useState(false);
  const [dragOffset, setDragOffset] = useState({ x: 0, y: 0 });

  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas) return;

    const ctx = canvas.getContext('2d');
    const width = canvas.width;
    const height = canvas.height;

    // Clear canvas on every render — this erases the previous batch
    ctx.clearRect(0, 0, width, height);

    // Draw background
    ctx.fillStyle = '#ffffff';
    ctx.fillRect(0, 0, width, height);

    // Draw light grid
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

    if (graphType === 'scatter') {
      // ----------------------------------------------------------------
      // CONSTELLATION DIAGRAM
      // Draws all points in data.xData / data.yData as-is.
      // The parent (App.jsx) REPLACES the arrays each batch — no
      // accumulation happens here, and there is no maxPoints cap.
      // ----------------------------------------------------------------

      const points = data.xData.map((x, i) => ({ x, y: data.yData[i] }));

      // Find max absolute value for symmetric axes.
      // Do NOT use Math.max(...largeArray) — spread blows the JS call stack
      // when the batch contains tens of thousands of points.
      let maxAbs = 0;
      for (let i = 0; i < points.length; i++) {
        const ax = Math.abs(points[i].x);
        const ay = Math.abs(points[i].y);
        if (ax > maxAbs) maxAbs = ax;
        if (ay > maxAbs) maxAbs = ay;
      }
      if (maxAbs === 0) maxAbs = 1;
      const axisRange = maxAbs * 1.5;

      // Center of plot area
      const centerX = padding.left + plotWidth / 2;
      const centerY = padding.top + plotHeight / 2;

      // Scale: pixels per unit
      const scale = Math.min(plotWidth, plotHeight) / (2 * axisRange);

      const xScale = (x) => centerX + x * scale;
      const yScale = (y) => centerY - y * scale;

      // Grid lines
      ctx.strokeStyle = '#e0e0e0';
      ctx.lineWidth = 1;
      const gridStep = axisRange / 5;
      for (let i = -5; i <= 5; i++) {
        const val = i * gridStep;
        ctx.beginPath();
        ctx.moveTo(xScale(val), padding.top);
        ctx.lineTo(xScale(val), height - padding.bottom);
        ctx.stroke();
        ctx.beginPath();
        ctx.moveTo(padding.left, yScale(val));
        ctx.lineTo(width - padding.right, yScale(val));
        ctx.stroke();
      }

      // Center axes
      ctx.strokeStyle = '#333';
      ctx.lineWidth = 2;
      ctx.beginPath();
      ctx.moveTo(padding.left, centerY);
      ctx.lineTo(width - padding.right, centerY);
      ctx.stroke();
      ctx.beginPath();
      ctx.moveTo(centerX, padding.top);
      ctx.lineTo(centerX, height - padding.bottom);
      ctx.stroke();

      // Axis labels
      ctx.fillStyle = '#333';
      ctx.font = '12px sans-serif';
      ctx.textAlign = 'center';
      ctx.fillText('In-phase (I)', width / 2, height - 10);
      ctx.save();
      ctx.translate(15, height / 2);
      ctx.rotate(-Math.PI / 2);
      ctx.fillText('Quadrature (Q)', 0, 0);
      ctx.restore();

      // Tick marks and values
      ctx.font = '10px sans-serif';
      for (let i = -5; i <= 5; i++) {
        if (i === 0) continue;
        const val = i * gridStep;

        // X-axis ticks
        ctx.strokeStyle = '#333';
        ctx.lineWidth = 1;
        ctx.beginPath();
        ctx.moveTo(xScale(val), centerY - 5);
        ctx.lineTo(xScale(val), centerY + 5);
        ctx.stroke();
        ctx.fillStyle = '#333';
        ctx.textAlign = 'center';
        ctx.fillText(val.toFixed(1), xScale(val), centerY + 18);

        // Y-axis ticks
        ctx.beginPath();
        ctx.moveTo(centerX - 5, yScale(val));
        ctx.lineTo(centerX + 5, yScale(val));
        ctx.stroke();
        ctx.textAlign = 'right';
        ctx.fillText(val.toFixed(1), centerX - 8, yScale(val) + 4);
      }

      // Origin marker
      ctx.fillStyle = '#333';
      ctx.beginPath();
      ctx.arc(centerX, centerY, 3, 0, Math.PI * 2);
      ctx.fill();

      // Draw all points — no cap, no rolling window
      ctx.fillStyle = 'rgba(59, 130, 246, 0.6)';
      for (let i = 0; i < points.length; i++) {
        const screenX = xScale(points[i].x);
        const screenY = yScale(points[i].y);
        ctx.beginPath();
        ctx.arc(screenX, screenY, 2, 0, Math.PI * 2);
        ctx.fill();
      }

      // Info text
      ctx.fillStyle = '#666';
      ctx.font = '11px sans-serif';
      ctx.textAlign = 'right';
      ctx.fillText(`Points: ${points.length}`, width - 10, 25);

    } else if (graphType === 'line') {
      // LINE PLOT — unchanged from original
      let xMin = data.xData[0], xMax = data.xData[0];
      let yMin = data.yData[0], yMax = data.yData[0];
      for (let i = 1; i < data.xData.length; i++) {
        if (data.xData[i] < xMin) xMin = data.xData[i];
        if (data.xData[i] > xMax) xMax = data.xData[i];
      }
      for (let i = 1; i < data.yData.length; i++) {
        if (data.yData[i] < yMin) yMin = data.yData[i];
        if (data.yData[i] > yMax) yMax = data.yData[i];
      }

      const xRange = xMax - xMin || 1;
      const yRange = yMax - yMin || 1;
      const xPadding = xRange * 0.1;
      const yPadding = yRange * 0.1;

      const xScale = (x) => padding.left + ((x - (xMin - xPadding)) / (xRange + 2 * xPadding)) * plotWidth;
      const yScale = (y) => height - padding.bottom - ((y - (yMin - yPadding)) / (yRange + 2 * yPadding)) * plotHeight;

      // Axes
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

      // Axis labels
      ctx.fillStyle = '#333';
      ctx.font = '12px sans-serif';
      ctx.textAlign = 'center';
      ctx.fillText('Time (samples)', width / 2, height - 10);
      ctx.save();
      ctx.translate(15, height / 2);
      ctx.rotate(-Math.PI / 2);
      ctx.fillText('Speed (rad/s)', 0, 0);
      ctx.restore();

      // Ticks
      ctx.font = '10px sans-serif';
      const numTicks = 5;
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

      // Two series
      const combined = data.xData.map((x, i) => ({ x, y: data.yData[i] }));
      const refData = [];
      const measData = [];
      combined.forEach(point => {
        if (Math.abs(point.x - Math.round(point.x)) < 0.01) {
          refData.push(point);
        } else {
          measData.push({ x: Math.round(point.x - 0.1), y: point.y });
        }
      });

      if (refData.length > 0) {
        ctx.strokeStyle = '#3b82f6';
        ctx.lineWidth = 2;
        ctx.beginPath();
        refData.forEach((point, i) => {
          const sx = xScale(point.x);
          const sy = yScale(point.y);
          i === 0 ? ctx.moveTo(sx, sy) : ctx.lineTo(sx, sy);
        });
        ctx.stroke();
      }

      if (measData.length > 0) {
        ctx.strokeStyle = '#10b981';
        ctx.lineWidth = 2;
        ctx.setLineDash([5, 3]);
        ctx.beginPath();
        measData.forEach((point, i) => {
          const sx = xScale(point.x);
          const sy = yScale(point.y);
          i === 0 ? ctx.moveTo(sx, sy) : ctx.lineTo(sx, sy);
        });
        ctx.stroke();
        ctx.setLineDash([]);
      }

      // Legend
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

      ctx.fillStyle = '#666';
      ctx.font = '11px sans-serif';
      ctx.textAlign = 'right';
      ctx.fillText(`Points: ${data.xData.length}`, width - 10, height - padding.bottom + 35);
    }

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
          width={600}
          height={400}
          style={{ border: '1px solid #ddd', borderRadius: '4px' }}
        />
      </div>
    </div>
  );
};