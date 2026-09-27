let currentRootId = null;
let currentPath = '';
let selectedFiles = new Map(); // path -> { trackId, isDir, name }
let currentTrackId = null;
let dbMeta = null;
let fileMeta = null;
let fileMetaLoading = false;
let selectToken = 0;

const API_BASE = '';

async function api(url, options = {}) {
  const res = await fetch(API_BASE + url, options);
  if (!res.ok) {
    throw new Error(`HTTP ${res.status}: ${await res.text()}`);
  }
  if (res.status === 204 || res.headers.get('content-length') === '0') {
    return null;
  }
  return res.json();
}

function clearSelection() {
  currentTrackId = null;
  dbMeta = null;
  fileMeta = null;
  document.getElementById('no-selection').classList.remove('hidden');
  document.getElementById('meta-panel').classList.add('hidden');
}

async function loadRoots() {
  const roots = await api('/api/roots');
  const list = document.getElementById('roots-list');
  list.innerHTML = '';
  roots.forEach(r => {
    const li = document.createElement('li');
    const label = document.createElement('span');
    label.className = 'root-path';
    label.textContent = r.name || r.path;
    label.title = r.path;
    const meta = document.createElement('small');
    meta.textContent = `${r.type || ''} · ${r.last_scan || 'never'}`;
    const del = document.createElement('button');
    del.className = 'root-delete';
    del.textContent = '\u00d7';
    del.title = 'Remove root';
    del.onclick = (e) => {
      e.stopPropagation();
      deleteRoot(r.id, r.path);
    };
    li.appendChild(label);
    li.appendChild(meta);
    li.appendChild(del);
    li.onclick = (e) => selectRoot(r.id, r.path, e);
    list.appendChild(li);
  });
}

async function deleteRoot(id, path) {
  if (!confirm(`Remove this root?\n${path}\n\nThis also removes the index of ALL files under this root. Files on disk are NOT deleted.`)) {
    return;
  }
  try {
    await api(`/api/roots/${id}`, { method: 'DELETE' });
    if (currentRootId === id) {
      currentRootId = null;
      currentPath = '';
      selectedFiles.clear();
      clearSelection();
      document.getElementById('file-list').innerHTML = '';
      document.getElementById('breadcrumb').innerHTML = '';
      updateBatchButtons();
    }
    await loadRoots();
  } catch (e) {
    alert('Error: ' + e.message);
  }
}

const rootModal = document.getElementById('root-modal');
const rootType = document.getElementById('root-type');
const davFields = document.getElementById('dav-fields');
const rootPathLabel = document.getElementById('root-path-label');
const rootModalError = document.getElementById('root-modal-error');

function syncRootType() {
  const isDav = rootType.value === 'webdav';
  davFields.classList.toggle('hidden', !isDav);
  rootPathLabel.classList.toggle('hidden', isDav);
}

function openRootModal() {
  rootModalError.textContent = '';
  rootType.value = 'local';
  syncRootType();
  rootModal.classList.remove('hidden');
}

function closeRootModal() {
  rootModal.classList.add('hidden');
  document.getElementById('add-root-form').reset();
}

document.getElementById('add-root-open-btn').onclick = openRootModal;
document.getElementById('root-modal-cancel').onclick = closeRootModal;
rootType.onchange = syncRootType;

document.getElementById('add-root-form').onsubmit = async (e) => {
  e.preventDefault();
  const result = document.getElementById('add-root-result');
  let path;
  let user = '';
  let pass = '';
  const name = document.getElementById('root-name').value.trim();
  const type = rootType.value;
  if (rootType.value === 'webdav') {
    path = document.getElementById('dav-url').value.trim();
    user = document.getElementById('dav-user').value;
    pass = document.getElementById('dav-pass').value;
  } else {
    path = document.getElementById('new-root-path').value.trim();
  }
  if (!path) {
    rootModalError.textContent = 'Path/URL is required';
    return;
  }
  rootModalError.textContent = '';
  try {
    const res = await api('/api/roots', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ path, user, pass, name, type })
    });
    result.textContent = `Added root #${res.id}`;
    closeRootModal();
    await loadRoots();
  } catch (err) {
    rootModalError.textContent = 'Error: ' + err.message;
  }
};

