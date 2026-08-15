// Shows a catalog query resolving a byte range through a stored rumi header.

(function (rumi) {
  'use strict';

  var ROW_COUNT = 5;
  var CYCLE = 6000;
  var ROW_SEQUENCE = [2, 4, 1, 3, 0];
  var FRAME_RANGES = [[1, 2], [4], [6, 7, 8], [10, 11], [12]];
  var FRAME_WEIGHTS = [
    0.82, 1.16, 0.94, 1.28, 0.76, 1.08, 1.22,
    0.88, 1.3, 0.72, 1.12, 0.9, 1.24, 0.84
  ];
  var WINDOWS = [
    { y: [0, 512], x: [0, 512] },
    { y: [512, 1024], x: [0, 512] },
    { y: [0, 512], x: [512, 1024] },
    { y: [256, 768], x: [256, 768] },
    { y: [512, 1024], x: [512, 1024] }
  ];

  function clamp(value) {
    return Math.max(0, Math.min(1, value));
  }

  function ease(value) {
    value = clamp(value);
    return value < 0.5
      ? 2 * value * value
      : 1 - Math.pow(-2 * value + 2, 2) / 2;
  }

  function fadeBetween(value, start, fadeInEnd, fadeOutStart, end) {
    return ease((value - start) / (fadeInEnd - start)) *
      (1 - ease((value - fadeOutStart) / (end - fadeOutStart)));
  }

  function pointOnQuadratic(from, control, to, value) {
    var inverse = 1 - value;
    return {
      x: inverse * inverse * from.x + 2 * inverse * value * control.x + value * value * to.x,
      y: inverse * inverse * from.y + 2 * inverse * value * control.y + value * value * to.y
    };
  }

  function mixColor(from, to, value, alpha) {
    value = clamp(value);
    return 'rgba(' + [
      Math.round(from[0] + (to[0] - from[0]) * value),
      Math.round(from[1] + (to[1] - from[1]) * value),
      Math.round(from[2] + (to[2] - from[2]) * value),
      alpha
    ].join(', ') + ')';
  }

  rumi.flow = function (canvas) {
    if (!canvas || canvas.dataset.flowReady) return;
    canvas.dataset.flowReady = 'true';

    var context = canvas.getContext('2d');
    var reduced = window.matchMedia('(prefers-reduced-motion: reduce)').matches;
    var started = performance.now();
    var measuredWidth = 0;
    var measuredHeight = 0;
    var observer;

    function measure() {
      var rect = canvas.getBoundingClientRect();
      var dpr = Math.min(window.devicePixelRatio || 1, 2);
      var nextWidth = Math.max(1, Math.round(rect.width));
      var nextHeight = Math.max(1, Math.round(rect.height));
      var pixelWidth = Math.round(nextWidth * dpr);
      var pixelHeight = Math.round(nextHeight * dpr);

      measuredWidth = nextWidth;
      measuredHeight = nextHeight;

      if (canvas.width !== pixelWidth || canvas.height !== pixelHeight) {
        canvas.width = pixelWidth;
        canvas.height = pixelHeight;
      }
      context.setTransform(dpr, 0, 0, dpr, 0, 0);
    }

    function roundedRect(x, y, width, height, radius) {
      var safeRadius = Math.min(radius, width / 2, height / 2);
      context.beginPath();
      context.moveTo(x + safeRadius, y);
      context.arcTo(x + width, y, x + width, y + height, safeRadius);
      context.arcTo(x + width, y + height, x, y + height, safeRadius);
      context.arcTo(x, y + height, x, y, safeRadius);
      context.arcTo(x, y, x + width, y, safeRadius);
      context.closePath();
    }

    function fillRoundedRect(x, y, width, height, radius, fill) {
      roundedRect(x, y, width, height, radius);
      context.fillStyle = fill;
      context.fill();
    }

    function strokeRoundedRect(x, y, width, height, radius, stroke, lineWidth) {
      roundedRect(x, y, width, height, radius);
      context.strokeStyle = stroke;
      context.lineWidth = lineWidth || 1;
      context.stroke();
    }

    function line(fromX, fromY, toX, toY, color, width) {
      context.beginPath();
      context.moveTo(fromX, fromY);
      context.lineTo(toX, toY);
      context.strokeStyle = color;
      context.lineWidth = width || 1;
      context.stroke();
    }

    function dot(x, y, radius, fill) {
      context.beginPath();
      context.arc(x, y, radius, 0, Math.PI * 2);
      context.fillStyle = fill;
      context.fill();
    }

    function label(text, x, y, color, size, align) {
      context.fillStyle = color;
      context.font = '600 ' + size + 'px "SFMono-Regular", "Cascadia Code", Consolas, monospace';
      context.textAlign = align || 'left';
      context.textBaseline = 'alphabetic';
      context.fillText(text, x, y);
    }

    function fittedLabel(text, x, y, color, preferredSize, minimumSize, maxWidth) {
      var size = preferredSize;
      context.font = '600 ' + size + 'px "SFMono-Regular", "Cascadia Code", Consolas, monospace';

      while (size > minimumSize && context.measureText(text).width > maxWidth) {
        size -= 0.25;
        context.font = '600 ' + size + 'px "SFMono-Regular", "Cascadia Code", Consolas, monospace';
      }

      label(text, x, y, color, size);
    }

    function drawQuery(box, selectedRow, window, active, contentAlpha, compact, colors, stageColor) {
      fillRoundedRect(box.x, box.y, box.width, box.height, 8, colors.panel);
      if (active > 0) {
        fillRoundedRect(
          box.x,
          box.y,
          box.width,
          box.height,
          8,
          'rgba(145, 242, 188, ' + (active * 0.045) + ')'
        );
      }
      strokeRoundedRect(box.x + 0.5, box.y + 0.5, box.width - 1, box.height - 1, 8, stageColor);

      var code = 'rumi.read(df[' + selectedRow + '].path, df[' + selectedRow + '].header, ' +
        'y=(' + window.y[0] + ', ' + window.y[1] + '), x=(' + window.x[0] + ', ' + window.x[1] + '))';
      var baseline = box.y + box.height / 2 + (compact ? 3 : 4);

      context.save();
      context.globalAlpha = contentAlpha;
      label('>', box.x + (compact ? 10 : 14), baseline, stageColor, compact ? 8 : 10);
      fittedLabel(
        code,
        box.x + (compact ? 24 : 31),
        baseline,
        colors.ink,
        compact ? 8.5 : 11.5,
        compact ? 6.5 : 8.5,
        box.width - (compact ? 33 : 43)
      );
      context.restore();
    }

    function drawDatabaseGlyph(x, y, width, height, color) {
      context.beginPath();
      context.ellipse(x + width / 2, y + height * 0.2, width / 2, height * 0.2, 0, 0, Math.PI * 2);
      context.strokeStyle = color;
      context.lineWidth = 1;
      context.stroke();
      context.beginPath();
      context.moveTo(x, y + height * 0.2);
      context.lineTo(x, y + height * 0.76);
      context.bezierCurveTo(x, y + height, x + width, y + height, x + width, y + height * 0.76);
      context.lineTo(x + width, y + height * 0.2);
      context.stroke();
      context.beginPath();
      context.moveTo(x, y + height * 0.48);
      context.bezierCurveTo(x, y + height * 0.7, x + width, y + height * 0.7, x + width, y + height * 0.48);
      context.stroke();
    }

    function drawFileGlyph(x, y, width, height, color) {
      var fold = Math.min(width, height) * 0.28;
      context.beginPath();
      context.moveTo(x + 0.5, y + 0.5);
      context.lineTo(x + width - fold, y + 0.5);
      context.lineTo(x + width - 0.5, y + fold);
      context.lineTo(x + width - 0.5, y + height - 0.5);
      context.lineTo(x + 0.5, y + height - 0.5);
      context.closePath();
      context.strokeStyle = color;
      context.lineWidth = 1;
      context.stroke();
      line(x + width - fold, y, x + width - fold, y + fold, color, 1);
      line(x + width - fold, y + fold, x + width, y + fold, color, 1);
    }

    function drawHeaderBytes(x, y, width, height, row, active, activeColor, colors) {
      var pattern = [0.18, 0.31, 0.14, 0.24, 0.12, 0.27, 0.2];
      var cursor = x;
      var usable = width - 6 * 2;

      pattern.forEach(function (weight, index) {
        var segmentWidth = Math.max(2, usable * weight * (0.72 + ((row + index) % 3) * 0.1));
        context.fillStyle = active
          ? (index < 2 ? activeColor : 'rgba(145, 242, 188, 0.24)')
          : (index < 2 ? colors.headerSoft : colors.headerFaint);
        context.fillRect(cursor, y, Math.min(segmentWidth, x + width - cursor), height);
        cursor += segmentWidth + 2;
      });
    }

    function frameGeometry(x, width) {
      var total = FRAME_WEIGHTS.reduce(function (sum, weight) { return sum + weight; }, 0);
      var positions = [];
      var cursor = x;

      FRAME_WEIGHTS.forEach(function (weight) {
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

      var elapsed = reduced ? CYCLE * 0.78 : Math.max(0, now - started);
      var cycleIndex = reduced ? 0 : Math.floor(elapsed / CYCLE) % ROW_SEQUENCE.length;
      var selectedRow = reduced ? 2 : ROW_SEQUENCE[cycleIndex];
      var progress = reduced ? 0.78 : (elapsed % CYCLE) / CYCLE;
      var compact = measuredWidth < 520;
      var colors = {
        ink: 'rgba(241, 245, 249, 0.82)',
        soft: 'rgba(195, 204, 215, 0.64)',
        faint: 'rgba(98, 109, 123, 0.54)',
        rule: 'rgba(231, 237, 244, 0.15)',
        edge: 'rgba(231, 237, 244, 0.22)',
        panel: 'rgba(17, 23, 33, 0.58)',
        mint: '#91f2bc',
        header: '#9aa1bd',
        headerSoft: 'rgba(154, 161, 189, 0.42)',
        headerFaint: 'rgba(154, 161, 189, 0.2)',
        headerRgb: [154, 161, 189],
        stageRgb: [124, 135, 149],
        mintRgb: [145, 242, 188]
      };
      var cyclePresence = fadeBetween(progress, 0, 0.045, 0.93, 1);
      var queryTransit = fadeBetween(progress, 0.12, 0.18, 0.35, 0.43);
      var headerTransit = fadeBetween(progress, 0.48, 0.54, 0.73, 0.81);
      var selectionStrength = ease((progress - 0.27) / 0.1) *
        (1 - ease((progress - 0.91) / 0.07));
      var queryActivity = (1 - ease((progress - 0.22) / 0.1)) * cyclePresence;
      var catalogActivity = ease((progress - 0.25) / 0.1) *
        (1 - ease((progress - 0.55) / 0.12));
      var fileActivity = ease((progress - 0.64) / 0.12) *
        (1 - ease((progress - 0.94) / 0.06));
      var frameReveal = ease((progress - 0.61) / 0.12) *
        (1 - ease((progress - 0.94) / 0.06));
      var queryColor = mixColor(colors.stageRgb, colors.mintRgb, queryActivity, 0.9);
      var catalogColor = mixColor(colors.stageRgb, colors.mintRgb, catalogActivity, 0.9);
      var fileColor = mixColor(colors.stageRgb, colors.mintRgb, fileActivity, 0.9);
      var headerSignalColor = mixColor(colors.headerRgb, colors.mintRgb, headerTransit, 1);
      var readWindow = WINDOWS[selectedRow];
      var queryBox = compact
        ? { x: measuredWidth * 0.07, y: 24, width: measuredWidth * 0.86, height: 46 }
        : { x: measuredWidth * 0.1, y: measuredHeight * 0.07, width: measuredWidth * 0.82, height: 54 };
      var table = compact
        ? { x: measuredWidth * 0.12, y: 112, width: measuredWidth * 0.76, height: 210 }
        : { x: measuredWidth * 0.17, y: measuredHeight * 0.27, width: measuredWidth * 0.7, height: measuredHeight * 0.37 };
      var tableHeaderHeight = compact ? 26 : 30;
      var rowHeight = (table.height - tableHeaderHeight) / ROW_COUNT;
      var columns = [0, 0.14, 0.45, 0.66, 1];
      var selectedRowY = table.y + tableHeaderHeight + selectedRow * rowHeight;
      var selectedRowCenter = selectedRowY + rowHeight / 2;

      label('USER QUERY', queryBox.x, queryBox.y - 10, queryColor, compact ? 8 : 11);
      drawQuery(queryBox, selectedRow, readWindow, queryActivity, cyclePresence, compact, colors, queryColor);

      label('IN-MEMORY CATALOG', table.x, table.y - 10, catalogColor, compact ? 8 : 11);
      fillRoundedRect(table.x, table.y, table.width, table.height, 4, 'rgba(17, 23, 33, 0.42)');
      strokeRoundedRect(table.x + 0.5, table.y + 0.5, table.width - 1, table.height - 1, 4, colors.edge);
      strokeRoundedRect(
        table.x + 0.5,
        table.y + 0.5,
        table.width - 1,
        table.height - 1,
        4,
        mixColor(colors.stageRgb, colors.mintRgb, catalogActivity, 0.18 + catalogActivity * 0.42)
      );
      context.fillStyle = 'rgba(231, 237, 244, 0.055)';
      context.fillRect(table.x, table.y, table.width, tableHeaderHeight);

      for (var column = 1; column < columns.length - 1; column += 1) {
        var columnX = table.x + table.width * columns[column];
        line(columnX, table.y, columnX, table.y + table.height, colors.rule, 1);
      }
      for (var boundary = 0; boundary <= ROW_COUNT; boundary += 1) {
        var boundaryY = table.y + tableHeaderHeight + boundary * rowHeight;
        line(table.x, boundaryY, table.x + table.width, boundaryY, colors.rule, 1);
      }

      drawDatabaseGlyph(
        table.x + table.width * 0.045,
        table.y + 6,
        compact ? 14 : 17,
        compact ? 14 : 17,
        colors.soft
      );
      label('path', table.x + table.width * columns[1] + 9, table.y + 19, colors.soft, compact ? 6.5 : 7.5);
      label('query fields', table.x + table.width * columns[2] + 9, table.y + 19, colors.soft, compact ? 6.5 : 7.5);
      label('rumi header', table.x + table.width * columns[3] + 9, table.y + 19, colors.header, compact ? 6.5 : 7.5);

      if (selectionStrength > 0) {
        context.fillStyle = mixColor(
          colors.stageRgb,
          colors.mintRgb,
          catalogActivity,
          selectionStrength * 0.075
        );
        context.fillRect(table.x, selectedRowY, table.width, rowHeight);
        context.fillStyle = mixColor(
          colors.stageRgb,
          colors.mintRgb,
          catalogActivity,
          selectionStrength * 0.9
        );
        context.fillRect(table.x, selectedRowY, 2, rowHeight);
      }

      for (var row = 0; row < ROW_COUNT; row += 1) {
        var rowY = table.y + tableHeaderHeight + row * rowHeight;
        var rowCenter = rowY + rowHeight / 2;
        var active = row === selectedRow && selectionStrength > 0.45;
        var firstCellX = table.x + table.width * 0.045;
        var secondCellX = table.x + table.width * columns[1] + 9;
        var thirdCellX = table.x + table.width * columns[2] + 9;
        var headerCellX = table.x + table.width * columns[3] + 9;
        var headerCellWidth = table.width * (columns[4] - columns[3]) - 18;

        drawFileGlyph(
          firstCellX,
          rowCenter - (compact ? 7 : 8),
          compact ? 11 : 13,
          compact ? 14 : 16,
          active ? catalogColor : colors.faint
        );
        context.fillStyle = active ? 'rgba(195, 204, 215, 0.62)' : colors.faint;
        context.fillRect(secondCellX, rowCenter - 5, table.width * (0.16 + (row % 3) * 0.025), 3);
        context.fillRect(secondCellX, rowCenter + 3, table.width * (0.1 + (row % 2) * 0.03), 2);
        context.fillStyle = active ? 'rgba(145, 242, 188, 0.48)' : 'rgba(98, 109, 123, 0.2)';
        context.fillRect(thirdCellX, rowCenter - 5, table.width * (0.09 + (row % 2) * 0.025), 10);
        drawHeaderBytes(
          headerCellX,
          rowCenter - (compact ? 3 : 4),
          headerCellWidth,
          compact ? 6 : 8,
          row,
          active,
          catalogColor,
          colors
        );
      }

      var queryFrom = { x: queryBox.x + queryBox.width * 0.5, y: queryBox.y + queryBox.height };
      var queryTo = { x: table.x, y: selectedRowCenter };
      var queryControl = {
        x: queryFrom.x + (queryTo.x - queryFrom.x) * 0.58,
        y: queryFrom.y + (queryTo.y - queryFrom.y) * 0.2
      };
      context.save();
      context.setLineDash([3, 7]);
      context.beginPath();
      context.moveTo(queryFrom.x, queryFrom.y);
      context.quadraticCurveTo(queryControl.x, queryControl.y, queryTo.x, queryTo.y);
      context.strokeStyle = 'rgba(145, 242, 188, ' + (0.045 + queryTransit * 0.3) + ')';
      context.lineWidth = 1;
      context.stroke();
      context.restore();

      var queryDotProgress = ease((progress - 0.16) / 0.21);
      var queryDot = pointOnQuadratic(queryFrom, queryControl, queryTo, queryDotProgress);
      context.save();
      context.globalAlpha = queryTransit * 0.2;
      dot(queryDot.x, queryDot.y, compact ? 6 : 8, colors.mint);
      context.globalAlpha = queryTransit;
      dot(queryDot.x, queryDot.y, compact ? 2.5 : 3.5, colors.mint);
      context.restore();

      var file = compact
        ? { x: measuredWidth * 0.16, y: 380, width: measuredWidth * 0.68, height: 50 }
        : { x: measuredWidth * 0.22, y: measuredHeight * 0.79, width: measuredWidth * 0.68, height: 62 };
      label(
        'RUMI FILE',
        file.x,
        file.y - 10,
        fileColor,
        compact ? 8 : 11
      );
      fillRoundedRect(file.x, file.y, file.width, file.height, 4, 'rgba(17, 23, 33, 0.46)');
      strokeRoundedRect(file.x + 0.5, file.y + 0.5, file.width - 1, file.height - 1, 4, colors.edge);
      strokeRoundedRect(
        file.x + 0.5,
        file.y + 0.5,
        file.width - 1,
        file.height - 1,
        4,
        mixColor(colors.stageRgb, colors.mintRgb, fileActivity, 0.22 + fileActivity * 0.5)
      );
      if (fileActivity > 0) {
        fillRoundedRect(
          file.x,
          file.y,
          file.width,
          file.height,
          4,
          'rgba(145, 242, 188, ' + (fileActivity * 0.055) + ')'
        );
      }
      drawFileGlyph(
        file.x + 12,
        file.y + file.height / 2 - (compact ? 10 : 12),
        compact ? 16 : 19,
        compact ? 20 : 24,
        colors.soft
      );
      if (fileActivity > 0) {
        drawFileGlyph(
          file.x + 12,
          file.y + file.height / 2 - (compact ? 10 : 12),
          compact ? 16 : 19,
          compact ? 20 : 24,
          'rgba(145, 242, 188, ' + fileActivity + ')'
        );
      }

      var payloadX = file.x + (compact ? 40 : 48);
      var payloadWidth = file.width - (compact ? 50 : 60);
      var headerWidth = payloadWidth * 0.27;
      var payloadY = file.y + file.height * 0.64;
      label(
        'rumi file header · IFD',
        payloadX + headerWidth / 2,
        file.y + (compact ? 14 : 17),
        colors.header,
        compact ? 5.5 : 7.5,
        'center'
      );
      context.fillStyle = 'rgba(195, 204, 215, 0.28)';
      context.fillRect(payloadX, payloadY - 4, headerWidth * 0.25, 8);
      context.fillStyle = colors.header;
      context.fillRect(payloadX + headerWidth * 0.25, payloadY - 4, headerWidth * 0.75, 8);

      var frames = frameGeometry(payloadX + headerWidth + 5, payloadWidth - headerWidth - 5);
      var activeFrames = FRAME_RANGES[selectedRow];
      frames.forEach(function (frame, index) {
        var isActive = activeFrames.indexOf(index) !== -1;
        var activeHeight = compact ? 14 : 18;
        context.fillStyle = isActive
          ? mixColor(colors.stageRgb, colors.mintRgb, fileActivity, frameReveal * (0.18 + fileActivity * 0.76))
          : 'rgba(98, 109, 123, 0.14)';
        context.fillRect(
          frame.x + 1,
          payloadY - (isActive ? activeHeight / 2 : 5),
          Math.max(1, frame.width - 2),
          isActive ? activeHeight : 10
        );
      });

      var headerFrom = {
        x: table.x + table.width * 0.85,
        y: selectedRowCenter
      };
      var targetFrame = frames[activeFrames[Math.floor(activeFrames.length / 2)]];
      var fileTo = {
        x: targetFrame.x + targetFrame.width / 2,
        y: payloadY
      };
      var headerControl = {
        x: headerFrom.x + (fileTo.x - headerFrom.x) * 0.68,
        y: headerFrom.y + (fileTo.y - headerFrom.y) * 0.35
      };
      context.save();
      context.setLineDash([3, 7]);
      context.beginPath();
      context.moveTo(headerFrom.x, headerFrom.y);
      context.quadraticCurveTo(headerControl.x, headerControl.y, fileTo.x, fileTo.y);
      context.strokeStyle = mixColor(
        colors.headerRgb,
        colors.mintRgb,
        headerTransit,
        0.065 + headerTransit * 0.32
      );
      context.lineWidth = 1;
      context.stroke();
      context.restore();

      var headerDotProgress = ease((progress - 0.52) / 0.23);
      var headerDot = pointOnQuadratic(headerFrom, headerControl, fileTo, headerDotProgress);
      context.save();
      context.globalAlpha = headerTransit * 0.18;
      fillRoundedRect(
        headerDot.x - (compact ? 6 : 8),
        headerDot.y - (compact ? 4 : 5),
        compact ? 12 : 16,
        compact ? 8 : 10,
        3,
        headerSignalColor
      );
      context.globalAlpha = headerTransit;
      fillRoundedRect(
        headerDot.x - (compact ? 3 : 4),
        headerDot.y - (compact ? 2 : 2.5),
        compact ? 6 : 8,
        compact ? 4 : 5,
        1,
        headerSignalColor
      );
      context.restore();

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
