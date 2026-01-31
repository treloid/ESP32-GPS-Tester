#pragma once
#include <pgmspace.h>

const char PAGE_INDEX[] PROGMEM = R"RAWTEXT(
<!doctype html>
<html>
<head>
  <meta charset="utf-8"/>
  <meta name="viewport" content="width=device-width, initial-scale=1"/>
  <title>ESP32 GPS</title>
  <style>
    body { font-family: sans-serif; margin: 16px; }
    .card { padding: 12px; border: 1px solid #ccc; border-radius: 12px; max-width: 520px; }
    .row { display:flex; justify-content:space-between; padding: 6px 0; border-bottom: 1px dashed #ddd; }
    .row:last-child { border-bottom: none; }
    .k { color:#555; }
    .v { font-weight: 600; }
    .ok { color: #0a7; }
    .warn { color: #c80; }
    .bad { color: #c00; }
    code { background:#f6f6f6; padding:2px 6px; border-radius:6px; }
  </style>
  <link rel="stylesheet" href="https://unpkg.com/leaflet@1.9.4/dist/leaflet.css">
<script src="https://unpkg.com/leaflet@1.9.4/dist/leaflet.js"></script>
</head>
<body>
  <h2>ESP32 GPS Status</h2>
  <div class="card">
    <div class="row"><div class="k">Status</div><div id="status" class="v">-</div></div>
    <div class="row"><div class="k">Net Mode</div><div id="netMode" class="v">-</div></div>
    <div class="row"><div class="k">IP</div><div id="ip" class="v">-</div></div>

    <div class="row"><div class="k">GPS Connected</div><div id="gpsConnected" class="v">-</div></div>
    <div class="row"><div class="k">Has Fix</div><div id="hasFix" class="v">-</div></div>
    <div class="row"><div class="k">Fix Quality</div><div id="fixQuality" class="v">-</div></div>
    <div class="row"><div class="k">Satellites</div><div id="satellites" class="v">-</div></div>
    <div class="row"><div class="k">Latitude</div><div id="lat" class="v">-</div></div>
    <div class="row"><div class="k">Longitude</div><div id="lon" class="v">-</div></div>
    <div class="row"><div class="k">Speed (km/h)</div><div id="speedKmph" class="v">-</div></div>
    <div class="row"><div class="k">Altitude (m)</div><div id="altMeters" class="v">-</div></div>
    <div class="row"><div class="k">HDOP</div><div id="hdop" class="v">-</div></div>
    <div class="row"><div class="k">Location age (ms)</div><div id="ageMs" class="v">-</div></div>
    <div class="row"><div class="k">Chars processed</div><div id="charsProcessed" class="v">-</div></div>
  </div>

 <div id="mapWrap" style="max-width: 520px; margin-top: 12px;">
  <div id="mapMsg" class="card">Map status</div>
  <div id="map" style="height: 320px; border-radius: 12px; margin-top: 8px;"></div>
</div>

<h3>Wi-Fi Settings</h3>
<div class="card">
  <div class="row"><div class="k">SSID</div><div class="v"><input id="ssid" style="width:220px"></div></div>
  <div class="row"><div class="k">Password</div><div class="v"><input id="pass" type="password" style="width:220px"></div></div>
  <div class="row"><div class="k"></div>
    <div class="v">
      <button onclick="saveWifi()">Save & Reboot</button>
    </div>
  </div>
  <div id="wifiResult" style="margin-top:8px;color:#555;"></div>
</div>

<p>JSON endpoint: <code>/json</code></p>

<script>
  async function saveWifi() {
    const ssid = document.getElementById('ssid').value;
    const pass = document.getElementById('pass').value;

    const form = new URLSearchParams();
    form.append("ssid", ssid);
    form.append("pass", pass);

    const res = await fetch("/wifi", {
      method: "POST",
      headers: {"Content-Type":"application/x-www-form-urlencoded"},
      body: form.toString()
    });

    const txt = await res.text();
    document.getElementById("wifiResult").textContent = txt;
  }

  let map = null;
  let marker = null;
  let tiles = null;

  function setMapMsg(t) {
    const el = document.getElementById("mapMsg");
    if (!el) return;
    el.textContent = t;
    el.style.display = "block";
  }

  async function ensureMap() {
    if (map) return;

    setMapMsg("Initializing map");

    // Default view (until GPS fix arrives)
    const defaultLat = 47.3769;  // Zurich
    const defaultLon = 8.5417;

    map = L.map('map').setView([defaultLat, defaultLon], 13);

    // Use a single tile host to avoid any {s} issues
    tiles = L.tileLayer('https://a.tile.openstreetmap.org/{z}/{x}/{y}.png', {
      maxZoom: 19,
      attribution: '&copy; OpenStreetMap contributors'
    }).addTo(map);

    tiles.on('load', () => setMapMsg("Map OK (tiles loaded)"));
    tiles.on('tileerror', (e) => {
      setMapMsg("Tile load error (see browser console/network)");
      console.log("tileerror", e);
    });

    marker = L.marker([defaultLat, defaultLon]).addTo(map);

    // If map div size/layout changes, this fixes blank rendering
    setTimeout(() => map.invalidateSize(), 200);
  }

  async function refresh() {
    try {
      const r = await fetch('/json', {cache: 'no-store'});
      const j = await r.json();

      const statusEl = document.getElementById('status');
      statusEl.textContent = j.status;
      statusEl.className = "v " + (
        j.status === "FIX_OK" ? "ok" :
        j.status === "WAITING_FOR_FIX" ? "warn" : "bad"
      );

      // ✅ Include fixQuality here
      for (const k of ["netMode","ip","gpsConnected","hasFix","fixQuality","satellites","lat","lon","speedKmph","altMeters","hdop","ageMs","charsProcessed"]) {
        const el = document.getElementById(k);
        if (el) el.textContent = j[k];
      }

      // AP mode: hide map, show message
      if (j.netMode !== "STA") {
        document.getElementById("map").style.display = "none";
        setMapMsg("Map disabled (AP mode has no internet). Connect via STA.");
        return;
      }

      // STA mode: show map
      document.getElementById("map").style.display = "block";

      // Init map once (Leaflet already loaded via <script> in <head>)
      await ensureMap();

      const hasFix = (j.hasFix === true || j.hasFix === "true");
      const lat = Number(j.lat);
      const lon = Number(j.lon);

      if (!hasFix || !isFinite(lat) || !isFinite(lon) || (lat === 0 && lon === 0)) {
        setMapMsg("Waiting for GPS fix");
        return;
      }

      setMapMsg("Map OK (live position)");
      marker.setLatLng([lat, lon]);
      map.setView([lat, lon], Math.max(map.getZoom(), 16), {animate: true});
      setTimeout(() => map.invalidateSize(), 50);

    } catch (e) {
      setMapMsg("Map error (check internet / Leaflet / console).");
      console.log(e);
    }
  }

setInterval(refresh, 1000);
refresh();
</script>

</body>
</html>
)RAWTEXT";
