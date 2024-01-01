#include <Arduino.h>
#include "BLEDevice.h"
#include "esp_log.h"

#define TAG "client_gl50"

// The Glucose service we wish to connect to.
static BLEUUID glucose_servUUID("1808");
static BLEUUID device_information_servUUID("180A");
// The characteristics of the remote service we are interested in.
static BLEUUID    measurement_charUUID("2A18"); // Glucose Measurement
static BLEUUID    context_charUUID("2A34"); // Glucose Measurement Context
static BLEUUID    racp_charUUID("2A52"); // Record Access Control Point
static BLEUUID    serialNum_charUUID("2A25"); // Record Access Control Point

static boolean doConnect = false;
static boolean connected = false;
static boolean doScan = false;
static boolean doPair = false;
static BLEAdvertisedDevice* myDevice;

static boolean isContext = false;

BLERemoteCharacteristic* pRemoteChar_racp;
BLERemoteCharacteristic* pRemoteChar_serialNum;

static BLEClient* pClient;

static uint16_t year;
static uint8_t month;
static uint8_t day;
static uint8_t hour;
static uint8_t minute;
static uint16_t measurement;
char buffer[40];

static void my_gap_event_handler(esp_gap_ble_cb_event_t  event, esp_ble_gap_cb_param_t* param) {
	log_d(TAG, "custom gap event handler, event: %d", (uint8_t)event);
}

static void my_gatts_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t* param) {
	log_d(TAG, "custom gatts event handler, event: %d", (uint8_t)event);
}

static void my_gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t* param) {
	log_d(TAG, "custom gattc event handler, event: %d", (uint8_t)event);
	log_d(TAG, "custom gattc event handler, gattc_if: %d", (uint8_t)gattc_if);
	log_d(TAG, "custom gattc event handler, param: %d", (uint8_t)param->write.status);
	if (param->write.status == 5 && param->write.handle == 0x16) doPair = true;
}

class MySecurity : public BLESecurityCallbacks {
	uint32_t onPassKeyRequest(){
		log_d(TAG, "In onPassKeyRequest");
		return 982249;
	}
	void onPassKeyNotify(uint32_t pass_key){
		log_d(TAG, "In onPassKeyNotify");
	}
	bool onConfirmPIN(uint32_t pass_key){
		log_d(TAG, "In onConfirmPIN");
		return false;
	}
	bool onSecurityRequest(){
		log_d(TAG, "In onSecurityRequest");
		return true;
	}
	void onAuthenticationComplete(esp_ble_auth_cmpl_t auth_cmpl){
		log_d(TAG, "In onAuthenticationComplete");
		if(auth_cmpl.success){
			log_i("SECURITY", "remote BD_ADDR:");
			esp_log_buffer_hex("SECURITY", auth_cmpl.bd_addr, sizeof(auth_cmpl.bd_addr));
			log_i("SECURITY", "address type = %d", auth_cmpl.addr_type);
		}
	}
};

static void notifyCallback(BLERemoteCharacteristic* pBLERemoteCharacteristic,
                           uint8_t* pData,
                           size_t length,
                           bool isNotify) {
    log_d(TAG, "In notifyCallback");

    if (isNotify) {
      log_d(TAG, "This is a notification");
      uint8_t type = (byte)pData[0] >> 4;
      if (type == 1) { // context follows
        year = (pData[4] << 8 | pData[3]);
        month = pData[5];
        day = pData[6];
        hour = pData[7];
        minute = pData[8];
        measurement = (pData[9] << 8 | pData[10]);
        sprintf(buffer,"%d-%02d-%02d %02d:%02d %d """,year, month, day, hour, minute, measurement);
        isContext = true;
      } else if (isContext) {
        uint8_t marquage = pData[3];
        if (marquage == 1)
  	;
        else if (marquage == 2)
  	marquage = 3;
        else
  	marquage = 16;
        sprintf(buffer,"%s %d", buffer, marquage);
        Serial.println(buffer);
        isContext = false;
      } else {
        year = (pData[4] << 8 | pData[3]);
        month = pData[5];
        day = pData[6];
        hour = pData[7];
        minute = pData[8];
        measurement = (pData[9] << 8 | pData[10]);
        sprintf(buffer,"%d-%02d-%02d %02d:%02d %d """,year, month, day, hour, minute, measurement);
        Serial.println(buffer);
      }
    } else {
      log_d(TAG, "This is an indication");
      if (pData[0] == 6) {
	// This is a response code
        if (pData[2] == 1 && pData[3] == 1) {
	  //request was "Report stored values" and Response is success
	  /**/
	  // Confirmation to indication
          const uint8_t txValue[] = {0x1e};
          BLEUUID serv_UUID = BLEUUID("1808");
          BLEUUID char_UUID = BLEUUID("2A52");
	  BLERemoteService* pRemoteService;
	  BLERemoteCharacteristic* pRemoteChar;
          pRemoteService = pClient->getService(serv_UUID);
          if (pRemoteService == nullptr) {
            log_d(TAG, "Failed to find device information service UUID: %s", serv_UUID.toString().c_str());
            pClient->disconnect();
            return;
          }
          pRemoteChar = pRemoteService->getCharacteristic(char_UUID);
          if(pRemoteChar != nullptr && pRemoteChar->canWrite()) {
            pRemoteChar->writeValue((uint8_t*)txValue, 1, false);
            log_d(TAG, "Confirmation to indication sent");
          }
	  /**/
	  Serial.println("Traitement termine avec succes");
	} else {
	  //il y a eu un problème
	  Serial.println("*** Traitement en anomalie ***");
          log_d(TAG, "Abnormal Indication: response code: %d, operator: %d, request opcode: %d, response opcode: %d",
			  pData[0], pData[1], pData[2], pData[3]);
	}
      }
    }
}

