(() => {
  "use strict";

  const state = {
    view: "line",
    granularity: "day",
    anchor: 0,
    report: null,
    chart: null,
  };

  const elements = {
    viewTabs: Array.from(document.querySelectorAll("[data-view]")),
    viewPanels: Array.from(document.querySelectorAll("[data-view-panel]")),
    tabs: Array.from(document.querySelectorAll("[data-granularity]")),
    previous: document.getElementById("previousPeriod"),
    next: document.getElementById("nextPeriod"),
    title: document.getElementById("periodTitle"),
    total: document.getElementById("periodTotal"),
    notice: document.getElementById("notice"),
    chart: document.getElementById("lineChart"),
    calendar: document.getElementById("calendar"),
  };

  function query(anchor = state.anchor) {
    window.chrome.webview.postMessage(
      `query|${state.granularity}|${anchor || 0}`
    );
  }

  function formatUnits(value) {
    return `${formatNumber(value)} 字`;
  }

  function formatNumber(value) {
    return Number(value || 0).toLocaleString("zh-CN");
  }

  function selectView(view) {
    state.view = view;
    elements.viewTabs.forEach((tab) => {
      const active = tab.dataset.view === view;
      tab.classList.toggle("active", active);
      tab.setAttribute("aria-selected", String(active));
    });
    elements.viewPanels.forEach((panel) => {
      panel.hidden = panel.dataset.viewPanel !== view;
    });
    if (view === "line") {
      requestAnimationFrame(() => state.chart?.resize());
    }
  }

  function textColor(value, maximum) {
    if (!value || !maximum) {
      return getComputedStyle(document.documentElement)
        .getPropertyValue("--muted")
        .trim();
    }
    const ratio = Math.sqrt(Math.min(1, value / maximum));
    const hue = Math.round(205 - ratio * 201);
    const lightness = matchMedia("(prefers-color-scheme: dark)").matches
      ? 70
      : 42;
    return `hsl(${hue} 76% ${lightness}%)`;
  }

  function renderChart(report) {
    if (!state.chart) {
      state.chart = echarts.init(elements.chart, null, { renderer: "canvas" });
    }
    const styles = getComputedStyle(document.documentElement);
    const text = styles.getPropertyValue("--text").trim();
    const muted = styles.getPropertyValue("--muted").trim();
    const border = styles.getPropertyValue("--border").trim();
    const accent = styles.getPropertyValue("--accent").trim();
    const values = report.points.map((point) => ({
      value: point.value,
      symbolSize: point.current ? 10 : 6,
      itemStyle: point.current
        ? { color: accent, borderColor: text, borderWidth: 2 }
        : { color: accent },
    }));

    state.chart.setOption(
      {
        animationDuration: 240,
        textStyle: {
          color: text,
          fontFamily: "Segoe UI, Microsoft YaHei UI, sans-serif",
        },
        grid: { top: 28, right: 24, bottom: 42, left: 68 },
        tooltip: {
          trigger: "axis",
          formatter: (parameters) => {
            const index = parameters[0]?.dataIndex ?? 0;
            const point = report.points[index];
            return point
              ? `${point.period}<br><strong>${formatUnits(point.value)}</strong>`
              : "";
          },
        },
        xAxis: {
          type: "category",
          boundaryGap: false,
          data: report.points.map((point) => point.label),
          axisLabel: { color: muted, hideOverlap: true },
          axisLine: { lineStyle: { color: border } },
          axisTick: { show: false },
        },
        yAxis: {
          type: "value",
          minInterval: 1,
          axisLabel: {
            color: muted,
            formatter: (value) => Number(value).toLocaleString("zh-CN"),
          },
          splitLine: { lineStyle: { color: border, type: "dashed" } },
        },
        series: [
          {
            name: "输入字数",
            type: "line",
            data: values,
            smooth: 0.22,
            showSymbol: true,
            lineStyle: { color: accent, width: 2.5 },
            areaStyle: { color: accent, opacity: 0.08 },
          },
        ],
      },
      true
    );
  }

  function renderCalendar(report) {
    elements.calendar.replaceChildren();
    elements.calendar.className = `calendar ${report.granularity}`;

    if (report.granularity === "day") {
      ["一", "二", "三", "四", "五", "六", "日"].forEach((name) => {
        const weekday = document.createElement("div");
        weekday.className = "weekday";
        weekday.textContent = `周${name}`;
        elements.calendar.append(weekday);
      });
      for (let index = 0; index < report.weekdayOffset; index += 1) {
        const spacer = document.createElement("div");
        spacer.className = "calendar-spacer";
        elements.calendar.append(spacer);
      }
    }

    const maximum = Math.max(0, ...report.points.map((point) => point.value));
    report.points.forEach((point) => {
      const cell = document.createElement("div");
      cell.className = `period-cell${point.current ? " current" : ""}`;
      cell.title = `${point.period}：${formatUnits(point.value)}`;

      const label = document.createElement("div");
      label.className = "period-label";
      label.textContent = point.label;

      const value = document.createElement("div");
      value.className = "period-value";
      value.style.color = textColor(point.value, maximum);
      value.textContent = formatNumber(point.value);

      cell.append(label, value);
      if (point.current) {
        const badge = document.createElement("span");
        badge.className = "current-badge";
        badge.textContent = "当前";
        cell.append(badge);
      }
      elements.calendar.append(cell);
    });
  }

  function render(report) {
    state.report = report;
    state.granularity = report.granularity;
    state.anchor = report.anchor;

    elements.tabs.forEach((tab) => {
      const active = tab.dataset.granularity === report.granularity;
      tab.classList.toggle("active", active);
      tab.setAttribute("aria-selected", String(active));
    });
    elements.title.textContent = report.title;
    elements.previous.disabled = !report.previousAnchor;
    elements.previous.dataset.anchor = report.previousAnchor || 0;
    elements.next.disabled = !report.nextAnchor;
    elements.next.dataset.anchor = report.nextAnchor || 0;
    const total = report.points.reduce((sum, point) => sum + point.value, 0);
    elements.total.textContent = formatUnits(total);
    elements.notice.hidden = report.available;

    renderChart(report);
    renderCalendar(report);
  }

  elements.tabs.forEach((tab) => {
    tab.addEventListener("click", () => {
      if (state.granularity === tab.dataset.granularity) {
        return;
      }
      state.granularity = tab.dataset.granularity;
      state.anchor = 0;
      query(0);
    });
  });

  elements.viewTabs.forEach((tab) => {
    tab.addEventListener("click", () => selectView(tab.dataset.view));
  });

  elements.previous.addEventListener("click", () => {
    const anchor = Number(elements.previous.dataset.anchor || 0);
    if (anchor) {
      query(anchor);
    }
  });

  elements.next.addEventListener("click", () => {
    const anchor = Number(elements.next.dataset.anchor || 0);
    if (anchor) {
      query(anchor);
    }
  });

  window.chrome.webview.addEventListener("message", (event) => {
    if (event.data?.type === "report") {
      render(event.data);
    }
  });

  window.addEventListener("resize", () => state.chart?.resize());
  matchMedia("(prefers-color-scheme: dark)").addEventListener("change", () => {
    if (state.report) {
      renderChart(state.report);
      renderCalendar(state.report);
    }
  });

  window.chrome.webview.postMessage("ready");
})();
