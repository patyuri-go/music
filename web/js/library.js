const API_BASE = '';
const artistNameCache = new Map();

async function api(url, options = {}) {
  const res = await fetch(API_BASE + url, options);
  if (!res.ok) {
    throw new Error(`HTTP ${res.status}: ${await res.text()}`);
  }
  return res.json();
}

function esc(s) {
  return String(s == null ? '' : s).replace(/[&<>"']/g, c => ({
    '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'
  }[c]));
}

function fmtDuration(ms) {
  if (!ms || ms <= 0) return '-';
  const total = Math.round(ms / 1000);
  const m = Math.floor(total / 60);
  const s = total % 60;
  return `${m}:${String(s).padStart(2, '0')}`;
}

function fmtBytes(b) {
  if (!b || b <= 0) return '-';
  const mb = b / (1024 * 1024);
  return `${mb.toFixed(2)} MB (${b} bytes)`;
}

async function artistName(id) {
  if (artistNameCache.has(id)) return artistNameCache.get(id);
  try {
    const a = await api(`/api/artist/${id}`);
    artistNameCache.set(id, a.name);
    return a.name;
  } catch (e) {
    return `#${id}`;
  }
}

async function namesOf(ids) {
  const out = [];
  for (const id of (ids || [])) {
    out.push(await artistName(id));
  }
  return out;
}

// ---------------------------------------------------------------- tree ----

function makeNode(type, id, label, count) {
  const li = document.createElement('li');
  li.className = 'tree-node';
  li.dataset.type = type;
  li.dataset.id = id;

  const row = document.createElement('div');
  row.className = `node-row type-${type}`;

  const toggle = document.createElement('span');
  toggle.className = 'node-toggle' + (type === 'track' ? ' leaf' : '');
  toggle.textContent = '\u25B6';

  const labelEl = document.createElement('span');
  labelEl.className = 'node-label';
  labelEl.textContent = label;

  row.appendChild(toggle);
  row.appendChild(labelEl);
  if (count !== undefined && count !== null) {
    const c = document.createElement('span');
    c.className = 'node-count';
    c.textContent = `(${count})`;
    row.appendChild(c);
  }
  li.appendChild(row);

  const children = document.createElement('ul');
  children.className = 'children hidden';
  li.appendChild(children);

  toggle.onclick = (e) => {
    e.stopPropagation();
    toggleNode(li);
  };
  row.onclick = () => {
    selectNode(li);
    if (type !== 'track') toggleNode(li);
  };
  return li;
}

// Build a node whose children are already known (used for search results), so
// expanding it does not require another request.
function makeLoadedNode(type, id, label, count, childNodes) {
  const li = makeNode(type, id, label, count);
  const children = li.querySelector(':scope > ul.children');
  children.innerHTML = '';
  for (const c of (childNodes || [])) {
    children.appendChild(c);
  }
  li.dataset.loaded = '1';
  return li;
}

function renderSearchResults(data) {
  const tree = document.getElementById('tree');
  const status = document.getElementById('tree-status');
  tree.innerHTML = '';
  const artists = data.artists || [];
  if (artists.length === 0) {
    tree.innerHTML = '<li class="tree-loading">No matches</li>';
    status.textContent = 'No results';
    return;
  }
  let albumCount = 0;
  let trackCount = 0;
  for (const a of artists) {
    const albumNodes = (a.albums || []).map((al) => {
      albumCount++;
      const trackNodes = (al.tracks || []).map((t) => {
        trackCount++;
        const no = t.track_no > 0 ? `${t.track_no}. ` : '';
        return makeNode('track', t.id, no + t.title, null);
      });
      return makeLoadedNode('album', al.id, al.name, trackNodes.length, trackNodes);
    });
    tree.appendChild(
      makeLoadedNode('albumartist', a.id, a.name, albumNodes.length, albumNodes));
  }
  status.textContent =
    `${artists.length} artist(s), ${albumCount} album(s), ${trackCount} track(s)`;
}

