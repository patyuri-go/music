const API_BASE = '';

// 读接口全部走路由器，id 一律是 SourceRef 规范串（不透明字符串）。
// 写接口（元数据保存 / 歌词保存 / 文件同步 / 删除索引）本阶段不接，
// 页面只做浏览。
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

// [{id,name}] -> "A; B"：名字随引用一起返回，不再逐个回查。
function namesText(links) {
  return (links || []).map(l => l.name).filter(n => n).join('; ');
}

// --------------------------------------------------------- 顶部开关 ----

// 自动获取：开启时点开专辑/艺术家才会按 id 取全量；关闭时只展开搜索命中的。
function autoFetchEnabled() {
  const box = document.getElementById('auto-fetch');
  return !!(box && box.checked);
}

// 每页条数：与 /api/v1/search 的 ?limit= 一致，缺省 10，上限 200。
function pageSize() {
  const input = document.getElementById('page-size');
  const n = parseInt(input && input.value, 10);
  if (!Number.isFinite(n) || n <= 0) return 10;
  return Math.min(n, 200);
}

function updateNextButton() {
  const btn = document.getElementById('next-page');
  if (!btn) return;
  btn.disabled = !panes.some(p => p.lastQuery && p.hasMore && p.cursor);
}

// ---------------------------------------------------------------- tree ----

function trackLabel(t) {
  const no = t.track_no > 0 ? `${t.track_no}. ` : '';
  return no + t.title;
}

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
  row.onclick = (e) => {
    if (window.LinkSelect && window.LinkSelect.onRowClick(li, e)) return;
    selectNode(li);
    if (type !== 'track') toggleNode(li);
  };
  return li;
}

// 搜索命中的子项是**不完整**的（只有匹配到的那些），所以只标 partial：
// 自动获取开启时，第一次展开仍然按 id 取一次全量，再用全量替换这几条。
function makeLoadedNode(type, id, label, count, childNodes) {
  const li = makeNode(type, id, label, count);
  const children = li.querySelector(':scope > ul.children');
  children.innerHTML = '';
  for (const c of (childNodes || [])) {
    children.appendChild(c);
  }
  li.dataset.partial = '1';
  return li;
}

// 展开后子项数量会变（partial -> 全量），计数跟着改。
function setNodeCount(li, count) {
  const row = li.querySelector(':scope > .node-row');
  if (!row) return;
  let c = row.querySelector('.node-count');
  if (count === null || count === undefined) {
    if (c) c.remove();
    return;
  }
  if (!c) {
    c = document.createElement('span');
    c.className = 'node-count';
    row.appendChild(c);
  }
  c.textContent = `(${count})`;
}

function childLists(ul) {
  return [...ul.children].filter(li => li.classList.contains('tree-node'));
}

function directChild(ul, type, id) {
  for (const li of ul.children) {
    if (li.dataset && li.dataset.type === type && li.dataset.id === id) {
      return li;
    }
  }
  return null;
}

function sortNodes(ul) {
  const nodes = childLists(ul);
  nodes.sort((a, b) => {
    const la = a.querySelector(':scope > .node-row > .node-label');
    const lb = b.querySelector(':scope > .node-row > .node-label');
    return (la ? la.textContent : '').localeCompare(lb ? lb.textContent : '');
  });
  for (const n of nodes) ul.appendChild(n);
}

// 一个搜索结果条目 -> 节点（命中的子项已铺好，标 partial）。
function buildAlbumNode(al) {
  const trackNodes = (al.tracks || []).map(
    t => makeNode('track', t.id, trackLabel(t), null));
  if (trackNodes.length > 0) {
    return makeLoadedNode('album', al.id, al.name, trackNodes.length, trackNodes);
  }
  return makeNode('album', al.id, al.name, null);
}

function buildArtistNode(a) {
  const albumNodes = (a.albums || []).map(buildAlbumNode);
  if (albumNodes.length > 0) {
    return makeLoadedNode('albumartist', a.id, a.name, albumNodes.length, albumNodes);
  }
  return makeNode('albumartist', a.id, a.name, null);
}

