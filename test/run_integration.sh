#!/usr/bin/env bash
# 存储代理与现有功能集成测试。
#
# 单元测试（GTest）由 ctest 运行；本脚本只跑需要数据库/外部进程的集成测试。
#
#   ./test/run_integration.sh [config.json]
#
# 前置：cmake --build --preset default 已生成 build/bin/musicd 与
#       build/bin/test/{fake_pg,fake_storage,test_fixture,playlist_test,...}。
# 使用独立端口，不干扰正在运行的 musicd：
#   musicd 15667 / Jellyfin 19000 / Subsonic 14533 / fake_storage 15666
#
# 不依赖真实的 PostgreSQL 与存储服务：fake_pg 实现 PostgreSQL 线协议并内置
# 夹具数据，fake_storage 按实体 id 返回确定字节；fake_pg 端口由内核分配。
# 传入的 config.json 只用于取其余配置，临时配置里的数据库地址会被替换。

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

SOURCE_CONFIG="${1:-$ROOT/config.json}"
# config.json 被 gitignore。没有它也要能跑：下面会写一份最小配置。
SOURCE_CONFIG_MISSING=0
if [[ ! -f "$SOURCE_CONFIG" ]]; then
  SOURCE_CONFIG_MISSING=1
fi

MAIN_PORT=15667
JF_PORT=19000
STORAGE_PORT=15666

BIN_DIR="$ROOT/build/bin"
TEST_BIN_DIR="$BIN_DIR/test"

for bin in "$BIN_DIR/musicd" "$TEST_BIN_DIR/fake_pg" \
           "$TEST_BIN_DIR/fake_storage" "$TEST_BIN_DIR/test_fixture" \
           "$TEST_BIN_DIR/playlist_test" "$TEST_BIN_DIR/lyrics_test" \
           "$TEST_BIN_DIR/artists_test" "$TEST_BIN_DIR/storage_test" \
           "$TEST_BIN_DIR/query_api_test"; do
  if [[ ! -x "$bin" ]]; then
    echo "缺少可执行文件 $bin（先运行 cmake --build --preset default）" >&2
    exit 2
  fi
done

TMP_DIR="$(mktemp -d /tmp/opencode/musicd-it.XXXXXX)"
TMP_CONFIG="$TMP_DIR/config.json"
LOG_DIR="$TMP_DIR/logs"
mkdir -p "$LOG_DIR"

if [[ $SOURCE_CONFIG_MISSING -eq 1 ]]; then
  echo "未找到 $SOURCE_CONFIG，使用内置最小配置"
  SOURCE_CONFIG="$TMP_DIR/base.json"
  cat > "$SOURCE_CONFIG" <<'JSON'
{
  "main_port": 15667,
  "page_size": 50,
  "static_dir": "web",
  "jellyfin": { "enabled": true, "port": 9000 },
  "subsonic": { "enabled": true, "port": 4533 },
  "db": {
    "PostgreSQL": {
      "url": "postgresql://127.0.0.1:5432/music",
      "user": "musicd",
      "dbname": "music"
    }
  },
  "libraries": [
    { "type": "local", "id": "local-main", "storage_port": 15666,
      "storage_timeout_ms": 5000 }
  ]
}
JSON
fi

PG_PID=""
echo "== 启动 fake_pg（端口由内核分配）=="
"$TEST_BIN_DIR/fake_pg" --port 0 > "$LOG_DIR/fake_pg.log" 2>&1 &
PG_PID=$!
PG_PORT=""
for ((i = 0; i < 100; ++i)); do
  PG_PORT="$(sed -n 's/^listening \([0-9]\+\)$/\1/p' "$LOG_DIR/fake_pg.log" | head -1)"
  [[ -n "$PG_PORT" ]] && break
  sleep 0.1
done
if [[ -z "$PG_PORT" ]]; then
  echo "fake_pg 未启动" >&2
  cat "$LOG_DIR/fake_pg.log" >&2
  exit 1
fi

# 只替换端口、超时与数据库地址；其余配置原样保留。
sed \
  -e "s/\"main_port\":[[:space:]]*[0-9]\+/\"main_port\": $MAIN_PORT/" \
  -e "s/\"port\":[[:space:]]*9000/\"port\": $JF_PORT/" \
  -e "s/\"port\":[[:space:]]*4533/\"port\": 14533/" \
  -e "s/\"storage_port\":[[:space:]]*[0-9]\+/\"storage_port\": $STORAGE_PORT/" \
  -e "s/\"storage_timeout_ms\":[[:space:]]*[0-9]\+/\"storage_timeout_ms\": 1200/" \
  -e "s#postgresql://[^\"]*#postgresql://127.0.0.1:$PG_PORT/music#" \
  "$SOURCE_CONFIG" > "$TMP_CONFIG"

