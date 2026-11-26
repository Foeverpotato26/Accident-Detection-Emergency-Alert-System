(function () {
  "use strict";

  const CFG = window.APP_CONFIG || {};
  const BASE = String(CFG.FIREBASE_URL || "").replace(/\/+$/, "");
  const $ = (id) => document.getElementById(id);

  let timer = null;
  let currentLat = null, currentLon = null;
  let lastUptime = null, lastUptimeChange = 0;

  const isPlaceholder = () => !BASE || BASE.includes("YOUR_PROJECT_ID");

  // ---------- helpers ----------
  const num = (v) => (typeof v === "number" && isFinite(v) ? v : null);
  const fmt = (v, d) => (num(v) === null ? "--" : v.toFixed(d));

  function formatUptime(s) {
    s = num(s);
    if (s === null) return "--";
    const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60);
    return (h ? h + " h " : "") + m + " min " + (s % 60) + " s";
  }

  function classify(data) {
    if (data.status !== "ALERT") return "normal";
    return data.alertType === "MANUAL_SOS" ? "sos" : "accident";
  }

  // ---------- rendering ----------
  function setIndicators(kind) {
    $("indNormal").className = "ind" + (kind === "normal" ? " on-normal" : "");
    $("indAccident").className = "ind" + (kind === "accident" ? " on-accident" : "");
    $("indSos").className = "ind" + (kind === "sos" ? " on-sos" : "");
  }

  function renderCurrent(data) {
    const kind = classify(data);
    const st = $("status");
    st.textContent = kind === "sos" ? "MANUAL SOS" : kind === "accident" ? "POSSIBLE ACCIDENT" : "NORMAL";
    st.className = "status " + (kind === "normal" ? "normal" : "alert");
    $("alertType").textContent = "Alert type: " + (data.alertType || "UNKNOWN");
    setIndicators(kind);

    $("acceleration").textContent = fmt(data.acceleration, 2);
    $("rotation").textContent = fmt(data.rotation, 2);
    $("temperature").textContent = fmt(data.temperature, 1);
    $("satellites").textContent = num(data.satellites) === null ? "--" : data.satellites;
    $("uptime").textContent = formatUptime(data.uptime);
    $("gpsStatus").textContent = data.gpsValid === true ? "Valid fix" : "No fix";

    const lat = num(data.latitude), lon = num(data.longitude);
    // Only a valid fix with real numbers is shown. 0,0 is never treated as a position.
    const ok = data.gpsValid === true && lat !== null && lon !== null && !(lat === 0 && lon === 0);
    currentLat = ok ? lat : null;
    currentLon = ok ? lon : null;
    $("latitude").textContent = ok ? lat.toFixed(6) : "--";
    $("longitude").textContent = ok ? lon.toFixed(6) : "--";
    $("locationMessage").textContent = ok
      ? "GPS coordinates received from the device."
      : "No valid GPS position available (the GPS needs a clear view of the sky).";
    $("mapButton").disabled = !ok;

    // device online / offline
    let age = null;
    if (num(data.updatedAt) !== null) {
      age = Date.now() - data.updatedAt;
      $("lastUpdate").textContent = new Date(data.updatedAt).toLocaleString();
    } else {
      if (data.uptime !== lastUptime) { lastUptime = data.uptime; lastUptimeChange = Date.now(); }
      age = Date.now() - lastUptimeChange;
      $("lastUpdate").textContent = new Date(lastUptimeChange).toLocaleTimeString();
    }
    const offline = age > (CFG.OFFLINE_AFTER_MS || 20000);
    $("device").textContent = offline ? "OFFLINE (no recent data from the ESP32)" : "Online";
    if (offline) { st.className = "status offline"; st.textContent += " (stale)"; }
  }

  function renderHistory(obj) {
    const body = $("historyBody");
    body.innerHTML = "";
    const keys = obj ? Object.keys(obj).sort().reverse() : [];
    $("historyEmpty").classList.toggle("hidden", keys.length > 0);
    $("historyTable").classList.toggle("hidden", keys.length === 0);

    keys.forEach((k) => {
      const e = obj[k] || {};
      const tr = document.createElement("tr");
      const add = (text) => { const td = document.createElement("td"); td.textContent = text; tr.appendChild(td); return td; };
      add(num(e.updatedAt) !== null ? new Date(e.updatedAt).toLocaleString() : "uptime " + (e.uptime ?? "?") + " s");
      add(e.alertType || "?");
      add(fmt(e.acceleration, 2));
      add(fmt(e.rotation, 2));
      const td = add("");
      if (e.gpsValid === true && num(e.latitude) !== null && num(e.longitude) !== null) {
        const a = document.createElement("a");
        a.href = "https://www.google.com/maps?q=" + e.latitude + "," + e.longitude;
        a.target = "_blank"; a.rel = "noopener noreferrer";
        a.textContent = e.latitude.toFixed(5) + ", " + e.longitude.toFixed(5);
        td.appendChild(a);
      } else td.textContent = "No GPS fix";
      body.appendChild(tr);
    });
  }

  // ---------- data loading ----------
  async function getJson(path, query) {
    const r = await fetch(BASE + path + ".json?" + (query || "") , { cache: "no-store" });
    if (r.status === 401 || r.status === 403) throw new Error("Access denied (" + r.status + "): check the database rules");
    if (!r.ok) throw new Error("HTTP error " + r.status);
    return r.json();
  }

  async function update() {
    const conn = $("connection");
    try {
      const cur = await getJson("/accidentAlert/current");
      if (!cur) {
        conn.textContent = "Connected to Firebase, but no data yet. Check that the ESP32 is running.";
        $("status").textContent = "NO DATA";
        $("status").className = "status offline";
        return;
      }
      renderCurrent(cur);

      try {
        const hist = await getJson("/accidentAlert/history",
          "orderBy=%22%24key%22&limitToLast=" + (CFG.HISTORY_LIMIT || 10));
        renderHistory(hist);
      } catch (e) { /* history is optional */ }

      conn.textContent = "Connected — data received successfully.";
    } catch (err) {
      conn.textContent = "Connection error: " + err.message;
    }
  }

  // ---------- UI ----------
  function showDashboard() {
    $("dashboard").classList.remove("hidden");
    update();
    clearInterval(timer);
    timer = setInterval(update, CFG.REFRESH_MS || 2000);
  }

  function openMap() {
    if (currentLat === null || currentLon === null) { alert("GPS coordinates are not available."); return; }
    window.open("https://www.google.com/maps?q=" + currentLat + "," + currentLon, "_blank", "noopener,noreferrer");
  }

  function init() {
    $("mapButton").addEventListener("click", openMap);
    if (isPlaceholder()) {
      $("dashboard").classList.remove("hidden");
      $("status").textContent = "NOT CONFIGURED";
      $("status").className = "status offline";
      $("alertType").textContent = "Edit Website/config.js with your Firebase URL.";
      $("connection").textContent = "Configuration missing.";
      return;
    }
    showDashboard();
  }

  init();
})();
