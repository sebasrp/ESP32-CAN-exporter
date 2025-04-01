#include <Arduino.h>
#include "canbusble_pinout.h"

// Name used as prefix or name for BLE, WiFi, OTA hostname
#define DEVICE_ID "ESP32Can"
// Define the PIN requested by Bluetooth Pairing for bonding
#define BLE_SECURITY_PASS 123456

#include <TaskScheduler.h>
#include "CAN.h"
#include <NimBLEDevice.h>

// Scheduler for periodic tasks, from arkhipenko/TaskScheduler
Scheduler ts;

#define RX_TASK_PRIO 9

// BLE setup
#define BLE_SERVICE_UUID "6E400001-59f2-4a41-9acd-cd56fb435d64"
#define BLE_RAW_CAN_CHARACTERISTIC_UUID "6E400013-59f2-4a41-9acd-cd56fb435d64"
#define BLE_REBOOT_CHARACTERISTIC_UUID "6E400012-59f2-4a41-9acd-cd56fb435000"
#define BLE_DEVICE_ID_PREFIX DEVICE_ID

static NimBLEServer *pServer;
NimBLECharacteristic *pRawCanCharacteristic = NULL;
NimBLECharacteristic *pRebootCharacteristic = NULL;
bool BLEdeviceConnected = false;

// Status LED control
bool ledstatus = false;
void toggle_status_led()
{
  ledstatus = !ledstatus;
  digitalWrite(LED_STATUS, (ledstatus ? HIGH : LOW));
}
Task tLedBlink(1000, -1, &toggle_status_led, &ts, true);

// Structure to hold raw messages
#define BLE_INTERVAL 1000
#define MAX_CAN_MSG_SIZE 13 // ID(4) + DLC(1) + DATA(8)
typedef struct
{
  uint32_t id;     // CAN message ID
  uint8_t dlc;     // Data Length Code
  uint8_t data[8]; // CAN message data (up to 8 bytes)
} raw_can_message_t;

raw_can_message_t current_can_msg;
uint8_t ble_raw_can_buffer[MAX_CAN_MSG_SIZE];

// Send raw CAN message via BLE
void send_raw_can_message()
{
  if (BLEdeviceConnected && pRawCanCharacteristic != NULL)
  {
    // Pack the message into buffer (ID + DLC + DATA)
    memcpy(ble_raw_can_buffer, &current_can_msg.id, sizeof(current_can_msg.id));
    memcpy(ble_raw_can_buffer + sizeof(current_can_msg.id), &current_can_msg.dlc, sizeof(current_can_msg.dlc));
    memcpy(ble_raw_can_buffer + sizeof(current_can_msg.id) + sizeof(current_can_msg.dlc),
           current_can_msg.data, current_can_msg.dlc);

    // Send notification with the raw message
    pRawCanCharacteristic->setValue(ble_raw_can_buffer, sizeof(current_can_msg.id) + sizeof(current_can_msg.dlc) + current_can_msg.dlc);
    pRawCanCharacteristic->notify();
  }
}
Task tNotify_raw(BLE_INTERVAL, -1, &send_raw_can_message, &ts, false);

// CAN message receive callback
void onReceive(int packetSize)
{
  // Get message ID
  current_can_msg.id = CAN.packetId();
  current_can_msg.dlc = packetSize;

  // Read the data
  for (int i = 0; i < packetSize && i < 8; i++)
  {
    current_can_msg.data[i] = CAN.read();
  }

  // Forward via BLE immediately
  send_raw_can_message();
}

// BLE CallBacks
class ServerCallbacks : public NimBLEServerCallbacks
{
  void onConnect(NimBLEServer *pServer)
  {
    BLEdeviceConnected = true;
    log_i("Client connected");
  };

  void onConnect(NimBLEServer *pServer, NimBLEConnInfo &connInfo) override
  {
    BLEdeviceConnected = true;
    log_i("Client address: %s", connInfo.getAddress().toString().c_str());
  }

  void onDisconnect(NimBLEServer *pServer, NimBLEConnInfo &connInfo, int reason) override
  {
    BLEdeviceConnected = false;
    log_i("Client disconnected - start advertising");
    NimBLEDevice::startAdvertising();
  }
} serverCallbacks;