async function toggleNode(li) {
  const type = li.dataset.type;
  if (type === 'track') return;
  const children = li.querySelector(':scope > ul.children');
  const toggle = li.querySelector(':scope > .node-row > .node-toggle');

  if (li.dataset.loaded === '1') {
    const hidden = children.classList.toggle('hidden');
    toggle.textContent = hidden ? '\u25B6' : '\u25BC';
    return;
  }

  children.innerHTML = '<li class="tree-loading">Loading\u2026</li>';
  children.classList.remove('hidden');
  toggle.textContent = '\u25BC';

  try {
    if (type === 'albumartist') {
      const a = await api(`/api/artist/${li.dataset.id}`);
      children.innerHTML = '';
      for (const album of (a.albums || [])) {
        children.appendChild(makeNode('album', album.id, album.name, null));
      }
      if (!a.albums || a.albums.length === 0) {
        children.innerHTML = '<li class="tree-loading">No albums</li>';
      }
    } else if (type === 'album') {
      const al = await api(`/api/album/${li.dataset.id}`);
      children.innerHTML = '';
      for (const t of (al.tracks || [])) {
        const no = t.track_no > 0 ? `${t.track_no}. ` : '';
        children.appendChild(makeNode('track', t.id, no + t.title, null));
      }
      if (!al.tracks || al.tracks.length === 0) {
        children.innerHTML = '<li class="tree-loading">No tracks</li>';
      }
    }
    li.dataset.loaded = '1';
  } catch (e) {
    children.innerHTML = `<li class="tree-loading">Error: ${esc(e.message)}</li>`;
  }
}

function selectNode(li) {
  document.querySelectorAll('.node-row.selected').forEach(el => el.classList.remove('selected'));
  li.querySelector(':scope > .node-row').classList.add('selected');
  const type = li.dataset.type;
  const id = parseInt(li.dataset.id, 10);
  if (type === 'albumartist') showAlbumArtist(id);
  else if (type === 'album') showAlbum(id);
  else if (type === 'track') showTrack(id);
}

async function loadAlbumArtists() {
  const tree = document.getElementById('tree');
  const status = document.getElementById('tree-status');
  tree.innerHTML = '<li class="tree-loading">Loading\u2026</li>';
  status.textContent = '';
  try {
    const list = await api('/api/album-artists');
    artistNameCache.clear();
    for (const a of list) artistNameCache.set(a.id, a.name);
    tree.innerHTML = '';
    for (const a of list) {
      tree.appendChild(makeNode('albumartist', a.id, a.name, a.album_count));
    }
    status.textContent = `${list.length} album artist(s)`;
  } catch (e) {
    tree.innerHTML = `<li class="tree-loading">Error: ${esc(e.message)}</li>`;
    status.textContent = 'Failed to load';
  }
}

// -------------------------------------------------------------- detail ----

function showPlaceholder(show) {
  document.getElementById('no-selection').classList.toggle('hidden', !show);
  document.getElementById('detail').classList.toggle('hidden', show);
}

async function pollJobs(jobIds) {
  const pending = new Set(jobIds);
  const deadline = Date.now() + 10 * 60 * 1000;
  while (pending.size > 0 && Date.now() < deadline) {
    await new Promise((r) => setTimeout(r, 1000));
    for (const id of Array.from(pending)) {
      let j;
      try {
        j = await api(`/api/jobs/${id}`);
      } catch (e) {
        continue;
      }
      if (j.state === 'done' || j.state === 'error') pending.delete(id);
    }
  }
}

function actionsHtml(kind) {
  const what =
    kind === 'artist'
      ? 'all songs of this album artist'
      : kind === 'album'
        ? 'all songs of this album'
        : 'this song';
  return `<div class="detail-section"><h3>Actions</h3>
    <div class="action-row">
      <button class="act-sync-from">File \u2192 DB</button>
      <button class="act-sync-to">DB \u2192 File</button>
      <button class="act-delete danger">Delete Index</button>
    </div>
    <div class="action-note">Applies to ${what}.</div>
    <div id="action-result" class="save-result"></div>
  </div>`;
}