class MyClientCallback : public BLEClientCallbacks {
  void onConnect(BLEClient* pclient) {
  }

  void onDisconnect(BLEClient* pclient) {
    connected = false;
    log_d(TAG, "onDisconnect");
  }
};

bool ConnectCharacteristic(BLERemoteService* pRemoteService, BLEUUID l_charUUID) {
    // Obtain a reference to the characteristic in the service of the remote BLE server.
    BLERemoteCharacteristic* pRemoteCharacteristic;
    pRemoteCharacteristic = pRemoteService->getCharacteristic(l_charUUID);
    if (pRemoteCharacteristic == nullptr) {
      log_d(TAG, "Failed to find our characteristic UUID: %s", l_charUUID.toString().c_str());
      return false;
    }
    log_d(TAG, " - Found characteristic: %s - handle: %d", pRemoteCharacteristic->getHandle());

    if(pRemoteCharacteristic->canNotify())
      pRemoteCharacteristic->registerForNotify(notifyCallback);

    return true;
}

bool connectToServer() {
    log_d(TAG, "Forming a connection to %s", myDevice->getAddress().toString().c_str());

BLEDevice::setEncryptionLevel(ESP_BLE_SEC_ENCRYPT);
BLEDevice::setSecurityCallbacks(new MySecurity());
BLESecurity *pSecurity = new BLESecurity();
pSecurity->setKeySize();
pSecurity->setAuthenticationMode(ESP_LE_AUTH_REQ_SC_MITM_BOND);
pSecurity->setCapability(ESP_IO_CAP_NONE);
pSecurity->setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
pSecurity->setKeySize(16);

    pClient = BLEDevice::createClient();
    log_d(TAG, " - Created client");

    pClient->setClientCallbacks(new MyClientCallback());

    // Connect to the remove BLE Server.
    pClient->connect(myDevice);  // if you pass BLEAdvertisedDevice instead of address, it will be recognized type of peer device address (public or private)
    log_d(TAG, " - Connected to server");
    pClient->setMTU(517); //set client to request maximum MTU from server (default is 23 otherwise)
  
    connected = true;

/*
    if (ConnectCharacteristic(pRemoteService, measurement_charUUID) == false)
	connected = false;
    else if (ConnectCharacteristic(pRemoteService, context_charUUID) == false)
	connected = false;
    else if (ConnectCharacteristic(pRemoteService, racp_charUUID) == false)
	connected = false;

    if (connected == false) {
       pClient->disconnect();
       Serial.println("At least one characteristic UUID not found");	
       return false;
    }
*/


    return true;
}

/**
 * Scan for BLE servers and find the first one that advertises the service we are looking for.
 */
class MyAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {
 /**
   * Called for each advertising BLE server.
   */
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    log_d(TAG, "BLE Advertised Device found: %s", advertisedDevice.toString().c_str());
    // We have found a device, let us now see if it's our glucometer
    if (advertisedDevice.haveName() && advertisedDevice.getName() == "Beurer GL50EVO"
        && advertisedDevice.getAddress().toString() == "ed:ac:3e:ea:54:ff") {
      log_d(TAG, "Beurer trouve");
      BLEDevice::getScan()->stop();
      myDevice = new BLEAdvertisedDevice(advertisedDevice);
      doConnect = true;
      doScan = true;
    } // Found our device (glucometer)
  } // onResult
}; // MyAdvertisedDeviceCallbacks


void setup() {
  Serial.begin(115200);
  delay (5000);
  Serial.println("Debut du traitement...Attendre une vingtaine de secondes environ...");
  BLEDevice::init("");

  BLEDevice::setCustomGattcHandler(my_gattc_event_handler);
/*
BLEDevice::setCustomGapHandler(my_gap_event_handler);
BLEDevice::setCustomGattsHandler(my_gatts_event_handler);
*/

  // Retrieve a Scanner and set the callback we want to use to be informed when we
  // have detected a new device.  Specify that we want active scanning and start the
  // scan to run for 5 seconds.
  BLEScan* pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setInterval(1349);
  pBLEScan->setWindow(449);
  pBLEScan->setActiveScan(true);
  pBLEScan->start(5, false);


} // End of setup.


