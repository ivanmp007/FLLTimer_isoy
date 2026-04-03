# Guía de Hardware — FLL Timer con LEDs ESP32

## Visión general del sistema

El sistema usa una red de nodos ESP32 comunicados por **ESP-NOW** (Wi-Fi sin router, broadcast).  
Todos los dispositivos ejecutan el **mismo firmware** (`firmware/firmware.ino`).

```
[PC / Browser]
      |  USB Serial
      v
[ESP32 MAESTRO]  ←──ESP-NOW broadcast──→  [ESP32 MESA A]
                                      ←──ESP-NOW broadcast──→  [ESP32 MESA B]
                                      ←──ESP-NOW broadcast──→  [ESP32 ARRANCADOR]
```

---

## Tipos de dispositivo

| Dispositivo | Descripción | Diferencia hardware |
|---|---|---|
| **Maestro** | Conectado al PC por USB. Recibe órdenes de la web y coordina la red. | Solo cable USB al PC |
| **Mesa** | Muestra el estado de preparación de un equipo en su tira LED. | Tira LED WS2812B + pulsador de árbitro |
| **Arrancador** | LED indicador muestra si se puede iniciar. Pulsador arranca la partida. | 1× LED WS2812B en GPIO 17 + pulsador de inicio (GPIO 22) + compilar con `#define STARTER_MODE` |

> El dispositivo **Arrancador** usa el mismo ESP32 que una Mesa. Añade un pulsador en GPIO 22 y **un único LED WS2812B en GPIO 17** como indicador de inicio. La única diferencia de firmware es la línea `#define STARTER_MODE`.

---

## Pinout — Todas las placas

### Pines compartidos (todos los dispositivos)

| Pin GPIO | Función | Dirección | Notas |
|---|---|---|---|
| **16** | Tira LED WS2812B (`LED_PIN`) | Salida | Data de la tira NeoPixel/WS2812B |
| **23** | Pulsador de árbitro (`BUTTON_PIN`) | Entrada | `INPUT_PULLUP` — conectar a GND al pulsar |

### Pines adicionales — Solo dispositivo Arrancador

| Pin GPIO | Función | Dirección | Notas |
|---|---|---|---|
| **17** | LED indicador WS2812B (`STARTER_IND_PIN`) | Salida | 1× LED WS2812B individual |
| **22** | Pulsador de inicio de partida (`STARTER_BUTTON_PIN`) | Entrada | `INPUT_PULLUP` — conectar a GND al pulsar |

---

## Esquema de conexión

### Tira LED WS2812B (todos los dispositivos)

```
ESP32 3.3V/5V ─── VCC (tira LED)
ESP32 GND     ─── GND (tira LED)
ESP32 GPIO 16 ─── DIN (data entrada tira LED)
                  (usar resistencia de 330 Ω en serie con DIN recomendada)
```

> **Alimentación:** Las tiras de muchos LEDs consumen más corriente de la que puede dar el ESP32.  
> Usar fuente 5V externa para la tira. Conectar GND externo con GND del ESP32.

### LED indicador del Arrancador (GPIO 17, solo Arrancador)

```
ESP32 GPIO 17 ─── [330 Ω] ─── DIN del LED WS2812B individual
ESP32 5V      ─── VCC del LED
ESP32 GND     ─── GND del LED
```
*(Un único LED WS2812B — puede ser un trozo de 1 píxel cortado de una tira)*

### Pulsador de árbitro (GPIO 23)

```
ESP32 GPIO 23 ─── Pulsador ─── GND
```
*(Pull-up interno activado. Sin pulsar = HIGH, pulsado = LOW)*

### Pulsador de inicio (GPIO 22, solo Arrancador)

```
ESP32 GPIO 22 ─── Pulsador ─── GND
```
*(Pull-up interno activado. Sin pulsar = HIGH, pulsado = LOW)*

> El Arrancador también tiene la **tira principal WS2812B** (GPIO 16) que muestra los mismos efectos visuales que cualquier nodo Mesa.

---

## Comportamiento del LED indicador (GPIO 17, Arrancador con `STARTER_MODE`)

El LED individual en GPIO 17 actúa como indicador de inicio:

| Color LED | Condición |
|---|---|
| **Apagado** | Estado por defecto — no se puede iniciar aún |
| **Amarillo fijo** | Se puede iniciar la partida (`canStart = true`) |
| **Apagado** | La partida ha comenzado (`PARTIDA` o `FINAL`) |

### Ciclo de prueba (botón de árbitro GPIO 23)

Cada pulsación corta del botón de árbitro (GPIO 23) avanza un ciclo de colores de diagnóstico **en la tira principal** (GPIO 16):

**Apagado → Amarillo → Naranja → Verde → Apagado → …**

Esto permite verificar la tira principal antes de la competición. El LED indicador (GPIO 17) no se ve afectado por este ciclo.

Cuando llega la señal `canStart = true`, el LED indicador se pone **amarillo fijo** independientemente del estado de la tira principal.

---

## Comportamiento del pulsador de inicio (GPIO 22, Arrancador)

1. La tira LED muestra **amarillo fijo** → se puede iniciar.
2. Se pulsa el botón (GPIO 22).
3. Si el Arrancador **es el maestro** (conectado al PC): notifica directamente a la web (`start_request_rx` por Serial).
4. Si **no es el maestro**: envía paquete `PKT_START_REQUEST` por ESP-NOW al maestro, que lo reenvía a la web.
5. La web dispara el inicio de la partida exactamente igual que si se hubiera pulsado el botón "Inicio" en pantalla.

