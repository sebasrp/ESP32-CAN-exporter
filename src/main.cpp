#include <Arduino.h>

// Name used as prefix or name for BLE, WiFi, OTA hostname
#define DEVICE_ID "ESP32Can"

// Define the PIN requested by Bluetooth Pairing for bonding
#define BLE_SECURITY_PASS 123456

// Fast update frequency
#define FAST_MESSAGES_FREQUENCY_HZ 25

// Scheduler for periodic tasks, from arkhipenko/TaskScheduler
#include <TaskScheduler.h>
Scheduler ts;

// BLE setup
#include <NimBLEDevice.h>
#define BLE_ENGINEDATA_SERVICE_UUID "dcfcad04-aee4-4a00-b97e-b49fb12a8480"
#define BLE_SLOW_ENGINEDATA_CHARACTERISTIC_UUID "5b50e778-9b9e-427d-ba76-2ec4b063bd4c"
#define BLE_FAST_ENGINEDATA_CHARACTERISTIC_UUID "c6bfeea2-1266-40ef-8191-3c6542d21c1f"
#define BLE_REBOOT_CHARACTERISTIC_UUID "73b08ee0-66ec-41d8-9c28-3f9e5dbef024"
#define BLE_DEVICE_ID_PREFIX DEVICE_ID

static NimBLEServer *pServer;
NimBLECharacteristic *pFastCharacteristic = NULL;
NimBLECharacteristic *pSlowCharacteristic = NULL;
NimBLECharacteristic *pRebootCharacteristic = NULL;
bool BLEdeviceConnected = false;

// BLE CallBacks
class ServerCallbacks : public NimBLEServerCallbacks
{
  void onConnect(NimBLEServer *pServer)
  {
    BLEdeviceConnected = true;
    log_i("Client connected");
    NimBLEDevice::startAdvertising();
  };

  void onConnect(NimBLEServer *pServer, NimBLEConnInfo &connInfo) override
  {
    log_i("Client address: %s", connInfo.getAddress().toString().c_str());
    NimBLEDevice::startAdvertising();
    onConnect(pServer);
  };

  void onDisconnect(NimBLEServer *pServer, NimBLEConnInfo &connInfo, int reason) override
  {
    BLEdeviceConnected = false;
    log_i("Client disconnected - start advertising");
  }
} serverCallbacks;

class CharacteristicCallbacks : public NimBLECharacteristicCallbacks
{
  void onSubscribe(NimBLECharacteristic *pCharacteristic, NimBLEConnInfo &connInfo, uint16_t subValue) override
  {

    // Start notifications when a client subscribes
    log_i("Initialize message version variables: FAST %d, SLOW %d", message_fast.msgtype, message_slow.msgtype);
  };

  void onWrite(NimBLECharacteristic *pCharacteristic, ble_gap_conn_desc *desc)
  {
    log_i("Characteristic %s, written value: %d", pCharacteristic->getUUID().toString().c_str(), pCharacteristic->getValue().c_str());
    if (pCharacteristic->getUUID() == pRebootCharacteristic->getUUID())
    {
      log_e("reboot");
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
  NimBLEService *pService = pServer->createService(BLE_ENGINEDATA_SERVICE_UUID);

  // Create a BLE Characteristic - fast frequency messages
  pFastCharacteristic = pService->createCharacteristic(BLE_FAST_ENGINEDATA_CHARACTERISTIC_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::READ_AUTHEN | NIMBLE_PROPERTY::NOTIFY);
  pFastCharacteristic->setCallbacks(&chrCallbacks);

  // Create a BLE Characteristic - slow frequency messages
  pSlowCharacteristic = pService->createCharacteristic(BLE_SLOW_ENGINEDATA_CHARACTERISTIC_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::READ_AUTHEN | NIMBLE_PROPERTY::NOTIFY);
  pSlowCharacteristic->setCallbacks(&chrCallbacks);

  // Create a BLE Characteristic - write a true value to reboot the component
  pRebootCharacteristic = pService->createCharacteristic(BLE_REBOOT_CHARACTERISTIC_UUID, NIMBLE_PROPERTY::WRITE_NR);
  pRebootCharacteristic->setCallbacks(&chrCallbacks);

  // Start the service
  pService->start();
}

void loop()
{
  ts.execute();
}