function clearPlaceholders(ul) {
  ul.querySelectorAll(':scope > li.tree-loading').forEach(el => el.remove());
}

function paneCounts(pane) {
  let albums = 0;
  let tracks = 0;
  for (const artist of childLists(pane.tree)) {
    const kids = artist.querySelector(':scope > ul.children');
    if (!kids) continue;
    for (const album of childLists(kids)) {
      albums++;
      const tks = album.querySelector(':scope > ul.children');
      if (tks) tracks += childLists(tks).length;
    }
  }
  return { artists: childLists(pane.tree).length, albums, tracks };
}

function paintStatus(pane, note) {
  const c = paneCounts(pane);
  let text = `${c.artists} artist(s), ${c.albums} album(s), ${c.tracks} track(s)`;
  if (pane.hasMore) text += ' \u00b7 more available';
  if (note) text += ` \u00b7 ${note}`;
  pane.status.textContent = text;
}

// v1 搜索的两种信封：单库 {mode:"single", value:{items,next_cursor,has_more}}；
// 聚合 {mode:"aggregate", items:[{value:hit,origin}], next_cursor, has_more}。
// 这里统一成扁平结构，其余逻辑不关心来源形态。
function v1Page(json) {
  const body = json || {};
  if (body.value && Array.isArray(body.value.items)) {
    return {
      hits: body.value.items,
      next_cursor: body.value.next_cursor || null,
      has_more: !!body.value.has_more,
    };
  }
  const hits = (Array.isArray(body.items) ? body.items : [])
    .map(it => (it && it.value) ? it.value : it)
    .filter(it => it && it.ref);
  return {
    hits,
    next_cursor: body.next_cursor || null,
    has_more: !!body.has_more,
  };
}

// 引用对象 {ref,library_id,type,id} -> 规范串（接口只认它）。
function refId(ref) {
  return ref && ref.ref ? ref.ref : '';
}

// 命中的艺术家链接形如 {name, artist|album_artist:{...}}。
function linkRef(link, key) {
  if (!link) return null;
  return link[key] || null;
}

// 命中列表 -> 「专辑艺术家 -> 专辑 -> 曲目」树，与后端聚树同一口径：
// - 专辑命中自己就是专辑节点（hit.album 为 null）；
// - 曲目命中靠 hit.album 找归属，没有就丢弃；
// - 艺术家命中只有节点本身，专辑由展开时再取。
function hitsToTree(hits) {
  const root = new Map();

  const groupOf = (hit) => {
    const link = (hit.album_artists && hit.album_artists[0]) ||
                 (hit.artists && hit.artists[0]) || null;
    const ref = link ? (link.album_artist || link.artist) : null;
    const id = refId(ref);
    const name = (link && link.name) || '(No album artist)';
    if (!root.has(id)) root.set(id, { id, name, albums: new Map() });
    return root.get(id);
  };

  for (const hit of hits) {
    const kind = hit.kind;
    if (kind === 'artist' || kind === 'album_artist') {
      const id = refId(hit.ref);
      if (!root.has(id)) {
        root.set(id, { id, name: hit.title || '', albums: new Map() });
      }
      continue;
    }
    if (kind === 'album') {
      const artist = groupOf(hit);
      const id = refId(hit.ref);
      if (!artist.albums.has(id)) {
        artist.albums.set(id, { id, name: hit.title || '', tracks: [] });
      }
      continue;
    }
    if (kind !== 'track') continue;
    const album = hit.album || null;
    const albumRef = linkRef(album, 'album');
    const albumId = refId(albumRef);
    if (!albumId) continue;
    const artist = groupOf(hit);
    if (!artist.albums.has(albumId)) {
      artist.albums.set(albumId, {
        id: albumId,
        name: album.title || '',
        tracks: [],
      });
    }
    const tracks = artist.albums.get(albumId).tracks;
    const id = refId(hit.ref);
    if (!tracks.some(t => t.id === id)) {
      // v1 的命中不含 track_no，标签只显示标题。
      tracks.push({ id, title: hit.title || '', track_no: 0 });
    }
  }

  const artists = [];
  for (const artist of root.values()) {
    artists.push({
      id: artist.id,
      name: artist.name,
      albums: [...artist.albums.values()],
    });
  }
  return { artists };
}