function wireEntityActions(kind, id) {
  const detail = document.getElementById('detail');
  const from = detail.querySelector('.act-sync-from');
  const to = detail.querySelector('.act-sync-to');
  const del = detail.querySelector('.act-delete');
  if (!from) return;
  from.onclick = () => runEntityAction(kind, id, 'sync-from-file');
  to.onclick = () => runEntityAction(kind, id, 'sync-to-file');
  del.onclick = () => runEntityAction(kind, id, 'delete-index');
}

async function runEntityAction(kind, id, action) {
  const detail = document.getElementById('detail');
  const result = document.getElementById('action-result');
  const buttons = detail.querySelectorAll(
    '.act-sync-from, .act-sync-to, .act-delete');
  const key =
    kind === 'artist' ? 'artistId' : kind === 'album' ? 'albumId' : 'trackId';
  if (action === 'delete-index' &&
      !confirm('Remove this from the database index? Files on disk are NOT deleted.')) {
    return;
  }
  buttons.forEach((b) => { b.disabled = true; });
  if (result) {
    result.className = 'save-result';
    result.textContent = 'Working\u2026';
  }
  try {
    const res = await api('/api/fs/batch', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify([{ action, [key]: id }])
    });
    const jobIds = (res || [])
      .filter((r) => r && typeof r.jobId === 'number')
      .map((r) => r.jobId);
    if (jobIds.length > 0) await pollJobs(jobIds);
    if (action === 'delete-index') {
      let removed = 0;
      (res || []).forEach((r) => {
        if (r && typeof r.tracks_removed === 'number') removed += r.tracks_removed;
      });
      await refreshTree();
      showPlaceholder(true);
      alert(`Removed ${removed} track index(es).`);
      return;
    }
    if (result) {
      result.className = 'save-result ok';
      result.textContent = 'Done.';
    }
    if (kind === 'artist') await showAlbumArtist(id);
    else if (kind === 'album') await showAlbum(id);
    else await showTrack(id);
  } catch (e) {
    if (result) {
      result.className = 'save-result err';
      result.textContent = 'Error: ' + e.message;
    }
    buttons.forEach((b) => { b.disabled = false; });
  }
}

async function showAlbumArtist(id) {
  showPlaceholder(false);
  const detail = document.getElementById('detail');
  detail.innerHTML = '<div class="placeholder">Loading\u2026</div>';
  let a;
  try {
    a = await api(`/api/artist/${id}`);
  } catch (e) {
    detail.innerHTML = `<div class="placeholder">Error: ${esc(e.message)}</div>`;
    return;
  }
  const albums = a.albums || [];
  let html = `<h2>${esc(a.name)}</h2><div class="subtitle">Album Artist</div>`;
  html += `<div class="detail-section"><h3>Information</h3><div class="info-grid">`;
  html += `<div class="k">ID</div><div class="v">${a.id}</div>`;
  html += `<div class="k">Name</div><div class="v">${esc(a.name)}</div>`;
  html += `<div class="k">Sort name</div><div class="v">${esc(a.sort_name)}</div>`;
  html += `<div class="k">Albums</div><div class="v">${albums.length}</div>`;
  html += `</div></div>`;
  html += `<div class="detail-section"><h3>Albums</h3><ul class="mini-list" id="artist-albums">`;
  for (const al of albums) {
    html += `<li data-album-id="${al.id}">${esc(al.name)}${al.year ? ` (${al.year})` : ''}</li>`;
  }
  if (albums.length === 0) html += '<li>No albums</li>';
  html += `</ul></div>`;
  html += actionsHtml('artist');
  detail.innerHTML = html;

  detail.querySelectorAll('#artist-albums li[data-album-id]').forEach(li => {
    li.onclick = () => showAlbum(parseInt(li.dataset.albumId, 10));
  });
  wireEntityActions('artist', id);
}

