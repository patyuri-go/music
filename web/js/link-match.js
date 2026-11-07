// 递归 + 匹配 + 调链接接口。匹配规则：
// 目标库曲目前 10 条里，标题、专辑归一化后全等，且至少一个艺术家名相同。
(function () {
  const SEARCH_LIMIT = 10;

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

  function artistNames(entry) {
    const list = entry && Array.isArray(entry.artists) ? entry.artists : [];
    return list.map(a => (a && a.name) || '').filter(Boolean);
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

  async function searchTracks(libraryId, query) {
    const url = `/api/v1/search?q=${encodeURIComponent(query)}` +
      `&types=track&limit=${SEARCH_LIMIT}` +
      `&library_id=${encodeURIComponent(libraryId)}`;
    const page = v1Page(await api(url));
    return page.hits
      .filter(hit => (hit.kind || 'track') === 'track')
      .slice(0, SEARCH_LIMIT);
  }

  function fieldsMatch(source, hit) {
    const candidateAlbum = hit.album ? hit.album.title : '';
    if (norm(source.title) !== norm(hit.title)) return false;
    if (norm(source.album) !== norm(candidateAlbum)) return false;
    const candidateArtists = artistNames(hit);
    if (candidateArtists.length === 0 || source.artists.length === 0) {
      return false;
    }
    // 多艺术家只要有一个能对上就算相同。
    return candidateArtists.some(candidate =>
      source.artists.some(artist => norm(artist) === norm(candidate)));
  }

  async function linkPair(sourceRef, targetRef) {
    return api('/api/v1/entity-links', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ a: sourceRef, b: targetRef }),
    });
  }

  async function matchTrack(source, targetLibrary) {
    const composite =
      `${source.title} - ${source.artists.join('; ')} - ${source.album}`;
    let hits = await searchTracks(targetLibrary, composite);
    let query = composite;
    if (hits.length === 0) {
      // 本地库只按标题 LIKE，拼接串必然搜不到，退回标题再试一次。
      query = source.title;
      hits = await searchTracks(targetLibrary, query);
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
    LinkSelect.clearLog();
    LinkSelect.setBusy(true);
    LinkSelect.log(
      `开始：${nodes.length} 个选中项，目标库 ${LinkSelect.targetLabel() || targetLibrary}，` +
      `Auto fetch ${autoFetch ? '开' : '关'}`);

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
          album: track.album_name || '',
        };
        const label = `${source.title} - ${source.artists.join('; ')} - ${source.album}`;
        LinkSelect.log(prefix + label);

        try {
          const targetRef = await matchTrack(source, targetLibrary);
          if (targetRef) {
            linked++;
            LinkSelect.log(`    → 已链接 ${targetRef}`, 'ok');
          } else {
            unmatched++;
            LinkSelect.log('    → 前 10 条无匹配', 'muted');
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

  document.addEventListener('DOMContentLoaded', () => {
    const button = document.getElementById('link-start');
    if (button) button.onclick = start;
  });
})();
