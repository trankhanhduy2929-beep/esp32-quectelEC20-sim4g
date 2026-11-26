(() => {
  const seen = new WeakSet();
  const target = /^esp-(app|entity-table|log|entity-chart)$/;
  const scanEntity = "WiFi xung quanh";
  const scanWait = "Đang quét WiFi, chờ vài giây...";
  let panel = null;
  let list = null;
  let status = null;
  let selected = "";
  let scanning = false;

  const inject = (host) => {
    const root = host.shadowRoot;
    if (!root || seen.has(root)) return;
    seen.add(root);
    const link = document.createElement("link");
    link.rel = "stylesheet";
    link.href = "/0.css";
    root.appendChild(link);
  };

  const scan = (root) => {
    for (const el of root.querySelectorAll("*")) {
      if (target.test(el.tagName.toLowerCase())) inject(el);
      if (el.shadowRoot) scan(el.shadowRoot);
    }
  };

  const tableRoot = () => {
    const app = document.querySelector("esp-app");
    const table = app && app.shadowRoot && app.shadowRoot.querySelector("esp-entity-table");
    return table && table.shadowRoot;
  };

  const row = (root, label) => {
    if (!root) return null;
    return [...root.querySelectorAll(".entity-row")].find((item) => {
      const name = item.querySelector(":scope > div:nth-child(2)");
      return name && name.textContent.trim() === label;
    });
  };

  const setSsid = (value) => {
    const input = row(tableRoot(), "WiFi SSID")?.querySelector("input");
    if (!input) return;
    input.value = value;
    input.dispatchEvent(new Event("change", { bubbles: true }));
  };

  const focusPassword = () => {
    const input = row(tableRoot(), "WiFi Password")?.querySelector("input");
    if (!input) return;
    const reduce = window.matchMedia("(prefers-reduced-motion: reduce)").matches;
    input.scrollIntoView({ block: "center", behavior: reduce ? "auto" : "smooth" });
    input.focus({ preventScroll: true });
  };

  const press = (label) => {
    const button = row(tableRoot(), label)?.querySelector("button");
    if (!button) return false;
    button.click();
    return true;
  };

  const parse = (value) => {
    const found = new Map();
    for (const line of value.split("\n")) {
      const match = line.match(/^(.*): (-?\d+)dB$/);
      if (!match || !match[1]) continue;
      const ssid = match[1];
      const rssi = Number(match[2]);
      if (!found.has(ssid) || found.get(ssid) < rssi) found.set(ssid, rssi);
    }
    return [...found.entries()]
      .map(([ssid, rssi]) => ({ ssid, rssi }))
      .sort((a, b) => b.rssi - a.rssi);
  };

  const select = (ssid) => {
    selected = ssid;
    setSsid(ssid);
    for (const button of list.querySelectorAll(".wifi-network")) {
      button.setAttribute("aria-pressed", String(button.dataset.ssid === ssid));
    }
    status.textContent = `Đã chọn "${ssid}". Nhập mật khẩu rồi bấm Kết nối WiFi.`;
    setTimeout(focusPassword, 250);
  };

  const render = (networks) => {
    list.replaceChildren();
    if (!networks.length) {
      const empty = document.createElement("p");
      empty.className = "wifi-picker-empty";
      empty.textContent = "Chưa có danh sách mạng. Bấm Quét lại để tìm WiFi.";
      list.append(empty);
      return 0;
    }
    for (const network of networks) {
      const button = document.createElement("button");
      button.type = "button";
      button.className = "wifi-network";
      button.dataset.ssid = network.ssid;
      button.setAttribute("aria-pressed", String(network.ssid === selected));
      const name = document.createElement("span");
      name.className = "wifi-network-name";
      name.textContent = network.ssid;
      const rssi = document.createElement("span");
      rssi.className = "wifi-network-rssi";
      rssi.textContent = `${network.rssi} dBm`;
      button.append(name, rssi);
      button.addEventListener("click", () => select(network.ssid));
      list.append(button);
    }
    return networks.length;
  };

  const load = async () => {
    if (!panel) return;
    try {
      const response = await fetch(`/text_sensor/${encodeURIComponent(scanEntity)}`, { cache: "no-store" });
      if (!response.ok) throw new Error(String(response.status));
      const data = await response.json();
      return render(parse(data.value || ""));
    } catch (_) {
      status.textContent = "Không tải được danh sách WiFi. Thử bấm Quét lại.";
      return -1;
    }
  };

  const build = (root) => {
    const ssidRow = row(root, "WiFi SSID");
    if (!ssidRow) return;

    if (panel && root.contains(panel)) return;

    panel = document.createElement("div");
    panel.className = "wifi-picker-row";

    const head = document.createElement("div");
    head.className = "wifi-picker-head";
    const title = document.createElement("span");
    title.className = "wifi-picker-title";
    title.textContent = "Chọn mạng WiFi";
    const refresh = document.createElement("button");
    refresh.type = "button";
    refresh.className = "wifi-refresh";
    refresh.textContent = "Quét lại";
    refresh.addEventListener("click", () => {
      if (scanning) return;
      if (!press("Quét WiFi")) {
        status.textContent = "Không tìm thấy nút quét WiFi.";
        return;
      }
      scanning = true;
      refresh.disabled = true;
      list.setAttribute("aria-busy", "true");
      status.textContent = scanWait;
      setTimeout(async () => {
        scanning = false;
        refresh.disabled = false;
        const count = await load();
        list.setAttribute("aria-busy", "false");
        if (status.textContent === scanWait) {
          status.textContent = count > 0
            ? "Đã cập nhật danh sách. Chọn mạng để điền SSID."
            : "Không thấy mạng WiFi. Thử Quét lại hoặc nhập SSID thủ công.";
        }
      }, 6000);
    });
    head.append(title, refresh);

    list = document.createElement("div");
    list.className = "wifi-networks";
    list.setAttribute("role", "group");
    list.setAttribute("aria-label", "Danh sách mạng WiFi");
    list.setAttribute("aria-busy", "false");

    status = document.createElement("p");
    status.className = "wifi-picker-hint";
    status.setAttribute("role", "status");
    status.setAttribute("aria-live", "polite");
    status.textContent = "Chọn mạng để điền SSID. Nhập mật khẩu ở ô WiFi Password rồi bấm Kết nối WiFi.";

    const resetHint = document.createElement("p");
    resetHint.className = "wifi-picker-hint";
    resetHint.textContent = "Không thấy mạng? Bấm Xóa WiFi và khởi động lại, rồi kết nối AP để vào trang cấu hình.";

    panel.append(head, list, status, resetHint);
    ssidRow.after(panel);
    load();
  };

  const tick = () => {
    scan(document);
    const root = tableRoot();
    if (root) build(root);
  };

  tick();
  window.addEventListener("load", tick);
  setInterval(tick, 1500);
  setInterval(load, 4000);
})();