void loop() {
  if (doConnect == true) {
    if (connectToServer()) {
      log_d(TAG, "We are now connected to the BLE Server.");
    } else {
      Serial.println("We have failed to connect to the server; there is nothin more we will do.");
    }
    doConnect = false;
  }

  if (connected) {
    log_d(TAG, "Yes, connected");
    // On ........
    BLERemoteService* pRemoteService;
    BLERemoteCharacteristic* pRemoteChar;
    BLEUUID serv_UUID;
    BLEUUID char_UUID;

    serv_UUID = BLEUUID("1801");
    char_UUID = BLEUUID("2A05");
    pRemoteService = pClient->getService(serv_UUID);
    if (pRemoteService == nullptr) {
      log_d(TAG, "Failed to find Generic Attribute service UUID: %s", serv_UUID.toString().c_str());
      pClient->disconnect();
      return;
    }
    pRemoteChar = pRemoteService->getCharacteristic(char_UUID);
    if (pRemoteChar != nullptr && pRemoteChar->canIndicate()) {
      log_d(TAG, " - subscribing to indication");
      pRemoteChar->registerForNotify(notifyCallback, false, true);
    } else {
      log_d(TAG, "Failed to set indication on");
    }

    serv_UUID = BLEUUID("1808");
    char_UUID = BLEUUID("2A52");
    pRemoteService = pClient->getService(serv_UUID);
    if (pRemoteService == nullptr) {
      log_d(TAG, "Failed to find Generic Attribute service UUID: %s", serv_UUID.toString().c_str());
      pClient->disconnect();
      return;
    }
    pRemoteChar = pRemoteService->getCharacteristic(char_UUID);
    if (pRemoteChar != nullptr && pRemoteChar->canIndicate()) {
      log_d(TAG, " - subscribing to indication");
      pRemoteChar->registerForNotify(notifyCallback, false, true);
    } else {
      log_d(TAG, "Failed to set indication on");
    }

    serv_UUID = BLEUUID("1808");
    char_UUID = BLEUUID("2A34");
    pRemoteService = pClient->getService(serv_UUID);
    if (pRemoteService == nullptr) {
      log_d(TAG, "Failed to find Generic Attribute service UUID: %s", serv_UUID.toString().c_str());
      pClient->disconnect();
      return;
    }
    pRemoteChar = pRemoteService->getCharacteristic(char_UUID);
    if (pRemoteChar != nullptr && pRemoteChar->canNotify()) {
      log_d(TAG, " - subscribing to notification");
      pRemoteChar->registerForNotify(notifyCallback, true, true);
    } else {
      log_d(TAG, "Failed to set notification on service UUID: %s", serv_UUID.toString().c_str());
      while(1) { }
    }

    serv_UUID = BLEUUID("1808");
    char_UUID = BLEUUID("2A18");
    pRemoteService = pClient->getService(serv_UUID);
    if (pRemoteService == nullptr) {
      log_d(TAG, "Failed to find Generic Attribute service UUID: %s", serv_UUID.toString().c_str());
      pClient->disconnect();
      return;
    }
    pRemoteChar = pRemoteService->getCharacteristic(char_UUID);
    if (pRemoteChar != nullptr && pRemoteChar->canNotify()) {
      log_d(TAG, " - subscribing to notification");
      pRemoteChar->registerForNotify(notifyCallback, true, true);
    } else {
      log_d(TAG, "Failed to set notification on service UUID: %s", serv_UUID.toString().c_str());
      while(1) { }
    }

    serv_UUID = BLEUUID("180A");
    char_UUID = BLEUUID("2A25");
    pRemoteService = pClient->getService(serv_UUID);
    if (pRemoteService == nullptr) {
      log_d(TAG, "Failed to find generic device information service UUID: %s", serv_UUID.toString().c_str());
      pClient->disconnect();
      return;
    }
    pRemoteChar = pRemoteService->getCharacteristic(char_UUID);
    if(pRemoteChar != nullptr && pRemoteChar->canRead()) {
      std::string value = pRemoteChar->readValue().c_str();
      log_d(TAG, "The Serial Number is: : %s", value.c_str());
    }

    const uint8_t txValue[] = {0x1, 0x1}; // Get All records
    serv_UUID = BLEUUID("1808");
    char_UUID = BLEUUID("2A52");
    pRemoteService = pClient->getService(serv_UUID);
    if (pRemoteService == nullptr) {
      log_d(TAG, "Failed to find device information service UUID: %s", serv_UUID.toString().c_str());
      pClient->disconnect();
      return;
    }
    pRemoteChar = pRemoteService->getCharacteristic(char_UUID);
    if(pRemoteChar != nullptr && pRemoteChar->canWrite()) {
      pRemoteChar->writeValue((uint8_t*)txValue, 2, true);
      log_d(TAG, "Asked for all records");
    }
    while(1) { } // Arret
  }else if(doScan){
    BLEDevice::getScan()->start(0);  // this is just example to start scan after disconnect, most likely there is better way to do it in arduino
  }
  delay(1000); // Delay a second between loops.
}
