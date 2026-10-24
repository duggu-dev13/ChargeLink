# ChargeLink

An OCPP 1.6J charging network in C++17/20: a charger simulator, an asynchronous central
system (CSMS) and a PostgreSQL-backed store, with a small report tool.

## Architecture

```
simulator.exe ──WebSocket (ocpp1.6)──▶ csms.exe ──libpqxx pool──▶ PostgreSQL
 (one per charger)                      │
                                        ├─ Server: acceptor, one strand per connection
                                        ├─ Session: handshake, read/write loop, lifecycle hooks
                                        ├─ ChargerRegistry: newest connection per charger wins
                                        └─ Dispatcher → handlers (boot-first rule, transactions)
report.exe ────────────────────────────▶ Repository ──▶ PostgreSQL
```

| Folder | Contents |
|---|---|
| `src/common` | OCPP frames, messages, dispatcher, reliable sender, backoff |
| `src/simulator` | connector state machine, hardware model, charger controller, console |
| `src/csms` | network layer (Beast), registry, handlers, lifecycle hooks |
| `src/db` | `Repository`: prepared statements, connection pool, read queries |
| `src/report` | command-line report tool |
| `sql/` | database schema (`001_init.sql`) |
| `tests/` | GoogleTest suite |

Key behaviours: boot-first rule; idempotent `StartTransaction` (a retry gets the same id);
stop validation (unknown id, another charger's id, meter going backwards); offline queue with
retry on the charger side; database rules that mirror the C++ rules (one open transaction per
connector); online flag follows the connection.

## Build (Windows)

Needs Visual Studio 2026 (x64 developer prompt), vcpkg and Docker.

```bat
docker compose up -d
cmake --preset default
cmake --build --preset default
```

## Run

```bat
set CHARGELINK_DB=host=127.0.0.1 port=5432 dbname=chargelink user=chargelink password=<password>
.\build\csms.exe -v
.\build\simulator.exe 127.0.0.1 9000 CP001
```

Simulator commands: `plug 16`, `tap [TAG]`, `unplug`, `quit` (see `help`).
Without `CHARGELINK_DB` the CSMS keeps everything in memory.

Reports: `report chargers`, `report transactions [limit] [charger]`, `report energy`.

## Configuration

| Variable | Used by | Meaning |
|---|---|---|
| `CHARGELINK_DB` | csms, report | libpq connection string |
| `CHARGELINK_TEST_DB` | tests | same, for the database tests (they skip without it) |

`csms.exe [port] [threads] [-v]`: defaults 9000 and one thread per core.

## Tests

```bat
scripts\check.bat
```

Database tests only touch rows whose ids start with `TEST_`.

## Known limits

- Expired or unknown tags are reported to chargers as `Blocked`.
- A charger that vanishes without closing its connection stays online until the idle timeout.
- Fast reconnects can briefly show the wrong `online` value.
- Database calls block the network threads (bounded by the pool of 4).
- No TLS, no authentication of chargers, no remote commands from the CSMS to chargers.
- libpqxx 8 deprecation warnings are silenced; the linker flag `/FORCE:MULTIPLE` works around a
  duplicate-symbol clash with the DLL import library.