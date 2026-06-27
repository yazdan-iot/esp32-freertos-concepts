# esp32-freertos-concepts 🔧

A hands-on ESP32 project that covers core FreeRTOS concepts in a single, cohesive codebase. The goal is not to build a product — it's to demonstrate practical, real-world usage of every major FreeRTOS primitive in one place.

---

## 🧩 What this project does

A multi-task sensor monitoring system running on ESP32 that:

- 📡 Reads sensor data every 500ms and pushes it to a processing pipeline
- 🧮 Calculates a rolling average temperature every 5 samples
- 🔘 Handles button input via ISR with deferred processing in task context
- 💤 Manages power save mode by suspending and resuming tasks on demand
- 🩺 Monitors system health (heap, stack, CPU load) and logs it periodically
- 🚀 Runs a one-shot calibration task at startup that self-deletes after completion
- 👁️ Observes kernel events (calibration done, I2C bus freed) via a Queue Set
- 🔒 Guards shared resources with mutexes, recursive mutexes, and critical sections

---

## 📚 FreeRTOS concepts demonstrated

### 🧵 Tasks

| Task | Core | Priority | Stack | Notes |
|---|---|---|---|---|
| `SensorTask` | 1 | 2 | 4096 | Producer — reads sensors, pushes to queue |
| `ProcessTask` | 1 | 2 | 4096 | Consumer — reads queue, computes average |
| `ButtonTask` | 1 | 3 | 4096 | Deferred ISR handler, power save control |
| `SystemTask` | 0 | 1 | 4096 | Health monitor, receives task notifications |
| `MonitorTask` | 0 | 1 | 2048 | Observes kernel events via Queue Set |
| `CalibrationTask` | 1 | 2 | 2048 | One-shot startup task, self-deletes |
| Idle Task | — | 0 | — | Increments counter used for CPU load estimate |
| Timer Service | — | — | — | Runs heartbeat and watchdog callbacks |

**Concepts covered:** `xTaskCreatePinnedToCore`, `xTaskCreateStaticPinnedToCore` (static allocation), `vTaskDelete(NULL)` (self-delete), `vTaskSuspend` / `vTaskResume` (power save), `vTaskDelay`, task priorities, dual-core pinning.

---

### 📬 Queue

```cpp
QueueHandle_t sensorQueue = xQueueCreate(5, sizeof(SensorData));
```

`SensorTask` produces `SensorData` structs (temp, sensor1, sensor2). `ProcessTask` consumes them. The queue depth of 5 acts as a buffer — if the consumer is slow, the producer blocks rather than dropping data.

**Concepts covered:** producer/consumer pattern, `xQueueSend`, `xQueueReceive`, blocking on full/empty queue.

---

### 🚦 Binary Semaphore (ISR → Task)

```cpp
// ISR context — must use FromISR variant
xSemaphoreGiveFromISR(buttonSem, &xHigherPriorityTaskWoken);
portYIELD_FROM_ISR(xHigherPriorityTaskWoken);

// Task context
xSemaphoreTake(buttonSem, portMAX_DELAY);
```

The button ISR runs in interrupt context where `Serial`, mutexes, and blocking calls are not allowed. It only gives the semaphore. `ButtonTask` does the actual work in safe task context.

**Concepts covered:** ISR-safe API (`FromISR` variants), `portYIELD_FROM_ISR`, deferred interrupt processing pattern.

---

### 🎟️ Counting Semaphore

```cpp
SemaphoreHandle_t i2cBusSemaphore = xSemaphoreCreateCounting(2, 2);
```

Limits concurrent I2C bus users to 2. Any task that wants to use the bus must acquire a slot first and release it when done. If both slots are taken, the task blocks until one is freed.

**Concepts covered:** resource pooling, `xSemaphoreCreateCounting`, rate-limiting concurrent access.

---

### 🔒 Mutex

```cpp
SemaphoreHandle_t serialMutex = xSemaphoreCreateMutex();

if (xSemaphoreTake(serialMutex, portMAX_DELAY)) {
    Serial.printf(...);
    xSemaphoreGive(serialMutex);
}
```

Multiple tasks write to Serial. Without a mutex the output interleaves into garbage. Every `Serial.printf` in every task is wrapped with take/give.

**Concepts covered:** mutual exclusion, protecting shared peripherals, mutex vs semaphore.

---

### 🔁 Recursive Mutex

```cpp
SemaphoreHandle_t logMutex = xSemaphoreCreateRecursiveMutex();
```

`logSystemEvent()` can call itself on the error path (when the message is empty, it logs that error recursively). A plain mutex would deadlock on re-entry — a recursive mutex tracks the re-entry count and allows the same task to take it multiple times, releasing only when the count returns to zero.

**Concepts covered:** re-entrant functions, deadlock prevention, `xSemaphoreTakeRecursive` / `xSemaphoreGiveRecursive`.

---

### 🏁 Event Group

```cpp
EventGroupHandle_t systemEvents = xEventGroupCreate();
#define SENSOR_READY  (1 << 0)
#define PROCESS_READY (1 << 1)

// In SensorTask and ProcessTask (first iteration only):
xEventGroupSetBits(systemEvents, SENSOR_READY);

// In SystemTask — blocks until BOTH bits are set:
xEventGroupWaitBits(systemEvents, SENSOR_READY | PROCESS_READY,
                    pdTRUE, pdTRUE, portMAX_DELAY);
```

