#include <Arduino.h>

// Multi-task sensor monitoring system for ESP32.
// Demonstrates: Tasks (dynamic+static), Queues, Queue Sets, Mutex (regular+recursive),
// Binary/Counting Semaphores, Event Groups, Task Notifications, Software Timers,
// Critical Sections, Idle Hook, and Task suspend/resume/delete.

// ---------- Timers ----------
TimerHandle_t heartbeatTimer;
TimerHandle_t watchdogTimer;

// Written by SensorTask/ProcessTask, read by watchdog — volatile to prevent register caching.
volatile uint32_t lastActivity = 0;

// ---------- Queue: Sensor -> Process ----------
QueueHandle_t sensorQueue;
struct SensorData {
  float temp;
  int sensor1;
  int sensor2;
};

// ---------- Synchronization ----------
SemaphoreHandle_t serialMutex;        // protects Serial.print* across tasks
SemaphoreHandle_t buttonSem;          // binary semaphore signalled from ISR

// Counting semaphore: caps concurrent I2C bus users at 2.
#define I2C_BUS_SLOTS 2
SemaphoreHandle_t i2cBusSemaphore;

// Recursive mutex: logSystemEvent() can call itself on the error path,
// so a plain mutex would deadlock — recursive one tracks re-entry count instead.
SemaphoreHandle_t logMutex;

// Queue Set: lets MonitorTask block on both calibrationDoneSem and i2cBusSemaphore at once.
// sensorQueue is intentionally excluded — it's read directly by ProcessTask only.
QueueSetHandle_t monitorQueueSet;
SemaphoreHandle_t calibrationDoneSem;

// ---------- Event Group: startup sync ----------
EventGroupHandle_t systemEvents;
#define SENSOR_READY  (1 << 0)
#define PROCESS_READY (1 << 1)

// Written and read by ButtonTask; volatile for future cross-task reads.
volatile int mode = 0;

// Shared sample counter — protected by critical section (not mutex) because
// the operation is a single increment, too short to justify mutex overhead.
volatile uint32_t totalSamplesProcessed = 0;

// ESP32 dual-core spinlock: plain taskENTER_CRITICAL() only guards one core.
portMUX_TYPE totalSamplesProcessedLock = portMUX_INITIALIZER_UNLOCKED;

// Incremented in idle hook; SystemTask uses the delta as a CPU load estimate.
volatile uint32_t idleHookCounter = 0;

TaskHandle_t sensorTaskHandle  = NULL;
TaskHandle_t processTaskHandle = NULL;
TaskHandle_t systemTaskHandle  = NULL;

// Toggled on every 3rd button press; suspends SensorTask/ProcessTask to save power.
// Suspend (not delete) preserves their internal state (e.g. running average).
volatile bool powerSaveActive = false;

// ---------- Static allocation for ButtonTask ----------
// Static = TCB + stack from fixed arrays, not heap — no fragmentation risk.
#define BUTTON_TASK_STACK_WORDS 4096
StaticTask_t buttonTaskTCB;
StackType_t  buttonTaskStack[BUTTON_TASK_STACK_WORDS];

