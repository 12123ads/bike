"""SQLite 落库。

为什么服务端必须落库（DESIGN.md §9.1）：设备侧库层没有重发，
broker 或服务端任一环没接住的报文在库层就没有第二次机会。

用裸 aiosqlite 而不是 SQLAlchemy ORM：表只有五张，查询都是手写 SQL，
ORM 在这个规模只增加一层间接。SQLAlchemy 仍在依赖里，因为 amqtt 的
持久化插件要用。
"""

from __future__ import annotations

import json
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import aiosqlite

SCHEMA = """
PRAGMA journal_mode=WAL;

-- 位置点。t_srv 是排序与展示的依据，t_dev 只用于诊断（契约 §5.6）
CREATE TABLE IF NOT EXISTS loc (
    id       INTEGER PRIMARY KEY AUTOINCREMENT,
    dev      TEXT    NOT NULL,
    q        INTEGER NOT NULL,
    t_srv    INTEGER NOT NULL,
    t_dev    INTEGER NOT NULL,
    src      TEXT    NOT NULL,
    lat      REAL    NOT NULL,
    lon      REAL    NOT NULL,
    acc      REAL,
    speed    REAL,
    heading  INTEGER,
    sats     INTEGER,
    -- 同一个 (dev, q) 只存一次：契约 §5 的 q 掉电不清零，
    -- 断网补发时同一批点会被重发，靠这个唯一约束去重
    UNIQUE(dev, q)
);
CREATE INDEX IF NOT EXISTS loc_dev_time ON loc(dev, t_srv DESC);

CREATE TABLE IF NOT EXISTS tele (
    id      INTEGER PRIMARY KEY AUTOINCREMENT,
    dev     TEXT    NOT NULL,
    q       INTEGER NOT NULL,
    t_srv   INTEGER NOT NULL,
    t_dev   INTEGER NOT NULL,
    volt    REAL,
    csq     INTEGER,
    uptime  INTEGER,
    temp    INTEGER,
    UNIQUE(dev, q)
);
CREATE INDEX IF NOT EXISTS tele_dev_time ON tele(dev, t_srv DESC);

CREATE TABLE IF NOT EXISTS event (
    id      INTEGER PRIMARY KEY AUTOINCREMENT,
    dev     TEXT    NOT NULL,
    q       INTEGER NOT NULL,
    t_srv   INTEGER NOT NULL,
    t_dev   INTEGER NOT NULL,
    kind    TEXT    NOT NULL,
    detail  TEXT,
    UNIQUE(dev, q)
);
CREATE INDEX IF NOT EXISTS event_dev_time ON event(dev, t_srv DESC);

-- 未确认的下行队列。契约 §4.1：下行不 retain，服务端自己排队，
-- 因为 retain 每 topic 只留一条，连续两次密钥轮换会丢掉第一把。
CREATE TABLE IF NOT EXISTS pending_downlink (
    id       TEXT    PRIMARY KEY,       -- 契约 §5.5 的 ack.id
    dev      TEXT    NOT NULL,
    suffix   TEXT    NOT NULL,          -- dn/cmd 或 dn/secret
    payload  BLOB    NOT NULL,          -- 已序列化好的报文（可能含密钥材料）
    created  INTEGER NOT NULL,
    sent_at  INTEGER,                   -- 最后一次尝试发送的时间
    tries    INTEGER NOT NULL DEFAULT 0,
    acked    INTEGER NOT NULL DEFAULT 0,
    ack_ok   INTEGER,
    ack_err  TEXT
);
CREATE INDEX IF NOT EXISTS pending_open ON pending_downlink(dev, acked, created);

-- 每台设备的当前状态。派生出来的，可以随时从上面几张表重算
CREATE TABLE IF NOT EXISTS dev_state (
    dev        TEXT PRIMARY KEY,
    last_seen  INTEGER,
    lwt        INTEGER NOT NULL DEFAULT 0,
    kid        INTEGER NOT NULL DEFAULT 0,
    fw         TEXT,
    mode       TEXT,                    -- moving / parked
    state_json TEXT                     -- 最后一次发布的 state，用于重启后恢复 retain
);
"""


