// 左树（pane a）多选 + 右下角匹配面板。
// 只维护「选中了哪些节点」和面板显隐；真正的递归与匹配在 link-match.js。
(function () {
  const selected = new Map(); // ref -> { ref, type, label }

  function tree() {
    return document.getElementById('tree-a');
  }

  function panel() {
    return document.getElementById('link-panel');
  }

  function nodeLabel(li) {
    const label = li.querySelector(':scope > .node-row > .node-label');
    return label ? label.textContent : li.dataset.id;
  }

  function inLeftTree(li) {
    const root = tree();
    return !!root && root.contains(li);
  }

  // 树会被搜索/展开重绘，选中状态只能按 ref 重新点亮。
  function applyHighlights() {
    const root = tree();
    if (!root) return;
    for (const li of root.querySelectorAll('li.tree-node')) {
      const row = li.querySelector(':scope > .node-row');
      if (!row) continue;
      row.classList.toggle('multi-selected', selected.has(li.dataset.id));
    }
  }

  function syncPanel() {
    const box = panel();
    if (!box) return;
    const count = document.getElementById('link-selected-count');
    if (count) count.textContent = `已选 ${selected.size} 项`;
    box.classList.toggle('open', selected.size > 0);
    box.setAttribute('aria-hidden', selected.size > 0 ? 'false' : 'true');
  }

  function clearLog() {
    const log = document.getElementById('link-log');
    if (log) log.innerHTML = '';
  }

  function clear() {
    selected.clear();
    applyHighlights();
    syncPanel();
    clearLog();
  }

  function toggle(li) {
    const ref = li.dataset.id;
    if (!ref) return;
    if (selected.has(ref)) {
      selected.delete(ref);
    } else {
      selected.set(ref, {
        ref,
        type: li.dataset.type,
        label: nodeLabel(li),
      });
    }
    applyHighlights();
    syncPanel();
  }

  // 返回 true 表示这次点击已被多选逻辑吃掉，不再触发普通单选。
  function onRowClick(li, event) {
    const ctrl = event && (event.ctrlKey || event.metaKey);
    if (!ctrl) {
      // 普通点击左树：退出多选，回到原来的单选详情。
      if (selected.size > 0 && inLeftTree(li)) {
        clear();
      }
      return false;
    }
    if (!inLeftTree(li)) return false; // 右树不参与多选
    toggle(li);
    return true;
  }

  function log(text, kind) {
    const box = document.getElementById('link-log');
    if (!box) return;
    const line = document.createElement('div');
    line.className = 'link-log-line' + (kind ? ' ' + kind : '');
    line.textContent = text;
    box.appendChild(line);
    box.scrollTop = box.scrollHeight;
  }

  function setBusy(busy) {
    const button = document.getElementById('link-start');
    if (!button) return;
    button.disabled = busy;
    button.textContent = busy ? '匹配中…' : '开始匹配';
  }

  // 目标媒体库清单：只列启用中的库；当前值尽量保留。
  async function loadTargets() {
    const select = document.getElementById('link-target');
    if (!select) return;
    let libraries = [];
    try {
      const data = await api('/api/v1/libraries');
      libraries = (data.libraries || []).filter(l => l.enabled !== false);
    } catch (e) {
      log('读取媒体库清单失败：' + e.message, 'error');
    }
    const current = select.value;
    select.innerHTML = '';
    const placeholder = document.createElement('option');
    placeholder.value = '';
    placeholder.disabled = true;
    placeholder.textContent = '选择目标媒体库';
    select.appendChild(placeholder);
    for (const l of libraries) {
      const opt = document.createElement('option');
      opt.value = l.library_id;
      opt.textContent = l.display_name || l.library_id;
      select.appendChild(opt);
    }
    if (libraries.some(l => l.library_id === current)) {
      select.value = current;
    } else {
      placeholder.selected = true;
    }
  }

  document.addEventListener('DOMContentLoaded', () => {
    const clearButton = document.getElementById('link-clear');
    if (clearButton) clearButton.onclick = clear;
    loadTargets();
    const root = tree();
    if (root) {
      new MutationObserver(applyHighlights).observe(root, {
        childList: true,
        subtree: true,
      });
    }
    syncPanel();
  });

  window.LinkSelect = {
    onRowClick,
    selected: () => [...selected.values()],
    clear,
    log,
    clearLog,
    setBusy,
    applyHighlights,
    target: () => {
      const select = document.getElementById('link-target');
      return select ? select.value : '';
    },
    targetLabel: () => {
      const select = document.getElementById('link-target');
      return select && select.selectedOptions && select.selectedOptions[0]
        ? select.selectedOptions[0].textContent
        : '';
    },
    autoFetch: () => {
      const box = document.getElementById('link-auto-fetch');
      return !!(box && box.checked);
    },
  };
})();