// ISR: signals semaphore and yields. No Serial, no heavy logic — must return fast.
void IRAM_ATTR buttonISR() {
  BaseType_t xHigherPriorityTaskWoken = pdFALSE;
  xSemaphoreGiveFromISR(buttonSem, &xHigherPriorityTaskWoken);
  portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

// Recursive-mutex-protected logger. Re-entrant: if message is empty,
// calls itself once to log that error — a plain mutex would deadlock here.
void logSystemEvent(const char *message, bool isReentrantErrorPath = false) {
  if (xSemaphoreTakeRecursive(logMutex, portMAX_DELAY) == pdTRUE) {
    Serial.printf("[LOG] %s\n", message);

    if (!isReentrantErrorPath && message[0] == '\0') {
      logSystemEvent("LOG ERROR: previous message was empty", true);
    }

    xSemaphoreGiveRecursive(logMutex);
  }
}

// Acquires one I2C bus slot (counting semaphore) before simulating a transaction.
void useI2CBus(const char *taskName, int durationMs) {
  if (xSemaphoreTake(i2cBusSemaphore, pdMS_TO_TICKS(2000)) == pdTRUE) {
    logSystemEvent(taskName);
    vTaskDelay(pdMS_TO_TICKS(durationMs));
    xSemaphoreGive(i2cBusSemaphore);
  } else {
    logSystemEvent("I2C bus busy, transaction skipped");
  }
}

// Deferred interrupt handler: does the real button work in task context
// where Serial, mutexes, etc. are safe to use.
// Every 3rd press toggles power-save (suspend/resume the sensor pipeline).
void ButtonTask(void *pvParameters) {
  while (true) {
    if (xSemaphoreTake(buttonSem, portMAX_DELAY)) {
      mode += 1;
      if (mode > 2) mode = 0;

      logSystemEvent("Mode changed");

      // Task Notification: lighter than a semaphore/queue for one specific target task.
      if (systemTaskHandle != NULL) {
        xTaskNotify(systemTaskHandle, (uint32_t)mode, eSetValueWithOverwrite);
      }

      if (mode == 0) {
        powerSaveActive = !powerSaveActive;

        if (powerSaveActive) {
          logSystemEvent("Entering power-save: suspending sensor pipeline");
          if (sensorTaskHandle != NULL)  vTaskSuspend(sensorTaskHandle);
          if (processTaskHandle != NULL) vTaskSuspend(processTaskHandle);
        } else {
          logSystemEvent("Resuming sensor pipeline");
          if (sensorTaskHandle != NULL)  vTaskResume(sensorTaskHandle);
          if (processTaskHandle != NULL) vTaskResume(processTaskHandle);
        }
      }
    }
  }
}

// One-shot task: runs calibration at startup, then deletes itself to free resources.
void CalibrationTask(void *pvParameters) {
  logSystemEvent("Calibration started");
  vTaskDelay(pdMS_TO_TICKS(800));

  useI2CBus("CalibrationTask", 50);

  logSystemEvent("Calibration complete, task deleting itself");
  xSemaphoreGive(calibrationDoneSem);

  vTaskDelete(NULL);
}

// Timer callbacks run in the Timer Service Task context — keep them short.
void heartbeatCallback(TimerHandle_t xTimer) {
  logSystemEvent("System Alive");
}

void watchdogCallback(TimerHandle_t xTimer) {
  if (millis() - lastActivity > 5000 && !powerSaveActive) {
    logSystemEvent("WATCHDOG: System stuck!");
  }
}

// Producer: reads sensors every 500ms, pushes data onto sensorQueue.
// Swap random() calls with real driver reads for actual hardware use.
void SensorTask(void *pvParameters) {
  SensorData data;
  bool firstRunDone = false;

  while (true) {
    useI2CBus("SensorTask", 20);

    data.temp    = random(250, 351) / 10.0;
    data.sensor1 = random(0, 100);
    data.sensor2 = random(0, 100);

    xQueueSend(sensorQueue, &data, portMAX_DELAY);

    // Critical section (not mutex): single-instruction shared counter increment.
    taskENTER_CRITICAL(&totalSamplesProcessedLock);
    totalSamplesProcessed += 1;
    taskEXIT_CRITICAL(&totalSamplesProcessedLock);

    if (xSemaphoreTake(serialMutex, portMAX_DELAY)) {
      Serial.printf("Temp: %.1f  S1:%d  S2:%d\n", data.temp, data.sensor1, data.sensor2);
      xSemaphoreGive(serialMutex);
    }

    lastActivity = millis();

    if (!firstRunDone) {
      xEventGroupSetBits(systemEvents, SENSOR_READY);
      firstRunDone = true;
    }

    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

// Consumer: blocks on queue, accumulates 5 samples, prints average temp.
void ProcessTask(void *pvParameters) {
  SensorData receivedData;
  int   count   = 0;
  float tempSum = 0;
  bool  firstRunDone = false;

  while (true) {
    if (xQueueReceive(sensorQueue, &receivedData, portMAX_DELAY)) {
      tempSum += receivedData.temp;
      count += 1;

      taskENTER_CRITICAL(&totalSamplesProcessedLock);
      totalSamplesProcessed += 1;
      taskEXIT_CRITICAL(&totalSamplesProcessedLock);

      if (count == 5) {
        float average = tempSum / count;

        if (xSemaphoreTake(serialMutex, portMAX_DELAY)) {
          Serial.printf("Average Temp = %.2f C\n", average);
          xSemaphoreGive(serialMutex);
        }

        tempSum = 0;
        count   = 0;
      }

      lastActivity = millis();

      if (!firstRunDone) {
        xEventGroupSetBits(systemEvents, PROCESS_READY);
        firstRunDone = true;
      }
    }
  }
}

// Blocks on a Queue Set containing calibrationDoneSem and i2cBusSemaphore,
// and logs whichever fires — without polling either one individually.
void MonitorTask(void *pvParameters) {
  while (true) {
    QueueSetMemberHandle_t activeMember = xQueueSelectFromSet(monitorQueueSet, pdMS_TO_TICKS(5000));

    if (activeMember == calibrationDoneSem) {
      xSemaphoreTake(calibrationDoneSem, 0); // consume it
      logSystemEvent("MonitorTask observed: calibration finished");
    } else if (activeMember == i2cBusSemaphore) {
      logSystemEvent("MonitorTask observed: an I2C bus slot freed up");
    }
    // activeMember == NULL: 5s timeout, nothing ready — loop normally.
  }
}

// Waits for both tasks to be ready, then runs periodic health reports.
// Also receives Task Notifications from ButtonTask on mode change.
void SystemTask(void *pvParameters) {
  xEventGroupWaitBits(systemEvents, SENSOR_READY | PROCESS_READY, pdTRUE, pdTRUE, portMAX_DELAY);

  logSystemEvent("SYSTEM READY!");

  uint32_t lastIdleCount = 0;

  while (true) {
    uint32_t notifiedMode;
    // Non-blocking check for mode-change notification from ButtonTask.
    if (xTaskNotifyWait(0, 0xFFFFFFFF, &notifiedMode, 0) == pdTRUE) {
      Serial.printf("[SYSTEM] Notified of mode change: %u\n", notifiedMode);
    }

    uint32_t   heap      = ESP.getFreeHeap();
    UBaseType_t stack    = uxTaskGetStackHighWaterMark(NULL);
    uint32_t   idleDelta = idleHookCounter - lastIdleCount;
    lastIdleCount        = idleHookCounter;

    if (xSemaphoreTake(serialMutex, portMAX_DELAY)) {
      Serial.printf("[SYSTEM] Heap: %u bytes | Stack: %u | IdleTicks: %u | Samples: %u\n",
                    heap, stack, idleDelta, totalSamplesProcessed);
      xSemaphoreGive(serialMutex);
    }

    if (heap < 50000) {
      logSystemEvent("[WARNING] Low Heap!");
    }

    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}

// Stack sizes and priorities in one place — avoids magic numbers at call sites.
namespace TaskConfig {
  constexpr uint32_t SENSOR_STACK      = 4096;
  constexpr uint32_t PROCESS_STACK     = 4096;
  constexpr uint32_t SYSTEM_STACK      = 4096;
  constexpr uint32_t MONITOR_STACK     = 2048;
  constexpr uint32_t CALIBRATION_STACK = 2048;

  constexpr UBaseType_t SENSOR_PRIORITY      = 2;
  constexpr UBaseType_t PROCESS_PRIORITY     = 2;
  constexpr UBaseType_t BUTTON_PRIORITY      = 3;
  constexpr UBaseType_t SYSTEM_PRIORITY      = 1;
  constexpr UBaseType_t MONITOR_PRIORITY     = 1;
  constexpr UBaseType_t CALIBRATION_PRIORITY = 2;
}

// Idle hook: must never block. Just increments a counter for CPU load estimation.
extern "C" void vApplicationIdleHook(void) {
  idleHookCounter += 1;
}

void setup() {
  Serial.begin(115200);

  // Seed RNG and create all kernel objects BEFORE any task starts.
  randomSeed(micros());

  sensorQueue        = xQueueCreate(5, sizeof(SensorData));
  serialMutex        = xSemaphoreCreateMutex();
  buttonSem          = xSemaphoreCreateBinary();
  systemEvents       = xEventGroupCreate();
  logMutex           = xSemaphoreCreateRecursiveMutex();
  i2cBusSemaphore    = xSemaphoreCreateCounting(I2C_BUS_SLOTS, I2C_BUS_SLOTS);
  calibrationDoneSem = xSemaphoreCreateBinary();

  if (!sensorQueue || !serialMutex || !buttonSem || !systemEvents || !logMutex || !i2cBusSemaphore || !calibrationDoneSem) {
    Serial.println("FATAL: failed to create a kernel object, halting.");
    while (true) { delay(1000); }
  }

  // Queue Set capacity must cover total slots of all members; add members before any task blocks on it.
  monitorQueueSet = xQueueCreateSet(2);
  BaseType_t r1 = xQueueAddToSet(calibrationDoneSem, monitorQueueSet);
  BaseType_t r2 = xQueueAddToSet(i2cBusSemaphore, monitorQueueSet);

  if (!monitorQueueSet || r1 != pdPASS || r2 != pdPASS) {
    Serial.println("FATAL");
  }

  pinMode(0, INPUT_PULLUP);
  attachInterrupt(0, buttonISR, FALLING);

  BaseType_t ok = pdPASS;
  ok &= xTaskCreatePinnedToCore(SensorTask, "SensorTask", TaskConfig::SENSOR_STACK, NULL, TaskConfig::SENSOR_PRIORITY, &sensorTaskHandle, 1);
  ok &= xTaskCreatePinnedToCore(ProcessTask, "ProcessTask", TaskConfig::PROCESS_STACK, NULL, TaskConfig::PROCESS_PRIORITY, &processTaskHandle, 1);
  ok &= xTaskCreatePinnedToCore(SystemTask, "SystemTask", TaskConfig::SYSTEM_STACK, NULL, TaskConfig::SYSTEM_PRIORITY, &systemTaskHandle, 0);
  ok &= xTaskCreatePinnedToCore(MonitorTask, "MonitorTask", TaskConfig::MONITOR_STACK, NULL, TaskConfig::MONITOR_PRIORITY,  NULL, 0);
  ok &= xTaskCreatePinnedToCore(CalibrationTask, "CalibrationTask", TaskConfig::CALIBRATION_STACK, NULL, TaskConfig::CALIBRATION_PRIORITY,  NULL, 1);

  // ButtonTask: statically allocated — TCB and stack from global arrays, not heap.
  TaskHandle_t buttonTaskHandle = xTaskCreateStaticPinnedToCore(ButtonTask, "ButtonTask", BUTTON_TASK_STACK_WORDS, NULL, TaskConfig::BUTTON_PRIORITY, buttonTaskStack, &buttonTaskTCB, 1);

  if (ok != pdPASS || buttonTaskHandle == NULL) {
    Serial.println("FATAL: failed to create one or more tasks, halting.");
    while (true) { delay(1000); }
  }

  heartbeatTimer = xTimerCreate("Heartbeat", pdMS_TO_TICKS(2000), pdTRUE, NULL, heartbeatCallback);
  watchdogTimer  = xTimerCreate("Watchdog",  pdMS_TO_TICKS(1000), pdTRUE, NULL, watchdogCallback);

  xTimerStart(heartbeatTimer, 0);
  xTimerStart(watchdogTimer,  0);
}

void loop() {
  // Intentionally empty — all work runs in FreeRTOS tasks.
}