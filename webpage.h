#pragma once

#include <Arduino.h>

const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Motion Monitor</title>
  <style>
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body {
      background: #ffffff;
      color: #111827;
      font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
      padding: 12px;
      max-width: 800px;
      margin: 0 auto;
    }
    .stats-bar {
      display: flex;
      flex-wrap: wrap;
      gap: 12px;
      padding: 8px 12px;
      border: 1px solid #e5e7eb;
      border-radius: 6px;
      margin-bottom: 12px;
      font-size: 0.95rem;
      background: #f9fafb;
    }
    .stat-item {
      display: flex;
      gap: 6px;
    }
    .stat-label {
      color: #6b7280;
      font-weight: 500;
    }
    .stat-val {
      font-weight: 700;
    }
    .chart-box {
      border: 1px solid #e5e7eb;
      border-radius: 6px;
      padding: 8px;
      margin-bottom: 12px;
    }
    .chart-legend {
      display: flex;
      gap: 16px;
      font-size: 0.8rem;
      margin-bottom: 4px;
      color: #4b5563;
    }
    .legend-tag {
      display: inline-block;
      width: 12px;
      height: 3px;
      vertical-align: middle;
      margin-right: 4px;
    }
    canvas {
      width: 100%;
      height: 180px;
      display: block;
      background: #ffffff;
    }
    .controls {
      display: flex;
      gap: 8px;
      margin-bottom: 12px;
    }
    button {
      flex: 1;
      padding: 8px 12px;
      border: 1px solid #d1d5db;
      border-radius: 6px;
      background: #ffffff;
      color: #111827;
      font-size: 0.85rem;
      font-weight: 600;
      cursor: pointer;
    }
    button:active {
      background: #f3f4f6;
    }
    .history-box {
      border: 1px solid #e5e7eb;
      border-radius: 6px;
      padding: 8px;
    }
    .history-title {
      font-size: 0.85rem;
      font-weight: 700;
      color: #374151;
      margin-bottom: 6px;
    }
    .history-scroll {
      max-height: 240px;
      overflow-y: auto;
      border: 1px solid #f3f4f6;
    }
    table {
      width: 100%;
      border-collapse: collapse;
      font-size: 0.8rem;
      text-align: left;
    }
    th {
      background: #f9fafb;
      color: #4b5563;
      padding: 6px 8px;
      font-weight: 600;
      border-bottom: 1px solid #e5e7eb;
      position: sticky;
      top: 0;
    }
    td {
      padding: 6px 8px;
      border-bottom: 1px solid #f3f4f6;
    }
    tr:nth-child(even) {
      background: #fafafa;
    }
  </style>
