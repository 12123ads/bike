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
    `compare_digest`，攻击者可以用它来打满 CPU。这里按来源 IP 限速。
    """

    max_attempts: int = 10
    window: int = 300

    _fails: dict[str, list[float]] = field(default_factory=dict)

    def blocked(self, ip: str) -> bool:
        now = time.time()
        hits = [t for t in self._fails.get(ip, []) if now - t < self.window]
        self._fails[ip] = hits
        return len(hits) >= self.max_attempts

    def record_failure(self, ip: str) -> None:
        self._fails.setdefault(ip, []).append(time.time())

    def reset(self, ip: str) -> None:
        self._fails.pop(ip, None)
