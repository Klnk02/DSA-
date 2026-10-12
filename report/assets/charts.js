(function () {
  'use strict';

  var charts = [];

  /* ---------------- 色板 ---------------- */
  function readVars() {
    var s = getComputedStyle(document.documentElement);
    return {
      accent: s.getPropertyValue('--accent').trim() || '#2f6fed',      // 蓝：DSA 主色
      accent2: s.getPropertyValue('--accent2').trim() || '#0e9f8a',    // 绿：真收益
      ink: s.getPropertyValue('--ink').trim() || '#1a2233',
      muted: s.getPropertyValue('--muted').trim() || '#5b6675',
      rule: s.getPropertyValue('--rule').trim() || '#e4e8ef',
      bg2: s.getPropertyValue('--bg2').trim() || '#ffffff',
      neutral: '#93a0b4',      // 对照系列中性灰蓝
      neutralSoft: '#c9d1dd',  // 弱化系列浅灰
      warn: '#f59e0b',         // 阈值 / 平衡点标注琥珀
      warnInk: '#a16207'
    };
  }

  function hexToRgba(hex, alpha) {
    var h = hex.replace('#', '');
    if (h.length === 3) h = h[0] + h[0] + h[1] + h[1] + h[2] + h[2];
    var n = parseInt(h, 16);
    return 'rgba(' + ((n >> 16) & 255) + ',' + ((n >> 8) & 255) + ',' + (n & 255) + ',' + alpha + ')';
  }

  function areaGradient(color) {
    return new echarts.graphic.LinearGradient(0, 0, 0, 1, [
      { offset: 0, color: hexToRgba(color, 0.20) },
      { offset: 0.55, color: hexToRgba(color, 0.06) },
      { offset: 1, color: hexToRgba(color, 0) }
    ]);
  }

  /* ---------------- 共享片段 ---------------- */
  function baseOption(c) {
    return {
      textStyle: { fontFamily: "'InstrumentSans', 'PingFang SC', sans-serif", color: c.ink },
      animation: false
    };
  }

  function tooltipStyle(c, extra) {
    return Object.assign({
      appendToBody: true,
      confine: true,
      backgroundColor: c.bg2,
      borderColor: c.rule,
      borderWidth: 1,
      padding: [11, 14],
      textStyle: { color: c.ink, fontSize: 12 },
      extraCssText: 'box-shadow:0 12px 30px rgba(26,34,51,.14);border-radius:10px;'
    }, extra || {});
  }

  function legendStyle(c) {
    return { top: 10, right: 8, icon: 'circle', itemWidth: 8, itemHeight: 8, itemGap: 18, textStyle: { color: c.muted, fontSize: 12 } };
  }

  function axisPointerLine(c) {
    return { type: 'line', lineStyle: { color: c.neutralSoft, type: 'dashed', width: 1 } };
  }

  function xAxisStyle(c, extra) {
    return Object.assign({
      axisLine: { lineStyle: { color: c.rule } },
      axisTick: { show: false },
      axisLabel: { color: c.muted, fontSize: 11 },
      nameTextStyle: { color: c.muted, fontSize: 12 }
    }, extra || {});
  }

  function yAxisStyle(c, extra) {
    return Object.assign({
      axisLine: { show: false },
      axisTick: { show: false },
      axisLabel: { color: c.muted, fontSize: 11 },
      nameTextStyle: { color: c.muted, fontSize: 12 },
      splitLine: { lineStyle: { color: c.rule, type: 'dashed' } }
    }, extra || {});
  }

  function tooltipRows(params) {
    return params.map(function (p) {
      return '<div style="display:flex;align-items:center;gap:8px;margin:2px 0;line-height:1.55;">' +
        '<span style="width:9px;height:9px;border-radius:50%;background:' + p.color + ';flex:none;"></span>' +
        '<span style="color:#5b6675;">' + p.seriesName + '</span>' +
        '<span style="margin-left:auto;padding-left:18px;font-weight:700;font-variant-numeric:tabular-nums;color:#1a2233;">' +
        p.value + '</span></div>';
    }).join('');
  }

  function register(chart) {
    charts.push(chart);
    window.addEventListener('resize', function () { chart.resize(); });
  }

  function init(id) {
    var el = document.getElementById(id);
    if (!el) return null;
    return echarts.init(el, null, { renderer: 'svg' });
  }

  /* ---------------- 图 1：吞吐 vs 尺寸（对数） ---------------- */
  function chartThroughput(c) {
    var el = init('chart-throughput');
    if (!el) return;
    var sizeLabels = ['4K', '8K', '16K', '32K', '64K', '128K', '256K', '512K', '1M', '2M'];
    var cpu = [63.4, 89.8, 97.2, 35.9, 36.1, 35.5, 35.9, 36.0, 26.8, 16.4];
    var dsa = [0.81, 1.62, 3.45, 5.86, 9.94, 15.0, 19.7, 23.5, 26.2, 27.4];

    el.setOption(Object.assign({}, baseOption(c), {
      grid: { left: 58, right: 28, top: 48, bottom: 48 },
      legend: legendStyle(c),
      tooltip: tooltipStyle(c, {
        trigger: 'axis',
        axisPointer: axisPointerLine(c),
        formatter: function (params) {
          var title = '<div style="font-weight:700;font-size:13px;margin-bottom:5px;">块大小 ' + params[0].axisValue + '</div>';
          return title + tooltipRows(params) +
            '<div style="margin-top:7px;padding-top:6px;border-top:1px solid #e4e8ef;color:#93a0b4;font-size:11px;">吞吐 · GiB/s</div>';
        }
      }),
      xAxis: xAxisStyle(c, {
        type: 'category', data: sizeLabels,
        name: '尺寸', nameLocation: 'middle', nameGap: 32
      }),
      yAxis: yAxisStyle(c, {
        type: 'log', name: '吞吐 (GiB/s)', min: 0.5, max: 200
      }),
      series: [
        {
          name: 'CPU memcpy', type: 'line', data: cpu,
          symbol: 'circle', symbolSize: 6, showSymbol: true,
          lineStyle: { width: 2, color: c.neutral },
          itemStyle: { color: c.neutral, borderColor: '#fff', borderWidth: 1.5 },
          emphasis: { itemStyle: { borderColor: c.neutral } }
        },
        {
          name: 'DSA 单发', type: 'line', data: dsa,
          symbol: 'circle', symbolSize: 6, showSymbol: true,
          lineStyle: { width: 2.5, color: c.accent },
          itemStyle: { color: c.accent, borderColor: '#fff', borderWidth: 1.5 },
          areaStyle: { color: areaGradient(c.accent) },
          markLine: {
            silent: true, symbol: 'none',
            lineStyle: { type: 'dashed', color: c.warn, width: 1.5 },
            label: { formatter: '盈亏平衡 ≈ 1 MB', color: c.warnInk, fontSize: 11, position: 'insideEndTop' },
            data: [{ xAxis: '1M' }]
          }
        }
      ]
    }));
    register(el);
  }

  /* ---------------- 图 2：四种模式每 GB CPU 时间 ---------------- */
  function chartOffload(c) {
    var el = init('chart-offload');
    if (!el) return;
    var names = ['CPU memcpy', 'DSA 同步\nbusy-poll', 'DSA 异步\n+ sleep', 'DSA 异步\n+ umwait'];
    var vals = [82.9, 35.0, 8.5, 35.1];
    var colors = [c.neutral, c.neutralSoft, c.accent2, c.neutralSoft];

    el.setOption(Object.assign({}, baseOption(c), {
      grid: { left: 58, right: 28, top: 44, bottom: 52 },
      tooltip: tooltipStyle(c, {
        trigger: 'axis',
        axisPointer: { type: 'shadow', shadowStyle: { color: 'rgba(26,34,51,0.05)' } },
        formatter: function (params) {
          var p = params[0];
          var extra = p.dataIndex === 2
            ? '<div style="color:#0e9f8a;font-weight:700;font-size:11px;margin-top:4px;">释放率 89.76%</div>'
            : '';
          return '<div style="font-weight:700;font-size:13px;margin-bottom:5px;">' + p.name.replace('\n', ' ') + '</div>' +
            '<div style="display:flex;align-items:baseline;gap:6px;">' +
            '<span style="color:#5b6675;">每 GB CPU 时间</span>' +
            '<span style="margin-left:auto;font-weight:700;font-variant-numeric:tabular-nums;color:#1a2233;font-size:15px;">' +
            p.value + ' ms</span></div>' + extra;
        }
      }),
      xAxis: xAxisStyle(c, {
        type: 'category', data: names,
        axisLabel: { color: c.muted, fontSize: 10.5, interval: 0, lineHeight: 13 }
      }),
      yAxis: yAxisStyle(c, { type: 'value', name: '每 GB CPU 时间 (ms)', max: 100 }),
      series: [{
        name: '每 GB CPU 时间', type: 'bar', data: vals,
        barMaxWidth: 62,
        itemStyle: {
          color: function (p) { return colors[p.dataIndex]; },
          borderRadius: [4, 4, 0, 0]
        },
        label: {
          show: true, position: 'top', fontWeight: 700, fontSize: 12, lineHeight: 16,
          formatter: function (p) {
            return p.dataIndex === 2 ? p.value + '\n↓ 89.76%' : p.value + '';
          },
          color: function (p) { return p.dataIndex === 2 ? c.accent2 : c.muted; }
        },
        markLine: {
          silent: true, symbol: 'none',
          lineStyle: { type: 'dashed', color: c.neutralSoft, width: 1 },
          label: { formatter: 'CPU 基准 82.9', color: c.neutral, fontSize: 11, position: 'insideEndTop' },
          data: [{ yAxis: 82.9 }]
        }
      }]
    }));
    register(el);
  }

  /* ---------------- 图 3：4KB Batch 打包吞吐 ---------------- */
  function chartBatch(c) {
    var el = init('chart-batch');
    if (!el) return;
    var n = [4, 8, 16, 32, 64, 128, 256];
    var t = [1.62, 3.08, 3.80, 4.43, 39.77, 52.61, 65.28];

    el.setOption(Object.assign({}, baseOption(c), {
      grid: { left: 58, right: 28, top: 48, bottom: 48 },
      legend: legendStyle(c),
      tooltip: tooltipStyle(c, {
        trigger: 'axis',
        axisPointer: axisPointerLine(c),
        formatter: function (params) {
          var title = '<div style="font-weight:700;font-size:13px;margin-bottom:5px;">batch 大小 N = ' + params[0].axisValue + '</div>';
          return title + tooltipRows(params) +
            '<div style="margin-top:7px;padding-top:6px;border-top:1px solid #e4e8ef;color:#93a0b4;font-size:11px;">吞吐 · GB/s（冷拷贝，防 cache 预热）</div>';
        }
      }),
      xAxis: xAxisStyle(c, {
        type: 'category', data: n,
        name: 'batch 大小 N', nameLocation: 'middle', nameGap: 32
      }),
      yAxis: yAxisStyle(c, { type: 'log', name: '吞吐 (GB/s)', min: 1, max: 100 }),
      series: [
        {
          name: 'DSA batch', type: 'line', data: t,
          symbol: 'circle', symbolSize: 6, showSymbol: true,
          lineStyle: { width: 2.5, color: c.accent2 },
          itemStyle: { color: c.accent2, borderColor: '#fff', borderWidth: 1.5 },
          areaStyle: { color: areaGradient(c.accent2) },
          label: {
            show: true, position: 'top', distance: 8, fontWeight: 700, fontSize: 11,
            color: c.accent2,
            formatter: function (p) { return p.value >= 39 ? p.value.toFixed(1) : ''; }
          },
          markLine: {
            silent: true, symbol: 'none',
            data: [
              {
                xAxis: 64, name: '批量阈值 N=64',
                lineStyle: { type: 'dashed', color: c.warn, width: 1.5 },
                label: { formatter: '批量阈值 N=64', color: c.warnInk, fontSize: 11, position: 'insideEndTop' }
              },
              {
                yAxis: 7.9, name: 'CPU 冷拷贝基线 7.9',
                lineStyle: { type: 'dashed', color: c.neutral, width: 1 },
                label: { formatter: 'CPU 冷拷贝基线 7.9', color: c.neutral, fontSize: 11, position: 'insideEndBottom' }
              }
            ]
          }
        },
        {
          name: 'CPU 冷拷贝基线 7.9', type: 'line', data: [],
          lineStyle: { opacity: 0 }, itemStyle: { opacity: 0 }
        }
      ]
    }));
    register(el);
  }

  function render() {
    var c = readVars();
    chartThroughput(c);
    chartOffload(c);
    chartBatch(c);
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', render);
  } else {
    render();
  }
})();