async function showAlbum(id) {
  showPlaceholder(false);
  const detail = document.getElementById('detail');
  detail.innerHTML = '<div class="placeholder">Loading\u2026</div>';
  let al;
  try {
    al = await api(`/api/album/${id}`);
  } catch (e) {
    detail.innerHTML = `<div class="placeholder">Error: ${esc(e.message)}</div>`;
    return;
  }
  const artistNames = (await namesOf(al.album_artists)).join('; ');
  const tracks = al.tracks || [];

  let html = `<h2>${esc(al.name)}</h2><div class="subtitle">Album</div>`;
  html += `<div class="detail-section"><h3>Cover</h3><div class="cover-preview">`;
  if (al.cover_path) {
    html += `<img id="cover-img" src="/api/album/${al.id}/cover" alt="cover">`;
    html += `<div id="cover-missing" class="missing" style="display:none">Cover could not be loaded</div>`;
  } else {
    html += `<div class="missing">No cover</div>`;
  }
  html += `</div></div>`;

  html += `<div class="detail-section"><h3>Album Metadata</h3>`;
  html += `<label>Name <input type="text" id="alb-name" value="${esc(al.name)}"></label>`;
  html += `<label>Album Artists <input type="text" id="alb-artists" placeholder="multiple: ; or /" value="${esc(artistNames)}"></label>`;
  html += `<label>Year <input type="number" id="alb-year" min="0" value="${al.year || 0}"></label>`;
  html += `<label>Cover Path <input type="text" id="alb-cover" value="${esc(al.cover_path)}"></label>`;
  html += `<button id="alb-save">Save to DB</button>`;
  html += `<div id="album-result" class="save-result"></div>`;
  html += `</div>`;

  html += `<div class="detail-section"><h3>Tracks (${tracks.length})</h3><ul class="mini-list" id="album-tracks">`;
  for (const t of tracks) {
    const no = t.track_no > 0 ? `${t.track_no}. ` : '';
    html += `<li data-track-id="${t.id}">${esc(no + t.title)} <span class="node-count">${fmtDuration(t.duration_ms)}</span></li>`;
  }
  if (tracks.length === 0) html += '<li>No tracks</li>';
  html += `</ul></div>`;

  html += actionsHtml('album');

  detail.innerHTML = html;

  const img = document.getElementById('cover-img');
  if (img) {
    img.onerror = () => {
      img.style.display = 'none';
      const m = document.getElementById('cover-missing');
      if (m) m.style.display = 'block';
    };
  }
  detail.querySelectorAll('#album-tracks li[data-track-id]').forEach(li => {
    li.onclick = () => showTrack(parseInt(li.dataset.trackId, 10));
  });
  document.getElementById('alb-save').onclick = () => saveAlbum(al.id);
  wireEntityActions('album', al.id);
}

async function saveAlbum(id) {
  const btn = document.getElementById('alb-save');
  const result = document.getElementById('album-result');
  const payload = {
    name: document.getElementById('alb-name').value,
    album_artist: document.getElementById('alb-artists').value,
    year: parseInt(document.getElementById('alb-year').value, 10) || 0,
    cover_path: document.getElementById('alb-cover').value
  };
  btn.disabled = true;
  result.className = 'save-result';
  result.textContent = 'Saving\u2026';
  try {
    const res = await api(`/api/album/${id}/update`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload)
    });
    await refreshTree();
    await showAlbum(res.album_id);
    const el = document.getElementById('album-result');
    if (el) {
      el.className = 'save-result ok';
      el.textContent = res.merged
        ? `Saved (merged into album #${res.album_id})`
        : `Saved album #${res.album_id}`;
    }
  } catch (e) {
    result.className = 'save-result err';
    result.textContent = 'Error: ' + e.message;
    btn.disabled = false;
  }
}

