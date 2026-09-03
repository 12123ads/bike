"""网页界面的会话鉴权。

为什么不直接让网页用 Bearer token：那要求把 token 写进前端 JS 或 localStorage，
而这个 token 能下发指令（含远程开锁）。**localStorage 里的 token 会被任何一个
XSS 拿走且长期有效**，而 HttpOnly cookie 拿不走。

所以：登录页拿 API token 换一个短期会话 cookie，之后网页只带 cookie。
API token 本身**不进浏览器存储**。

会话存内存 —— 服务端重启就得重新登录。单车单用户，这是对的取舍：
落库会引入「会话表要不要清理」和「重启后旧会话是否还该有效」两个问题，
而重新登一次的成本是 5 秒。
"""

from __future__ import annotations

import hmac
import secrets
import time
from dataclasses import dataclass, field

COOKIE_NAME = "ebike_session"


@dataclass
class SessionStore:
    ttl: int
    #: token → 过期时刻（Unix 秒）
    _sessions: dict[str, float] = field(default_factory=dict)

    def create(self) -> tuple[str, int]:
        """签发一个会话，返回 (token, max_age)。"""
        self._prune()
        token = secrets.token_urlsafe(32)
        self._sessions[token] = time.time() + self.ttl
        return token, self.ttl

    def valid(self, token: str | None) -> bool:
        if not token:
            return False
        # 常数时间比较逐个查而不是 dict 查找：dict 查找的时间和 key 的内容相关，
        # 理论上可被用来试探。会话数是个位数，遍历不构成性能问题。
        now = time.time()
        for known, expiry in list(self._sessions.items()):
            if hmac.compare_digest(known, token):
                if expiry > now:
                    return True
                del self._sessions[known]
                return False
        return False

    def revoke(self, token: str | None) -> None:
        if not token:
            return
        for known in list(self._sessions):
            if hmac.compare_digest(known, token):
                del self._sessions[known]

    def _prune(self) -> None:
        now = time.time()
        for token, expiry in list(self._sessions.items()):
            if expiry <= now:
                del self._sessions[token]

    def __len__(self) -> int:
        self._prune()
        return len(self._sessions)


@dataclass
class LoginThrottle:
    """登录失败限速。

    API token 是 24 字节 urlsafe（约 190 bit），暴力破解本来就不现实，
    但**没有限速的登录接口会变成一个放大器** —— 每次请求都做一次
    `compare_digest`，攻击者可以用它来打满 CPU。这里按来源限速。

    键是**真实 socket 地址**（`web.py` 的 `rate_key`），不是任何请求头。
    审计 R2：曾经优先用 `X-Forwarded-For`，那等于把限速的键交给攻击者 ——
    实测每个请求换一个 XFF 值，连打 60 次错 token 一个 429 都没有。

    条目总数有硬上限（`max_tracked_ips`），否则来源轮换能把表撑爆。
    **逐出时跳过已达上限的键**（审计 R2）：按「最旧」丢会让灌表的一方
    把真实用户已经攒够的失败记录清掉，那样上限本身就成了绕过限速的手段。
    """

    max_attempts: int = 10
    window: int = 300
    #: _fails 的总条目上限（不同来源数）。真实用户是个位数；
    #: 超过它意味着来源在轮换。
    max_tracked_ips: int = 1024

    _fails: dict[str, list[float]] = field(default_factory=dict)

    def _recent(self, ip: str, now: float) -> list[float]:
        return [t for t in self._fails.get(ip, []) if now - t < self.window]

    def _prune_all(self, now: float) -> None:
        """清掉所有过期条目；仍然超上限时丢最旧的**未被封**的键。

        已达 `max_attempts` 的键不参与逐出 —— 它们正是限速要生效的对象。
        全都被封时宁可让表停在上限之上（每条只是几个 float），
        也不清掉任何一个正在生效的封禁。
        """
        for ip in [k for k, v in self._fails.items()
                   if not any(now - t < self.window for t in v)]:
            del self._fails[ip]

        while len(self._fails) >= self.max_tracked_ips:
            evictable = [k for k in self._fails
                         if len(self._recent(k, now)) < self.max_attempts]
            if not evictable:
                return
            del self._fails[min(evictable, key=lambda k: self._fails[k][0])]

    def blocked(self, ip: str) -> bool:
        now = time.time()
        self._prune_all(now)
        hits = self._recent(ip, now)
        self._fails[ip] = hits
        return len(hits) >= self.max_attempts

    def record_failure(self, ip: str) -> None:
        now = time.time()
        self._prune_all(now)
        self._fails.setdefault(ip, []).append(now)

    def reset(self, ip: str) -> None:
        self._fails.pop(ip, None)
