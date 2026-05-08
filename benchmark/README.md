# Benchmark — Passa vs HAProxy vs Nginx

Comparação de proxies TCP → UDS em ambiente controlado Docker.

## Uso

```bash
cd benchmark
docker compose up --build -d
sleep 5
docker compose exec bench python /bench.py
```

## Parâmetros uniformes

| Parâmetro | Valor |
|-----------|-------|
| Backends | 2× UDS echo server (Python) |
| Balanceamento | Round-robin (passa, haproxy) / least_conn (nginx) |
| Buffer proxy | 64 KB |
| CPU por proxy | variável (0.1 / 0.2 / 0.3) |
| Memória por proxy | 64 MB |
| Conexões benchmark | 500 |
| Concorrência | 20 threads |
| Payload | 1024 bytes (echo) |

## Resultados

### 0.1 CPU

| Proxy | RPS | p99 | Relativo |
|-------|-----|-----|----------|
| **passa** | 184 | 292ms | 100% |
| nginx | 157 | 297ms | 85% |
| haproxy | 149 | 294ms | 81% |

### 0.2 CPU

| Proxy | RPS | p99 | Relativo |
|-------|-----|-----|----------|
| **passa** | 214 | 201ms | 100% |
| nginx | 173 | 199ms | 81% |
| haproxy | 170 | 220ms | 79% |

### 0.3 CPU

| Proxy | RPS | p99 | Relativo |
|-------|-----|-----|----------|
| **passa** | 193 | 297ms | 100% |
| nginx | 162 | 200ms | 84% |
| haproxy | 156 | 300ms | 81% |

## Serviços

| Proxy | Porta | Imagem |
|-------|-------|--------|
| **passa** | 8080 | Build local (`..`) |
| **haproxy** | 8081 | `haproxy:3.3` |
| **nginx** | 8082 | `nginx:alpine` |

## Estrutura

```
benchmark/
├── docker-compose.yml   # Todos os serviços
├── haproxy.cfg          # HAProxy em modo TCP
├── nginx.conf           # Nginx stream module
├── backend.py           # UDS echo server
├── bench.py             # Script de benchmark
└── README.md
```
