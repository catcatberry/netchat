# netchat — C++ WebSocket Chat (Linux + Boost.Beast)

A tiny, production-grade online chat you can share via a single URL.  
Backend in **C++17** using **Boost.Asio/Beast** (HTTP + WebSocket). Frontend is a lightweight **HTML/JS** page.  
**Demo:** `https://chat.catcatberry.com/room/demo` (update with your own domain if you fork).

---

## ✨ Features
- **Share-by-link** rooms: visit `/room/<room-id>` to join a chatroom.
- **Real-time chat** over **WebSocket** (`/ws?room=<id>&name=<nick>`).
- **Zero external deps** at runtime (just the compiled binary + static files).
- **Simple deployment**: systemd service + Caddy reverse proxy for automatic HTTPS.
- **Portable**: runs on any modern Linux (tested on Ubuntu 22.04/24.04).

## 🧱 Tech Stack
- **C++17**, **Boost.Asio/Beast**, **CMake**
- Frontend: vanilla **HTML/CSS/JS** (no framework)
- Deployment: **systemd**, **Caddy** (or Nginx), optional **Cloudflare Tunnel**
- Target runtime: **HTTP** on port **8080** (TLS handled by the reverse proxy)

---

## 🗂 Project Layout
```
netchat/
├─ src/
│  └─ server.cpp          # HTTP + WebSocket server, in-memory rooms
├─ web/
│  └─ index.html          # Minimal client UI
├─ CMakeLists.txt         # Build script
├─ README.md              # This file
└─ LICENSE                # MIT
```

---

## 🚀 Quick Start (Local / Dev)

### Prereqs (Ubuntu)
```bash
sudo apt update
sudo apt install -y build-essential cmake libboost-all-dev git
```

### Build
```bash
cd netchat
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j
```

### Run
```bash
# Usage: ./netchat [port] [static_dir]
./netchat 8080 ../web
# open http://127.0.0.1:8080/room/demo
```

### Common Issues
- **Browser shows "Not Secure"** locally → expected, backend speaks plain HTTP. Use a reverse proxy for HTTPS.
- **`index.html 未找到`** → pass the correct static dir (use an absolute path), e.g. `./netchat 8080 /abs/path/to/web`.

---

## 🌐 Production (DigitalOcean Droplet + Caddy)

Below is the reference setup used for `chat.catcatberry.com`. Adapt hostnames & paths as needed.

### 1) Provision a server
- Ubuntu 22.04 / 24.04, 1 vCPU / 1 GB RAM is enough.
- Open ports **22/80/443** in the cloud firewall/security group.
- Configure DNS (at your registrar) to point `A` records for `@` and `chat` to your server IP.

### 2) Install dependencies (on the server)
```bash
sudo apt update
sudo apt -y install ufw rsync
sudo ufw allow OpenSSH && sudo ufw allow 80/tcp && sudo ufw allow 443/tcp && sudo ufw enable
```

### 3) Install Caddy (automatic HTTPS)
```bash
# Official apt repo
sudo apt -y install curl debian-keyring debian-archive-keyring apt-transport-https
curl -1sLf 'https://dl.cloudsmith.io/public/caddy/stable/gpg.key' | sudo gpg --dearmor -o /usr/share/keyrings/caddy-stable-archive-keyring.gpg
curl -1sLf 'https://dl.cloudsmith.io/public/caddy/stable/debian.deb.txt' | sudo tee /etc/apt/sources.list.d/caddy-stable.list
sudo apt update && sudo apt -y install caddy
```

**/etc/caddy/Caddyfile**
```caddyfile
chat.catcatberry.com {
    encode gzip
    reverse_proxy 127.0.0.1:8080
}

# optional: redirect root & www to chat subdomain
catcatberry.com, www.catcatberry.com {
    redir https://chat.catcatberry.com{uri} permanent
}
```
```bash
sudo caddy validate --config /etc/caddy/Caddyfile
sudo systemctl reload caddy
```