@dataclass
class Store:
    """所有 SQL 都在这里。上层不碰 SQL —— 换库只改这一个文件。"""

    path: str
    _db: aiosqlite.Connection | None = None

    async def open(self) -> None:
        Path(self.path).parent.mkdir(parents=True, exist_ok=True)
        self._db = await aiosqlite.connect(self.path)
        self._db.row_factory = aiosqlite.Row
        await self._db.executescript(SCHEMA)
        await self._db.commit()

    async def close(self) -> None:
        if self._db is not None:
            await self._db.close()
            self._db = None

    @property
    def db(self) -> aiosqlite.Connection:
        if self._db is None:
            raise RuntimeError("Store 还没 open()")
        return self._db

    # --- 写入 ----------------------------------------------------------------

    async def add_loc(self, dev: str, points: list[dict[str, Any]], t_srv: int) -> int:
        """存位置点，返回**真正新增**的条数。

        `INSERT OR IGNORE` 配合 UNIQUE(dev, q)：补发重复的点会被静默跳过，
        这正是想要的行为 —— 去重，而不是报错让整批失败。
        """
        rows = [
            (dev, p["q"], t_srv, p["t_dev"], p["src"], p["lat"], p["lon"],
             p.get("acc"), p.get("speed"), p.get("heading"), p.get("sats"))
            for p in points
        ]
        cur = await self.db.executemany(
            "INSERT OR IGNORE INTO loc"
            "(dev,q,t_srv,t_dev,src,lat,lon,acc,speed,heading,sats)"
            " VALUES(?,?,?,?,?,?,?,?,?,?,?)",
            rows,
        )
        await self.db.commit()
        return cur.rowcount

    async def add_tele(self, dev: str, d: dict[str, Any], t_srv: int) -> None:
        await self.db.execute(
            "INSERT OR IGNORE INTO tele(dev,q,t_srv,t_dev,volt,csq,uptime,temp)"
            " VALUES(?,?,?,?,?,?,?,?)",
            (dev, d["q"], t_srv, d["t_dev"], d.get("volt"), d.get("csq"),
             d.get("uptime"), d.get("temp")),
        )
        await self.db.commit()

    async def add_event(self, dev: str, d: dict[str, Any], t_srv: int) -> None:
        detail = json.dumps(d["detail"], separators=(",", ":")) if d.get("detail") else None
        await self.db.execute(
            "INSERT OR IGNORE INTO event(dev,q,t_srv,t_dev,kind,detail)"
            " VALUES(?,?,?,?,?,?)",
            (dev, d["q"], t_srv, d["t_dev"], d["kind"], detail),
        )
        await self.db.commit()

    # --- 读取 ----------------------------------------------------------------

    async def last_loc(self, dev: str) -> dict[str, Any] | None:
        cur = await self.db.execute(
            "SELECT * FROM loc WHERE dev=? ORDER BY t_srv DESC, id DESC LIMIT 1", (dev,))
        row = await cur.fetchone()
        return dict(row) if row else None

    async def prev_loc(self, dev: str) -> dict[str, Any] | None:
        """倒数第二个点，用来算位移判「是不是真的动了」。"""
        cur = await self.db.execute(
            "SELECT * FROM loc WHERE dev=? ORDER BY t_srv DESC, id DESC LIMIT 1 OFFSET 1",
            (dev,))
        row = await cur.fetchone()
        return dict(row) if row else None

    async def last_tele(self, dev: str) -> dict[str, Any] | None:
        cur = await self.db.execute(
            "SELECT * FROM tele WHERE dev=? ORDER BY t_srv DESC, id DESC LIMIT 1", (dev,))
        row = await cur.fetchone()
        return dict(row) if row else None

    async def track(self, dev: str, since: int, until: int,
                    limit: int, offset: int) -> list[dict[str, Any]]:
        cur = await self.db.execute(
            "SELECT t_srv,t_dev,src,lat,lon,acc,speed,heading,sats FROM loc"
            " WHERE dev=? AND t_srv>=? AND t_srv<=?"
            " ORDER BY t_srv ASC, id ASC LIMIT ? OFFSET ?",
            (dev, since, until, limit, offset),
        )
        return [dict(r) for r in await cur.fetchall()]

    async def events(self, dev: str, limit: int) -> list[dict[str, Any]]:
        cur = await self.db.execute(
            "SELECT t_srv,t_dev,kind,detail FROM event WHERE dev=?"
            " ORDER BY t_srv DESC, id DESC LIMIT ?", (dev, limit))
        out = []
        for r in await cur.fetchall():
            d = dict(r)
            d["detail"] = json.loads(d["detail"]) if d["detail"] else None
            out.append(d)
        return out

    # --- 下行队列（契约 §4.1） -------------------------------------------------

    async def enqueue_downlink(self, dn_id: str, dev: str, suffix: str,
                               payload: bytes) -> None:
        await self.db.execute(
            "INSERT INTO pending_downlink(id,dev,suffix,payload,created)"
            " VALUES(?,?,?,?,?)",
            (dn_id, dev, suffix, payload, int(time.time())),
        )
        await self.db.commit()

    async def pending_downlinks(self, dev: str) -> list[dict[str, Any]]:
        """按创建顺序取未确认的下行。

        顺序很重要：`dn/secret` 的连续两次轮换必须按序到达，否则设备会停在旧密钥上。
        """
        cur = await self.db.execute(
            "SELECT * FROM pending_downlink WHERE dev=? AND acked=0"
            " ORDER BY created ASC, rowid ASC", (dev,))
        return [dict(r) for r in await cur.fetchall()]

    async def mark_sent(self, dn_id: str) -> None:
        await self.db.execute(
            "UPDATE pending_downlink SET sent_at=?, tries=tries+1 WHERE id=?",
            (int(time.time()), dn_id))
        await self.db.commit()

    async def mark_acked(self, dn_id: str, ok: bool, err: str | None) -> bool:
        """销账。返回 False 表示这个 id 不在队列里 —— 可能是设备重发了 ack，
        也可能是伪造的，两种都只需要记日志。
        """
        cur = await self.db.execute(
            "UPDATE pending_downlink SET acked=1, ack_ok=?, ack_err=?"
            " WHERE id=? AND acked=0",
            (1 if ok else 0, err, dn_id))
        await self.db.commit()
        return cur.rowcount > 0

    # --- 设备状态 -------------------------------------------------------------

    async def touch(self, dev: str, t_srv: int) -> None:
        """更新 last_seen。**每条上行都调** —— 在线判定完全建立在它之上（契约 §4.2）。"""
        await self.db.execute(
            "INSERT INTO dev_state(dev,last_seen) VALUES(?,?)"
            " ON CONFLICT(dev) DO UPDATE SET last_seen=excluded.last_seen",
            (dev, t_srv))
        await self.db.commit()

    async def set_dev_fields(self, dev: str, **fields: Any) -> None:
        if not fields:
            return
        cols = ",".join(f"{k}=excluded.{k}" for k in fields)
        names = ",".join(fields)
        holes = ",".join("?" for _ in fields)
        await self.db.execute(
            f"INSERT INTO dev_state(dev,{names}) VALUES(?,{holes})"
            f" ON CONFLICT(dev) DO UPDATE SET {cols}",
            (dev, *fields.values()))
        await self.db.commit()

    async def dev_state(self, dev: str) -> dict[str, Any]:
        cur = await self.db.execute("SELECT * FROM dev_state WHERE dev=?", (dev,))
        row = await cur.fetchone()
        return dict(row) if row else {"dev": dev, "last_seen": None, "lwt": 0,
                                      "kid": 0, "fw": None, "mode": None,
                                      "state_json": None}

    async def all_states(self) -> list[dict[str, Any]]:
        cur = await self.db.execute("SELECT * FROM dev_state")
        return [dict(r) for r in await cur.fetchall()]

    # --- 保留策略（DESIGN.md §11 #9） -----------------------------------------

    async def prune(self, retention_days: int) -> int:
        """删掉过期轨迹。返回删掉的行数。0 天 = 永久保留，不删。"""
        if retention_days <= 0:
            return 0
        cutoff = int(time.time()) - retention_days * 86400
        total = 0
        for table in ("loc", "tele", "event"):
            cur = await self.db.execute(f"DELETE FROM {table} WHERE t_srv<?", (cutoff,))
            total += cur.rowcount
        await self.db.commit()
        return total