function selectRoot(id, path, event) {
  currentRootId = id;
  currentPath = '';
  selectedFiles.clear();
  currentTrackId = null;
  document.querySelectorAll('#roots-list li').forEach(li => li.classList.remove('active'));
  if (event && event.currentTarget) {
    event.currentTarget.classList.add('active');
  }
  document.getElementById('no-selection').classList.remove('hidden');
  document.getElementById('meta-panel').classList.add('hidden');
  document.getElementById('breadcrumb').innerHTML = `<span>${path}</span>`;
  loadBrowse();
}

async function loadBrowse() {
  if (!currentRootId) return;
  const path = currentPath ? encodeURIComponent(currentPath) : '';
  const url = `/api/fs/browse/${currentRootId}${path ? '/' + path : ''}`;
  const items = await api(url);
  renderBrowse(items);
}

function renderBrowse(items) {
  const container = document.getElementById('file-list');
  const filterEl = document.getElementById('filter-status');
  const filter = filterEl ? filterEl.value : 'all';
  container.innerHTML = '';

  const filtered = items.filter(i => {
    if (i.isDir) return true;
    if (filter === 'all') return true;
    return i.status === filter;
  });

  filtered.forEach(item => {
    const div = document.createElement('div');
    div.className = 'file-item';
    const isSelected = selectedFiles.has(item.path);
    if (isSelected) {
      div.classList.add('selected');
    }

    let badgeClass = 'badge-not-in-db';
    let badgeText = 'NEW';
    if (item.isDir) {
      badgeClass = 'badge-dir';
      badgeText = 'DIR';
    } else if (item.status === 'both') {
      badgeClass = 'badge-in-db';
      badgeText = 'IN DB';
    } else if (item.status === 'db') {
      badgeClass = 'badge-missing';
      badgeText = 'MISSING';
    } else {
      badgeClass = 'badge-not-in-db';
      badgeText = 'NEW';
    }

    div.innerHTML = `
      <input type="checkbox" ${isSelected ? 'checked' : ''} data-path="${item.path}">
      <span class="file-name">${item.name}</span>
      <span class="file-badges">
        <span class="badge ${badgeClass}">${badgeText}</span>
        ${item.trackId ? `<span class="badge badge-in-db">#${item.trackId}</span>` : ''}
      </span>
    `;

    div.querySelector('input[type="checkbox"]').onchange = (e) => {
      e.stopPropagation();
      if (e.target.checked) {
        selectedFiles.set(item.path, { trackId: item.trackId, isDir: item.isDir, name: item.name });
      } else {
        selectedFiles.delete(item.path);
      }
      div.classList.toggle('selected', e.target.checked);
      updateBatchButtons();
    };

    div.onclick = (e) => {
      if (e.target.tagName === 'INPUT') return;
      if (item.isDir) {
        const parts = currentPath ? currentPath.split('/') : [];
        parts.push(item.name);
        currentPath = parts.join('/');
        updateBreadcrumb();
        loadBrowse();
      } else if (item.inDb) {
        selectTrack(item.trackId, item.path, item.name);
      } else {
        selectDiskFile(item.path, item.name);
      }
    };

    container.appendChild(div);
  });

  updateBatchButtons();
}

function updateBreadcrumb() {
  const parts = currentPath ? currentPath.split('/') : [];
  let html = `<span onclick="navigateTo(${currentRootId}, '')">root</span>`;
  let path = '';
  parts.forEach((part, i) => {
    path += (i > 0 ? '/' : '') + part;
    html += '<span class="sep">/</span>';
    html += `<span onclick="navigateTo(${currentRootId}, '${path}')">${part}</span>`;
  });
  document.getElementById('breadcrumb').innerHTML = html;
}

function navigateTo(rootId, path) {
  currentRootId = rootId;
  currentPath = path;
  updateBreadcrumb();
  loadBrowse();
}

function updateBatchButtons() {
  let inDbCount = 0;
  let dirCount = 0;
  selectedFiles.forEach((info) => {
    if (info.isDir) dirCount++;
    else if (info.trackId > 0) inDbCount++;
  });
  const hasSelection = selectedFiles.size > 0;
  document.getElementById('batch-scan-btn').disabled = !hasSelection;
  document.getElementById('batch-delete-btn').disabled = !hasSelection;
  const canSync = inDbCount > 0 || dirCount > 0;
  document.getElementById('batch-sync-from-btn').disabled = !canSync;
  document.getElementById('batch-sync-to-btn').disabled = !canSync;
}

async function selectTrack(trackId, path, name) {
  const token = ++selectToken;
  currentTrackId = trackId;
  document.getElementById('no-selection').classList.add('hidden');
  document.getElementById('meta-panel').classList.remove('hidden');
  document.getElementById('file-name').textContent = name;
  document.getElementById('file-path').textContent = path;

  // Kick off the (potentially slow) file metadata request immediately so it
  // runs in parallel with the DB metadata rendering below instead of blocking it.
  fileMetaLoading = true;
  const fileMetaPromise = api(`/api/track/${trackId}/file-meta`).catch(() => null);
  const lyricsPromise = api(`/api/track/${trackId}/lyrics`).catch(() => null);

  let track;
  try {
    track = await api(`/api/track/${trackId}`);
  } catch (e) {
    console.error('Failed to load track:', e);
    return;
  }
  if (token !== selectToken) return;

  try {
    dbMeta = track;
    fileMeta = null;

    const namesOf = async (ids) => {
      const names = [];
      for (const id of (ids || [])) {
        try {
          const a = await api(`/api/artist/${id}`);
          names.push(a.name || '');
        } catch (e) { /* ignore */ }
      }
      return names;
    };

    const artistName = (await namesOf(track.artist_ids)).join('; ');
    let albumName = '';
    let albumYear = '';
    let albumArtistName = '';
    if (track.album_id > 0) {
      try {
        const album = await api(`/api/album/${track.album_id}`);
        albumName = album.name || '';
        albumYear = album.year || '';
        albumArtistName = (await namesOf(album.album_artists)).join('; ');
      } catch (e) { /* ignore */ }
    }
    dbMeta.artist_name = artistName;
    dbMeta.album_name = albumName;
    dbMeta.album_year = albumYear;
    dbMeta.album_artist_name = albumArtistName;

    document.getElementById('db-title').value = track.title || '';
    document.getElementById('db-artist').value = artistName;
    document.getElementById('db-album').value = albumName;
    document.getElementById('db-album-artist').value = albumArtistName;
    document.getElementById('db-year').value = albumYear;
    document.getElementById('db-track-no').value = track.track_no || 0;
    document.getElementById('db-disc-no').value = track.disc_no || 1;
    document.getElementById('db-rating').value = track.rating || 0;
    document.getElementById('db-genre').value = track.genre || '';
    dbMeta.genre = track.genre || '';
    dbMeta.album_id = track.album_id || 0;

    lyricsPromise.then((ly) => {
      if (token !== selectToken) return;
      const ta = document.getElementById('db-lyrics');
      if (ta) ta.value = (ly && ly.text) ? ly.text : '';
      const res = document.getElementById('lyrics-result');
      if (res) res.textContent = '';
    });

    const albumSection = document.getElementById('album-section');
    if (track.album_id > 0) {
      albumSection.classList.remove('hidden');
      document.getElementById('album-name').value = albumName;
      document.getElementById('album-artists').value = albumArtistName;
      document.getElementById('album-year').value = albumYear || 0;
      document.getElementById('album-result').textContent = '';
    } else {
      albumSection.classList.add('hidden');
    }

    const display = document.getElementById('file-meta-display');
    display.innerHTML = `
      <div class="meta-row"><span class="meta-label">File</span><span class="meta-value" style="color:#888">Loading file metadata…</span></div>
    `;
    document.getElementById('sync-from-file-btn').disabled = true;
    document.getElementById('sync-to-file-btn').disabled = true;
    showDiff();

    const fileMetaData = await fileMetaPromise;
    if (token !== selectToken) return;
    fileMetaLoading = false;
    fileMeta = fileMetaData;
    renderFileMetaPanel(fileMetaData);
    const fileMissing = !fileMetaData;
    document.getElementById('sync-from-file-btn').disabled = fileMissing;
    document.getElementById('sync-to-file-btn').disabled = fileMissing;
    showDiff();
  } catch (e) {
    console.error('Failed to load track:', e);
    fileMetaLoading = false;
  }
}

function renderFileMetaPanel(fileMetaData) {
  const display = document.getElementById('file-meta-display');
  if (fileMetaData) {
    display.innerHTML = `
      <div class="meta-row"><span class="meta-label">Title</span><span class="meta-value">${fileMetaData.title || ''}</span></div>
      <div class="meta-row"><span class="meta-label">Artist</span><span class="meta-value">${fileMetaData.artist || ''}</span></div>
      <div class="meta-row"><span class="meta-label">Album Artist</span><span class="meta-value">${(fileMetaData.album_artists || []).join('; ')}</span></div>
      <div class="meta-row"><span class="meta-label">Album</span><span class="meta-value">${fileMetaData.album || ''}</span></div>
      <div class="meta-row"><span class="meta-label">Genre</span><span class="meta-value">${fileMetaData.genre || ''}</span></div>
      <div class="meta-row"><span class="meta-label">Year</span><span class="meta-value">${fileMetaData.year || ''}</span></div>
      <div class="meta-row"><span class="meta-label">Track #</span><span class="meta-value">${fileMetaData.track_no || ''}</span></div>
      <div class="meta-row"><span class="meta-label">Disc #</span><span class="meta-value">${fileMetaData.disc_no || ''}</span></div>
      <div class="meta-row"><span class="meta-label">Comment</span><span class="meta-value">${fileMetaData.comment || ''}</span></div>
      <div class="meta-row"><span class="meta-label">Size</span><span class="meta-value">${fileMetaData.size_bytes ? (fileMetaData.size_bytes / 1048576).toFixed(2) + ' MB' : ''}</span></div>
      <div class="meta-row"><span class="meta-label">Bitrate</span><span class="meta-value">${fileMetaData.bitrate ? fileMetaData.bitrate + ' kbps' : ''}</span></div>
    `;
  } else {
    display.innerHTML = `
      <div class="meta-row"><span class="meta-label">Status</span><span class="meta-value" style="color:#e53935">Cannot read file metadata</span></div>
    `;
  }
}

async function selectDiskFile(path, name) {
  const token = ++selectToken;
  currentTrackId = null;
  dbMeta = null;
  fileMeta = null;
  fileMetaLoading = true;
  document.getElementById('no-selection').classList.add('hidden');
  document.getElementById('meta-panel').classList.remove('hidden');
  document.getElementById('file-name').textContent = name;
  document.getElementById('file-path').textContent = path;

  ['db-title', 'db-artist', 'db-album', 'db-album-artist', 'db-genre'].forEach(
    (id) => { document.getElementById(id).value = ''; });
  ['db-track-no', 'db-disc-no', 'db-rating', 'db-year'].forEach(
    (id) => { document.getElementById(id).value = 0; });
  document.getElementById('album-section').classList.add('hidden');
  const diskLyrics = document.getElementById('db-lyrics');
  if (diskLyrics) diskLyrics.value = '';
  const diskLyricsResult = document.getElementById('lyrics-result');
  if (diskLyricsResult) diskLyricsResult.textContent = '';

  document.getElementById('file-meta-display').innerHTML =
    '<div class="meta-row"><span class="meta-label">File</span><span class="meta-value" style="color:#888">Loading file metadata…</span></div>';
  document.getElementById('sync-from-file-btn').disabled = true;
  document.getElementById('sync-to-file-btn').disabled = true;
  document.getElementById('diff-list').innerHTML =
    '<h3>Not in Database</h3><p>This file is not indexed. Tick its checkbox and click “Import New” to add it, then DB metadata can be edited.</p>';

  const fileMetaData = await api(`/api/fs/file-meta/${encodeURIComponent(path)}`).catch(() => null);
  if (token !== selectToken) return;
  fileMetaLoading = false;
  fileMeta = fileMetaData;
  renderFileMetaPanel(fileMetaData);
  document.getElementById('sync-from-file-btn').disabled = true;
  document.getElementById('sync-to-file-btn').disabled = true;
}

function showDiff() {
  if (!dbMeta) return;
  const container = document.getElementById('diff-list');
  if (fileMetaLoading) {
    container.innerHTML = '<h3>Loading…</h3><p>Loading file metadata from the server…</p>';
    return;
  }
  if (!fileMeta) {
    container.innerHTML = '<h3>File Missing</h3><p style="color:#e53935">File not found on disk; only DB metadata is available.</p>';
    return;
  }
  const diffs = [];
  const pairs = [
    ['title', 'Title', dbMeta.title, fileMeta.title],
    ['artist', 'Artist', dbMeta.artist_name, fileMeta.artist],
    ['albumartist', 'Album Artist', dbMeta.album_artist_name, (fileMeta.album_artists || []).join('; ')],
    ['album', 'Album', dbMeta.album_name, fileMeta.album],
    ['genre', 'Genre', dbMeta.genre, fileMeta.genre],
    ['year', 'Year', dbMeta.album_year, fileMeta.year],
    ['track_no', 'Track #', dbMeta.track_no, fileMeta.track_no],
    ['disc_no', 'Disc #', dbMeta.disc_no, fileMeta.disc_no],
  ];
  pairs.forEach(([key, label, dbVal, fileVal]) => {
    if (String(dbVal) !== String(fileVal)) {
      diffs.push({ field: label, db: dbVal, file: fileVal });
    }
  });

  if (diffs.length === 0) {
    container.innerHTML = '<h3>Metadata Match</h3><p style="color:#4caf50">Database and file metadata are identical.</p>';
    return;
  }

  container.innerHTML = '<h3>Differences</h3>' + diffs.map(d => `
    <div class="diff-item">
      <span class="diff-field">${d.field}</span>
      <span class="diff-values">
        <span class="diff-db">DB: ${d.db || '(empty)'}</span>
        <span class="diff-file">File: ${d.file || '(empty)'}</span>
      </span>
    </div>
  `).join('');
}

document.getElementById('db-meta-form').onsubmit = async (e) => {
  e.preventDefault();
  if (!currentTrackId) return;
  const fields = {};
  const title = document.getElementById('db-title').value;
  if (title) fields.title = title;
  const artist = document.getElementById('db-artist').value.trim();
  if (artist) fields.artist = artist;
  const album = document.getElementById('db-album').value.trim();
  if (album) fields.album = album;
  const albumArtist = document.getElementById('db-album-artist').value.trim();
  if (albumArtist) fields.album_artist = albumArtist;
  fields.track_no = parseInt(document.getElementById('db-track-no').value) || 0;
  fields.disc_no = parseInt(document.getElementById('db-disc-no').value) || 1;
  fields.rating = parseInt(document.getElementById('db-rating').value) || 0;
  fields.year = parseInt(document.getElementById('db-year').value) || 0;
  fields.genre = document.getElementById('db-genre').value.trim();

  try {
    await api(`/api/track/${currentTrackId}/update`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(fields)
    });
    alert('Saved to database');
    const path = document.getElementById('file-path').textContent;
    const name = document.getElementById('file-name').textContent;
    selectTrack(currentTrackId, path, name);
  } catch (e) {
    alert('Error: ' + e.message);
  }
};

document.getElementById('sync-from-file-btn').onclick = async () => {
  if (!currentTrackId) return;
  try {
    const res = await api(`/api/track/${currentTrackId}/sync-from-file`, { method: 'POST' });
    if (res && res.jobId) await pollJobs([res.jobId]);
    alert('Database updated from file');
    const path = document.getElementById('file-path').textContent;
    const name = document.getElementById('file-name').textContent;
    selectTrack(currentTrackId, path, name);
  } catch (e) {
    alert('Error: ' + e.message);
  }
};

document.getElementById('sync-to-file-btn').onclick = async () => {
  if (!currentTrackId) return;
  try {
    const res = await api(`/api/track/${currentTrackId}/sync-to-file`, { method: 'POST' });
    if (res && res.jobId) await pollJobs([res.jobId]);
    alert('File updated from database');
  } catch (e) {
    alert('Error: ' + e.message);
  }
};

document.getElementById('lyrics-save-btn').onclick = async () => {
  if (!currentTrackId) return;
  const btn = document.getElementById('lyrics-save-btn');
  const result = document.getElementById('lyrics-result');
  const text = document.getElementById('db-lyrics').value;
  btn.disabled = true;
  result.textContent = 'Saving\u2026';
  result.style.color = '#888';
  try {
    await api(`/api/track/${currentTrackId}/lyrics`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ text })
    });
    result.textContent = text.trim() ? 'Lyrics saved to database' : 'Lyrics cleared';
    result.style.color = '#4caf50';
  } catch (e) {
    result.textContent = 'Error: ' + e.message;
    result.style.color = '#e53935';
  } finally {
    btn.disabled = false;
  }
};

document.getElementById('album-update-btn').onclick = async () => {
  if (!dbMeta || !dbMeta.album_id) return;
  const body = {
    name: document.getElementById('album-name').value.trim(),
    album_artist: document.getElementById('album-artists').value.trim(),
    year: parseInt(document.getElementById('album-year').value) || 0
  };
  const path = document.getElementById('file-path').textContent;
  const name = document.getElementById('file-name').textContent;
  try {
    const res = await api(`/api/album/${dbMeta.album_id}/update`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body)
    });
    await selectTrack(currentTrackId, path, name);
    document.getElementById('album-result').textContent =
      res.merged ? `Merged into album #${res.album_id}` : `Updated album #${res.album_id}`;
  } catch (e) {
    document.getElementById('album-result').textContent = 'Error: ' + e.message;
  }
};

document.getElementById('filter-status').onchange = loadBrowse;

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
      if (j.state === 'done' || j.state === 'error') {
        pending.delete(id);
      }
    }
  }
}

