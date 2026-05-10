# Benchmark — Passa

Benchmark do proxy passa TCP→UDS com varredura de BUF_SIZE.

## Uso

```bash
cd benchmark
docker compose up --build -d
sleep 5
docker compose exec bench python /bench.py
```

## Varredura de BUF_SIZE

```bash
docker compose exec bench python /bench_all_bufs.py
```

Testa 5 tamanhos de buffer (4K, 8K, 16K, 32K, 64K) com 5 execuções cada, descartando a pior.

## Arquitetura

| Componente | Descrição |
|-----------|-----------|
| **passa** | Proxy TCP→UDS (build local) |
| **backend1/2** | Servidores echo UDS (Python) |
| **bench** | Script de benchmark |

## Parâmetros

| Parâmetro | Valor |
|-----------|-------|
| Backends | 2× UDS echo (Python) |
| Balanceamento | Round-robin |
| Buffer proxy | 64 KB (padrão) |
| CPU proxy | 0.2 |
| Memória proxy | 64 MB |
| Conexões | 500 |
| Concorrência | 20 threads |
| Payload | 1024 bytes (echo) |

## Serviços

| Serviço | Porta |
|---------|-------|
| **passa** | 8080 |

## Estrutura

```
benchmark/
├── docker-compose.yml   # Serviços (passa + backends + bench)
├── backend.py           # UDS echo server
├── bench.py             # Benchmark rápido
├── bench_all_bufs.py    # Varredura de BUF_SIZE
└── README.md
```
