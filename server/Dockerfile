# 电瓶车定位服务端 —— 内置 MQTT broker + 落库 + HTTP API
#
# 两阶段构建：builder 装依赖到 venv，runtime 只带 venv 和源码。
# 用 slim 而不是 alpine：cryptography 和 argon2 在 alpine 上要编译
# （musl 没有 manylinux wheel），镜像反而更大、构建更慢。

FROM python:3.13-slim AS builder

# 只装构建期需要的东西，装完就丢在这一层里
RUN apt-get update && apt-get install -y --no-install-recommends \
        gcc libc6-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY pyproject.toml ./
COPY ebike_server ./ebike_server

# 先建 venv 再装：这样 runtime 阶段整个拷过去就行，不用重复 pip
RUN python -m venv /venv \
    && /venv/bin/pip install --no-cache-dir --upgrade pip \
    && /venv/bin/pip install --no-cache-dir .

# --- runtime ------------------------------------------------------------------

FROM python:3.13-slim

# tini 做 PID 1：uvicorn 和 amqtt 都靠 SIGTERM 优雅退出，
# 没有 init 进程时 docker stop 会等满 10 秒再 SIGKILL。
RUN apt-get update && apt-get install -y --no-install-recommends tini \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /venv /venv

# 非 root 跑。1000:1000 是宿主上常见的第一个普通用户 id，
# 这样 ./run 目录的属主在宿主侧也是可读的。
RUN groupadd -g 1000 ebike && useradd -u 1000 -g 1000 -m -s /sbin/nologin ebike

# 数据目录。config.py 的默认路径是 /opt/ebike-tracker/run，
# 容器里统一挂到 /data，用 EBIKE_DIR 覆盖。
ENV EBIKE_DIR=/data \
    PATH=/venv/bin:$PATH \
    PYTHONUNBUFFERED=1 \
    PYTHONDONTWRITEBYTECODE=1 \
    TZ=Asia/Shanghai

RUN mkdir -p /data && chown -R ebike:ebike /data
VOLUME ["/data"]

USER ebike
WORKDIR /data

# 8883 设备（TLS）／1883 明文（默认只绑 127.0.0.1，见 config.py）／8080 HTTP
EXPOSE 8883 8080

# 健康检查打 /health —— 它不鉴权，且不泄露任何东西。
# 用 python 而不是 curl：镜像里没装 curl，为一个健康检查装它不值得。
HEALTHCHECK --interval=30s --timeout=5s --start-period=10s --retries=3 \
    CMD ["python", "-c", "import urllib.request,os,sys; \
port=os.environ.get('EBIKE_HTTP_PORT','8080'); \
sys.exit(0 if urllib.request.urlopen(f'http://127.0.0.1:{port}/health',timeout=4).status==200 else 1)"]

ENTRYPOINT ["/usr/bin/tini", "--", "ebike-server"]
# 不写 -c：`run` 会去 $EBIKE_DIR/config.json 找，找不到就用内置默认值并 warn。
# 这样 `docker compose run --rm ebike-server init --hostname x` 和后面的
# `up` 用的是同一个配置文件，不需要在两处写死同一个路径。
CMD ["run"]