class CharacteristicCallbacks : public NimBLECharacteristicCallbacks
{
  void onSubscribe(NimBLECharacteristic *pCharacteristic, NimBLEConnInfo &connInfo, uint16_t subValue) override
  {
    String logmessage = "Client ID: ";
    logmessage += connInfo.getConnHandle();
    logmessage += " Address: ";
    logmessage += connInfo.getAddress().toString().c_str();

    if (subValue == 0)
    {
      logmessage += " Unsubscribed from ";
    }
    else if (subValue == 1)
    {
      logmessage += " Subscribed to notifications for ";
    }
    else if (subValue == 2)
    {
      logmessage += " Subscribed to indications for ";
    }
    else if (subValue == 3)
    {
      logmessage += " Subscribed to notifications and indications for ";
    }

    logmessage += std::string(pCharacteristic->getUUID()).c_str();
    log_i("Subscription update: %s", logmessage.c_str());
  };

  void onWrite(NimBLECharacteristic *pCharacteristic, ble_gap_conn_desc *desc)
  {
    log_i("Characteristic %s, written value: %s",
          pCharacteristic->getUUID().toString().c_str(),
          pCharacteristic->getValue().c_str());

    if (pCharacteristic->getUUID() == pRebootCharacteristic->getUUID())
    {
      log_e("Reboot command received");
      NimBLEDevice::getServer()->disconnect(desc->conn_handle);
      ESP.restart();
    }
  };
} chrCallbacks;

void setup()
{
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  log_e("Setting things up");

  // hold the device id to be used in broadcasting unit identifier strings
  const uint16_t chip = (uint16_t)((uint64_t)ESP.getEfuseMac() >> 32);

  // Generate device name based on mac address
  char ble_device_id[12];
  sprintf(ble_device_id, "%s-%04X", BLE_DEVICE_ID_PREFIX, chip);

  // Setup status LED
  gpio_reset_pin(LED_STATUS);
  gpio_set_direction(LED_STATUS, GPIO_MODE_OUTPUT);
  gpio_set_level(LED_STATUS, LOW);

  // Create the BLE Device
  NimBLEDevice::init(ble_device_id);
  NimBLEDevice::setPower(9); // Increase the transmit power
  NimBLEDevice::setSecurityAuth(true, true, true);
  NimBLEDevice::setSecurityPasskey(BLE_SECURITY_PASS);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);

  // Create the BLE Server
  pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(&serverCallbacks);
  pServer->advertiseOnDisconnect(true);

  // Create the BLE Service
  NimBLEService *pService = pServer->createService(BLE_SERVICE_UUID);

  // Create raw CAN message characteristic
  pRawCanCharacteristic = pService->createCharacteristic(
      BLE_RAW_CAN_CHARACTERISTIC_UUID,
      NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::NOTIFY);
  pRawCanCharacteristic->setCallbacks(&chrCallbacks);

  // Create reboot characteristic
  pRebootCharacteristic = pService->createCharacteristic(
      BLE_REBOOT_CHARACTERISTIC_UUID,
      NIMBLE_PROPERTY::WRITE_NR);
  pRebootCharacteristic->setCallbacks(&chrCallbacks);

  // Start the service
  pService->start();
  NimBLEAdvertising *pAdvertising = NimBLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(pService->getUUID());
  pAdvertising->enableScanResponse(true);
  pAdvertising->start();

  log_i("BLE server started, waiting for connections...");

  // Initialize CAN bus
  log_i("Setting up CANBUS");
  CAN.setPins(RX_GPIO_NUM, TX_GPIO_NUM);
  CAN.observe();

  // Start CAN bus at 500 kbps
  if (!CAN.begin(500E3))
  {
    log_e("Starting CAN failed!");
    // Blink rapidly to indicate error
    for (int i = 0; i < 10; i++)
    {
      digitalWrite(LED_STATUS, HIGH);
      delay(100);
      digitalWrite(LED_STATUS, LOW);
      delay(100);
    }
    ESP.restart();
  }
  else
  {
    log_i("Connected to CANBUS");
  }

  // Set up CAN message receive callback
  CAN.onReceive(onReceive);
  log_i("Setup complete - starting main loop");
}

void loop()
{
  ts.execute();
}