document.getElementById('batch-delete-btn').onclick = async () => {
  if (!currentRootId || selectedFiles.size === 0) return;
  const actions = [];
  selectedFiles.forEach((info, path) => {
    actions.push({ action: 'delete-index', rootId: currentRootId, path: path, isDir: info.isDir });
  });
  if (actions.length === 0) return;
  if (!confirm(`Remove ${actions.length} selected item(s) from the database index (folders are recursive)? Files on disk are NOT deleted.`)) {
    return;
  }
  try {
    const results = await api('/api/fs/batch', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(actions)
    });
    let removed = 0;
    (results || []).forEach((r) => {
      if (r && typeof r.tracks_removed === 'number') removed += r.tracks_removed;
    });
    clearSelection();
    selectedFiles.clear();
    updateBatchButtons();
    await loadBrowse();
    alert(`Removed ${removed} track index(es).`);
  } catch (e) {
    alert('Error: ' + e.message);
  }
};

document.getElementById('batch-scan-btn').onclick = async () => {
  if (!currentRootId || selectedFiles.size === 0) return;
  const actions = [];
  selectedFiles.forEach((info, path) => {
    const alreadyIndexed = !info.isDir && info.trackId > 0;
    if (!alreadyIndexed) {
      actions.push({ action: 'scan', rootId: currentRootId, path: path });
    }
  });
  if (actions.length === 0) {
    alert('Nothing new selected for import');
    return;
  }
  try {
    const results = await api('/api/fs/batch', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(actions)
    });
    const jobIds = [...new Set((results || [])
      .filter((r) => r && typeof r.jobId === 'number')
      .map((r) => r.jobId))];
    if (jobIds.length > 0) {
      alert(`Import queued (${jobIds.length} job(s)). Waiting for completion...`);
      await pollJobs(jobIds);
    } else {
      alert('Import complete');
    }
    selectedFiles.clear();
    updateBatchButtons();
    await loadBrowse();
  } catch (e) {
    alert('Error: ' + e.message);
  }
};