// 把一页搜索结果合并进已有树：新艺术家追加，已有艺术家只补新专辑/新曲目。
function mergeSearchResults(pane, data) {
  clearPlaceholders(pane.tree);
  let added = 0;
  for (const a of (data.artists || [])) {
    const artistLi = directChild(pane.tree, 'albumartist', a.id);
    if (!artistLi) {
      pane.tree.appendChild(buildArtistNode(a));
      added++;
      continue;
    }
    // 已经按 id 取过全量的节点是权威的，不再叠加。
    if (artistLi.dataset.loaded === '1' && artistLi.dataset.partial !== '1') {
      continue;
    }
    const albumsUl = artistLi.querySelector(':scope > ul.children');
    for (const al of (a.albums || [])) {
      const albumLi = directChild(albumsUl, 'album', al.id);
      if (!albumLi) {
        albumsUl.appendChild(buildAlbumNode(al));
        added++;
        continue;
      }
      if (albumLi.dataset.loaded === '1' && albumLi.dataset.partial !== '1') {
        continue;
      }
      const tracksUl = albumLi.querySelector(':scope > ul.children');
      for (const t of (al.tracks || [])) {
        if (directChild(tracksUl, 'track', t.id)) continue;
        tracksUl.appendChild(makeNode('track', t.id, trackLabel(t), null));
        added++;
      }
      const n = childLists(tracksUl).length;
      if (n > 0) setNodeCount(albumLi, n);
    }
    const cn = childLists(albumsUl).length;
    if (cn > 0) {
      setNodeCount(artistLi, cn);
      if (artistLi.dataset.loaded !== '1') artistLi.dataset.partial = '1';
    }
  }
  sortNodes(pane.tree);
  return added;
}

async function toggleNode(li) {
  const type = li.dataset.type;
  if (type === 'track') return;
  const children = li.querySelector(':scope > ul.children');
  const toggle = li.querySelector(':scope > .node-row > .node-toggle');

  const partial = li.dataset.partial === '1';
  const loaded = li.dataset.loaded === '1';

  // 自动获取关闭时，命中的节点只展开已有内容，不发请求。
  if (partial && !autoFetchEnabled() && !loaded) {
    const hidden = children.classList.toggle('hidden');
    toggle.textContent = hidden ? '\u25B6' : '\u25BC';
    return;
  }

  if (loaded && !partial) {
    const hidden = children.classList.toggle('hidden');
    toggle.textContent = hidden ? '\u25B6' : '\u25BC';
    return;
  }

  const previous = children.innerHTML;
  children.innerHTML = '<li class="tree-loading">Loading\u2026</li>';
  children.classList.remove('hidden');
  toggle.textContent = '\u25BC';

  try {
    if (type === 'albumartist') {
      const a = await api(`/api/artist/${li.dataset.id}`);
      const albums = a.albums || [];
      children.innerHTML = '';
      for (const album of albums) {
        children.appendChild(makeNode('album', album.id, album.name, null));
      }
      if (albums.length === 0) {
        children.innerHTML = '<li class="tree-loading">No albums</li>';
      } else {
        setNodeCount(li, albums.length);
      }
    } else if (type === 'album') {
      const al = await api(`/api/album/${li.dataset.id}`);
      const tracks = al.tracks || [];
      children.innerHTML = '';
      for (const t of tracks) {
        children.appendChild(makeNode('track', t.id, trackLabel(t), null));
      }
      if (tracks.length === 0) {
        children.innerHTML = '<li class="tree-loading">No tracks</li>';
      } else {
        setNodeCount(li, tracks.length);
      }
    }
    li.dataset.loaded = '1';
    delete li.dataset.partial;
  } catch (e) {
    // 取全量失败就还原命中的那几条，别把已有内容清空。
    children.innerHTML = previous;
    children.insertAdjacentHTML(
      'beforeend', `<li class="tree-loading">Error: ${esc(e.message)}</li>`);
  }
}

