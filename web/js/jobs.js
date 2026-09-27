const API = '';
const expanded = new Set();
let pollTimer = null;

function esc(s) {
  return String(s == null ? '' : s)
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;');
}

// Percent-decode a path/name. Falls back gracefully when the string contains
// literal '%' that is not a valid escape sequence.
function safeDecode(s) {
  if (!s) return '';
  try {
    return decodeURIComponent(s);
  } catch (e) {
    // decode only valid %XX runs
  }
  return String(s).replace(/(?:%[0-9A-Fa-f]{2})+/g, (m) => {
    try {
      return decodeURIComponent(m);
    } catch (e2) {
      return m;
    }
  });
}

function baseName(p) {
  const d = safeDecode(p);
  const i = Math.max(d.lastIndexOf('/'), d.lastIndexOf('\\'));
  return i >= 0 ? d.slice(i + 1) : d;
}

async function api(url) {
  const res = await fetch(API + url);
  if (!res.ok) {
    throw new Error('HTTP ' + res.status);
  }
  return res.json();
}

function render(jobs) {
  const container = document.getElementById('jobs');
  const showFinished = document.getElementById('show-finished').checked;
  const all = jobs.slice().sort((a, b) => b.id - a.id);
  const list = showFinished
    ? all
    : all.filter((j) => j.state === 'running' || j.state === 'queued');
  if (all.length === 0) {
    container.innerHTML = '<div class="empty">No jobs yet.</div>';
    return;
  }
  if (list.length === 0) {
    container.innerHTML =
      '<div class="empty">No active jobs. Tick \u201cshow finished\u201d to see history.</div>';
    return;
  }

  container.innerHTML = list
    .map((j) => {
      const open = expanded.has(j.id);
      const paths = j.current_paths || [];
      const filesHtml = paths.length
        ? paths
            .map((p) => {
              const name = esc(baseName(p));
              const full = esc(safeDecode(p));
              return `<li><span class="fname">${name}</span><span class="fpath">${full}</span></li>`;
            })
            .join('')
        : '<li class="muted">(no active files)</li>';
      const hasFiles = paths.length > 0;
      const toggleGlyph = hasFiles ? (open ? '\u25bc' : '\u25b6') : '\u2022';
      return `<div class="job ${esc(j.state)}" data-id="${j.id}">
        <div class="job-head">
          <span class="toggle ${hasFiles ? '' : 'disabled'}">${toggleGlyph}</span>
          <span class="job-id">#${j.id}</span>
          <span class="job-kind">${esc(j.kind)}</span>
          <span class="badge state-${esc(j.state)}">${esc(String(j.state).toUpperCase())}</span>
          <span class="job-stats">
            <span>proc ${j.processed}</span>
            <span>add ${j.added}</span>
            <span>upd ${j.updated}</span>
            <span>err ${j.errors}</span>
            ${j.threads ? `<span>thr ${j.threads}</span>` : ''}
          </span>
        </div>
        ${j.error ? `<div class="job-error">${esc(j.error)}</div>` : ''}
        <ul class="job-files ${open ? '' : 'hidden'}">${filesHtml}</ul>
      </div>`;
    })
    .join('');

  container.querySelectorAll('.job').forEach((el) => {
    const id = Number(el.dataset.id);
    const ul = el.querySelector('.job-files');
    const toggle = el.querySelector('.toggle');
    el.querySelector('.job-head').onclick = () => {
      const nowHidden = ul.classList.toggle('hidden');
      if (nowHidden) {
        expanded.delete(id);
        if (toggle) toggle.textContent = '\u25b6';
      } else {
        expanded.add(id);
        if (toggle) toggle.textContent = '\u25bc';
      }
    };
  });
}

function updateSummary(jobs) {
  const count = (s) => jobs.filter((j) => j.state === s).length;
  document.getElementById('summary-text').textContent =
    `${jobs.length} job(s) \u2014 ${count('running')} running, ` +
    `${count('queued')} queued, ${count('done')} done, ${count('error')} error`;
}

async function tick() {
  const dot = document.getElementById('live-dot');
  try {
    const jobs = await api('/api/jobs');
    updateSummary(jobs);
    render(jobs);
    dot.style.background = '#4caf50';
  } catch (e) {
    dot.style.background = '#e74c3c';
    document.getElementById('summary-text').textContent = 'Error: ' + e.message;
  }
}

function restartPolling() {
  if (pollTimer) {
    clearInterval(pollTimer);
  }
  const ms = Number(document.getElementById('interval').value) || 1000;
  pollTimer = setInterval(tick, ms);
}

document.getElementById('refresh-btn').onclick = tick;
document.getElementById('interval').onchange = restartPolling;
document.getElementById('show-finished').onchange = tick;

tick();
restartPolling();