FAKE_PID=""
MUSICD_PID=""

cleanup() {
  local code=$?
  if [[ -n "$MUSICD_PID" ]] && kill -0 "$MUSICD_PID" 2>/dev/null; then
    kill "$MUSICD_PID" 2>/dev/null
    wait "$MUSICD_PID" 2>/dev/null
  fi
  if [[ -n "$FAKE_PID" ]] && kill -0 "$FAKE_PID" 2>/dev/null; then
    kill "$FAKE_PID" 2>/dev/null
    wait "$FAKE_PID" 2>/dev/null
  fi
  # fake_pg 每连接一个子进程；父进程退出后子进程随客户端断开自行结束。
  if [[ -n "$PG_PID" ]] && kill -0 "$PG_PID" 2>/dev/null; then
    kill "$PG_PID" 2>/dev/null
    wait "$PG_PID" 2>/dev/null
  fi
  if [[ $code -eq 0 ]]; then
    rm -rf "$TMP_DIR"
  else
    echo "日志保留在 $LOG_DIR" >&2
  fi
}
trap cleanup EXIT

port_ready() {
  (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null
}

wait_http() {
  local port="$1" url="$2" tries="${3:-100}"
  for ((i = 0; i < tries; ++i)); do
    if curl -fsS -o /dev/null "$url" 2>/dev/null; then
      return 0
    fi
    sleep 0.2
  done
  return 1
}

echo "== 启动 fake_storage (:${STORAGE_PORT}) =="
"$TEST_BIN_DIR/fake_storage" "$STORAGE_PORT" > "$LOG_DIR/fake_storage.log" 2>&1 &
FAKE_PID=$!
if ! port_ready "$STORAGE_PORT"; then
  for ((i = 0; i < 50; ++i)); do
    port_ready "$STORAGE_PORT" && break
    sleep 0.2
  done
fi
if ! port_ready "$STORAGE_PORT"; then
  echo "fake_storage 未启动" >&2
  exit 1
fi

echo "== 夹具 id（fake_pg 内置数据）=="
set -a
# shellcheck disable=SC1090
eval "$("$TEST_BIN_DIR/test_fixture" env)"
set +a

echo "== 启动 musicd (:${MAIN_PORT}) =="
"$BIN_DIR/musicd" --config "$TMP_CONFIG" > "$LOG_DIR/musicd.log" 2>&1 &
MUSICD_PID=$!

if ! wait_http "$MAIN_PORT" "http://127.0.0.1:${MAIN_PORT}/api/playlists"; then
  echo "musicd 未就绪" >&2
  tail -n 40 "$LOG_DIR/musicd.log" >&2
  exit 1
fi
if ! wait_http "$JF_PORT" "http://127.0.0.1:${JF_PORT}/System/Info/Public"; then
  echo "Jellyfin 端口未就绪" >&2
  exit 1
fi

FAILED=0
run() {
  local name="$1"
  shift
  echo
  echo "---- $name ----"
  if ! "$@"; then
    FAILED=1
    echo "!! $name 失败" >&2
  fi
}

run "播放列表" "$TEST_BIN_DIR/playlist_test" 127.0.0.1 "$MAIN_PORT" "$JF_PORT"
run "歌词" "$TEST_BIN_DIR/lyrics_test" 127.0.0.1 "$MAIN_PORT" "$JF_PORT"
run "艺术家" "$TEST_BIN_DIR/artists_test" 127.0.0.1 "$MAIN_PORT"
run "存储代理" "$TEST_BIN_DIR/storage_test" 127.0.0.1 "$MAIN_PORT" "$STORAGE_PORT"

# GTest 自己解析命令行，地址走环境变量，避免与 --gtest_filter 之类的位置参数打架。
MUSICD_HOST=127.0.0.1 MUSICD_PORT="$MAIN_PORT" \
  run "查询接口" "$TEST_BIN_DIR/query_api_test"

echo
if [[ $FAILED -eq 0 ]]; then
  echo "全部集成测试通过"
else
  echo "存在失败用例" >&2
fi
exit "$FAILED"