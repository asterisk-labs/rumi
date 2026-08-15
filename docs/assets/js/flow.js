// Shows a frame lookup and read.

(function (rumi) {
  'use strict';

  var FRAME_COUNT = 16;
  var CYCLE = 3600;
  var SEQUENCE = [7, 14, 4, 15, 11];
  var WEIGHTS = [
    1.0, 0.72, 1.28, 0.9, 1.14, 0.68, 1.22, 0.84,
    1.08, 0.76, 1.32, 0.94, 0.7, 1.18, 0.86, 1.26
  ];

  function pointOnQuadratic(from, control, to, value) {
    var inverse = 1 - value;
    return {
      x: inverse * inverse * from.x + 2 * inverse * value * control.x + value * value * to.x,
      y: inverse * inverse * from.y + 2 * inverse * value * control.y + value * value * to.y
    };
  }

  function ease(value) {
    return value < 0.5
      ? 2 * value * value
      : 1 - Math.pow(-2 * value + 2, 2) / 2;
  }

  rumi.flow = function (canvas) {
    if (!canvas || canvas.dataset.flowReady) return;
    canvas.dataset.flowReady = 'true';

    var context = canvas.getContext('2d');
    var figure = canvas.closest('.rumi-flow');
    var caption = figure.querySelector('[data-flow-step]');
    var reduced = window.matchMedia('(prefers-reduced-motion: reduce)').matches;
    var started = performance.now();
    var measuredWidth = 0;
    var measuredHeight = 0;
    var lastCaption = '';
    var observer;

    function measure() {
      var rect = canvas.getBoundingClientRect();
      var dpr = Math.min(window.devicePixelRatio || 1, 2);

      measuredWidth = Math.max(1, Math.round(rect.width));
      measuredHeight = Math.max(1, Math.round(rect.height));
      canvas.width = Math.round(measuredWidth * dpr);
      canvas.height = Math.round(measuredHeight * dpr);
      context.setTransform(dpr, 0, 0, dpr, 0, 0);
    }

    function label(text, x, y, color, size, align) {
      context.fillStyle = color;
      context.font = '600 ' + size + 'px "SFMono-Regular", "Cascadia Code", Consolas, monospace';
      context.textAlign = align || 'left';
      context.textBaseline = 'alphabetic';
      context.fillText(text, x, y);
    }

    function line(fromX, fromY, toX, toY, color, width) {
      context.beginPath();
      context.moveTo(fromX, fromY);
      context.lineTo(toX, toY);
      context.strokeStyle = color;
      context.lineWidth = width || 1;
      context.stroke();
    }

    function panel(box, fill, stroke, accent) {
      context.fillStyle = fill;
      context.fillRect(box.x, box.y, box.width, box.height);
      context.strokeStyle = stroke;
      context.lineWidth = 1;
      context.strokeRect(box.x + 0.5, box.y + 0.5, box.width - 1, box.height - 1);
      context.fillStyle = accent;
      context.fillRect(box.x, box.y, 2, box.height);
    }

    function frameGeometry(x, width) {
      var total = WEIGHTS.reduce(function (sum, weight) { return sum + weight; }, 0);
      var positions = [];
      var cursor = x;

      WEIGHTS.forEach(function (weight) {
        var frameWidth = width * weight / total;
        positions.push({ x: cursor, width: frameWidth });
        cursor += frameWidth;
      });
      return positions;
    }

    function draw(now) {
      if (!canvas.isConnected) {
        if (observer) observer.disconnect();
        return;
      }

      if (!measuredWidth || !measuredHeight) measure();
      context.clearRect(0, 0, measuredWidth, measuredHeight);

      var elapsed = reduced ? CYCLE * 0.84 : now - started;
      var cycleIndex = Math.floor(elapsed / CYCLE) % SEQUENCE.length;
      var frameIndex = reduced ? 16 : SEQUENCE[cycleIndex];
      var progress = reduced ? 0.84 : (elapsed % CYCLE) / CYCLE;
      var compact = measuredWidth < 520;
      var pad = Math.max(12, Math.min(24, measuredWidth * 0.04));
      var ink = 'rgba(241, 245, 249, 0.92)';
      var soft = 'rgba(195, 204, 215, 0.72)';
      var faint = 'rgba(98, 109, 123, 0.72)';
      var rule = 'rgba(231, 237, 244, 0.12)';
      var violet = '#8f7cff';
      var gold = '#ffd54a';
      var mint = '#91f2bc';
      var ember = '#ff7a4f';

      var gridSize = compact
        ? Math.min(108, measuredWidth * 0.36)
        : Math.min(190, measuredWidth * 0.31, measuredHeight * 0.39);
      var grid = compact
        ? { x: pad, y: 54, width: gridSize, height: gridSize }
        : { x: pad, y: 86, width: gridSize, height: gridSize };
      var rail = compact
        ? { x: measuredWidth * 0.51, y: 78, width: measuredWidth * 0.45 }
        : { x: measuredWidth * 0.44, y: 122, width: measuredWidth * 0.52 };
      var fileY = compact
        ? 194
        : Math.min(measuredHeight - 122, Math.max(248, measuredHeight * 0.62));
      var stageY = measuredHeight - (compact ? 34 : 44);
      var headerPanel = {
        x: rail.x - (compact ? 8 : 12),
        y: rail.y - (compact ? 34 : 46),
        width: rail.width + (compact ? 16 : 24),
        height: compact ? 62 : 82
      };
      var filePanel = {
        x: rail.x - (compact ? 8 : 12),
        y: fileY - (compact ? 40 : 52),
        width: rail.width + (compact ? 16 : 24),
        height: compact ? 82 : 104
      };
      var columns = 4;
      var rows = 4;
      var gap = compact ? 3 : 4;
      var cellWidth = (grid.width - gap * (columns - 1)) / columns;
      var cellHeight = (grid.height - gap * (rows - 1)) / rows;
      var selectedColumn = frameIndex % columns;
      var selectedRow = Math.floor(frameIndex / columns);
      var selectedX = grid.x + selectedColumn * (cellWidth + gap);
      var selectedY = grid.y + selectedRow * (cellHeight + gap);

      label('RASTER / FRAME ' + String(frameIndex).padStart(2, '0'), grid.x, grid.y - 18, soft, compact ? 8 : 9);

      for (var cell = 0; cell < FRAME_COUNT; cell += 1) {
        var column = cell % columns;
        var row = Math.floor(cell / columns);
        var x = grid.x + column * (cellWidth + gap);
        var y = grid.y + row * (cellHeight + gap);
        var isSelected = cell === frameIndex;
        var pulse = 0.78 + Math.sin(now / 180) * 0.18;

        context.fillStyle = isSelected
          ? 'rgba(145, 242, 188, ' + pulse + ')'
          : 'rgba(116, 87, 255, ' + (0.035 + (cell % 5) * 0.012) + ')';
        context.fillRect(x, y, cellWidth, cellHeight);
        context.strokeStyle = isSelected ? mint : rule;
        context.lineWidth = isSelected ? 1.5 : 1;
        context.strokeRect(x + 0.5, y + 0.5, cellWidth - 1, cellHeight - 1);
      }

      panel(
        headerPanel,
        'rgba(145, 242, 188, 0.025)',
        'rgba(145, 242, 188, 0.2)',
        mint
      );
      label(
        'RUMI HEADER',
        headerPanel.x + (compact ? 8 : 12),
        headerPanel.y + (compact ? 17 : 21),
        mint,
        compact ? 8 : 9
      );
      label(
        'BYTE COUNTS',
        headerPanel.x + headerPanel.width - (compact ? 8 : 12),
        headerPanel.y + (compact ? 17 : 21),
        faint,
        compact ? 7 : 8,
        'right'
      );
      line(rail.x, rail.y, rail.x + rail.width, rail.y, 'rgba(145, 242, 188, 0.2)', 1);

      var headerPositions = [];
      for (var count = 0; count < FRAME_COUNT; count += 1) {
        var headerX = rail.x + rail.width * (count + 0.5) / FRAME_COUNT;
        var tickHeight = 8 + (count % 4) * 3;
        headerPositions.push(headerX);
        line(
          headerX,
          rail.y - tickHeight / 2,
          headerX,
          rail.y + tickHeight / 2,
          count === frameIndex && progress > 0.24 ? gold : rule,
          count === frameIndex && progress > 0.24 ? 3 : 1
        );
      }

      panel(
        filePanel,
        'rgba(116, 87, 255, 0.035)',
        'rgba(143, 124, 255, 0.22)',
        violet
      );
      label(
        'RUMI FILE',
        filePanel.x + (compact ? 8 : 12),
        filePanel.y + (compact ? 18 : 22),
        violet,
        compact ? 8 : 9
      );
      label(
        'OPENZL FRAMES',
        filePanel.x + filePanel.width - (compact ? 8 : 12),
        filePanel.y + (compact ? 18 : 22),
        ember,
        compact ? 7 : 8,
        'right'
      );

      var fixedWidth = rail.width * 0.15;
      var payloadX = rail.x + fixedWidth + 8;
      var payloadWidth = rail.width - fixedWidth - 8;
      context.fillStyle = violet;
      context.fillRect(rail.x, fileY - 4, fixedWidth * 0.22, 8);
      context.fillStyle = gold;
      context.fillRect(rail.x + fixedWidth * 0.22, fileY - 4, fixedWidth * 0.78, 8);
      label('IFD', rail.x, fileY + 23, faint, compact ? 7 : 8);

      var frames = frameGeometry(payloadX, payloadWidth);
      frames.forEach(function (frame, index) {
        var active = index === frameIndex && progress > 0.62;
        context.fillStyle = active ? ember : 'rgba(255, 122, 79, 0.12)';
        context.fillRect(frame.x + 1, fileY - (active ? 12 : 5), Math.max(1, frame.width - 2), active ? 24 : 10);
      });

      var tilePoint = {
        x: selectedX + cellWidth / 2,
        y: selectedY + cellHeight / 2
      };
      var headerPoint = { x: headerPositions[frameIndex], y: rail.y };
      var targetFrame = frames[frameIndex];
      var filePoint = { x: targetFrame.x + targetFrame.width / 2, y: fileY };
      var firstControl = {
        x: tilePoint.x + (headerPoint.x - tilePoint.x) * 0.55,
        y: tilePoint.y - (compact ? 44 : 72)
      };

      context.save();
      context.setLineDash([4, 7]);
      context.beginPath();
      context.moveTo(tilePoint.x, tilePoint.y);
      context.quadraticCurveTo(firstControl.x, firstControl.y, headerPoint.x, headerPoint.y);
      context.strokeStyle = progress > 0.1 ? 'rgba(145, 242, 188, 0.38)' : 'rgba(145, 242, 188, 0.08)';
      context.lineWidth = 1;
      context.stroke();
      context.restore();

      line(
        headerPoint.x,
        headerPoint.y + 10,
        filePoint.x,
        filePoint.y - 14,
        progress > 0.48 ? 'rgba(255, 213, 74, 0.42)' : 'rgba(255, 213, 74, 0.08)',
        1
      );

      var moving;
      if (progress < 0.5) {
        moving = pointOnQuadratic(tilePoint, firstControl, headerPoint, ease(Math.max(0, (progress - 0.08) / 0.42)));
      } else {
        var second = ease(Math.min(1, (progress - 0.5) / 0.36));
        moving = {
          x: headerPoint.x + (filePoint.x - headerPoint.x) * second,
          y: headerPoint.y + (filePoint.y - headerPoint.y) * second
        };
      }

      context.beginPath();
      context.arc(moving.x, moving.y, compact ? 3 : 4, 0, Math.PI * 2);
      context.fillStyle = progress < 0.5 ? mint : gold;
      context.fill();

      var activeStage = progress < 0.28 ? 0 : progress < 0.62 ? 1 : 2;
      var stages = compact
        ? ['REQUEST', 'LOCATE', 'FETCH']
        : ['01  REQUEST FRAME', '02  LOCATE FROM RUMI HEADER', '03  FETCH OPENZL FRAME'];
      var stageWidth = (measuredWidth - pad * 2) / stages.length;
      stages.forEach(function (stage, index) {
        label(
          stage,
          pad + stageWidth * index,
          stageY,
          index === activeStage ? ink : faint,
          compact ? 7 : 8
        );
      });

      var captionText = activeStage === 0
        ? 'request frame ' + String(frameIndex).padStart(2, '0')
        : activeStage === 1
          ? 'reconstruct its byte offset'
          : 'fetch OpenZL frame ' + String(frameIndex).padStart(2, '0');
      if (captionText !== lastCaption) {
        caption.textContent = captionText;
        lastCaption = captionText;
      }

      if (!reduced) requestAnimationFrame(draw);
    }

    if ('ResizeObserver' in window) {
      observer = new ResizeObserver(function () {
        measure();
        if (reduced) draw(performance.now());
      });
      observer.observe(canvas);
    } else {
      window.addEventListener('resize', measure);
    }

    measure();
    requestAnimationFrame(draw);
  };
})((window.rumi = window.rumi || {}));
