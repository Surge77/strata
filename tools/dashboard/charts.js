// A minimal time-series chart on a canvas: a sliding window of lines, a
// zero-based y axis with four gridlines, and nothing that needs a library.

const PADDING = { top: 8, right: 12, bottom: 20, left: 40 };
const GRIDLINES = 4;
const LABEL_GAP = 6;

function cssVar(element, name) {
  return getComputedStyle(element).getPropertyValue(name).trim();
}

// A "nice" axis maximum (1, 2, 2.5 or 5 times a power of ten) at or above
// `value`, so gridline labels read as round numbers.
export function niceMax(value) {
  if (!(value > 0)) return 1;
  const magnitude = 10 ** Math.floor(Math.log10(value));
  for (const step of [1, 2, 2.5, 5, 10]) {
    if (step * magnitude >= value) return step * magnitude;
  }
  return 10 * magnitude;
}

function resize(canvas) {
  const ratio = window.devicePixelRatio || 1;
  const { width, height } = canvas.getBoundingClientRect();
  const pixelWidth = Math.max(1, Math.round(width * ratio));
  const pixelHeight = Math.max(1, Math.round(height * ratio));
  if (canvas.width !== pixelWidth || canvas.height !== pixelHeight) {
    canvas.width = pixelWidth;
    canvas.height = pixelHeight;
  }
  const context = canvas.getContext("2d");
  context.setTransform(ratio, 0, 0, ratio, 0, 0);
  return { context, width, height };
}

// lines: [{ color, points: [[seconds, value|null], ...] }]. `now` is the right
// edge in seconds; the window covers `windowSeconds` before it. The axis top
// is a round number of `unit`s (1 MiB for bytes reads better than 10^8).
export function drawChart(canvas, { lines, now, windowSeconds, format, unit = 1 }) {
  const { context, width, height } = resize(canvas);
  context.clearRect(0, 0, width, height);

  let highest = 0;
  for (const line of lines) {
    for (const [, value] of line.points) {
      if (value !== null && value > highest) highest = value;
    }
  }
  const top = niceMax(highest / unit) * unit;
  context.font = `11px ${cssVar(canvas, "--font-mono") || "monospace"}`;
  const labels = [];
  for (let i = 0; i <= GRIDLINES; i += 1) {
    labels.push(format((top * i) / GRIDLINES));
  }
  // The plot starts after the widest axis label, so "100 MiB/s" is never cut.
  const left = Math.max(
    PADDING.left,
    Math.ceil(Math.max(...labels.map((label) => context.measureText(label).width))) + LABEL_GAP * 2,
  );
  const plotWidth = width - left - PADDING.right;
  const plotHeight = height - PADDING.top - PADDING.bottom;
  if (plotWidth <= 0 || plotHeight <= 0) return;
  const x = (seconds) => left + ((seconds - (now - windowSeconds)) / windowSeconds) * plotWidth;
  const y = (value) => PADDING.top + plotHeight - (value / top) * plotHeight;

  context.lineWidth = 1;
  context.textBaseline = "middle";
  context.textAlign = "right";
  labels.forEach((label, i) => {
    const lineY = Math.round(y((top * i) / GRIDLINES)) + 0.5;
    context.strokeStyle = cssVar(canvas, "--grid");
    context.beginPath();
    context.moveTo(left, lineY);
    context.lineTo(width - PADDING.right, lineY);
    context.stroke();
    context.fillStyle = cssVar(canvas, "--text-faint");
    context.fillText(label, left - LABEL_GAP, lineY);
  });
  context.textAlign = "center";
  context.textBaseline = "top";
  for (const offset of [windowSeconds, windowSeconds / 2, 0]) {
    const label = offset === 0 ? "now" : `-${Math.round(offset / 60)}m`;
    context.fillText(label, x(now - offset), PADDING.top + plotHeight + 6);
  }

  context.save();
  context.beginPath();
  context.rect(left, PADDING.top, plotWidth, plotHeight);
  context.clip();
  context.lineWidth = 1.5;
  context.lineJoin = "round";
  for (const line of lines) {
    context.strokeStyle = line.color;
    context.beginPath();
    let drawing = false;
    for (const [seconds, value] of line.points) {
      if (value === null) {
        drawing = false;
        continue;
      }
      if (drawing) context.lineTo(x(seconds), y(value));
      else context.moveTo(x(seconds), y(value));
      drawing = true;
    }
    context.stroke();
  }
  context.restore();
}
