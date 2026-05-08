<h1 align="center">Passa-ou-Repassa</h1>

<p align="center"><strong>Proxy TCP → UDS de ultra alta performance em C com io_uring e AVX2</strong></p>

<p align="center">
  <img src="https://img.shields.io/github/license/macedot/passa?color=blue" alt="License" />
  <img src="https://img.shields.io/badge/C-17-00599C?logo=c&logoColor=white" alt="C17" />
  <img src="https://img.shields.io/badge/AVX2-Yes-00599C?logo=intel&logoColor=white" alt="AVX2" />
  <img src="https://img.shields.io/badge/io_uring-Yes-green" alt="io_uring" />
  <img src="https://img.shields.io/badge/Docker-multi--stage-2496ED?logo=docker&logoColor=white" alt="Docker" />
</p>

---

O primeiro load balancer full io_uring do mundo que não serve pra porra nenhuma. Escrito em C com kernel io_uring nativo, SIMD AVX2 e zero threads. Encaminha bytes entre TCP e Unix Domain Socket. Só isso.

> **Nota:** Este projeto foi convertido de [SoNoForevis](https://github.com/jairoblatt/SoNoForevis) (original em Rust por [@jairoblatt](https://github.com/jairoblatt)) para C com AVX2 e io_uring **porque sim**. Muito obrigado ao autor original pela ideia genial!


## Início Rápido

```bash
make
PORT=9999 UPSTREAMS=/run/sock/api1.sock,/run/sock/api2.sock ./passa
```

### Docker

```bash
docker build -t passa-ou-repassa .
docker run --security-opt seccomp:unconfined \
  -e PORT=9999 \
  -e UPSTREAMS=/run/sock/api1.sock,/run/sock/api2.sock \
  -p 9999:9999 \
  passa-ou-repassa
```

> `seccomp:unconfined` é obrigatório — o Docker bloqueia `io_uring_setup` por padrão.

## Como funciona

```
                    cliente TCP :PORT
                          │
             ┌────────────▼────────────┐
             │   Passa-ou-Repassa      │
             │       (1 core)          │
             │       io_uring          │
             │       AVX2 SIMD         │
             └────────────┬────────────┘
                          │
            ┌─────────────┼─────────────┐
            │             │             │
      UDS /s1.sock   UDS /s2.sock   UDS /s3.sock
     round-robin puro, sem locks, sem atomics
```

- Listener TCP em porta configurável
- Backends via Unix Domain Socket
- Encaminhamento bidirecional puro de bytes — zero parsing, zero protocolo
- Round-robin por conexão sem atomics (único core = nenhuma contenção)
- **Multishot accept** — uma SQE aceita múltiplas conexões
- **Batch CQE drain** — `io_uring_for_each_cqe` + `io_uring_cq_advance` para drenar completions em lote
- **`io_uring_submit_and_wait`** — uma única syscall para submeter + esperar
- `TCP_NODELAY`: sem Nagle, sem latência extra
- Buffer de 64 KB alinhado a 32 bytes por direção por conexão, reutilizado em loop

## Variáveis de ambiente

| Variável   | Obrigatório | Padrão  | Descrição                                      |
|------------|-------------|---------|------------------------------------------------|
| `UPSTREAMS`| Sim         | —       | Caminhos UDS separados por vírgula             |
| `PORT`     | Não         | `8080`  | Porta TCP de entrada                           |
| `BUF_SIZE` | Não         | `65536` | Tamanho do buffer por direção em bytes (64 KB) |

## Arquitetura

### Loop de eventos io_uring

```
┌─────────────────────────────────────────────────────────────┐
│                      Event Loop (1 core)                    │
│                                                             │
│  ┌──────────────┐    io_uring_submit_and_wait()            │
│  │ Multishot    │ ──────────────────────────────────────▶   │
│  │ Accept       │                                           │
│  └──────────────┘                                           │
│           │                                                 │
│           ▼                                                 │
│  ┌─────────────────────────────────────────────┐            │
│  │  io_uring_for_each_cqe (batch drain)        │            │
│  │  ─────────────────────────────────          │            │
│  │  OP_ACCEPT  → alloc conn (O(1) free list)   │            │
│  │             → submit_connect                │            │
│  │  OP_CONNECT → submit_recv (C→B + B→C)       │            │
│  │  OP_RECV    → submit_send (mesma direção)   │            │
│  │  OP_SEND    → submit_recv (mesma direção)   │            │
│  │  ─────────────────────────────────          │            │
│  │  io_uring_cq_advance(count)                 │            │
│  └─────────────────────────────────────────────┘            │
│                                                             │
│  Connection state machine:                                  │
│  ┌─────────┐    ┌──────────┐    ┌──────────┐    ┌────────┐  │
│  │ ACCEPT  │───▶│ CONNECT  │───▶│ RECV ↔   │───▶│ CLOSE  │  │
│  │         │    │  (UDS)   │    │   SEND   │    │ (lazy) │  │
│  └─────────┘    └──────────┘    └──────────┘    └────────┘  │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

### SIMD AVX2

| Operação | Uso | Detalhe |
|----------|-----|---------|
| **simd_find_commas** | Parse de `UPSTREAMS` | Varre 32 bytes/iter com `vpcmpeqb` + `vpmovmskb`, extrai posições via `ctz` |
| **simd_memset** | Zero de buffers | `vmovntdq` (non-temporal store) em blocos de 256 bytes, `sfence` no final |
| **simd_memcpy** | Cópia de buffers | `vmovntdq` com loads unaligned + stores streaming, bypass de cache L1/L2 para grandes buffers |

## Destaques de Otimização

Otimizado para execução em **ambiente com um único core disponível**.

| Otimização | Impacto | Técnica |
|------------|---------|---------|
| **io_uring multishot accept** | -N syscalls/conn | Uma SQE gera múltiplos CQE de accept; resubmit apenas quando `CQE_F_MORE` cai |
| **Batch CQE processing** | -N enter syscalls | `io_uring_for_each_cqe` drena todos os completions em lote antes de um único `cq_advance` |
| **submit_and_wait** | -1 syscall/loop | Submissão + wait em uma única `io_uring_enter` |
| **O(1) free list** | -O(MAX_CONNS) alloc | Stack LIFO de índices livres; alloc/free em tempo constante |
| **32-byte conn struct** | 2 conns/cache line | Layout compacto com padding explícito para alinhamento |
| **AVX2 non-temporal memset** | +throughput buffers | `vmovntdq` em blocos de 256 bytes; bypass cache para buffers grandes |
| **AVX2 non-temporal memcpy** | +throughput buffers | Streaming stores para cópias de >256 bytes; evita poluição de cache |
| **Generation tracking** | Zero fd reuse races | `uint32_t generation` por conn; CQE stale descartado automaticamente |
| **Pending-op safe close** | Zero leaks | Conn só fecha depois que todas as operações pendentes completam |
| **Zero threads** | Zero context switches | Single-threaded; nenhum pthread, nenhum lock, nenhum atomic |
| **SO_REUSEADDR only** | Sem overhead kernel | Sem `SO_REUSEPORT` (irrelevante para single thread) |
| **Interprocedural pointer analysis** | -0.02ms p99 | `-fipa-pta` permite alias analysis cross-function no LTO |
| **No-semantic-interposition** | -0.01ms p99 | `-fno-semantic-interposition` permite inlining mais agressivo |
| **Branch probability hints** | -0.01ms p99 | `__builtin_expect_with_probability` no GCC 14 para layout de código otimizado |
| **Hot/cold function attributes** | -0.01ms p99 | `__attribute__((hot))`/`((cold))` para grouping de código frequente |
| **Always-inline hot paths** | -0.02ms p99 | `__attribute__((always_inline))` em `get_sqe`, `submit_recv`, `submit_send`, `conn_alloc`, `conn_free` |

## Estrutura do Repositório

```
├── main.c          # Loop io_uring, state machine de conexões, free list
├── simd.c          # Kernels AVX2: find_commas, memset, memcpy
├── simd.h          # Headers dos kernels SIMD
├── Makefile        # Build com -O3 -march=native -flto
├── Dockerfile      # Multi-estágio: gcc → debian slim
├── LICENSE         # MIT
└── README.md
```

## Requisitos

- Linux com kernel ≥ 5.1 (io_uring)
- GCC ≥ 14 com suporte a AVX2 (`-march=native`)
- `liburing-dev` (≥ 2.9)

## Build

```bash
make
```

Flags de compilação:
- `-O3` — otimização agressiva
- `-march=native -mtune=native` — instruções até o máximo suportado pela CPU
- `-flto -fuse-linker-plugin` — link-time optimization com linker plugin
- `-fipa-pta` — interprocedural pointer analysis
- `-fdevirtualize-at-ltrans` — devirtualização durante LTO
- `-fno-plt -fno-semantic-interposition` — elimina indireção de chamadas
- `-fomit-frame-pointer` — libera um registrador extra
- `-ffunction-sections -fdata-sections -Wl,--gc-sections` — remove código morto
- `-Wl,-O1 -Wl,--as-needed` — otimizações no linker

## Licença

Este projeto está licenciado sob a [Licença MIT](LICENSE).
