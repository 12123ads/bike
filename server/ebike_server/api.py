"""HTTP API：FastAPI + Bearer。

DESIGN.md §9.4 定的对外接口形态。`/track` 是四份历史文档里唯一被写下来过的
路由名（§9.3 说全项目只有四个契约级字面量，`/track` 是其中一个），所以保留它。

鉴权是单个 Bearer token，不是用户系统 —— 一辆车、一个人用。
"""

from __future__ import annotations

import logging
import time
from typing import Annotated, Any

from fastapi import Depends, FastAPI, HTTPException, Header, Query

from . import contract as ct
from .config import ServerConfig
from .service import Service

log = logging.getLogger("ebike.api")


def build_app(svc: Service, cfg: ServerConfig) -> FastAPI:
    # ⚠ **交互式文档全部关掉。** FastAPI 默认在 /docs、/redoc、/openapi.json
    # 三个地址无鉴权地公开完整 schema —— 包括 `/cmd/{id}/{cmd}` 和
    # `/secret/{id}` 的存在与请求体形状。它们调不动受保护接口，但等于把
    # 「这里能远程开锁、能下发密钥」直接告诉任何能连到这个端口的人。
    # 这个服务只有一个人用，schema 看源码就有，没有公开的理由。
    app = FastAPI(title="ebike-tracker", version="0.1.0",
                  description="电瓶车定位服务端。契约见 docs/MQTT-CONTRACT.md",
                  docs_url=None, redoc_url=None, openapi_url=None)

    if cfg.web.enabled:
        # 网页挂在 /、/ui/*、/api/*，用会话 cookie 鉴权（见 web.py 的说明）。
        # JSON API 继续用 Bearer，两套不混。
        from .web import build_web_router
        app.include_router(build_web_router(svc, cfg))

    async def auth(authorization: Annotated[str | None, Header()] = None) -> None:
        """Bearer 校验。

        用 `secrets.compare_digest` 而不是 `==`：token 比较的时间差可以被用来
        逐字节猜 token。这个接口能远程开锁（如果打开了），值得做对。

        ⚠ 按字节比较（审计 M5）：str 版的 compare_digest 只认 ASCII，
        HTTP 头经 latin-1 解码可含非 ASCII → TypeError → 未认证 500。
        token 生成是 urlsafe（纯 ASCII），编码后比较语义不变。
        """
        import secrets as _s
        expected = f"Bearer {cfg.api_token}".encode()
        if not authorization or not _s.compare_digest(
                authorization.encode("utf-8", "replace"), expected):
            raise HTTPException(status_code=401, detail="unauthorized")

    def check_dev(device_id: str) -> str:
        if cfg.device(device_id) is None:
            raise HTTPException(status_code=404, detail=f"未配置的设备 {device_id}")
        return device_id

    @app.get("/health")
    async def health() -> dict[str, Any]:
        """存活检查，不鉴权 —— 要能给监控和容器 HEALTHCHECK 用。

        **只回 `{"ok": true}`，不回设备清单。** 之前它带着
        `devices: [...]`，那是无鉴权地泄露设备 id，而 id 就是 MQTT 用户名
        和 topic 层级（契约 §3 §4）。设备清单要看就用 `/devices`（带 Bearer）。
        """
        return {"ok": True}

    @app.get("/devices", dependencies=[Depends(auth)])
    async def devices() -> list[dict[str, Any]]:
        out = []
        for d in cfg.devices:
            state = await svc.publish_state(d.id)
            out.append({"id": d.id, "report_interval": d.report_interval,
                        "state": state})
        return out

    @app.get("/state/{device_id}", dependencies=[Depends(auth)])
    async def state(device_id: str) -> dict[str, Any]:
        return await svc.publish_state(check_dev(device_id))

    @app.get("/track", dependencies=[Depends(auth)])
    async def track(
        device_id: str = Query(..., alias="dev"),
        since: int = Query(0, description="起始 Unix 秒（服务端时钟）"),
        until: int = Query(0, description="结束 Unix 秒，0 = 现在"),
        limit: int = Query(500, ge=1, le=5000),
        offset: int = Query(0, ge=0),
    ) -> dict[str, Any]:
        """轨迹。分页用 limit/offset —— DESIGN.md §11 #9 提到的分页问题。

        时间一律是 `t_srv`（服务端时钟），因为设备时钟在拿到 NITZ 之前是错的
        （契约 §5.6）。`t_dev` 一并返回，用于诊断。
        """
        dev = check_dev(device_id)
        rows = await svc.store.track(dev, since, until or int(time.time()),
                                     limit, offset)
        return {"dev": dev, "count": len(rows), "points": rows}

    @app.get("/events", dependencies=[Depends(auth)])
    async def events(device_id: str = Query(..., alias="dev"),
                     limit: int = Query(100, ge=1, le=1000)) -> dict[str, Any]:
        dev = check_dev(device_id)
        return {"dev": dev, "events": await svc.store.events(dev, limit)}

    @app.get("/pending", dependencies=[Depends(auth)])
    async def pending(device_id: str = Query(..., alias="dev")) -> dict[str, Any]:
        """未确认的下行队列（契约 §4.1）。诊断 secret 轮换延迟用。

        经过 `redact()` —— payload 里可能有明文密钥（契约 §6.2）。
        """
        dev = check_dev(device_id)
        rows = await svc.store.pending_downlinks(dev)
        safe = [
            {k: v for k, v in r.items() if k != "payload"} | {"bytes": len(r["payload"])}
            for r in rows
        ]
        return {"dev": dev, "pending": ct.redact(safe)}

    @app.post("/cmd/{device_id}/{cmd}", dependencies=[Depends(auth)])
    async def send_cmd(device_id: str, cmd: str,
                       args: dict[str, Any] | None = None) -> dict[str, Any]:
        """下发指令（契约 §6.1）。

        指令**入队后立即返回**，不等设备确认 —— 省电档下设备可能几十分钟才上线
        （契约 §4.1），HTTP 请求不可能等那么久。查是否送达用 `/pending`。
        """
        dev = check_dev(device_id)
        if cmd not in ct.COMMANDS:
            raise HTTPException(status_code=400,
                               detail=f"未知指令，闭集是 {sorted(ct.COMMANDS)}")
        if cmd == "unlock" and not cfg.allow_remote_unlock:
            # 契约 §6.1：远程开锁绕过 §5.2 的挑战应答，默认关闭
            raise HTTPException(
                status_code=403,
                detail="远程开锁已禁用。它绕过 BLE 挑战应答，"
                       "打开需要在配置里设 allow_remote_unlock=true")
        try:
            dn_id = await svc.enqueue_cmd(dev, cmd, args)
        except ct.ContractError as e:
            # 报文层的拒绝（未知 args 形状、超过 MAX_DOWNLINK_BYTES）是
            # **客户端错误**，不是 500。审计 M1 加下行上限后这条路径才活起来。
            raise HTTPException(status_code=400, detail=str(e)) from e
        return {"queued": dn_id, "dev": dev, "cmd": cmd}

    @app.post("/secret/{device_id}", dependencies=[Depends(auth)])
    async def send_secret(device_id: str, body: dict[str, Any]) -> dict[str, Any]:
        """下发 per-user 密钥（契约 §6.2）。

        body: `{"op":"set","uid":1,"kid":8,"key_b64":"..."}`
        响应里**不回显** `key_b64`。
        """
        dev = check_dev(device_id)
        op = body.get("op")
        if op not in ct.SECRET_OPS:
            raise HTTPException(status_code=400,
                               detail=f"未知 op，闭集是 {sorted(ct.SECRET_OPS)}")
        try:
            dn_id = await svc.enqueue_secret(
                dev, op, uid=body.get("uid"), kid=body.get("kid"),
                key_b64=body.get("key_b64"))
        except ct.ContractError as e:
            raise HTTPException(status_code=400, detail=str(e)) from e
        return {"queued": dn_id, "dev": dev, "op": op}

    return app
