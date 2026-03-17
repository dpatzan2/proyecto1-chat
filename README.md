# Chat Application — CC3064 Proyecto 1

Chat cliente-servidor en C++17 usando **sockets TCP**, **multithreading (pthreads/std::thread)** y **Protocol Buffers**.

---

## Estructura del proyecto

```
chat_project/
├── Makefile
├── README.md
├── include/
│   └── framing.h              ← TCP framing (header 5 bytes)
├── protos/
│   ├── common.proto
│   ├── client-side/
│   │   ├── register.proto
│   │   ├── message_general.proto
│   │   ├── message_dm.proto
│   │   ├── change_status.proto
│   │   ├── list_users.proto
│   │   ├── get_user_info.proto
│   │   └── quit.proto
│   └── server-side/
│       ├── all_users.proto
│       ├── for_dm.proto
│       ├── broadcast_messages.proto
│       ├── get_user_info_response.proto
│       └── server_response.proto
├── src/
│   ├── server/
│   │   └── server.cpp
│   └── client/
│       └── client.cpp
└── gen/                       ← Auto-generado por protoc (no editar)
```

---

## Dependencias

```bash
# Ubuntu/Debian
sudo apt install build-essential protobuf-compiler libprotobuf-dev
```

---

## Compilar

```bash
make          # compila protos, server y client
make protos   # solo genera los .pb.h / .pb.cc
make server   # solo el servidor
make client   # solo el cliente
make clean    # limpia todo
```

---

## Ejecutar

```bash
# Terminal 1 — Servidor
./server 8080

# Terminal 2 — Cliente
./client alice 127.0.0.1 8080

# Terminal 3 — Otro cliente
./client bob 127.0.0.1 8080
```

---

## Comandos del cliente

| Comando | Descripción |
|---|---|
| `/broadcast <mensaje>` | Enviar mensaje a todos los usuarios |
| `/dm <usuario> <mensaje>` | Enviar mensaje directo privado |
| `/status active\|busy\|inactive` | Cambiar tu status |
| `/list` | Listar usuarios conectados |
| `/info <usuario>` | Ver información (IP, status) de un usuario |
| `/help` | Mostrar ayuda |
| `/quit` | Desconectarse del servidor |
| `<usuario> <mensaje>` | Atajo para DM |

---

## Protocolo

### Framing TCP (5-byte header)

```
┌─────────┬──────────────────────┬─────────────────────┐
│ 1 byte  │  4 bytes big-endian  │  N bytes            │
│  type   │  payload length      │  protobuf payload   │
└─────────┴──────────────────────┴─────────────────────┘
```

### Tabla de tipos

| Tipo | Dirección       | Proto                   |
|------|-----------------|-------------------------|
| 1    | client→server   | register                |
| 2    | client→server   | message_general         |
| 3    | client→server   | message_dm              |
| 4    | client→server   | change_status           |
| 5    | client→server   | list_users              |
| 6    | client→server   | get_user_info           |
| 7    | client→server   | quit                    |
| 10   | server→client   | server_response         |
| 11   | server→client   | all_users               |
| 12   | server→client   | for_dm                  |
| 13   | server→client   | broadcast_messages      |
| 14   | server→client   | get_user_info_response  |

---

## Status codes del servidor

| Código | Significado |
|--------|-------------|
| 200    | OK |
| 404    | Usuario no encontrado |
| 409    | Conflicto (nombre/IP duplicado) |

---

## Funcionalidades implementadas

### Servidor
- [x] Multithreading: un thread por cliente (`std::thread` detached)
- [x] Registro de usuarios (nombre único + IP única)
- [x] Liberación de usuarios al desconectarse
- [x] Listado de usuarios conectados
- [x] Información de usuario específico
- [x] Broadcasting (chat general)
- [x] Mensajes directos (DM)
- [x] Cambio de status por solicitud del cliente
- [x] Asignación automática de status INACTIVO por timeout (`INACTIVITY_TIMEOUT_SEC`)
- [x] Monitor de inactividad en thread dedicado

### Cliente
- [x] Conexión y registro automático al iniciar
- [x] Thread receptor de mensajes independiente
- [x] Chat general (`/broadcast`)
- [x] Mensajes privados (`/dm`)
- [x] Cambio de status (`/status`)
- [x] Listado de usuarios (`/list`)
- [x] Info de usuario (`/info`)
- [x] Ayuda (`/help`)
- [x] Salida limpia (`/quit`)

---

## Ajustar timeout de inactividad

En `src/server/server.cpp`, línea:

```cpp
static constexpr int INACTIVITY_TIMEOUT_SEC = 60;
```

Cámbialo a `10` o `15` para facilitar la evaluación.
