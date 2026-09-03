"""SQLite 落库。

为什么服务端必须落库（DESIGN.md §9.1）：设备侧库层没有重发，
broker 或服务端任一环没接住的报文在库层就没有第二次机会。

用裸 aiosqlite 而不是 SQLAlchemy ORM：表只有六张，查询都是手写 SQL，
ORM 在这个规模只增加一层间接。

SQLAlchemy 仍在依赖里，但**不是因为「amqtt 的持久化插件要用」**（本项目
从未使用那个插件）。真实原因：`certs.py` 导入 `amqtt.contrib.cert`，
而 `amqtt/contrib/__init__.py` 顶层就 `from sqlalchemy import ...`。
同理 `pwdlib` 也是 `certs.py` 直接 import 的、靠 amqtt 的传递依赖装进来的 ——
已在 pyproject 里显式声明，免得 amqtt 换版本时突然 ImportError。
"""

from __future__ import annotations

import json
import logging
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import aiosqlite

_log = logging.getLogger("ebike.store")

SCHEMA = """
-- 增量回收（审计 R5）。默认 auto_vacuum=NONE 时 `DELETE` 只把页放进
-- freelist，**文件永不缩小** —— 容器里用的是具名卷，用户看到的是「卷一直
-- 在长」而没有任何手段缩回去。
--
-- 选 INCREMENTAL 而不是 FULL：FULL 在每次 commit 时搬页，写放大明显；
-- INCREMENTAL 只是把空页记进 freelist，由 `prune()` 之后显式
-- `PRAGMA incremental_vacuum` 分批回收。
--
-- ⚠ **这一条必须在 `journal_mode=WAL` 之前**（本机实测）：`auto_vacuum`
-- 只能在库还空着、且**尚未进入 WAL** 的时候设置。反过来写的话它静默失效
-- （`PRAGMA auto_vacuum` 读回来是 0），于是回收永远不生效而且没有任何报错。
--
-- ⚠ 只对**新建**库生效。`auto_vacuum` 是建库时写进 header 的，对已有库
-- 这条 PRAGMA 是空操作。所以 `reclaim()` 里用 `freelist_count` 判断到底
-- 回收了没有，没回收就 warn 出「要手动 VACUUM 一次」。
PRAGMA auto_vacuum=INCREMENTAL;
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

-- 需要跨进程重启保持的小状态。目前两个键：
--   dn_seq        下行 id 的高水位（见 max_dn_seq 的说明）
--   interval:<dev> 设备实际生效的上报周期（`interval` 指令被 ack 后写这里）
-- 用键值表而不是给 dev_state 加列：这个 schema 没有迁移机制
-- （只有 CREATE TABLE IF NOT EXISTS），加列不会作用到已存在的库上。
CREATE TABLE IF NOT EXISTS meta (
    k TEXT PRIMARY KEY,
    v TEXT NOT NULL
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

    async def downlink(self, dn_id: str) -> dict[str, Any] | None:
        """按 id 取一条下行（不论是否已确认）。`up/ack` 到达时要看它发的是什么。"""
        cur = await self.db.execute(
            "SELECT * FROM pending_downlink WHERE id=?", (dn_id,))
        row = await cur.fetchone()
        return dict(row) if row else None

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

    async def reserve_dn_seq(self, n: int = 1) -> int:
        """预留 n 个下行序号，返回预留段的**起始值**（含）。

        为什么要落库：`id` 是 `pending_downlink` 的主键，而 `mark_acked` 只置
        `acked=1` 不删行。序号只存在进程内存里的话，**重启后第一条命令生成的
        `c-1` 会撞上历史那一行**，IntegrityError 冒到 FastAPI 变成 500。

        `INSERT ... ON CONFLICT DO UPDATE ... RETURNING` 一条语句完成读改写，
        不给并发留窗口（SQLite 3.35+ 支持 RETURNING，本机 3.46）。
        """
        cur = await self.db.execute(
            "INSERT INTO meta(k,v) VALUES('dn_seq',?)"
            " ON CONFLICT(k) DO UPDATE SET v=CAST(v AS INTEGER)+?"
            " RETURNING CAST(v AS INTEGER)", (str(n), n))
        row = await cur.fetchone()
        await self.db.commit()
        high = int(row[0])
        return high - n + 1

    async def sync_dn_seq_to_existing(self) -> int:
        """把高水位抬到已有队列行之上，返回抬到的值。

        用于 `meta` 表刚建出来（旧库升级）而 `pending_downlink` 里已经有
        `c-7` 这种行的情况 —— 只靠计数器从 0 起会立刻撞主键。
        """
        cur = await self.db.execute(
            "SELECT id FROM pending_downlink WHERE id GLOB '[cs]-*'")
        top = 0
        for (dn_id,) in await cur.fetchall():
            try:
                top = max(top, int(dn_id.split("-", 1)[1]))
            except (IndexError, ValueError):
                continue
        cur = await self.db.execute(
            "SELECT CAST(v AS INTEGER) FROM meta WHERE k='dn_seq'")
        row = await cur.fetchone()
        cur_seq = int(row[0]) if row else 0
        if top > cur_seq:
            await self.db.execute(
                "INSERT INTO meta(k,v) VALUES('dn_seq',?)"
                " ON CONFLICT(k) DO UPDATE SET v=excluded.v", (str(top),))
            await self.db.commit()
            return top
        return cur_seq

    async def prune_downlinks(self, retention_days: int) -> int:
        """删掉已确认且过了保留期的下行行。返回删掉的行数。0 天 = 永久保留。

        只删 `acked=1` 的：未确认的那条可能是唯一还能开锁的密钥（契约 §4.1），
        无论多老都不能删。保留一段时间而不是 ack 即删，是为了 `/pending`
        与日志能回溯「那次密钥轮换到底送到没有」。
        """
        if retention_days <= 0:
            return 0
        cutoff = int(time.time()) - retention_days * 86400
        cur = await self.db.execute(
            "DELETE FROM pending_downlink WHERE acked=1 AND created<?", (cutoff,))
        await self.db.commit()
        return cur.rowcount

    # --- meta（跨重启的小状态） -----------------------------------------------

    async def get_meta(self, key: str) -> str | None:
        cur = await self.db.execute("SELECT v FROM meta WHERE k=?", (key,))
        row = await cur.fetchone()
        return str(row[0]) if row else None

    async def set_meta(self, key: str, value: str) -> None:
        await self.db.execute(
            "INSERT INTO meta(k,v) VALUES(?,?)"
            " ON CONFLICT(k) DO UPDATE SET v=excluded.v", (key, value))
        await self.db.commit()

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
        if total:
            await self.reclaim()
        return total

    async def reclaim(self) -> int:
        """把 `DELETE` 腾出来的页还给文件系统。返回回收的页数（审计 R5）。

        `DELETE` 只把页挂进 freelist，文件大小不变 —— 单车场景下一年也就
        几 MB，**所以这不是容量问题**，是「卷只增不减且没有任何手段缩回去」
        的运维问题（容器用具名卷）。周期改短或加车会放大：把 900 s 改成 60 s
        就是 15 倍。

        `PRAGMA incremental_vacuum` 需要建库时就是 `auto_vacuum=INCREMENTAL`
        （见 SCHEMA 里那条）。**旧库不是**，那条 PRAGMA 对它是空操作 ——
        这里通过 `freelist_count` 判断是否真的回收了，没回收就明确 warn，
        让人知道要手动 `VACUUM` 一次而不是以为回收在跑。
        """
        cur = await self.db.execute("PRAGMA freelist_count")
        row = await cur.fetchone()
        before = int(row[0]) if row else 0
        if before == 0:
            return 0

        # ⚠ **必须把结果行读干**（本机实测）：`PRAGMA incremental_vacuum` 是
        # 一条语句而不是一次性动作，回收发生在**步进游标**的过程里。
        # 只 `execute` 不 `fetchall` 只会走一步 —— 1583 个空闲页里只回收 1 个，
        # 文件一点没缩，而且没有任何报错。
        #
        # 显式带页数参数：不带参数是「回收全部」，带上 before 同样是全部，
        # 但把「要回收多少」写进 SQL 便于对着 freelist_count 核对。
        cur = await self.db.execute(f"PRAGMA incremental_vacuum({before})")
        await cur.fetchall()
        await self.db.commit()

        cur = await self.db.execute("PRAGMA freelist_count")
        row = await cur.fetchone()
        after = int(row[0]) if row else 0
        freed = before - after
        if freed <= 0:
            _log.warning(
                "有 %d 个空闲页但回收不掉 —— 这个库建的时候不是 "
                "auto_vacuum=INCREMENTAL（旧库）。要缩小文件得停机跑一次 "
                "`sqlite3 %s 'VACUUM;'`", before, self.path)
            return 0
        _log.info("回收了 %d 个空闲页", freed)
        return freed
