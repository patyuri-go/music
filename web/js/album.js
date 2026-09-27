const params = new URLSearchParams(location.search);
const albumId = parseInt(params.get('albumId') || '1', 10);

const coverEl = document.getElementById('cover');
const albumNameEl = document.getElementById('album-name');
const artistNameEl = document.getElementById('artist-name');
const yearEl = document.getElementById('year');
const trackListEl = document.getElementById('track-list');

const playerTitleEl = document.getElementById('player-title');
const playerArtistEl = document.getElementById('player-artist');
const playBtn = document.getElementById('play-btn');
const progressEl = document.getElementById('progress');
const timeEl = document.getElementById('time');

let album = null;
let tracks = [];
let currentTrack = null;
const audio = new Audio();
let isPlaying = false;

function formatTime(ms) {
  if (!ms || ms <= 0) return '--:--';
  const s = Math.floor(ms / 1000);
  const m = Math.floor(s / 60);
  const sec = s % 60;
  return m + ':' + (sec < 10 ? '0' : '') + sec;
}

async function loadAlbum() {
  const res = await fetch('/api/album/' + albumId);
  if (!res.ok) {
    albumNameEl.textContent = 'Album not found';
    return;
  }
  album = await res.json();
  tracks = album.tracks || [];

  coverEl.src = '/api/album/' + albumId + '/cover';
  albumNameEl.textContent = album.name || 'Unknown Album';
  artistNameEl.textContent = 'Artist ' + album.artist_id;
  yearEl.textContent = album.year ? ('Year ' + album.year) : '';

  trackListEl.innerHTML = '';
  tracks.forEach((t, i) => {
    const li = document.createElement('li');
    const num = document.createElement('span');
    num.className = 'track-num';
    num.textContent = t.track_no || (i + 1);
    const title = document.createElement('span');
    title.className = 'track-title';
    title.textContent = t.title || 'Unknown';
    const dur = document.createElement('span');
    dur.className = 'track-duration';
    dur.textContent = formatTime(t.duration_ms);
    const btn = document.createElement('button');
    btn.className = 'play-btn';
    btn.textContent = '▶';
    btn.onclick = () => playTrack(t, btn);
    li.appendChild(num);
    li.appendChild(title);
    li.appendChild(dur);
    li.appendChild(btn);
    trackListEl.appendChild(li);
  });
}

async function playTrack(track, btnEl) {
  if (currentTrack && currentTrack.id === track.id) {
    if (isPlaying) {
      audio.pause();
      isPlaying = false;
      playBtn.textContent = '▶';
      return;
    }
    audio.play();
    isPlaying = true;
    playBtn.textContent = '⏸';
    return;
  }

  currentTrack = track;
  audio.src = '/api/track/' + track.id + '/file';
  audio.play();
  isPlaying = true;
  playBtn.textContent = '⏸';
  playerTitleEl.textContent = track.title || 'Unknown';
  playerArtistEl.textContent = 'Track ' + track.id;

  fetch('/api/track/' + track.id + '/play', { method: 'POST' });

  document.querySelectorAll('.play-btn').forEach(b => b.textContent = '▶');
  if (btnEl) btnEl.textContent = '⏸';
}

playBtn.onclick = () => {
  if (!currentTrack) return;
  if (isPlaying) {
    audio.pause();
    isPlaying = false;
    playBtn.textContent = '▶';
  } else {
    audio.play();
    isPlaying = true;
    playBtn.textContent = '⏸';
  }
};

audio.ontimeupdate = () => {
  if (!audio.duration) return;
  progressEl.value = (audio.currentTime / audio.duration) * 100;
  const cur = Math.floor(audio.currentTime);
  const dur = Math.floor(audio.duration);
  timeEl.textContent = Math.floor(cur / 60) + ':' + (cur % 60 < 10 ? '0' : '') + (cur % 60) +
    ' / ' + Math.floor(dur / 60) + ':' + (dur % 60 < 10 ? '0' : '') + (dur % 60);
};

audio.onended = () => {
  isPlaying = false;
  playBtn.textContent = '▶';
  progressEl.value = 0;
  timeEl.textContent = '0:00 / 0:00';
};

progressEl.oninput = () => {
  if (!audio.duration) return;
  audio.currentTime = (progressEl.value / 100) * audio.duration;
};

loadAlbum();
