"use strict";

let token = "";
let selected = null;
let timer = null;
let generation = 0;
let eventKey = "";
const images = [];
const markers = new Map();
const status = document.getElementById("status");
const map = L.map("map").setView([35.6812, 139.7671], 12);
L.tileLayer("https://tile.openstreetmap.org/{z}/{x}/{y}.png", {
  attribution: '&copy; <a href="https://www.openstreetmap.org/copyright">OpenStreetMap</a>',
  maxZoom: 19
}).addTo(map);

async function api(path, data) {
  const response = await fetch(path, {
    method: data === undefined ? "GET" : "POST",
    headers: { Authorization: "Bearer" + " " + token, "Content-Type": "application/json" },
    body: data === undefined ? undefined : JSON.stringify(data),
    cache: "no-store"
  });
  if (!response.ok) throw new Error(`通信エラー (${response.status})`);
  return path.endsWith("/image") ? response.blob() : response.json();
}

function node(tag, text, parent) {
  const element = document.createElement(tag);
  if (text !== undefined) element.textContent = text;
  if (parent) parent.append(element);
  return element;
}

function clearEvents() {
  images.forEach(url => URL.revokeObjectURL(url));
  images.length = 0;
  document.getElementById("events").replaceChildren();
  eventKey = "";
}

async function showEvents(currentGeneration) {
  if (!selected) return;
  const vehicle = selected;
  const { events } = await api(`/api/events?vehicle=${encodeURIComponent(vehicle)}`);
  if (generation !== currentGeneration || selected !== vehicle) return;
  const key = JSON.stringify(events);
  if (key === eventKey) return;
  clearEvents();
  eventKey = key;
  const container = document.getElementById("events");
  if (!events.length) node("p", "イベントはありません", container);
  for (const event of events) {
    const card = node("article", undefined, container);
    card.className = "event";
    node("strong", `${event.vehicle} — 衝撃 ${event.acceleration_g.toFixed(2)} g`, card);
    node("p", new Date(event.received_at * 1000).toLocaleString(), card);
    if (event.has_image) {
      try {
        const blob = await api(`/api/events/${event.id}/image`);
        if (generation !== currentGeneration || selected !== vehicle) return;
        const url = URL.createObjectURL(blob);
        images.push(url);
        const img = node("img", undefined, card);
        img.alt = "衝撃検出時の駐車監視写真";
        img.src = url;
      } catch (error) {
        node("p", error.message, card);
        eventKey = "";
      }
    } else {
      node("p", "写真なし（カメラ未接続または撮影失敗）", card);
    }
  }
}

async function refresh(currentGeneration) {
  try {
    const { vehicles } = await api("/api/vehicles");
    if (generation !== currentGeneration) return;
    const container = document.getElementById("vehicles");
    container.replaceChildren();
    for (const vehicle of vehicles) {
      const stale = Date.now() / 1000 - vehicle.last_seen > 30;
      const positionStale = !vehicle.position_at || Date.now() / 1000 - vehicle.position_at > 30;
      const card = node("div", undefined, container);
      card.className = `vehicle${selected === vehicle.id ? " selected" : ""}`;
      const choose = node("button", vehicle.id, card);
      choose.onclick = () => {
        selected = vehicle.id;
        clearEvents();
        if (vehicle.lat !== null) map.setView([vehicle.lat, vehicle.lon], 16);
      };
      const state = node("p", stale ? "オフライン" : "オンライン", card);
      if (stale) state.className = "stale";
      node("p", vehicle.lat === null ? "GNSS 測位待ち" :
        `${vehicle.speed.toFixed(1)} km/h${positionStale ? " — 最終測位位置（古い位置）" : ""}`, card);
      node("p", `最終受信: ${new Date(vehicle.last_seen * 1000).toLocaleString()}`, card);
      const arm = node("button", vehicle.parking_armed ? "監視を解除" : "駐車監視を有効化", card);
      arm.onclick = async () => {
        arm.disabled = true;
        try {
          await api(`/api/vehicles/${encodeURIComponent(vehicle.id)}/parking`,
            { parking_armed: !vehicle.parking_armed });
          status.textContent = "監視設定を送信しました（次回通信時に端末へ反映）";
        } catch (error) {
          status.textContent = error.message;
        } finally {
          arm.disabled = false;
        }
      };
      if (vehicle.lat !== null) {
        const coordinates = [vehicle.lat, vehicle.lon];
        if (!markers.has(vehicle.id)) {
          const marker = L.marker(coordinates).addTo(map);
          marker.on("click", () => { selected = vehicle.id; clearEvents(); });
          markers.set(vehicle.id, marker);
          if (markers.size === 1) map.setView(coordinates, 15);
        }
        const marker = markers.get(vehicle.id);
        marker.setLatLng(coordinates).setOpacity(stale || positionStale ? 0.4 : 1);
        marker.bindTooltip(node("span", vehicle.id));
      }
    }
    if (!vehicles.length) node("p", "端末からの通信を待っています", container);
    await showEvents(currentGeneration);
    if (generation === currentGeneration) status.textContent = "接続中 — 2 秒ごとに更新";
  } catch (error) {
    if (generation === currentGeneration) status.textContent = error.message;
  } finally {
    if (generation === currentGeneration) {
      timer = setTimeout(() => refresh(currentGeneration), 2000);
    }
  }
}

function disconnect() {
  generation++;
  clearTimeout(timer);
  token = "";
  selected = null;
  clearEvents();
  markers.forEach(marker => marker.remove());
  markers.clear();
  document.getElementById("vehicles").replaceChildren();
  document.getElementById("token").value = "";
  status.textContent = "切断しました";
}

document.getElementById("connect").onsubmit = event => {
  event.preventDefault();
  const enteredToken = document.getElementById("token").value;
  disconnect();
  token = enteredToken;
  refresh(generation);
};
document.getElementById("disconnect").onclick = disconnect;
