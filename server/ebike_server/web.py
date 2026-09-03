"""网页界面的路由。

**故意和 `/`-前缀的 JSON API 分开**（挂在 `/ui` 和 `/api`）：
JSON API 用 Bearer，网页用会话 cookie（见 `websession.py` 的理由）。
两套鉴权混在同一批路由上会让「哪条路能绕过哪个检查」很难看清。

⚠ 网页能做的事和 API 一样多（含远程开锁，如果打开了），
所以它跟 API 共用 `http_bind` —— 默认只绑 `127.0.0.1`。
"""

from __future__ import annotations

import json
import logging
import time
from typing import Annotated, Any

from fastapi import APIRouter, Cookie, Depends, HTTPException, Request, Response
from fastapi.responses import HTMLResponse, JSONResponse

from . import contract as ct
from .config import ServerConfig
from .service import Service
from .web_assets import INDEX_HTML, LOGIN_HTML
from .websession import COOKIE_NAME, LoginThrottle, SessionStore

log = logging.getLogger("ebike.web")


def build_web_router(svc: Service, cfg: ServerConfig) -> APIRouter:
    router = APIRouter()
    sessions = SessionStore(ttl=cfg.web.session_ttl)
    throttle = LoginThrottle()

    def rate_key(request: Request) -> str:
        """限速的键：**真实 socket 地址**（审计 R2）。

        以前这里优先用 `X-Forwarded-For` 的第一段，理由写的是「伪造它最多是
        绕过自己的限速」。那句话是错的 —— 限速的作用对象就是攻击者，
        「绕过自己的限速」等于限速不存在。实测：每个请求换一个 XFF 值，
        连打 60 次错 token 一个 429 都没有。

        更糟的是 `LoginThrottle` 的 `max_tracked_ips` 上限会被当成放大器：
        轮换伪造来源把表灌满时，按最旧丢键会把**真实用户的限速记录**挤掉。

        socket 地址不可伪造（TCP 三次握手），代价是反向代理后面所有请求
        共享一个键。那个代价是对的：宁可把代理后的用户当成一个整体限速，
        也不给任意一个请求头开一条绕过的路。真要按真实客户端分开限，
        得在代理层做，而不是信任一个客户端可写的头。
        """
        return request.client.host if request.client else "?"

    def log_source(request: Request) -> str:
        """日志用的来源描述。XFF **只到这里**，不参与任何判断。"""
        peer = rate_key(request)
        fwd = request.headers.get("x-forwarded-for")
        return f"{peer}（XFF: {fwd.split(',')[0].strip()}）" if fwd else peer

    async def require_session(
        ebike_session: Annotated[str | None, Cookie()] = None,
    ) -> None:
        if not sessions.valid(ebike_session):
            raise HTTPException(status_code=401, detail="需要登录")

    # --- 登录 ---------------------------------------------------------------

    @router.get("/ui/login", response_class=HTMLResponse)
    async def login_page() -> HTMLResponse:
        return HTMLResponse(LOGIN_HTML)

    @router.post("/ui/login")
    async def do_login(request: Request, response: Response) -> JSONResponse:
        key = rate_key(request)
        if throttle.blocked(key):
            # 429 而不是 401：让人知道是被限速了，不然会以为 token 记错了
            raise HTTPException(status_code=429, detail="尝试太频繁，等 5 分钟再试")

        try:
            body = await request.json()
        except (json.JSONDecodeError, ValueError):
            raise HTTPException(status_code=400, detail="请求体不是 JSON") from None
        # 审计 L14：合法 JSON 不一定是 dict（"x"、[1,2] 都能 parse），
        # body.get 会 AttributeError → 500。未认证即可触发。
        if not isinstance(body, dict):
            raise HTTPException(status_code=400, detail="请求体必须是 JSON 对象")

        import secrets as _s
        supplied = str(body.get("token") or "")
        # 审计 M5：compare_digest 只认 ASCII-only str，非 ASCII 直接抛
        # TypeError → 500，且发生在限速计数之前（畸形 token 无限打不触发
        # 429）。先按字节比较：token 生成本来就是 urlsafe（ASCII），
        # 任何非 ASCII 输入必然不匹配，编码后比较既安全又不受限制。
        if not _s.compare_digest(supplied.encode("utf-8", "replace"),
                                 cfg.api_token.encode("utf-8")):
            throttle.record_failure(key)
            log.warning("登录失败，来源 %s", log_source(request))
            raise HTTPException(status_code=401, detail="token 不对")

        throttle.reset(key)
        token, max_age = sessions.create()
        resp = JSONResponse({"ok": True})
        resp.set_cookie(
            COOKIE_NAME, token,
            max_age=max_age,
            httponly=True,       # JS 拿不到 —— XSS 也偷不走
            samesite="strict",   # 挡 CSRF：跨站请求不带这个 cookie
            secure=cfg.web.cookie_secure,
            path="/",
        )
        log.info("登录成功，来源 %s", log_source(request))
        return resp

    @router.post("/ui/logout")
    async def do_logout(
        ebike_session: Annotated[str | None, Cookie()] = None,
    ) -> JSONResponse:
        sessions.revoke(ebike_session)
        resp = JSONResponse({"ok": True})
        resp.delete_cookie(COOKIE_NAME, path="/")
        return resp

    # --- 页面 ---------------------------------------------------------------

    @router.get("/", response_class=HTMLResponse)
    @router.get("/ui", response_class=HTMLResponse)
    async def index(
        ebike_session: Annotated[str | None, Cookie()] = None,
    ) -> HTMLResponse:
        """没登录就给登录页，不做 302 —— 302 到 /ui/login 之后地址栏变了，
        用户刷新会停在登录页而不是回到地图。"""
        if not sessions.valid(ebike_session):
            return HTMLResponse(LOGIN_HTML)
        return HTMLResponse(INDEX_HTML)

    # --- 网页用的 JSON 接口（会话鉴权） --------------------------------------

    @router.get("/api/config", dependencies=[Depends(require_session)])
    async def web_config() -> dict[str, Any]:
        """前端启动需要的东西。

        ⚠ **高德 key 在这里发给浏览器** —— JS API 的 key 本来就是公开的
        （任何用了高德地图的网页都能在源码里看到）。真正的保护手段是在高德控制台
        给这个 key 配「域名白名单」。**这个接口要求已登录**，所以至少不是
        对全互联网敞开。
        """
        return {
            "devices": [
                {"id": d.id, "report_interval": d.report_interval}
                for d in cfg.devices
            ],
            "gaode_key": cfg.web.resolve_gaode_key(),
            "gaode_security_code": cfg.web.gaode_security_code,
            "allow_remote_unlock": cfg.allow_remote_unlock,
            "commands": sorted(ct.COMMANDS),
        }

    @router.get("/api/state/{device_id}", dependencies=[Depends(require_session)])
    async def web_state(device_id: str) -> dict[str, Any]:
        _check_dev(cfg, device_id)
        return await svc.publish_state(device_id)

    @router.get("/api/track", dependencies=[Depends(require_session)])
    async def web_track(dev: str, since: int = 0, until: int = 0,
                        limit: int = 2000) -> dict[str, Any]:
        _check_dev(cfg, dev)
        limit = max(1, min(limit, 5000))
        rows = await svc.store.track(dev, since, until or int(time.time()),
                                     limit, 0)
        return {"dev": dev, "count": len(rows), "points": rows}

    @router.get("/api/events", dependencies=[Depends(require_session)])
    async def web_events(dev: str, limit: int = 50) -> dict[str, Any]:
        _check_dev(cfg, dev)
        return {"dev": dev, "events": await svc.store.events(
            dev, max(1, min(limit, 500)))}

    @router.get("/api/pending", dependencies=[Depends(require_session)])
    async def web_pending(dev: str) -> dict[str, Any]:
        _check_dev(cfg, dev)
        rows = await svc.store.pending_downlinks(dev)
        safe = [
            {k: v for k, v in r.items() if k != "payload"}
            | {"bytes": len(r["payload"])}
            for r in rows
        ]
        return {"dev": dev, "pending": ct.redact(safe)}

    @router.post("/api/cmd/{device_id}/{cmd}",
                 dependencies=[Depends(require_session)])
    async def web_cmd(device_id: str, cmd: str) -> dict[str, Any]:
        """下发指令。和 API 侧走同一套检查 —— 尤其是远程开锁的 403。"""
        _check_dev(cfg, device_id)
        if cmd not in ct.COMMANDS:
            raise HTTPException(status_code=400, detail="未知指令")
        if cmd == "unlock" and not cfg.allow_remote_unlock:
            raise HTTPException(
                status_code=403,
                detail="远程开锁已禁用（它绕过 BLE 挑战应答）。"
                       "要用请在配置里设 allow_remote_unlock=true")
        dn_id = await svc.enqueue_cmd(device_id, cmd)
        return {"queued": dn_id, "cmd": cmd}

    return router


def _check_dev(cfg: ServerConfig, device_id: str) -> None:
    if cfg.device(device_id) is None:
        raise HTTPException(status_code=404, detail=f"未配置的设备 {device_id}")