document.getElementById('batch-sync-from-btn').onclick = async () => {
  const actions = [];
  selectedFiles.forEach((info, path) => {
    if (info.isDir) {
      actions.push({ action: 'sync-from-file', rootId: currentRootId, path: path });
    } else if (info.trackId > 0) {
      actions.push({ action: 'sync-from-file', trackId: info.trackId });
    }
  });
  if (actions.length === 0) {
    alert('Nothing to sync');
    return;
  }
  try {
    const results = await api('/api/fs/batch', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(actions)
    });
    const jobIds = (results || [])
      .filter((r) => r && typeof r.jobId === 'number')
      .map((r) => r.jobId);
    if (jobIds.length > 0) {
      await pollJobs(jobIds);
    }
    alert('Sync from file complete');
    selectedFiles.clear();
    updateBatchButtons();
    loadBrowse();
  } catch (e) {
    alert('Error: ' + e.message);
  }
};

document.getElementById('batch-sync-to-btn').onclick = async () => {
  const actions = [];
  selectedFiles.forEach((info, path) => {
    if (info.isDir) {
      actions.push({ action: 'sync-to-file', rootId: currentRootId, path: path });
    } else if (info.trackId > 0) {
      actions.push({ action: 'sync-to-file', trackId: info.trackId });
    }
  });
  if (actions.length === 0) {
    alert('Nothing to sync');
    return;
  }
  try {
    const results = await api('/api/fs/batch', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(actions)
    });
    const jobIds = (results || [])
      .filter((r) => r && typeof r.jobId === 'number')
      .map((r) => r.jobId);
    if (jobIds.length > 0) {
      await pollJobs(jobIds);
    }
    alert('Sync to file complete');
    selectedFiles.clear();
    updateBatchButtons();
    loadBrowse();
  } catch (e) {
    alert('Error: ' + e.message);
  }
};

document.getElementById('refresh-btn').onclick = () => {
  if (currentRootId) {
    loadBrowse();
  } else {
    loadRoots();
  }
};

loadRoots();
