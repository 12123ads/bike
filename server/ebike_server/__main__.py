"""入口：`python -m ebike_server` 或 `ebike-server`。

一个进程跑三件事：内置 MQTT broker、ingest/派生、HTTP API。
契约 §1 说了代价：broker 和业务同生共死，单车场景可接受。
"""

from __future__ import annotations

import argparse
import asyncio
import logging
import signal
import sys
from pathlib import Path

import uvicorn

from . import certs, config
from .api import build_app
from .config import split_bind
from .service import Service


def _setup_logging(verbose: bool) -> None:
    logging.basicConfig(
        level=logging.DEBUG if verbose else logging.INFO,
        format="%(asctime)s %(levelname)-7s %(name)s: %(message)s",
    )
    # amqtt 的 DEBUG 非常吵（每个包一行），除非显式 -v 否则压到 WARNING
    if not verbose:
        logging.getLogger("amqtt").setLevel(logging.WARNING)
        logging.getLogger("transitions").setLevel(logging.WARNING)


async def _run(cfg: config.ServerConfig) -> None:
    svc = Service(cfg)
    await svc.start()

    app = build_app(svc, cfg)
    host, port = split_bind(cfg.http_bind, 8080)
    server = uvicorn.Server(uvicorn.Config(
        app, host=host, port=port, log_level="info", access_log=False))

    stop = asyncio.Event()
    loop = asyncio.get_running_loop()
    for sig in (signal.SIGINT, signal.SIGTERM):
        loop.add_signal_handler(sig, stop.set)

    http_task = asyncio.create_task(server.serve(), name="http")
    logging.info("HTTP API 在 http://%s:%d", host, port)
    if getattr(cfg, "_token_generated", False):
        logging.warning("本次运行生成的 API token（未配置 api_token）: %s",
                        cfg.api_token)

    await stop.wait()
    logging.info("收到停止信号，正在关闭")
    server.should_exit = True
    await http_task
    await svc.stop()


def cmd_init(args: argparse.Namespace) -> int:
    """生成配置、证书和设备口令。跑服务之前先跑一次这个。"""
    # 容器里 EBIKE_DIR=/data，且必须绑 0.0.0.0 —— 绑 127.0.0.1 等于谁都连不上
    in_docker = Path("/.dockerenv").exists()

    # 没给 -c 就用默认位置。**一定要落一个配置文件**，否则 api_token
    # 每次启动都是新的随机值，HA 和脚本每次都要改。
    path = Path(args.config) if args.config else config.default_config_path()
    existed = path.exists()
    config.write_default(path, docker=in_docker)
    print(f"配置文件：{path}{'（已存在，未改动）' if existed else '（新建）'}")

    cfg = config.load(path)
    certs_dir = Path(cfg.mqtt.certfile).parent
    certs.ensure_ca(certs_dir)
    certs.ensure_server_cert(certs_dir, args.hostname)
    print(f"\nCA 与服务端证书: {certs_dir}")
    print(f"  设备侧要把 {certs_dir / 'ca.crt'} 用 AT+FSWRITE 写进模组 FS")
    print(f"  服务端证书的 CN = {args.hostname}，设备的 "
          f'AT+SSLCFG="hostname" 必须填这个')

    passwd = Path(cfg.mqtt.password_file)
    for d in cfg.devices:
        pw = certs.make_password(passwd, d.id)
        print(f"\n设备 {d.id}:")
        print(f"  MQTT 用户名 = {d.id}")
        print(f"  MQTT 口令   = {pw}      ← 只显示这一次，烧进固件")
        if cfg.mqtt.mode == "cert":
            k, c = certs.make_device_cert(certs_dir, d.id, cfg.mqtt.cert_uri_domain)
            print(f"  客户端证书  = {c}")
            print(f"  客户端私钥  = {k}")
    ha_pw = certs.make_password(passwd, "ha")
    print(f"\nHome Assistant:\n  用户名 = ha\n  口令   = {ha_pw}")
    print("  只能订阅 ebike/v1/+/state")
    print(f"\nHTTP API token = {cfg.api_token}")
    print(f"  （在 {path} 里，要改直接改那个文件）")
    return 0


def cmd_run(args: argparse.Namespace) -> int:
    # 显式给了 -c 就必须存在；没给则用默认位置，不存在就用内置默认值
    if args.config:
        path: Path | None = Path(args.config)
    else:
        default = config.default_config_path()
        path = default if default.exists() else None
        if path is None:
            logging.warning("没有配置文件（%s 不存在），用内置默认值跑。"
                            "api_token 会是随机的 —— 先跑一次 init 更省事", default)

    try:
        cfg = config.load(path)
    except FileNotFoundError as e:
        print(f"{e}\n先跑 `ebike-server -c <配置> init --hostname <主机名或IP>`",
              file=sys.stderr)
        return 1

    missing = [p for p in (cfg.mqtt.certfile, cfg.mqtt.keyfile)
               if cfg.mqtt.tls_bind and not Path(p).exists()]
    if missing:
        print(f"缺少证书 {missing}，先跑 `ebike-server init --hostname <主机名>`",
              file=sys.stderr)
        return 1
    if cfg.mqtt.mode == "password" and not Path(cfg.mqtt.password_file).exists():
        print(f"缺少口令文件 {cfg.mqtt.password_file}，先跑 `ebike-server init`",
              file=sys.stderr)
        return 1
    asyncio.run(_run(cfg))
    return 0


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(prog="ebike-server",
                                description="电瓶车定位服务端（内置 MQTT broker）")
    p.add_argument("-c", "--config", help="配置文件路径（JSON 或 YAML）")
    p.add_argument("-v", "--verbose", action="store_true")
    sub = p.add_subparsers(dest="cmd", required=True)

    p_init = sub.add_parser("init", help="生成证书与设备口令")
    p_init.add_argument("--hostname", required=True,
                        help="设备连接时用的主机名或 IP，会写进服务端证书 CN")
    p_init.set_defaults(func=cmd_init)

    p_run = sub.add_parser("run", help="启动服务")
    p_run.set_defaults(func=cmd_run)

    args = p.parse_args(argv)
    _setup_logging(args.verbose)
    return int(args.func(args))


if __name__ == "__main__":
    raise SystemExit(main())