`SystemTask` doesn't start reporting health until both pipeline tasks have confirmed they're running. The AND flag (`pdTRUE`) means it waits for all specified bits simultaneously.

**Concepts covered:** multi-task synchronization, AND/OR wait conditions, startup sequencing.

---

### 📨 Task Notifications

```cpp
// Sender (ButtonTask) — lighter than a semaphore for a single known target:
xTaskNotify(systemTaskHandle, (uint32_t)mode, eSetValueWithOverwrite);

// Receiver (SystemTask) — non-blocking poll each cycle:
xTaskNotifyWait(0, 0xFFFFFFFF, &notifiedMode, 0);
```

ButtonTask signals SystemTask directly on every mode change. Task Notifications are faster and cheaper than a semaphore or queue when the target task is known and a single 32-bit value is sufficient.

**Concepts covered:** direct-to-task signaling, `eSetValueWithOverwrite`, non-blocking notification check.

---

### 👀 Queue Set

```cpp
QueueSetHandle_t monitorQueueSet = xQueueCreateSet(2);
xQueueAddToSet(calibrationDoneSem, monitorQueueSet);
xQueueAddToSet(i2cBusSemaphore,    monitorQueueSet);

// MonitorTask — single blocking call, any member wakes it:
QueueSetMemberHandle_t active = xQueueSelectFromSet(monitorQueueSet, pdMS_TO_TICKS(5000));
```

`MonitorTask` needs to react to two different events without polling either one. A Queue Set lets a single task block on multiple queues/semaphores simultaneously and find out which one fired.

**Concepts covered:** multi-source blocking, `xQueueSelectFromSet`, observer pattern.

---

### ⏱️ Software Timers

```cpp
heartbeatTimer = xTimerCreate("Heartbeat", pdMS_TO_TICKS(2000), pdTRUE, NULL, heartbeatCallback);
watchdogTimer  = xTimerCreate("Watchdog",  pdMS_TO_TICKS(1000), pdTRUE, NULL, watchdogCallback);
```

- 💓 **Heartbeat** — logs "System Alive" every 2 seconds.
- 🐕 **Watchdog** — checks `lastActivity` every second. If no activity for 5 seconds and power save is off, logs a warning.

Callbacks run inside the Timer Service Task — they must not block.

**Concepts covered:** auto-reload timers, `xTimerStart`, timer callback constraints.

---

### ⚡ Critical Sections (Dual-Core Safe)

```cpp
portMUX_TYPE totalSamplesProcessedLock = portMUX_INITIALIZER_UNLOCKED;

taskENTER_CRITICAL(&totalSamplesProcessedLock);
totalSamplesProcessed++;
taskEXIT_CRITICAL(&totalSamplesProcessedLock);
```

On single-core FreeRTOS, `taskENTER_CRITICAL()` disables interrupts and is sufficient. On ESP32's dual-core architecture, a second core can still access the variable while interrupts on core 1 are disabled — `portMUX_TYPE` adds a spinlock that covers both cores.

**Concepts covered:** dual-core race conditions, `portMUX_TYPE`, when to use critical sections vs mutexes.

---

### 🗃️ Static Allocation

```cpp
StaticTask_t buttonTaskTCB;
StackType_t  buttonTaskStack[BUTTON_TASK_STACK_WORDS];

xTaskCreateStaticPinnedToCore(ButtonTask, "ButtonTask",
    BUTTON_TASK_STACK_WORDS, NULL, 3,
    buttonTaskStack, &buttonTaskTCB, 1);
```

`ButtonTask` uses pre-allocated TCB and stack arrays instead of heap. This eliminates heap fragmentation risk for a high-priority task and is the required approach in safety-critical systems.

**Concepts covered:** static vs dynamic allocation, `xTaskCreateStaticPinnedToCore`, heap fragmentation prevention.

---

### 😴 Idle Hook

```cpp
extern "C" void vApplicationIdleHook(void) {
    idleHookCounter++;
}
```

Runs every time the idle task executes (i.e., when no other task is ready). `SystemTask` reads the delta between cycles as a rough CPU load estimate — more idle ticks = less CPU usage.

**Concepts covered:** `vApplicationIdleHook`, CPU utilization estimation, idle task behavior.

---

### 💨 `volatile` keyword

Variables written by one task/ISR and read by another are declared `volatile` to prevent the compiler from caching them in a register:

```cpp
volatile uint32_t lastActivity = 0;
volatile bool     powerSaveActive = false;
volatile int      mode = 0;
```

**Concepts covered:** compiler optimization vs shared memory, when `volatile` is necessary.

---

## 📁 Project structure

```
esp32-freertos-concepts/
├── src/
│   └── main.cpp      # All source code
├── platformio.ini    # PlatformIO build config
└── README.md
```

---

## ⚙️ Build & flash

Built with [PlatformIO](https://platformio.org/) targeting ESP32.

```bash
pio run --target upload
pio device monitor --baud 115200
```

---

## 🔌 Hardware

- ESP32 devkit (any variant)
- Tactile button on GPIO 0 with pull-up (or use the built-in BOOT button)
- No other hardware required — sensor reads are simulated with `random()`

To use real sensors, replace the `random()` calls in `SensorTask` with your driver reads over the same I2C bus that `useI2CBus()` already guards.
