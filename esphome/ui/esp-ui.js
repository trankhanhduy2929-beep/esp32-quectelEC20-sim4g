(() => {
  const seen = new WeakSet();
  const target = /^esp-(app|entity-table|log|entity-chart)$/;

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

  const tick = () => scan(document);
  tick();
  window.addEventListener("load", tick);
  setInterval(tick, 1500);
})();