> Si la tira no está en amarillo (`canStart = false`), el pulsador de inicio es ignorado.

---

## Comportamiento del pulsador de árbitro (GPIO 23)

| Pulsación | Estado actual | Transición |
|---|---|---|
| Corta | REPOSO | → PREPARACION |
| Corta | PREPARACION | → READY |
| Corta | MUST_GO | → READY |
| Corta | READY | → PREPARACION (o MUST_GO si ya pasó el tiempo) |
| Larga (>800 ms) | Cualquiera | → REPOSO |

---

## Estados de los nodos

| Valor | Estado | Color LED tira | Descripción |
|---|---|---|---|
| 0 | REPOSO | Apagado | En espera |
| 1 | PREPARACION | Amarillo | Árbitro preparando equipo |
| 2 | MUST_GO | Amarillo parpadeo | Tiempo de preparación agotado |
| 3 | READY | Verde | Listo para iniciar |
| 4 | PARTIDA | Plasma animado | Partida en curso |
| 5 | FINAL | Rojo | Tiempo terminado |

---

## Instalación del firmware

### Dependencias Arduino (Gestor de librerías)

- **FastLED** ≥ 3.6
- **ArduinoJson** ≥ 6.x
- **ESP32 Arduino Core** ≥ 3.x (Board: `ESP32 Dev Module`)

### Configuración de placa (Arduino IDE)

- **Board:** `ESP32 Dev Module`
- **Upload Speed:** `921600`
- **CPU Frequency:** `240 MHz`
- **Flash Size:** `4MB`
- **Partition Scheme:** `Default 4MB with spiffs`

### Pasos

1. Abrir `firmware/firmware.ino` en Arduino IDE.
2. Para **dispositivos Mesa y Maestro**: dejar la línea `// #define STARTER_MODE` comentada (es el valor por defecto).
3. Para el **dispositivo Arrancador**: descomentar `#define STARTER_MODE` al inicio del archivo.
4. Conectar el ESP32 por USB.
5. Seleccionar el puerto COM correcto.
6. Compilar y subir.
7. Repetir para cada dispositivo, ajustando el paso 2/3 según el rol.

---

## Identificación de dispositivos

Al arrancar, cada ESP32 imprime por Serial:

```
EFUSE_MAC=XX:XX:XX:XX:XX:XX
SELF_ID=XXXXXXXX
```

El `SELF_ID` es un identificador único de 8 dígitos hexadecimales derivado del MAC del chip.  
Se usa en la web para asignar nombres a cada nodo (sección "Configuración ESP32").

---

## Topología de red

- Protocolo: **ESP-NOW** (IEEE 802.11 broadcast, sin router)
- Canal Wi-Fi: heredado del modo STA (por defecto canal 1 o el del AP más próximo)
- Alcance típico: 50–100 m en línea de visión
- Latencia: < 5 ms
- No requiere acceso a internet ni red Wi-Fi

### El maestro

El ESP32 conectado al PC reclama el rol de maestro enviando `claim_master` por Serial (la web lo hace automáticamente al conectar).

Solo el maestro:
- Responde al estado completo de la red via Serial → Browser
- Envía `PKT_CAN_START` broadcasts periódicos (cada 2 s)
- Procesa `PKT_START_REQUEST` y notifica a la web
- Reenvía comandos `set_all` / `set_node` de la web

---

## Lista de materiales sugerida

### Por dispositivo Mesa / Maestro

| Componente | Cantidad |
|---|---|
| ESP32 Dev Module (30 pines) | 1 |
| Tira LED WS2812B (densidad según necesidad) | 1 |
| Pulsador táctil o de armario | 1 |
| Resistencia 330 Ω (en serie con DIN) | 1 |
| Fuente 5V (si tira > 5 LEDs) | 1 |
| Caja/soporte | 1 |

### Dispositivo Arrancador

Mismos componentes que una Mesa, más:

| Componente adicional | Cantidad |
|---|---|
| Pulsador grande / de botón (para inicio de partida) | 1 |

> No se necesita ningún LED externo adicional. La tira WS2812B existente hace de indicador.

---

## Resumen de cambios de firmware respecto a la versión base

| Identificador | Descripción |
|---|---|
| `#define STARTER_MODE` | Descomenta para compilar el Arrancador; comentado = Mesa/Maestro |
| `STARTER_BUTTON_PIN 22` | Pin del pulsador de inicio (pull-up, activo LOW) |
| `PKT_CAN_START = 5` | Nuevo tipo de paquete ESP-NOW: bandera canStart (maestro → todos) |
| `PKT_START_REQUEST = 6` | Nuevo tipo de paquete ESP-NOW: solicitud de inicio (arrancador → maestro) |
| `canStartFlag` | Variable local que almacena si se puede iniciar |
| `starterColorIdx` | Índice del ciclo de colores de prueba (solo `STARTER_MODE`) |
| `showStarterVisual()` | Aplica el color correcto a la tira WS2812B del arrancador |
| `handleShortPress()` | En `STARTER_MODE`: avanza ciclo de prueba en lugar de cambiar estado del nodo |
| `applyLocalVisuals()` | En `STARTER_MODE`: siempre llama a `showStarterVisual()` |
| `updateStarterButton()` | Gestiona el debounce y acción del pulsador de inicio |
| `updateCanStartHeartbeat()` | El maestro re-difunde `canStartFlag` cada 2 s |
| Comando serial `set_can_start` | La web notifica al firmware el valor de canStart |
| Evento serial `start_request_rx` | El firmware notifica a la web que se pulsó el arrancador |