async function showTrack(id) {
  showPlaceholder(false);
  const detail = document.getElementById('detail');
  detail.innerHTML = '<div class="placeholder">Loading\u2026</div>';
  let t;
  try {
    t = await api(`/api/track/${id}`);
  } catch (e) {
    detail.innerHTML = `<div class="placeholder">Error: ${esc(e.message)}</div>`;
    return;
  }
  const artistNames = (await namesOf(t.artist_ids)).join('; ');
  let albumName = '';
  let albumYear = 0;
  let albumArtistNames = '';
  if (t.album_id > 0) {
    try {
      const al = await api(`/api/album/${t.album_id}`);
      albumName = al.name;
      albumYear = al.year || 0;
      albumArtistNames = (await namesOf(al.album_artists)).join('; ');
    } catch (e) { /* ignore */ }
  }

  let html = `<h2>${esc(t.title)}</h2><div class="subtitle">Track</div>`;

  html += `<div class="detail-section"><h3>Track Metadata</h3>`;
  html += `<label>Title <input type="text" id="trk-title" value="${esc(t.title)}"></label>`;
  html += `<label>Artists <input type="text" id="trk-artist" placeholder="multiple: ; or /" value="${esc(artistNames)}"></label>`;
  html += `<label>Album <input type="text" id="trk-album" value="${esc(albumName)}"></label>`;
  html += `<label>Album Artists <input type="text" id="trk-album-artist" placeholder="multiple: ; or /" value="${esc(albumArtistNames)}"></label>`;
  html += `<label>Track No <input type="number" id="trk-track" min="0" value="${t.track_no || 0}"></label>`;
  html += `<label>Disc No <input type="number" id="trk-disc" min="1" value="${t.disc_no || 1}"></label>`;
  html += `<label>Year <input type="number" id="trk-year" min="0" value="${albumYear}"></label>`;
  html += `<label>Genre <input type="text" id="trk-genre" value="${esc(t.genre || '')}"></label>`;
  html += `<label>Rating <input type="number" id="trk-rating" min="0" max="5" value="${t.rating || 0}"></label>`;
  html += `<button id="trk-save">Save to DB</button>`;
  html += `<div id="track-result" class="save-result"></div>`;
  html += `</div>`;

  html += `<div class="detail-section"><h3>Lyrics</h3>`;
  html += `<label>Lyrics (LRC or plain text) <textarea id="trk-lyrics" placeholder="Paste lyrics here\u2026"></textarea></label>`;
  html += `<button id="trk-lyrics-save">Save Lyrics to DB</button>`;
  html += `<div id="lyrics-result" class="save-result"></div>`;
  html += `</div>`;

  html += `<div class="detail-section"><h3>File / Playback Info</h3><div class="info-grid">`;
  html += `<div class="k">ID</div><div class="v">${t.id}</div>`;
  html += `<div class="k">Path</div><div class="v">${esc(t.path)}</div>`;
  html += `<div class="k">Album ID</div><div class="v">${t.album_id || '-'}</div>`;
  html += `<div class="k">Duration</div><div class="v">${fmtDuration(t.duration_ms)} (${t.duration_ms || 0} ms)</div>`;
  html += `<div class="k">Format</div><div class="v">${esc(t.format)}</div>`;
  html += `<div class="k">Size</div><div class="v">${fmtBytes(t.size_bytes)}</div>`;
  html += `<div class="k">Bitrate</div><div class="v">${t.bitrate || '-'} kbps</div>`;
  html += `<div class="k">Sample rate</div><div class="v">${t.sample_rate || '-'} Hz</div>`;
  html += `<div class="k">Channels</div><div class="v">${t.channels || '-'}</div>`;
  html += `<div class="k">Added at</div><div class="v">${esc(t.added_at)}</div>`;
  html += `<div class="k">Play count</div><div class="v">${t.play_count || 0}</div>`;
  html += `</div></div>`;

  html += actionsHtml('track');

  detail.innerHTML = html;
  document.getElementById('trk-save').onclick = () => saveTrack(t.id);
  document.getElementById('trk-lyrics-save').onclick = () => saveTrackLyrics(t.id);
  wireEntityActions('track', t.id);
  try {
    const ly = await api(`/api/track/${t.id}/lyrics`);
    const ta = document.getElementById('trk-lyrics');
    if (ta) ta.value = ly.text || '';
  } catch (e) { /* ignore */ }
}

