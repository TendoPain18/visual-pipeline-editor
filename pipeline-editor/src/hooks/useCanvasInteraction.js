import { useState } from 'react';

export const useCanvasInteraction = (GRID_SIZE) => {
  const [draggingBlock, setDraggingBlock] = useState(null);
  const [dragOffset, setDragOffset] = useState({ x: 0, y: 0 });
  const [currentConnection, setCurrentConnection] = useState(null);
  const [mousePosition, setMousePosition] = useState(null);
  const [hoveredConnectionPoint, setHoveredConnectionPoint] = useState(null);
  const [draggingWaypoint, setDraggingWaypoint] = useState(null);
  const [hoveredWaypoint, setHoveredWaypoint] = useState(null);
  const [hoveredConnection, setHoveredConnection] = useState(null);
  const [hoveredBlock, setHoveredBlock] = useState(null);
  const [selectionBox, setSelectionBox] = useState(null);
  const [isSelecting, setIsSelecting] = useState(false);
  const [selectionStart, setSelectionStart] = useState(null);

  const snapToGrid = (value) => Math.round(value / GRID_SIZE) * GRID_SIZE;

  const resetInteractionState = () => {
    setDraggingBlock(null);
    setDraggingWaypoint(null);
    setIsSelecting(false);
    setSelectionBox(null);
    setSelectionStart(null);
  };

  return {
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
    snapToGrid,
    resetInteractionState
  };
};
