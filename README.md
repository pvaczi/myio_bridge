# MyIO Bridge – Visual Studio projekt beállítás

## Mappastruktúra

```
MyIO/                          ← Solution mappa
├── MyIO.sln
├── common/                    ← Megosztott kód (Static Library)
│   ├── myio/
│   │   ├── types.hpp
│   │   ├── IMyIOClient.hpp
│   │   ├── dsens_parser.hpp
│   │   ├── dsens_parser.cpp
│   │   ├── http_client.hpp
│   │   └── http_client.cpp
│   ├── ha/
│   │   └── IHAPublisher.hpp
│   ├── config/
│   │   └── config_types.hpp
│   └── boost_json.cpp
├── discovery/                 ← Console App (szervizprogram)
│   └── main.cpp
└── bridge/                    ← Console App (híd)
    ├── myio/
    │   ├── async_http_client.hpp
    │   └── async_http_client.cpp
    ├── ha/
    │   ├── mqtt5_publisher.hpp
    │   └── mqtt5_publisher.cpp
    ├── config/
    │   ├── config_reader.hpp
    │   └── config_reader.cpp
    ├── bridge.hpp
    ├── bridge.cpp
    ├── main.cpp
    └── boost_json.cpp
```

---

## Visual Studio Solution létrehozása

### 1. Új Solution

File → New → Project → **Blank Solution**
- Name: `MyIO`
- Location: a `MyIO/` mappa szülőkönyvtára

---

### 2. `myio_common` projekt (Static Library)

Add → New Project → **Static Library (C++)**
- Name: `myio_common`
- Location: `MyIO/common/`

Fájlok hozzáadása (Add → Existing Item):
- `common/myio/dsens_parser.cpp`
- `common/myio/http_client.cpp`
- `common/boost_json.cpp`

Header fájlokat is add hozzá (nem fordulnak, de láthatók lesznek):
- `common/myio/*.hpp`
- `common/ha/*.hpp`
- `common/config/*.hpp`

**Project Properties** (minden konfigurációra):

| Beállítás | Érték |
|---|---|
| C/C++ → Language Standard | **ISO C++23 Standard (/std:c++latest)** |
| C/C++ → Additional Include Dirs | `$(SolutionDir)common;$(BOOST_ROOT)` |
| C/C++ → Character Set | **Use Unicode Character Set** |

---

### 3. `myio_discovery` projekt (Console App)

Add → New Project → **Console App (C++)**
- Name: `myio_discovery`
- Location: `MyIO/discovery/`

Fájlok hozzáadása:
- `discovery/main.cpp`

**Project → References → Add Reference → myio_common** ✓

**Project Properties**:

| Beállítás | Érték |
|---|---|
| C/C++ → Language Standard | **ISO C++23 Standard (/std:c++latest)** |
| C/C++ → Additional Include Dirs | `$(SolutionDir)common;$(BOOST_ROOT)` |
| Linker → Additional Deps | `$(BOOST_ROOT)\lib\...` (vcpkg kezeli) |

---

### 4. `myio_bridge` projekt (Console App)

Add → New Project → **Console App (C++)**
- Name: `myio_bridge`
- Location: `MyIO/bridge/`

Fájlok hozzáadása:
- `bridge/myio/async_http_client.cpp`
- `bridge/ha/mqtt5_publisher.cpp`
- `bridge/config/config_reader.cpp`
- `bridge/bridge.cpp`
- `bridge/main.cpp`
- `bridge/boost_json.cpp`

Header fájlok:
- `bridge/myio/*.hpp`
- `bridge/ha/*.hpp`
- `bridge/config/*.hpp`
- `bridge/bridge.hpp`

**Project → References → Add Reference → myio_common** ✓

**Project Properties**:

| Beállítás | Érték |
|---|---|
| C/C++ → Language Standard | **ISO C++23 Standard (/std:c++latest)** |
| C/C++ → Additional Include Dirs | `$(SolutionDir)common;$(SolutionDir)bridge;$(BOOST_ROOT)` |
| C/C++ → Conformance Mode | **Yes (/permissive-)** |

> ⚠️ A bridge-nek `$(SolutionDir)bridge` is kell include pathként,
> mert pl. `bridge/myio/async_http_client.hpp` tartalmaz `#include "myio/async_http_client.hpp"`-t.

---

### 5. Boost és vcpkg

```
vcpkg install boost-beast boost-json boost-asio boost-system boost-mqtt5
```

Ha vcpkg integrált (vcpkg integrate install), automatikusan beállítja a VS projekteket.

---

### 6. Build sorrend

A VS automatikusan kezeli, ha a References be vannak állítva:
1. `myio_common` (először)
2. `myio_discovery` és `myio_bridge` (párhuzamosan)

---

### 7. Futtatás

**discovery:**
```
myio_discovery.exe discovery_input.json bridge.json
```

**bridge:**
```
myio_bridge.exe bridge.json
```

---

## Fontos: boost_json.cpp

Mindkét projektben (`common` és `bridge`) van egy `boost_json.cpp` fájl.
Ez szándékos – a Boost.JSON implementáció (`#include <boost/json/src.hpp>`)
csak **egy** fordítási egységben szerepelhet projektenként.
A `common` projektben lévő gondoskodik a discovery-ról,
a `bridge` projektben lévő a bridge-ről.