async function saveTrackLyrics(id) {
  const btn = document.getElementById('trk-lyrics-save');
  const result = document.getElementById('lyrics-result');
  const text = document.getElementById('trk-lyrics').value;
  btn.disabled = true;
  result.className = 'save-result';
  result.textContent = 'Saving\u2026';
  try {
    await api(`/api/track/${id}/lyrics`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ text })
    });
    result.className = 'save-result ok';
    result.textContent = text.trim() ? 'Lyrics saved to database' : 'Lyrics cleared';
  } catch (e) {
    result.className = 'save-result err';
    result.textContent = 'Error: ' + e.message;
  } finally {
    btn.disabled = false;
  }
}

async function saveTrack(id) {
  const btn = document.getElementById('trk-save');
  const result = document.getElementById('track-result');
  const payload = {
    title: document.getElementById('trk-title').value,
    artist: document.getElementById('trk-artist').value,
    album: document.getElementById('trk-album').value,
    album_artist: document.getElementById('trk-album-artist').value,
    track_no: parseInt(document.getElementById('trk-track').value, 10) || 0,
    disc_no: parseInt(document.getElementById('trk-disc').value, 10) || 1,
    year: parseInt(document.getElementById('trk-year').value, 10) || 0,
    genre: document.getElementById('trk-genre').value,
    rating: parseInt(document.getElementById('trk-rating').value, 10) || 0
  };
  btn.disabled = true;
  result.className = 'save-result';
  result.textContent = 'Saving\u2026';
  try {
    await api(`/api/track/${id}/update`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload)
    });
    await refreshTree();
    await showTrack(id);
    const el = document.getElementById('track-result');
    if (el) {
      el.className = 'save-result ok';
      el.textContent = 'Saved to database';
    }
  } catch (e) {
    result.className = 'save-result err';
    result.textContent = 'Error: ' + e.message;
    btn.disabled = false;
  }
}

// --------------------------------------------------------------- boot ----

let lastSelectedType = null;
let lastSelectedId = null;

async function refreshTree() {
  const sel = document.querySelector('.node-row.selected');
  if (sel) {
    const li = sel.parentElement;
    lastSelectedType = li.dataset.type;
    lastSelectedId = li.dataset.id;
  }
  const box = document.getElementById('search-box');
  if (box) box.value = '';
  await loadAlbumArtists();
  if (lastSelectedType && lastSelectedId) {
    const node = document.querySelector(
      `.tree-node[data-type="${lastSelectedType}"][data-id="${lastSelectedId}"]`);
    if (node) node.querySelector(':scope > .node-row').classList.add('selected');
  }
}

document.getElementById('refresh-btn').onclick = async () => {
  await refreshTree();
};

let searchTimer = null;
const searchBox = document.getElementById('search-box');
searchBox.oninput = () => {
  const q = searchBox.value.trim();
  if (searchTimer) clearTimeout(searchTimer);
  searchTimer = setTimeout(async () => {
    if (!q) {
      await loadAlbumArtists();
      return;
    }
    try {
      const data = await api(`/api/library/search?q=${encodeURIComponent(q)}`);
      renderSearchResults(data);
    } catch (e) {
      document.getElementById('tree-status').textContent =
        'Search error: ' + e.message;
    }
  }, 250);
};

showPlaceholder(true);
loadAlbumArtists();