</head>
<body>

  <div class="stats-bar">
    <div class="stat-item"><span class="stat-label">Reps:</span> <span class="stat-val" id="valReps">0</span></div>
    <div class="stat-item"><span class="stat-label">State:</span> <span class="stat-val" id="valState">IDLE</span></div>
    <div class="stat-item"><span class="stat-label">Angle:</span> <span class="stat-val" id="valAngle">0°</span></div>
    <div class="stat-item"><span class="stat-label">Velocity:</span> <span class="stat-val" id="valVel">0°/s</span></div>
    <div class="stat-item"><span class="stat-label">Q:</span> <span class="stat-val" id="valQ" style="color: #16a34a;">100%</span></div>
    <div class="stat-item"><span class="stat-label">Jerk:</span> <span class="stat-val" id="valJerk">0.0</span></div>
    <div class="stat-item"><span class="stat-label">HR:</span> <span class="stat-val" id="valBpm" style="color: #dc2626;">--</span></div>
    <div class="stat-item"><span class="stat-label">SpO2:</span> <span class="stat-val" id="valSpo2" style="color: #0284c7;">--</span></div>
    <div class="stat-item"><span class="stat-label">Pulse Ox:</span> <span class="stat-val" id="valPulseOx">Disconnected</span></div>
    <div class="stat-item"><span class="stat-label">Core Temp:</span> <span class="stat-val" id="valTemp" style="color: #d97706;">--°C</span></div>
  </div>

  <div class="chart-box">
    <div class="chart-legend">
      <span><span class="legend-tag" style="background: #0284c7;"></span>Angle (0-140°)</span>
      <span><span class="legend-tag" style="background: #16a34a;"></span>Quality Q (0-100%)</span>
      <span><span class="legend-tag" style="background: #ef4444;"></span>HR (0-200 bpm)</span>
      <span><span class="legend-tag" style="background: #06b6d4;"></span>SpO2 (0-100%)</span>
    </div>
    <canvas id="liveChart"></canvas>
  </div>

  <div class="controls">
    <button onclick="sendCommand('calibrate')">Calibrate</button>
    <button onclick="sendCommand('reset')">Reset</button>
  </div>

  <div class="history-box">
    <div class="history-title">Completed Reps</div>
    <div class="history-scroll">
      <table>
        <thead>
          <tr>
            <th>#</th>
            <th>Quality</th>
            <th>Peak</th>
            <th>Concentric</th>
            <th>Eccentric</th>
            <th>HR / SpO2</th>
            <th>Diagnostics</th>
          </tr>
        </thead>
        <tbody id="repTableBody">
        </tbody>
      </table>
    </div>
  </div>

  <script>
    const canvas = document.getElementById('liveChart');
    const ctx = canvas.getContext('2d');

    const MAX_POINTS = 120;
    const angleHistory = new Array(MAX_POINTS).fill(0);
    const qHistory = new Array(MAX_POINTS).fill(100);
    const hrHistory = new Array(MAX_POINTS).fill(0);
    const spo2History = new Array(MAX_POINTS).fill(0);
    let renderedRepCount = 0;

    function resizeCanvas() {
      canvas.width = canvas.parentElement.clientWidth - 16;
      canvas.height = 180;
    }
    window.addEventListener('resize', resizeCanvas);
    resizeCanvas();

    function drawChart() {
      const w = canvas.width;
      const h = canvas.height;
      ctx.clearRect(0, 0, w, h);

      // Light grid
      ctx.strokeStyle = '#f1f5f9';
      ctx.lineWidth = 1;
      for (let y = 0; y <= h; y += h / 4) {
        ctx.beginPath();
        ctx.moveTo(0, y);
        ctx.lineTo(w, y);
        ctx.stroke();
      }

      // SpO2 (Cyan, 0-100%)
      const hasSpo2 = spo2History.some(v => v > 0);
      if (hasSpo2) {
        ctx.strokeStyle = '#06b6d4';
        ctx.lineWidth = 2;
        ctx.beginPath();
        let started = false;
        for (let i = 0; i < MAX_POINTS; i++) {
          if (spo2History[i] > 0) {
            const x = (i / (MAX_POINTS - 1)) * w;
            const y = h - (spo2History[i] / 100) * (h - 20) - 10;
            if (!started) { ctx.moveTo(x, y); started = true; }
            else { ctx.lineTo(x, y); }
          }
        }
        if (started) ctx.stroke();
      }

      // HR (Red, 0-200 bpm)
      const hasHr = hrHistory.some(v => v > 0);
      if (hasHr) {
        ctx.strokeStyle = '#ef4444';
        ctx.lineWidth = 2;
        ctx.beginPath();
        let started = false;
        for (let i = 0; i < MAX_POINTS; i++) {
          if (hrHistory[i] > 0) {
            const x = (i / (MAX_POINTS - 1)) * w;
            const y = h - (hrHistory[i] / 200) * (h - 20) - 10;
            if (!started) { ctx.moveTo(x, y); started = true; }
            else { ctx.lineTo(x, y); }
          }
        }
        if (started) ctx.stroke();
      }

      // Quality (Green)
      ctx.strokeStyle = '#16a34a';
      ctx.lineWidth = 2;
      ctx.beginPath();
      for (let i = 0; i < MAX_POINTS; i++) {
        const x = (i / (MAX_POINTS - 1)) * w;
        const y = h - (qHistory[i] / 100) * (h - 20) - 10;
        if (i === 0) ctx.moveTo(x, y);
        else ctx.lineTo(x, y);
      }
      ctx.stroke();

      // Angle (Blue)
      ctx.strokeStyle = '#0284c7';
      ctx.lineWidth = 2;
      ctx.beginPath();
      for (let i = 0; i < MAX_POINTS; i++) {
        const x = (i / (MAX_POINTS - 1)) * w;
        const y = h - (angleHistory[i] / 140) * (h - 20) - 10;
        if (i === 0) ctx.moveTo(x, y);
        else ctx.lineTo(x, y);
      }
      ctx.stroke();
    }

    async function pollData() {
      try {
        const res = await fetch('/api/data');
        if (!res.ok) return;
        const d = await res.json();

        document.getElementById('valReps').innerText = d.reps;
        document.getElementById('valState').innerText = d.state;
        document.getElementById('valAngle').innerText = d.angle.toFixed(1) + '°';
        document.getElementById('valVel').innerText = (d.vel >= 0 ? '+' : '') + d.vel.toFixed(1) + '°/s';
        document.getElementById('valJerk').innerText = d.jerk.toFixed(1);

        const qEl = document.getElementById('valQ');
        qEl.innerText = d.q.toFixed(0) + '%';
        if (d.q >= 75) qEl.style.color = '#16a34a';
        else if (d.q >= 50) qEl.style.color = '#ca8a04';
        else qEl.style.color = '#dc2626';

        document.getElementById('valBpm').innerText = (d.bpm && d.bpm > 0) ? (d.bpm + ' bpm') : '--';
        document.getElementById('valSpo2').innerText = (d.spo2 && d.spo2 > 0) ? (d.spo2 + '%') : '--';
        document.getElementById('valPulseOx').innerText = d.pulseOxStatus || (d.pulseOxConnected ? 'Active' : 'Disconnected');

        if (d.temp !== undefined) {
          document.getElementById('valTemp').innerText = d.temp.toFixed(1) + '°C';
        }

        angleHistory.shift();
        angleHistory.push(d.angle);
        qHistory.shift();
        qHistory.push(d.q);
        hrHistory.shift();
        hrHistory.push(d.bpm || 0);
        spo2History.shift();
        spo2History.push(d.spo2 || 0);
        drawChart();

        // Update Rep History List
        if (d.history && d.history.length !== renderedRepCount) {
          const tbody = document.getElementById('repTableBody');
          tbody.innerHTML = '';
          d.history.forEach(r => {
            const tr = document.createElement('tr');
            const hrStr = (r.bpm && r.bpm > 0) ? `${r.bpm} bpm / ${r.spo2}%` : '--';
            tr.innerHTML = `
              <td><b>${r.rep}</b></td>
              <td>${r.q.toFixed(1)}%</td>
              <td>${r.peak.toFixed(1)}°</td>
              <td>${r.con.toFixed(2)}s</td>
              <td>${r.ecc.toFixed(2)}s</td>
              <td>${hrStr}</td>
              <td>${r.diag}</td>
            `;
            tbody.appendChild(tr);
          });
          renderedRepCount = d.history.length;
          const scrollBox = document.querySelector('.history-scroll');
          scrollBox.scrollTop = scrollBox.scrollHeight;
        } else if (d.history && d.history.length === 0 && renderedRepCount > 0) {
          document.getElementById('repTableBody').innerHTML = '';
          renderedRepCount = 0;
        }
      } catch (err) {}
    }

    async function sendCommand(cmd) {
      try {
        await fetch('/api/cmd?action=' + cmd);
        if (cmd === 'reset') {
          renderedRepCount = 0;
          document.getElementById('repTableBody').innerHTML = '';
        }
      } catch (e) {}
    }

    setInterval(pollData, 100);
  </script>
</body>
</html>
)rawliteral";
