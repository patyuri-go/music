// 递归 + 匹配 + 调链接接口。匹配规则：
// 目标库曲目前 10 条里，标题、专辑归一化后全等，且至少一个艺术家名相同。
(function () {
  const DEFAULT_SEARCH_LIMIT = 10;

  function norm(value) {
    return String(value == null ? '' : value)
      .trim()
      .toLowerCase()
      .replace(/[\s\u3000]+/g, ' ');
  }

  function treeRoot() {
    return document.getElementById('tree-a');
  }

  function refOfHit(hit) {
    if (!hit) return '';
    if (typeof hit.ref === 'string') return hit.ref;
    return hit.ref && hit.ref.ref ? hit.ref.ref : '';
  }

  // 数组元素可能是接口返回的 {name, ...}，也可能是已提取好的名字字符串
  // （start() 里构造的 source 就是后者），两种都要能取出名字。
  function nameOf(entry) {
    if (typeof entry === 'string') return entry.trim();
    return (entry && entry.name) || '';
  }

  function artistNames(entry) {
    const list = entry && Array.isArray(entry.artists) ? entry.artists : [];
    return list.map(nameOf).filter(Boolean);
  }

  function albumArtistNames(entry) {
    const list =
      entry && Array.isArray(entry.album_artists) ? entry.album_artists : [];
    return list.map(nameOf).filter(Boolean);
  }

  // 搜索串和比对优先用专辑艺术家；平台/曲目没有时回退到曲目艺术家。
  function matchArtistNames(entry) {
    const albumArtists = albumArtistNames(entry);
    return albumArtists.length > 0 ? albumArtists : artistNames(entry);
  }

  function nodeOf(ref) {
    const root = treeRoot();
    if (!root) return null;
    for (const li of root.querySelectorAll('li.tree-node')) {
      if (li.dataset.id === ref) return li;
    }
    return null;
  }

  // 不勾 Auto fetch：只用树上已经渲染出来的内容递归，不额外取全量。
  function renderedTrackRefs(li) {
    if (!li) return [];
    if (li.dataset.type === 'track') return [li.dataset.id];
    const children = li.querySelector(':scope > ul.children');
    if (!children) return [];
    const out = [];
    for (const child of children.children) {
      if (!child.classList || !child.classList.contains('tree-node')) continue;
      out.push(...renderedTrackRefs(child));
    }
    return out;
  }

  async function albumTrackRefs(albumRef, cache) {
    if (cache.has(albumRef)) return cache.get(albumRef);
    const album = await api(`/api/album/${albumRef}`);
    const refs = (album.tracks || []).map(t => t.id).filter(Boolean);
    cache.set(albumRef, refs);
    return refs;
  }

  // 勾 Auto fetch：按 ref 把专辑/艺术家的全量内容取下来。
  async function fullTrackRefs(node, cache) {
    if (node.type === 'track') return [node.ref];
    if (node.type === 'album') {
      return albumTrackRefs(node.ref, cache);
    }
    if (node.type === 'albumartist' || node.type === 'artist') {
      const artist = await api(`/api/artist/${node.ref}`);
      const out = [];
      for (const album of artist.albums || []) {
        if (!album || !album.id) continue;
        out.push(...await albumTrackRefs(album.id, cache));
      }
      return out;
    }
    return [];
  }

  async function collectTrackRefs(nodes, autoFetch) {
    const cache = new Map();
    const seen = new Set();
    const refs = [];
    for (const node of nodes) {
      let batch = [];
      try {
        if (autoFetch) {
          batch = await fullTrackRefs(node, cache);
        } else {
          const li = nodeOf(node.ref);
          if (!li) {
            LinkSelect.log(`跳过「${node.label}」：树上找不到对应节点`, 'muted');
            continue;
          }
          batch = renderedTrackRefs(li);
          if (batch.length === 0) {
            LinkSelect.log(`跳过「${node.label}」：没有已加载的曲目`, 'muted');
          }
        }
      } catch (e) {
        LinkSelect.log(`展开「${node.label}」失败：${e.message}`, 'error');
        continue;
      }
      for (const ref of batch) {
        if (seen.has(ref)) continue;
        seen.add(ref);
        refs.push(ref);
      }
    }
    return refs;
  }

  async function searchTracks(libraryId, query, limit) {
    const url = `/api/v1/search?q=${encodeURIComponent(query)}` +
      `&types=track&limit=${limit}` +
      `&library_id=${encodeURIComponent(libraryId)}`;
    const page = v1Page(await api(url));
    return page.hits
      .filter(hit => (hit.kind || 'track') === 'track')
      .slice(0, limit);
  }

  function fieldsMatch(source, hit) {
    const candidateAlbum = hit.album ? hit.album.title : '';
    if (norm(source.title) !== norm(hit.title)) return false;
    if (norm(source.album) !== norm(candidateAlbum)) return false;
    const candidateArtists = matchArtistNames(hit);
    const sourceArtists = matchArtistNames(source);
    if (candidateArtists.length === 0 || sourceArtists.length === 0) {
      return false;
    }
    // 多艺术家只要有一个能对上就算相同。
    return candidateArtists.some(candidate =>
      sourceArtists.some(artist => norm(artist) === norm(candidate)));
  }

  async function linkPair(sourceRef, targetRef) {
    return api('/api/v1/entity-links', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ a: sourceRef, b: targetRef }),
    });
  }

  async function matchTrack(source, targetLibrary, limit) {
    const composite =
      `${source.title} - ${matchArtistNames(source).join('; ')} - ${source.album}`;
    let hits = await searchTracks(targetLibrary, composite, limit);
    let query = composite;
    if (hits.length === 0) {
      // 本地库只按标题 LIKE，拼接串必然搜不到，退回标题再试一次。
      query = source.title;
      hits = await searchTracks(targetLibrary, query, limit);
    }
    for (const hit of hits) {
      if (refOfHit(hit) === source.ref) continue;
      if (!fieldsMatch(source, hit)) continue;
      const targetRef = refOfHit(hit);
      await linkPair(source.ref, targetRef);
      return targetRef;
    }
    return '';
  }

  async function start() {
    const nodes = LinkSelect.selected();
    if (nodes.length === 0) return;
    const targetLibrary = LinkSelect.target();
    if (!targetLibrary) {
      LinkSelect.log('请先选择目标媒体库', 'error');
      return;
    }
    const autoFetch = LinkSelect.autoFetch();
    const limit = LinkSelect.searchLimit
      ? LinkSelect.searchLimit()
      : DEFAULT_SEARCH_LIMIT;
    LinkSelect.clearLog();
    LinkSelect.setBusy(true);
    LinkSelect.log(
      `开始：${nodes.length} 个选中项，目标库 ${LinkSelect.targetLabel() || targetLibrary}，` +
      `Auto fetch ${autoFetch ? '开' : '关'}，搜索条数 ${limit}`);

    let linked = 0;
    let unmatched = 0;
    let failed = 0;
    let skipped = 0;
    try {
      const refs = await collectTrackRefs(nodes, autoFetch);
      LinkSelect.log(`递归得到 ${refs.length} 个曲目`, 'muted');

      for (let i = 0; i < refs.length; i++) {
        const ref = refs[i];
        const prefix = `[${i + 1}/${refs.length}] `;
        if (ref.split(':')[0] === targetLibrary) {
          skipped++;
          LinkSelect.log(prefix + `${ref} → 跳过（源与目标同一库）`, 'muted');
          continue;
        }

        let track;
        try {
          track = await api(`/api/track/${ref}`);
        } catch (e) {
          failed++;
          LinkSelect.log(prefix + `${ref} → 读取曲目失败：${e.message}`, 'error');
          continue;
        }

        const source = {
          ref,
          title: track.title || '',
          artists: artistNames(track),
          album_artists: albumArtistNames(track),
          album: track.album_name || '',
        };
        const label =
          `${source.title} - ${matchArtistNames(source).join('; ')} - ${source.album}`;
        LinkSelect.log(prefix + label);

        try {
          const targetRef = await matchTrack(source, targetLibrary, limit);
          if (targetRef) {
            linked++;
            LinkSelect.log(`    → 已链接 ${targetRef}`, 'ok');
          } else {
            unmatched++;
            LinkSelect.log(`    → 前 ${limit} 条无匹配`, 'muted');
          }
        } catch (e) {
          failed++;
          LinkSelect.log(`    → 链接失败：${e.message}`, 'error');
        }
      }
    } catch (e) {
      LinkSelect.log('匹配中断：' + e.message, 'error');
    } finally {
      LinkSelect.setBusy(false);
    }

    LinkSelect.log(
      `完成：链接 ${linked}，未匹配 ${unmatched}，失败 ${failed}，跳过 ${skipped}`,
      'summary');
  }

  // 左树恰好 1 个曲目、右树恰好 1 个曲目时，允许跳过搜索直接链接。
  function directPair() {
    const left = LinkSelect.selected();
    if (left.length !== 1 || left[0].type !== 'track') return null;
    const right = LinkSelect.rightTrack ? LinkSelect.rightTrack() : null;
    if (!right || right.type !== 'track') return null;
    return { left: left[0], right };
  }

  function syncDirect() {
    const button = document.getElementById('link-direct');
    if (!button) return;
    const pair = directPair();
    button.hidden = !pair;
    if (pair) {
      button.title =
        `直接链接「${pair.left.label}」↔「${pair.right.label}」`;
    }
  }

  async function directLink() {
    const pair = directPair();
    if (!pair) return;
    const button = document.getElementById('link-direct');
    if (button) button.disabled = true;
    try {
      await linkPair(pair.left.ref, pair.right.ref);
      LinkSelect.log(
        `已链接「${pair.left.label}」↔「${pair.right.label}」`, 'ok');
    } catch (e) {
      LinkSelect.log(`直接链接失败：${e.message}`, 'error');
    } finally {
      if (button) button.disabled = false;
      syncDirect();
    }
  }

  // 删除接口成功返回 204、失败返回 JSON，和 api() 的 res.json() 不兼容，
  // 这里单独处理：ok -> 已删除；404 -> 本来就没有链接。
  async function deleteLink(ref) {
    const res = await fetch(
      `${API_BASE}/api/v1/entity-links/${ref}`, { method: 'DELETE' });
    if (res.ok) return 'deleted';
    const text = await res.text();
    if (res.status === 404) return 'missing';
    throw new Error(`HTTP ${res.status}: ${text}`);
  }

  // 对左树多选项递归出的所有曲目，删除它们所在的整条链接。
  async function bulkDelete() {
    const nodes = LinkSelect.selected();
    if (nodes.length === 0) return;
    const autoFetch = LinkSelect.autoFetch();
    const question =
      `删除 ${nodes.length} 个选中项递归得到的所有曲目链接？` +
      `\n整条链接（含其他库的成员）都会被删除，且不可撤销。`;
    if (!confirm(question)) return;

    const button = document.getElementById('link-delete');
    if (button) button.disabled = true;
    LinkSelect.clearLog();
    LinkSelect.log(
      `开始删除：${nodes.length} 个选中项，Auto fetch ${autoFetch ? '开' : '关'}`);

    let deleted = 0;
    let missing = 0;
    let failed = 0;
    try {
      const refs = await collectTrackRefs(nodes, autoFetch);
      LinkSelect.log(`递归得到 ${refs.length} 个曲目`, 'muted');

      for (let i = 0; i < refs.length; i++) {
        const ref = refs[i];
        const prefix = `[${i + 1}/${refs.length}] `;
        try {
          const result = await deleteLink(ref);
          if (result === 'deleted') {
            deleted++;
            LinkSelect.log(prefix + `${ref} → 已删除链接`, 'ok');
          } else {
            missing++;
            LinkSelect.log(prefix + `${ref} → 未关联，跳过`, 'muted');
          }
        } catch (e) {
          failed++;
          LinkSelect.log(prefix + `${ref} → 删除失败：${e.message}`, 'error');
        }
      }
    } catch (e) {
      LinkSelect.log('删除中断：' + e.message, 'error');
    } finally {
      if (button) button.disabled = false;
    }

    LinkSelect.log(
      `完成：删除 ${deleted}，未关联 ${missing}，失败 ${failed}`, 'summary');
  }

  document.addEventListener('DOMContentLoaded', () => {
    const button = document.getElementById('link-start');
    if (button) button.onclick = start;
    const direct = document.getElementById('link-direct');
    if (direct) direct.onclick = directLink;
    const batchDelete = document.getElementById('link-delete');
    if (batchDelete) batchDelete.onclick = bulkDelete;
    document.addEventListener('linkselect:change', syncDirect);
    syncDirect();
  });
})();
