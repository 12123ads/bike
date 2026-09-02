"""证书与口令的生成。

契约 §2 的默认模式是「TLS + 用户名口令」，所以这里要生成的是：
- 自签 CA（`ca.crt` / `ca.key`）—— 设备侧要把 `ca.crt` 写进模组 FS（DESIGN.md §8.2）
- 服务端证书（`server.crt` / `server.key`）
- 设备口令（写进 argon2 格式的 `passwd`）

mTLS 模式（`mqtt.mode = "cert"`）额外需要每设备证书，也在这里生成。

复用 `amqtt.contrib.cert` 的生成函数，不自己拼 x509 —— 那份代码就是 amqtt 自己
的 `UserAuthCertPlugin` 所期待的 SAN 格式（`spiffe://<domain>/device/<id>`），
自己拼很容易格式对不上而只在运行时才发现。
"""

from __future__ import annotations

import secrets
from pathlib import Path

from amqtt.contrib.cert import (
    generate_device_csr,
    generate_root_creds,
    generate_server_csr,
    load_ca,
    sign_csr,
    write_key_and_crt,
)
from pwdlib import PasswordHash
from pwdlib.hashers.argon2 import Argon2Hasher

_ORG = "ebike-tracker"
_COUNTRY = "CN"


def ensure_ca(certs_dir: Path) -> tuple[Path, Path]:
    """生成 CA，已存在就不动。返回 (ca.key, ca.crt)。"""
    certs_dir.mkdir(parents=True, exist_ok=True)
    ca_key_p = certs_dir / "ca.key"
    ca_crt_p = certs_dir / "ca.crt"
    if ca_key_p.exists() and ca_crt_p.exists():
        return ca_key_p, ca_crt_p
    key, crt = generate_root_creds(_COUNTRY, "NA", "NA", _ORG, "ebike-ca")
    write_key_and_crt(key, crt, "ca", certs_dir)
    ca_key_p.chmod(0o600)
    return ca_key_p, ca_crt_p


def ensure_server_cert(certs_dir: Path, common_name: str) -> tuple[Path, Path]:
    """生成服务端证书。

    `common_name` 必须是**设备实际连接时用的主机名或 IP** —— 模组侧
    `AT+SSLCFG="hostname",...` 会校验它（DESIGN.md §8.2）。填错的话
    TLS 握手会在设备侧失败，而设备侧的报错通常只有一个 ERROR。
    """
    certs_dir.mkdir(parents=True, exist_ok=True)
    srv_key_p = certs_dir / "server.key"
    srv_crt_p = certs_dir / "server.crt"
    if srv_key_p.exists() and srv_crt_p.exists():
        return srv_key_p, srv_crt_p
    ca_key_p, ca_crt_p = ensure_ca(certs_dir)
    ca_key, ca_crt = load_ca(str(ca_key_p), str(ca_crt_p))
    key, csr = generate_server_csr(_COUNTRY, _ORG, common_name)
    crt = sign_csr(csr, ca_key, ca_crt, validity_days=3650)
    write_key_and_crt(key, crt, "server", certs_dir)
    srv_key_p.chmod(0o600)
    return srv_key_p, srv_crt_p


def make_device_cert(certs_dir: Path, device_id: str, uri_domain: str) -> tuple[Path, Path]:
    """mTLS 模式用的设备证书。SAN 格式必须和 UserAuthCertPlugin 对齐。

    ⚠ 生成出来的 `.key` 要用 `AT+FSWRITE` 灌进模组 FS，而那条链路是 9600 baud
    （DESIGN.md §8.7）。一份 2048 位 RSA 私钥 PEM ≈ 1.7 KB，灌一次约 2 秒，
    不算问题；但 DESIGN.md §8.8 那个「私钥在产线上以什么形式存在」的问题本文不回答。
    """
    certs_dir.mkdir(parents=True, exist_ok=True)
    ca_key_p, ca_crt_p = ensure_ca(certs_dir)
    ca_key, ca_crt = load_ca(str(ca_key_p), str(ca_crt_p))
    key, csr = generate_device_csr(
        _COUNTRY, _ORG, device_id,
        uri_san=f"spiffe://{uri_domain}/device/{device_id}",
        dns_san=device_id,
    )
    crt = sign_csr(csr, ca_key, ca_crt, validity_days=3650)
    write_key_and_crt(key, crt, device_id, certs_dir)
    (certs_dir / f"{device_id}.key").chmod(0o600)
    return certs_dir / f"{device_id}.key", certs_dir / f"{device_id}.crt"


def make_password(passwd_file: Path, username: str,
                  password: str | None = None) -> str:
    """把一个账号写进 argon2 口令文件，返回明文口令（**只在这一刻可见**）。

    `amqtt` 的 `FileAuthPlugin` 只认 argon2（和已弃用的 sha512_crypt），
    不是 mosquitto_passwd 的格式，两者不能混用。
    """
    password = password or secrets.token_urlsafe(24)
    hasher = PasswordHash((Argon2Hasher(),))
    line = f"{username}:{hasher.hash(password)}"

    passwd_file.parent.mkdir(parents=True, exist_ok=True)
    lines: list[str] = []
    if passwd_file.exists():
        lines = [
            ln for ln in passwd_file.read_text(encoding="utf-8").splitlines()
            if ln.strip() and not ln.startswith(f"{username}:")
        ]
    lines.append(line)
    passwd_file.write_text("\n".join(lines) + "\n", encoding="utf-8")
    passwd_file.chmod(0o600)
    return password