function selectNode(li) {
  document.querySelectorAll('.node-row.selected').forEach(el => el.classList.remove('selected'));
  li.querySelector(':scope > .node-row').classList.add('selected');
  const type = li.dataset.type;
  const id = li.dataset.id;
  if (type === 'albumartist') showAlbumArtist(id);
  else if (type === 'album') showAlbum(id);
  else if (type === 'track') showTrack(id);
}

// -------------------------------------------------------------- detail ----

function showPlaceholder(show) {
  document.getElementById('no-selection').classList.toggle('hidden', !show);
  document.getElementById('detail').classList.toggle('hidden', show);
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
  html += `<div class="k">ID</div><div class="v">${esc(a.id)}</div>`;
  html += `<div class="k">Name</div><div class="v">${esc(a.name)}</div>`;
  html += `<div class="k">Sort name</div><div class="v">${esc(a.sort_name)}</div>`;
  html += `<div class="k">Albums</div><div class="v">${albums.length}</div>`;
  html += `</div></div>`;
  html += `<div class="detail-section"><h3>Albums</h3><ul class="mini-list" id="artist-albums">`;
  for (const al of albums) {
    html += `<li data-album-id="${esc(al.id)}">${esc(al.name)}${al.year ? ` (${al.year})` : ''}</li>`;
  }
  if (albums.length === 0) html += '<li>No albums</li>';
  html += `</ul></div>`;
  detail.innerHTML = html;

  detail.querySelectorAll('#artist-albums li[data-album-id]').forEach(li => {
    li.onclick = () => showAlbum(li.dataset.albumId);
  });
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
  const artistNames = namesText(al.album_artists);
  const tracks = al.tracks || [];

  let html = `<h2>${esc(al.name)}</h2><div class="subtitle">Album</div>`;
  html += `<div class="detail-section"><h3>Cover</h3><div class="cover-preview">`;
  if (al.artwork) {
    html += `<img id="cover-img" src="/api/album/${al.id}/cover" alt="cover">`;
    html += `<div id="cover-missing" class="missing" style="display:none">Cover could not be loaded</div>`;
  } else {
    html += `<div class="missing">No cover</div>`;
  }
  html += `</div></div>`;

  html += `<div class="detail-section"><h3>Album Metadata</h3><div class="info-grid">`;
  html += `<div class="k">ID</div><div class="v">${esc(al.id)}</div>`;
  html += `<div class="k">Name</div><div class="v">${esc(al.name)}</div>`;
  html += `<div class="k">Album Artists</div><div class="v">${esc(artistNames)}</div>`;
  html += `<div class="k">Year</div><div class="v">${al.year || '-'}</div>`;
  html += `<div class="k">Genres</div><div class="v">${esc((al.genres || []).join(', '))}</div>`;
  html += `</div></div>`;

  html += `<div class="detail-section"><h3>Tracks (${tracks.length})</h3><ul class="mini-list" id="album-tracks">`;
  for (const t of tracks) {
    html += `<li data-track-id="${esc(t.id)}">${esc(trackLabel(t))} <span class="node-count">${fmtDuration(t.duration_ms)}</span></li>`;
  }
  if (tracks.length === 0) html += '<li>No tracks</li>';
  html += `</ul></div>`;

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
    li.onclick = () => showTrack(li.dataset.trackId);
  });
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
  const artistNames = namesText(t.artists);
  let albumName = t.album_name || '';
  let albumYear = 0;
  let albumArtistNames = namesText(t.album_artists);
  if (t.album_id) {
    try {
      const al = await api(`/api/album/${t.album_id}`);
      albumName = al.name || albumName;
      albumYear = al.year || 0;
      albumArtistNames = namesText(al.album_artists) || albumArtistNames;
    } catch (e) { /* ignore */ }
  }

  let html = `<h2>${esc(t.title)}</h2><div class="subtitle">Track</div>`;

  html += `<div class="detail-section"><h3>Track Metadata</h3><div class="info-grid">`;
  html += `<div class="k">ID</div><div class="v">${esc(t.id)}</div>`;
  html += `<div class="k">Title</div><div class="v">${esc(t.title)}</div>`;
  html += `<div class="k">Artists</div><div class="v">${esc(artistNames)}</div>`;
  html += `<div class="k">Album</div><div class="v">${esc(albumName)}</div>`;
  html += `<div class="k">Album Artists</div><div class="v">${esc(albumArtistNames)}</div>`;
  html += `<div class="k">Track No</div><div class="v">${t.track_no || 0}</div>`;
  html += `<div class="k">Disc No</div><div class="v">${t.disc_no || 0}</div>`;
  html += `<div class="k">Year</div><div class="v">${albumYear || t.year || '-'}</div>`;
  html += `<div class="k">Genre</div><div class="v">${esc(t.genre || '')}</div>`;
  html += `<div class="k">Rating</div><div class="v">${t.rating || 0}</div>`;
  html += `</div></div>`;

  html += `<div class="detail-section"><h3>Lyrics</h3>`;
  html += `<textarea id="trk-lyrics" readonly placeholder="No lyrics"></textarea>`;
  html += `</div>`;

  html += `<div class="detail-section"><h3>File / Playback Info</h3><div class="info-grid">`;
  html += `<div class="k">Album ID</div><div class="v">${esc(t.album_id || '-')}</div>`;
  html += `<div class="k">Duration</div><div class="v">${fmtDuration(t.duration_ms)} (${t.duration_ms || 0} ms)</div>`;
  html += `<div class="k">Format</div><div class="v">${esc(t.format)}</div>`;
  html += `<div class="k">Size</div><div class="v">${fmtBytes(t.size_bytes)}</div>`;
  html += `<div class="k">Bitrate</div><div class="v">${t.bitrate || '-'} kbps</div>`;
  html += `<div class="k">Sample rate</div><div class="v">${t.sample_rate || '-'} Hz</div>`;
  html += `<div class="k">Channels</div><div class="v">${t.channels || '-'}</div>`;
  html += `<div class="k">Added at</div><div class="v">${esc(t.added_at)}</div>`;
  html += `<div class="k">Play count</div><div class="v">${t.play_count || 0}</div>`;
  html += `</div></div>`;

  detail.innerHTML = html;
  try {
    const ly = await api(`/api/track/${t.id}/lyrics`);
    const ta = document.getElementById('trk-lyrics');
    if (ta) ta.value = ly.text || '';
  } catch (e) { /* ignore */ }
}

