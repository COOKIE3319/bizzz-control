#!/usr/bin/env bash
# 启动 Bizzz Control 网页控制台（Web Bluetooth 需要 localhost/https）
cd "$(dirname "$0")"
PORT="${1:-8777}"
echo "打开: http://localhost:${PORT}"
python3 -m http.server "$PORT"
