/**
 * Dashboard en tiempo real del NodoSensor1.
 *
 * Consulta GET /api/dashboard cada 5 s. Las claves que muestra son las mismas
 * de la trama del sensor: value, status, hx710b_raw, pressure_kpa,
 * water_level_cm, net_flow_l_min, timestamp, message_id.
 *
 * Robustez:
 *  - Toda cifra pasa por fmtNumber(): null/undefined/NaN/Infinity -> "—".
 *  - Cada consulta tiene timeout (AbortController); si falla, se reintenta con
 *    espera creciente (5 s → 60 s) y se muestra el aviso, sin congelar la UI.
 *  - Si Chart.js no carga (sin internet) el resto del panel sigue funcionando.
 */
(function () {
  "use strict";

  const POLL_MS = 5000;
  const MAX_BACKOFF_MS = 60000;
  const FETCH_TIMEOUT_MS = 8000;
  const TABLE_ROWS = 25;
  const OFFLINE_DEFAULT_S = Number(document.body.dataset.offlineSeconds) || 120;

  const $ = (id) => document.getElementById(id);

  // ---------- Formateo seguro ----------
  function isNum(v) {
    return typeof v === "number" && Number.isFinite(v);
  }
  function toNum(v) {
    if (v === null || v === undefined || v === "") return null;
    const n = typeof v === "number" ? v : Number(v);
    return Number.isFinite(n) ? n : null;
  }
  function fmtNumber(v, decimals) {
    const n = toNum(v);
    if (n === null) return "—";
    return n.toLocaleString("es-CO", {
      minimumFractionDigits: decimals,
      maximumFractionDigits: decimals,
    });
  }
  function fmtText(v) {
    return v === null || v === undefined || v === "" ? "—" : String(v);
  }
  function parseLocal(ts) {
    if (typeof ts !== "string" || ts.length < 19) return null;
    const d = new Date(ts.replace(" ", "T"));
    return Number.isNaN(d.getTime()) ? null : d;
  }
  function fmtAgo(seconds) {
    if (!isNum(seconds) || seconds < 0) return "";
    if (seconds < 60) return `hace ${Math.round(seconds)} s`;
    if (seconds < 3600) return `hace ${Math.floor(seconds / 60)} min`;
    if (seconds < 86400) return `hace ${Math.floor(seconds / 3600)} h`;
    return `hace ${Math.floor(seconds / 86400)} d`;
  }
  function setText(id, text, isNull) {
    const el = $(id);
    if (!el) return;
    el.textContent = text;
    el.classList.toggle("is-null", Boolean(isNull));
  }
  function setValue(id, v, decimals) {
    const txt = fmtNumber(v, decimals);
    setText(id, txt, txt === "—");
  }
  function setPill(id, textId, state, text) {
    const pill = $(id);
    if (pill) pill.dataset.state = state;
    setText(textId, text, false);
  }

  // ---------- Estado de la app ----------
  let lastData = null;          // última respuesta válida
  let lastOkAt = null;          // Date de la última consulta exitosa
  let failures = 0;
  let timer = null;
  const charts = { distance: null, level: null, available: null };

  // ---------- Tanque (SVG) ----------
  const TANK_TOP = 22;     // y del sensor (borde superior útil)
  const TANK_BOTTOM = 278; // y del fondo
  function yForDistance(cm, scale) {
    const ratio = Math.min(Math.max(cm / scale, 0), 1);
    return TANK_TOP + ratio * (TANK_BOTTOM - TANK_TOP);
  }
  function drawTank(latest, cfg) {
    const hero = document.querySelector(".hero");
    const minCm = toNum(cfg && cfg.min_distance_cm);
    const maxCm = toNum(cfg && cfg.max_distance_cm);
    const value = latest && latest.status === "ok" ? toNum(latest.value) : null;
    const scale = Math.max((maxCm || 100) * 1.25, value ? value * 1.1 : 0, 20);

    const placeLimit = (lineId, labelId, cm) => {
      const line = $(lineId), label = $(labelId);
      if (cm === null) { line.setAttribute("visibility", "hidden"); label.setAttribute("visibility", "hidden"); return; }
      const y = yForDistance(cm, scale);
      line.setAttribute("visibility", "visible"); label.setAttribute("visibility", "visible");
      line.setAttribute("y1", y); line.setAttribute("y2", y); label.setAttribute("y", y);
    };
    placeLimit("limitMin", "limitMinLabel", minCm);
    placeLimit("limitMax", "limitMaxLabel", maxCm);

    const water = $("tankWater"), beam = $("tankBeam");
    if (value === null) {
      hero.dataset.state = "none";
      water.setAttribute("y", TANK_BOTTOM); water.setAttribute("height", 0);
      beam.setAttribute("y2", TANK_TOP);
      setText("tankCaption", latest ? "Ultrasónico sin lectura" : "Sin lectura", true);
      return;
    }
    hero.dataset.state = "ok";
    const surface = yForDistance(value, scale);
    water.setAttribute("y", surface);
    water.setAttribute("height", Math.max(TANK_BOTTOM - surface, 0));
    beam.setAttribute("y2", surface);
    setText("tankCaption", `Superficie a ${fmtNumber(value, 1)} cm del sensor`, false);
  }

  // ---------- Bloques de la UI ----------
  const STATE_TEXT = {
    normal: "Dentro del rango",
    urgency: "Fuera del rango configurado",
    sensor_error: "Sensor ultrasónico sin respuesta",
  };

  function classify(latest, cfg) {
    if (!latest) return "none";
    if (latest.status !== "ok" || toNum(latest.value) === null) return "sensor_error";
    const v = toNum(latest.value);
    const minCm = toNum(cfg && cfg.min_distance_cm);
    const maxCm = toNum(cfg && cfg.max_distance_cm);
    if (minCm !== null && v < minCm) return "urgency";
    if (maxCm !== null && v > maxCm) return "urgency";
    return "normal";
  }

  function renderReading(data) {
    const latest = data.latest;
    const cfg = data.config || {};
    const state = classify(latest, cfg);

    setValue("valValue", latest && latest.status === "ok" ? latest.value : null, 2);
    const rs = $("readingState");
    rs.dataset.state = state;
    rs.textContent = state === "none" ? "Sin lectura todavía" : STATE_TEXT[state];

    const minCm = toNum(cfg.min_distance_cm), maxCm = toNum(cfg.max_distance_cm);
    setText("valRange", minCm !== null && maxCm !== null
      ? `${fmtNumber(minCm, 0)} a ${fmtNumber(maxCm, 0)} cm` : "—", minCm === null);
    setText("valTimestamp", fmtText(latest && latest.timestamp), !(latest && latest.timestamp));
    setText("valReceived", fmtText(latest && latest.received_at), !(latest && latest.received_at));
    drawTank(latest, cfg);
  }

  function renderVariables(data) {
    const l = data.latest || {};
    setValue("valLevel", l.water_level_cm, 2);
    setValue("valFlow", l.net_flow_l_min, 2);
    setValue("valPressure", l.pressure_kpa, 3);
    setValue("valHx", l.hx710b_raw, 0);

    const calibrated = toNum(l.pressure_kpa) !== null;
    setText("noteLevel", calibrated ? "Calculado a partir de la presión" : "Requiere calibración del HX710B", false);
    const hxNull = toNum(l.hx710b_raw) === null;
    setText("noteHx", data.latest ? (hxNull ? "HX710B sin lectura en la última trama" : "Lectura del ADC de presión") : "", false);
    $("noteHx").className = "tile-note" + (data.latest && hxNull ? " tone-warn" : "");

    const status = $("valStatus");
    if (!data.latest) { status.textContent = "—"; status.className = "tile-value tile-text is-null"; }
    else if (l.status === "ok") { status.textContent = "Respondiendo"; status.className = "tile-value tile-text tone-ok"; }
    else { status.textContent = "Sin respuesta"; status.className = "tile-value tile-text tone-bad"; }

    const st = (data.state && data.state.current_state) || null;
    const alertEl = $("valAlertState");
    alertEl.textContent = st ? ({ normal: "Normal", urgency: "Urgencia", sensor_error: "Error de sensor" }[st] || st) : "—";
    alertEl.className = "tile-value tile-text " + (st === "normal" ? "tone-ok" : st ? "tone-bad" : "is-null");
    setText("noteAlertState", data.state && data.state.changed_at ? `Desde ${data.state.changed_at}` : "", false);
  }

  function renderTrace(data) {
    const l = data.latest || {};
    const s = data.summary || {};
    setText("valMsgId", fmtText(l.message_id), !l.message_id);
    const src = { sensor: "Reloj del sensor", gateway: "Reloj de la Central (sensor sin hora)", cloud: "Hora del Cloud (sin reloj válido)" };
    setText("valTsSource", src[l.timestamp_source] || "—", !l.timestamp_source);
    setText("valGateway", fmtText(l.gateway_id), !l.gateway_id);
    setText("valGatewayTime", fmtText(l.gateway_time), !l.gateway_time);
    setText("valIntegrity", isNum(l.frame_length) && l.frame_crc32
      ? `${l.frame_length} bytes, CRC32 ${l.frame_crc32}` : "—", !l.frame_crc32);
    setText("valDuplicates", isNum(l.duplicates) ? `${l.duplicates} desde que arrancó la Central` : "—", !isNum(l.duplicates));
    setText("valTotals", isNum(s.total)
      ? `${fmtNumber(s.total, 0)} (${fmtNumber(s.ok_count, 0)} correctas, ${fmtNumber(s.error_count, 0)} con error)` : "—", !isNum(s.total));
  }

  function renderStatusPills(data) {
    const link = data.link || {};
    const latest = data.latest;
    const secs = toNum(link.seconds_since_last);
    const limit = toNum(link.offline_after_s) || OFFLINE_DEFAULT_S;

    if (!latest) setPill("pillNode", "txtNode", "wait", "El sensor aún no ha enviado datos");
    else if (link.online) setPill("pillNode", "txtNode", "ok", `Sensor en línea, ${fmtAgo(secs)}`);
    else setPill("pillNode", "txtNode", "bad", `Sensor sin reportar ${fmtAgo(secs)}`);

    // ACK: la Central solo responde ACK después de guardar la trama en su microSD.
    if (!latest || latest.central_ack === null || latest.central_ack === undefined) {
      setPill("pillAck", "txtAck", "wait", "ACK sin datos");
    } else if (latest.central_ack && secs !== null && secs < limit) {
      const dup = isNum(latest.duplicates) && latest.duplicates > 0 ? ` (${latest.duplicates} reintentos)` : "";
      setPill("pillAck", "txtAck", dup ? "warn" : "ok", `ACK confirmado por la Central${dup}`);
    } else {
      setPill("pillAck", "txtAck", "bad", "Sin ACK reciente");
    }
  }

  function renderTable(data) {
    const body = $("historyBody");
    const rows = (Array.isArray(data.readings) ? data.readings : []).slice(-TABLE_ROWS).reverse();
    body.textContent = "";
    if (rows.length === 0) {
      const tr = document.createElement("tr");
      const td = document.createElement("td");
      td.colSpan = 9; td.className = "empty";
      td.textContent = "Todavía no hay lecturas guardadas. Revisa que el NodoCentral esté enviando al Cloud.";
      tr.appendChild(td); body.appendChild(tr);
      return;
    }
    for (const r of rows) {
      const cells = [
        fmtText(r.received_at), fmtText(r.timestamp),
        r.status === "ok" ? fmtNumber(r.value, 2) : "—",
        r.status === "ok" ? "ok" : "sensor_error",
        fmtNumber(r.hx710b_raw, 0), fmtNumber(r.pressure_kpa, 3),
        fmtNumber(r.water_level_cm, 2), fmtNumber(r.net_flow_l_min, 2),
        r.central_ack === true ? "Sí" : r.central_ack === false ? "No" : "—",
      ];
      const tr = document.createElement("tr");
      cells.forEach((c, i) => {
        const td = document.createElement("td");
        td.textContent = c; // textContent: nunca se interpreta HTML
        if (c === "—") td.className = "is-null";
        if (i === 3 && c === "sensor_error") td.className = "tone-bad";
        tr.appendChild(td);
      });
      body.appendChild(tr);
    }
  }

  // ---------- Gráficos ----------
  function chartsReady() {
    if (charts.available === null) charts.available = typeof window.Chart === "function";
    if (!charts.available) { $("chartFallback").hidden = false; return false; }
    if (charts.distance) return true;

    const css = getComputedStyle(document.documentElement);
    const c = (n) => css.getPropertyValue(n).trim();
    Chart.defaults.font.family = c("--font-body");
    Chart.defaults.color = c("--ink-soft");
    const common = {
      responsive: true, maintainAspectRatio: false, animation: false, spanGaps: false,
      interaction: { mode: "index", intersect: false },
      plugins: {
        legend: { position: "bottom", labels: { boxWidth: 12 } },
        tooltip: {
          callbacks: {
            label: (ctx) => `${ctx.dataset.label}: ${ctx.parsed.y === null ? "sin dato" : fmtNumber(ctx.parsed.y, 2)}`,
          },
        },
      },
      scales: { x: { ticks: { maxTicksLimit: 8, autoSkip: true }, grid: { display: false } } },
    };
    charts.distance = new Chart($("chartDistance"), {
      type: "line",
      data: { labels: [], datasets: [
        { label: "Distancia (cm)", data: [], borderColor: c("--water"), backgroundColor: c("--water"), borderWidth: 2, pointRadius: 0, tension: 0.25 },
        { label: "Mínimo", data: [], borderColor: c("--warn"), borderDash: [4, 4], borderWidth: 1.5, pointRadius: 0 },
        { label: "Máximo", data: [], borderColor: c("--alert"), borderDash: [4, 4], borderWidth: 1.5, pointRadius: 0 },
      ] },
      options: { ...common, scales: { ...common.scales, y: { title: { display: true, text: "cm" } } } },
    });
    charts.level = new Chart($("chartLevel"), {
      type: "line",
      data: { labels: [], datasets: [
        { label: "Nivel (cm)", data: [], borderColor: c("--water"), borderWidth: 2, pointRadius: 0, yAxisID: "y" },
        { label: "Caudal (L/min)", data: [], borderColor: c("--ink"), borderWidth: 1.5, pointRadius: 0, yAxisID: "y1" },
      ] },
      options: { ...common, scales: { ...common.scales,
        y: { position: "left", title: { display: true, text: "cm" } },
        y1: { position: "right", title: { display: true, text: "L/min" }, grid: { drawOnChartArea: false } },
      } },
    });
    return true;
  }

  function renderCharts(data) {
    if (!chartsReady()) return;
    const rows = Array.isArray(data.readings) ? data.readings : [];
    const labels = rows.map((r) => {
      const d = parseLocal(r.received_at);
      return d ? d.toLocaleTimeString("es-CO", { hour: "2-digit", minute: "2-digit", second: "2-digit" }) : "";
    });
    const minCm = toNum(data.config && data.config.min_distance_cm);
    const maxCm = toNum(data.config && data.config.max_distance_cm);

    const dist = charts.distance.data;
    dist.labels = labels;
    dist.datasets[0].data = rows.map((r) => (r.status === "ok" ? toNum(r.value) : null)); // null = hueco
    dist.datasets[1].data = rows.map(() => minCm);
    dist.datasets[2].data = rows.map(() => maxCm);
    charts.distance.update("none");

    const lvl = charts.level.data;
    lvl.labels = labels;
    lvl.datasets[0].data = rows.map((r) => toNum(r.water_level_cm));
    lvl.datasets[1].data = rows.map((r) => toNum(r.net_flow_l_min));
    charts.level.update("none");

    setText("chartCount", rows.length ? `Últimas ${rows.length} lecturas` : "Sin lecturas", false);
  }

  // ---------- Ciclo de consulta ----------
  function renderAll(data) {
    // Cada bloque se protege por separado: un fallo no congela los demás.
    for (const fn of [renderReading, renderVariables, renderTrace, renderStatusPills, renderTable, renderCharts]) {
      try { fn(data); } catch (err) { console.error(`Error en ${fn.name}:`, err); }
    }
  }

  function showError(message) {
    const banner = $("errorBanner");
    banner.textContent = message;
    banner.hidden = false;
  }

  function scheduleNext() {
    const delay = failures === 0 ? POLL_MS : Math.min(POLL_MS * 2 ** failures, MAX_BACKOFF_MS);
    clearTimeout(timer);
    timer = setTimeout(poll, delay);
    setText("footPoll", failures ? `Próximo intento en ${Math.round(delay / 1000)} s.` : "", false);
  }

  async function poll() {
    const controller = new AbortController();
    const abortTimer = setTimeout(() => controller.abort(), FETCH_TIMEOUT_MS);
    try {
      const res = await fetch("/api/dashboard?limit=300", { cache: "no-store", signal: controller.signal });
      let data = null;
      try { data = await res.json(); } catch (_) { data = null; }
      if (!res.ok || !data || data.ok !== true) {
        throw new Error((data && data.error) || `HTTP ${res.status}`);
      }
      lastData = data;
      lastOkAt = new Date();
      failures = 0;
      $("errorBanner").hidden = true;
      setPill("pillCloud", "txtCloud", "ok", "NodoCloud conectado");
      renderAll(data);
    } catch (err) {
      failures += 1;
      const why = err && err.name === "AbortError" ? "no respondió a tiempo" : (err && err.message) || "error de red";
      setPill("pillCloud", "txtCloud", "bad", "Sin conexión con NodoCloud");
      showError(`No se pudieron actualizar los datos (${why}). ` +
        (lastData ? "Se muestran los últimos valores recibidos." : "Revisa que NodoCloud y MariaDB estén activos."));
    } finally {
      clearTimeout(abortTimer);
      scheduleNext();
    }
  }

  // "hace X s" se recalcula cada segundo sin esperar a la próxima consulta.
  setInterval(() => {
    if (!lastData || !lastData.latest || !lastOkAt) { setText("valAgo", "", false); return; }
    const base = toNum(lastData.link && lastData.link.seconds_since_last);
    if (base === null) return;
    const secs = base + (Date.now() - lastOkAt.getTime()) / 1000;
    setText("valAgo", `(${fmtAgo(secs)})`, false);
  }, 1000);

  // Al volver a la pestaña, actualizar de inmediato.
  document.addEventListener("visibilitychange", () => {
    if (!document.hidden) { clearTimeout(timer); poll(); }
  });

  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", poll);
  else poll();
})();