// --------------------------------------------------------------- panes ----

// 一棵树 = 一个 pane：来源库选择器 + 搜索类型 + 搜索框 + 树 + 状态行，
// 彼此完全独立。
function createPane(key) {
  const pane = {
    select: document.getElementById(`source-${key}`),
    type: document.getElementById(`type-${key}`),
    search: document.getElementById(`search-${key}`),
    tree: document.getElementById(`tree-${key}`),
    status: document.getElementById(`status-${key}`),
    lastQuery: '',
    cursor: null,
    hasMore: false,
    timer: null,
  };

  pane.search.oninput = () => {
    const q = pane.search.value.trim();
    if (pane.timer) clearTimeout(pane.timer);
    pane.timer = setTimeout(() => {
      if (!q) {
        pane.lastQuery = '';
        resetPane(pane);
        return;
      }
      runSearch(pane, q);
    }, 250);
  };

  // 换来源库：同一关键词在该库重搜；还没搜过就只重置提示。
  pane.select.onchange = () => {
    if (pane.lastQuery) {
      runSearch(pane, pane.lastQuery);
    } else {
      resetPane(pane);
    }
  };

  // 换搜索类型：types 计入搜索游标的 revision，旧 cursor 直接失效，
  // 所以只能按新类型重搜第一页。
  if (pane.type) {
    pane.type.onchange = () => {
      if (pane.lastQuery) {
        runSearch(pane, pane.lastQuery);
      } else {
        resetPane(pane);
      }
    };
  }

  return pane;
}

const panes = [createPane('a'), createPane('b')];