### 4) Deploy the app (binary + web)
Option A: **Build on your dev machine and upload** a tarball:
```bash
# On dev machine
cd netchat
tar czf netchat_release.tgz -C "$(pwd)/build/Debug" netchat -C "$(pwd)" web
scp netchat_release.tgz user@server:/tmp/

# On server
sudo mkdir -p /opt/netchat
sudo tar xzf /tmp/netchat_release.tgz -C /opt/netchat
sudo chown -R www-data:www-data /opt/netchat
```

Option B (robust on 1GB servers): **Compile on the server** (avoids lib version mismatch).  
If you hit OOM while compiling, add swap and use `-j1`.
```bash
# Upload sources:
tar czf netchat_src.tgz src web CMakeLists.txt && scp netchat_src.tgz user@server:/tmp/

# On server:
sudo apt install -y build-essential cmake libboost-all-dev
mkdir -p /opt/netchat_build && tar xzf /tmp/netchat_src.tgz -C /opt/netchat_build
cd /opt/netchat_build && mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j1
sudo install -m755 netchat /opt/netchat/netchat
rsync -a ../web/ /opt/netchat/web/
sudo chown -R www-data:www-data /opt/netchat
```

### 5) systemd service
**/etc/systemd/system/netchat.service**
```ini
[Unit]
Description=netchat C++ chat server
After=network.target

[Service]
User=www-data
Group=www-data
WorkingDirectory=/opt/netchat
ExecStart=/opt/netchat/netchat 8080 /opt/netchat/web
Restart=always
RestartSec=2
StandardOutput=append:/var/log/netchat.log
StandardError=append:/var/log/netchat.log

[Install]
WantedBy=multi-user.target
```
```bash
sudo systemctl daemon-reload
sudo systemctl enable --now netchat
systemctl status netchat --no-pager
```

### 6) Verify
```bash
# On the server:
curl -i http://127.0.0.1:8080/room/demo

# From the internet:
https://chat.catcatberry.com/room/demo
```

---

## 🔌 API / Protocol

### HTTP
- `GET /` or `/room/<room>` → serves `web/index.html`
- Static files under `/web/*`

### WebSocket
- Endpoint: `GET /ws?room=<room>&name=<nick>` (upgrades to WS)
- **Broadcast model**: all messages within the same `room` are forwarded to all peers
- Message format (MVP): plain text lines (you can extend to JSON with timestamps/types)

---

## ⚙️ Configuration

Binary accepts two args:
```bash
./netchat <port> <static_dir>
# defaults: port=8080, static_dir=./web
```
- The backend is **HTTP only**; TLS is **terminated by the reverse proxy** (Caddy).

---

## 🩺 Troubleshooting

- **502 from Caddy**: backend not listening → `systemctl status netchat`, `curl http://127.0.0.1:8080/room/demo`
- **`index.html 未找到`**: wrong static dir → check `ExecStart` and ensure `/opt/netchat/web/index.html` exists
- **WS fails on HTTPS**: ensure you access the proxy URL (e.g. `https://chat.catcatberry.com`) not the raw `http://:8080`
- **OOM during build on 1GB servers**:
  ```bash
  sudo fallocate -l 2G /swapfile && sudo chmod 600 /swapfile && sudo mkswap /swapfile && sudo swapon /swapfile
  make -j1
  ```
- **Firewall**: `sudo ufw allow 80,443/tcp`

---

## 🔒 Security Notes
- This MVP has **no auth, no rate limiting, no history persistence**. For public use consider:
  - Room passwords / one-time join tokens
  - Basic rate limiting / IP throttle
  - Message size limits & input validation
  - Optional persistence (e.g., SQLite) and moderation tools

---

## 🧭 Roadmap Ideas
- JSON message format with timestamps & system/user types
- Last N message history replay on join
- Optional password-protected rooms / invite tokens
- Docker image + CI
- Metrics/healthz endpoints & structured logging

---

## 📄 License
MIT — see `LICENSE`.

## 👤 Author
`catcatberry` — original implementation & deployment (C++17 + Boost.Beast, Caddy, systemd, DigitalOcean).