function resetPane(pane) {
  pane.cursor = null;
  pane.hasMore = false;
  pane.tree.innerHTML = '<li class="tree-loading">Type a keyword to search</li>';
  pane.status.textContent = '';
  updateNextButton();
}

// 首屏搜索：清空树，按第一页重建。
async function runSearch(pane, q) {
  pane.lastQuery = q;
  pane.cursor = null;
  pane.hasMore = false;
  try {
    const data = await fetchSearch(q, pane.select.value, null,
                                   pane.type ? pane.type.value : '');
    pane.tree.innerHTML = '';
    mergeSearchResults(pane, data);
    if (childLists(pane.tree).length === 0) {
      pane.tree.innerHTML = '<li class="tree-loading">No matches</li>';
      pane.status.textContent = 'No results';
      updateNextButton();
      return;
    }
    pane.cursor = data.next_cursor || null;
    pane.hasMore = !!data.has_more && !!pane.cursor;
    paintStatus(pane);
  } catch (e) {
    pane.status.textContent = 'Search error: ' + e.message;
  }
  updateNextButton();
}

// 获取下一分页：同一个关键词 + 同一个来源库，带上一页的 cursor 再搜一次，
// 结果合并进现有树，不清空已展示的条目。
async function fetchNextPage(pane) {
  if (!pane.lastQuery || !pane.cursor || !pane.hasMore) return;
  pane.status.textContent = 'Loading more\u2026';
  try {
    const data = await fetchSearch(pane.lastQuery, pane.select.value, pane.cursor,
                                   pane.type ? pane.type.value : '');
    const added = mergeSearchResults(pane, data);
    pane.cursor = data.next_cursor || null;
    pane.hasMore = !!data.has_more && !!pane.cursor;
    paintStatus(pane, `${added} new item(s) merged`);
  } catch (e) {
    paintStatus(pane, 'Next page error: ' + e.message);
  }
}

async function fetchSearch(q, library, cursor, type) {
  let url = `/api/v1/search?q=${encodeURIComponent(q)}&limit=${pageSize()}`;
  if (type) url += `&types=${encodeURIComponent(type)}`;
  if (library) url += `&library_id=${encodeURIComponent(library)}`;
  if (cursor) url += `&cursor=${encodeURIComponent(cursor)}`;
  const page = v1Page(await api(url));
  return {
    ...hitsToTree(page.hits),
    next_cursor: page.next_cursor,
    has_more: page.has_more,
  };
}

// 来源库清单来自注册表：id + 显示名，只列可用且支持搜索的库。
async function loadSources() {
  let libraries = [];
  try {
    const data = await api('/api/v1/libraries');
    libraries = (data.libraries || []).filter(
      l => l.enabled !== false && (l.supported || []).includes('search'));
  } catch (e) { /* 拿不到就只剩“全部来源” */ }

  for (const pane of panes) {
    const current = pane.select.value;
    pane.select.innerHTML = '';
    const all = document.createElement('option');
    all.value = '';
    all.textContent = 'All sources';
    pane.select.appendChild(all);
    for (const l of libraries) {
      const opt = document.createElement('option');
      opt.value = l.library_id;
      opt.textContent = l.display_name || l.library_id;
      pane.select.appendChild(opt);
    }
    // 刷新后尽量保留原来的选择。
    pane.select.value = libraries.some(l => l.library_id === current) ? current : '';
  }
}

document.getElementById('refresh-btn').onclick = async () => {
  await loadSources();
  for (const pane of panes) {
    if (pane.lastQuery) {
      await runSearch(pane, pane.lastQuery);
    } else {
      resetPane(pane);
    }
  }
};

// 两棵树各自用自己的 cursor 续页。
document.getElementById('next-page').onclick = async () => {
  for (const pane of panes.slice()) {
    await fetchNextPage(pane);
  }
};

// 改分页大小后，当前结果集与 cursor 不再对应，直接按新大小重搜。
document.getElementById('page-size').onchange = async () => {
  for (const pane of panes) {
    if (pane.lastQuery) await runSearch(pane, pane.lastQuery);
  }
};

showPlaceholder(true);
loadSources();
panes.forEach(resetPane);
