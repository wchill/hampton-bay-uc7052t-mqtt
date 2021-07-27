// Bridge between Home Assistant/MQTT and Hampton Bay ceiling fans.
// Emulates a Rhine UC7052T remote

// Original source https://github.com/owenb321/hampton-bay-fan-mqtt

#include <ELECHOUSE_CC1101_SRC_DRV.h>
#include <RCSwitch.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <Bounce2.h>
#include <ArduinoOTA.h>
#include <ArduinoJson.h>

#include "settings.h"

#define BASE_TOPIC "home/hamptonbay"
#define SPIREG_MODIFY_TOPIC BASE_TOPIC "/1337/spiwrite/+"
#define MESSAGE_TOPIC BASE_TOPIC "/last_message"
#define STATUS_TOPIC BASE_TOPIC "/status"
#define SUBSCRIBE_TOPIC_FAN_SET BASE_TOPIC "/+/fan/set"
#define SUBSCRIBE_TOPIC_FAN_STATE BASE_TOPIC "/+/fan/state"
#define SUBSCRIBE_TOPIC_SPEED_SET BASE_TOPIC "/+/speed/set"
#define SUBSCRIBE_TOPIC_SPEED_STATE BASE_TOPIC "/+/speed/state"
#define SUBSCRIBE_TOPIC_DIRECTION_SET BASE_TOPIC "/+/direction/set"
#define SUBSCRIBE_TOPIC_DIRECTION_STATE BASE_TOPIC "/+/direction/state"
#define SUBSCRIBE_TOPIC_LIGHT_SET BASE_TOPIC "/+/light/set"
#define SUBSCRIBE_TOPIC_LIGHT_STATE BASE_TOPIC "/+/light/state"

// Set receive and transmit pin numbers (GDO0 and GDO2)
#ifdef ESP32 // for esp32! Receiver on GPIO pin 4. Transmit on GPIO pin 2.
#define RX_PIN 4
#define TX_PIN 2
#elif ESP8266 // for esp8266! Receiver on pin 4 = D2. Transmit on pin 5 = D1.
#define RX_PIN 4
#define TX_PIN 5
#else // for Arduino! Receiver on interrupt 0 => that is pin #2. Transmit on pin 6.
#define RX_PIN 0
#define TX_PIN 6
#endif

// Set CC1101 frequency
// Determined empirically via SDR
#define FREQUENCY 303.87

// RC-switch settings
#define RF_PROTOCOL 11
#define RF_REPEATS 24
#define NO_RF_REPEAT_TIME 300

// Define fan states
#define FAN_HI 0b011111
#define FAN_MED 0b101111
#define FAN_LOW 0b110111

// Define commands
#define LIGHT_TOGGLE 0b111110
#define FAN_OFF 0b111101
#define FAN_REV 0b111011

// Define text strings
#define FAN_HI_TEXT "high"
#define FAN_MED_TEXT "medium"
#define FAN_LOW_TEXT "low"
#define FAN_FORWARD_TEXT "forward"
#define FAN_REVERSE_TEXT "reverse"
#define ON_TEXT "ON"
#define OFF_TEXT "OFF"

RCSwitch mySwitch = RCSwitch();
WiFiClient espClient;
PubSubClient client(espClient);

// Keep track of states for all dip settings
struct fan
{
    bool fanOn;
    bool lightOn;
    bool forward;
    uint8_t fanSpeed;
    bool configured;
};
fan fans[16];
long lastvalue;
unsigned long lasttime;

// The ID returned from the RF code appears to be inversed and reversed
//   e.g. a dip setting of on off off off (1000) yields 1110
// Convert between IDs from MQTT from dip switch settings and what is used in the RF codes
const byte dipToRfIds[16] = {
    [0] = 15,
    [1] = 7,
    [2] = 11,
    [3] = 3,
    [4] = 13,
    [5] = 5,
    [6] = 9,
    [7] = 1,
    [8] = 14,
    [9] = 6,
    [10] = 10,
    [11] = 2,
    [12] = 12,
    [13] = 4,
    [14] = 8,
    [15] = 0,
};
const char *idStrings[16] = {
    [0] = "0000",
    [1] = "0001",
    [2] = "0010",
    [3] = "0011",
    [4] = "0100",
    [5] = "0101",
    [6] = "0110",
    [7] = "0111",
    [8] = "1000",
    [9] = "1001",
    [10] = "1010",
    [11] = "1011",
    [12] = "1100",
    [13] = "1101",
    [14] = "1110",
    [15] = "1111",
};
char idchars[] = "01";

void setup_wifi()
{

    delay(10);
    // We start by connecting to a WiFi network
    Serial.println();
    Serial.print("Connecting to ");
    Serial.println(WIFI_SSID);

    WiFi.begin(WIFI_SSID, WIFI_PASS);

    while (WiFi.status() != WL_CONNECTED)
    {
        delay(500);
        Serial.print(".");
    }

    randomSeed(micros());

#ifdef ENABLE_OTA
    ArduinoOTA.setPort(OTA_PORT);
    ArduinoOTA.setHostname(HOSTNAME);
    ArduinoOTA.setPassword(OTA_PASSWORD);

    ArduinoOTA.onStart([]()
                       {
                           String type;
                           if (ArduinoOTA.getCommand() == U_FLASH)
                           {
                               type = "sketch";
                           }
                           else
                           { // U_FS
                               type = "filesystem";
                           }

                           // NOTE: if updating FS this would be the place to unmount FS using FS.end()
                           Serial.println("Start updating " + type);
                       });
    ArduinoOTA.onEnd([]()
                     { Serial.println("\nEnd"); });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total)
                          { Serial.printf("Progress: %u%%\r", (progress / (total / 100))); });
    ArduinoOTA.onError([](ota_error_t error)
                       {
                           Serial.printf("Error[%u]: ", error);
                           if (error == OTA_AUTH_ERROR)
                           {
                               Serial.println("Auth Failed");
                           }
                           else if (error == OTA_BEGIN_ERROR)
                           {
                               Serial.println("Begin Failed");
                           }
                           else if (error == OTA_CONNECT_ERROR)
                           {
                               Serial.println("Connect Failed");
                           }
                           else if (error == OTA_RECEIVE_ERROR)
                           {
                               Serial.println("Receive Failed");
                           }
                           else if (error == OTA_END_ERROR)
                           {
                               Serial.println("End Failed");
                           }
                       });

    ArduinoOTA.begin();
#endif

    Serial.println("");
    Serial.println("WiFi connected");
    Serial.println("IP address: ");
    Serial.println(WiFi.localIP());
}

void transmitCommand(int fanId, int payload)
{
    ELECHOUSE_cc1101.SetTx();               // set Transmit on
    mySwitch.disableReceive();              // Receiver off
    mySwitch.enableTransmit(TX_PIN);        // Transmit on
    mySwitch.setRepeatTransmit(RF_REPEATS); // transmission repetitions.
    mySwitch.setProtocol(RF_PROTOCOL);      // send Received Protocol
    ELECHOUSE_cc1101.setPA(12);

    // Format:
    //   pppp1abcdef
    //   p - device id
    //   a - hi
    //   b - med
    //   c - low
    //   d - for/rev
    //   e - fan
    //   f - light

    int rfCode = dipToRfIds[fanId] << 7 | 0b1 << 6 | payload;
    Serial.print("Transmitting ");
    Serial.println(rfCode, BIN);
    mySwitch.send(rfCode, 12);
    ELECHOUSE_cc1101.SetRx();       // set Receive on
    mySwitch.disableTransmit();     // set Transmit off
    mySwitch.enableReceive(RX_PIN); // Receiver on

    postStateUpdate(fanId);
}

void publishDeviceConfig(int fanId)
{
    // Publish a config message to enable Home Assistant MQTT auto discovery

    fans[fanId].configured = true;
    char configTopic[64];
    char baseTopic[32];
    char deviceName[32];
    char uniqId[32];
    // Allocate on heap to prevent stack overflow
    char *outputBuffer = new char[512];

    // Fan config
    DynamicJsonDocument doc(512);
    sprintf(baseTopic, "home/hamptonbay/%s", idStrings[fanId]);
    sprintf(configTopic, "homeassistant/%s/hamptonbay_%s_%s/config", "fan", idStrings[fanId], "fan");
    sprintf(deviceName, "Hampton Bay %s (%s)", "fan", idStrings[fanId]);
    sprintf(uniqId, "hamptonbay_%s_%s", "fan", idStrings[fanId]);
    doc["~"] = baseTopic;
    doc["avty_t"] = STATUS_TOPIC;
    doc["cmd_t"] = "~/fan/set";
    doc["stat_t"] = "~/fan/state";
    doc["pr_mode_cmd_t"] = "~/speed/set";
    doc["pr_mode_stat_t"] = "~/speed/state";
    doc["name"] = deviceName;
    doc["uniq_id"] = uniqId;
    JsonArray presetModes = doc.createNestedArray("pr_modes");
    presetModes.add(FAN_LOW_TEXT);
    presetModes.add(FAN_MED_TEXT);
    presetModes.add(FAN_HI_TEXT);
    size_t len = measureJson(doc);
    serializeJson(doc, outputBuffer, 512);

    client.beginPublish(configTopic, len, true);
    client.write((unsigned char *)outputBuffer, len);
    bool ret = client.endPublish();

    // Light config
    doc.clear();
    sprintf(configTopic, "homeassistant/%s/hamptonbay_%s_%s/config", "light", idStrings[fanId], "light");
    sprintf(deviceName, "Hampton Bay %s (%s)", "light", idStrings[fanId]);
    sprintf(uniqId, "hamptonbay_%s_%s", "light", idStrings[fanId]);
    doc["~"] = baseTopic;
    doc["avty_t"] = STATUS_TOPIC;
    doc["cmd_t"] = "~/light/set";
    doc["stat_t"] = "~/light/state";
    doc["name"] = deviceName;
    doc["uniq_id"] = uniqId;
    len = measureJson(doc);
    serializeJson(doc, outputBuffer, 512);

    client.beginPublish(configTopic, len, true);
    client.write((unsigned char *)outputBuffer, len);
    client.endPublish();

    delete[] outputBuffer;
}

void callback(char *topic, byte *payload, unsigned int length)
{
    Serial.print("Message arrived [");
    Serial.print(topic);
    Serial.print("] ");
    for (int i = 0; i < length; i++)
    {
        Serial.print((char)payload[i]);
    }
    Serial.println();

    char payloadChar[length + 1];
    sprintf(payloadChar, "%s", payload);
    payloadChar[length] = '\0';

    // Get ID after the base topic + a slash
    char id[5];
    memcpy(id, &topic[sizeof(BASE_TOPIC)], 4);
    id[4] = '\0';
    if (strspn(id, idchars))
    {
        uint8_t idint = strtol(id, (char **)NULL, 2);
        char *attr;
        char *action;
        // Split by slash after ID in topic to get attribute and action
        attr = strtok(topic + sizeof(BASE_TOPIC) + 5, "/");
        action = strtok(NULL, "/");
        // Convert payload to lowercase
        for (int i = 0; payloadChar[i]; i++)
        {
            payloadChar[i] = tolower(payloadChar[i]);
        }

        if (idint >= 0 && idint < 16)
        {
            if (strcmp(attr, "fan") == 0)
            {
                if (strcmp(payloadChar, "on") == 0)
                {
                    fans[idint].fanOn = true;
                }
                else if (strcmp(payloadChar, "off") == 0)
                {
                    fans[idint].fanOn = false;
                }

                if (strcmp(action, "set") == 0)
                {
                    if (fans[idint].fanOn)
                    {
                        transmitCommand(idint, fans[idint].fanSpeed);
                    }
                    else
                    {
                        transmitCommand(idint, FAN_OFF);
                    }
                }
            }
            else if (strcmp(attr, "speed") == 0)
            {
                if (strcmp(payloadChar, FAN_LOW_TEXT) == 0)
                {
                    fans[idint].fanSpeed = FAN_LOW;
                }
                else if (strcmp(payloadChar, FAN_MED_TEXT) == 0)
                {
                    fans[idint].fanSpeed = FAN_MED;
                }
                else if (strcmp(payloadChar, FAN_HI_TEXT) == 0)
                {
                    fans[idint].fanSpeed = FAN_HI;
                }

                if (strcmp(action, "set") == 0)
                {
                    transmitCommand(idint, fans[idint].fanSpeed);
                }
            }
            else if (strcmp(attr, "light") == 0)
            {
                if (strcmp(payloadChar, "on") == 0)
                {
                    fans[idint].lightOn = true;
                }
                else if (strcmp(payloadChar, "off") == 0)
                {
                    fans[idint].lightOn = false;
                }

                if (strcmp(action, "set") == 0)
                {
                    transmitCommand(idint, LIGHT_TOGGLE);
                }
            }
            else if (strcmp(attr, "direction") == 0)
            {
                if (strcmp(payloadChar, FAN_FORWARD_TEXT) == 0)
                {
                    fans[idint].forward = true;
                }
                else if (strcmp(payloadChar, FAN_REVERSE_TEXT) == 0)
                {
                    fans[idint].forward = false;
                }

                if (strcmp(action, "set") == 0)
                {
                    transmitCommand(idint, FAN_REV);
                }
            }
#ifdef ENABLE_AUTODISCOVERY
            if (!fans[idint].configured)
            {
                publishDeviceConfig(idint);
            }
#endif
        }
        else if (idint == 1337)
        {
#ifdef ENABLE_SPIWRITEREG
            if (strcmp(attr, "spiwrite") == 0)
            {
                int reg = atoi(action);
                int val = atoi(payloadChar);
                ELECHOUSE_cc1101.SpiWriteReg(reg, val);
            }
#endif
        }
    }
    else
    {
        // Invalid ID
        return;
    }
}

void postStateUpdate(int id)
{
    char outTopic[100];

    sprintf(outTopic, "%s/%s/fan/state", BASE_TOPIC, idStrings[id]);
    client.publish(outTopic, fans[id].fanOn ? "ON" : "OFF", true);

    sprintf(outTopic, "%s/%s/speed/state", BASE_TOPIC, idStrings[id]);
    switch (fans[id].fanSpeed)
    {
    case FAN_HI:
        client.publish(outTopic, FAN_HI_TEXT, true);
        break;
    case FAN_MED:
        client.publish(outTopic, FAN_MED_TEXT, true);
        break;
    case FAN_LOW:
        client.publish(outTopic, FAN_LOW_TEXT, true);
        break;
    }
    sprintf(outTopic, "%s/%s/light/state", BASE_TOPIC, idStrings[id]);
    client.publish(outTopic, fans[id].lightOn ? "ON" : "OFF", true);
    sprintf(outTopic, "%s/%s/direction/state", BASE_TOPIC, idStrings[id]);
    client.publish(outTopic, fans[id].forward ? "forward" : "reverse", true);
}

void reconnect()
{
    // Loop until we're reconnected
    while (!client.connected())
    {
        Serial.print("Attempting MQTT connection...");
        // Attempt to connect
        if (client.connect(MQTT_CLIENT_NAME, MQTT_USER, MQTT_PASS, STATUS_TOPIC, 0, true, "offline"))
        {
            Serial.println("connected");
            // Once connected, publish an announcement...
            client.publish(STATUS_TOPIC, "online", true);

            // ... and resubscribe
#ifdef ENABLE_SPIWRITEREG
            client.subscribe(SPIREG_MODIFY_TOPIC);
#endif
            client.subscribe(SUBSCRIBE_TOPIC_FAN_SET);
            client.subscribe(SUBSCRIBE_TOPIC_FAN_STATE);
            client.subscribe(SUBSCRIBE_TOPIC_SPEED_SET);
            client.subscribe(SUBSCRIBE_TOPIC_SPEED_STATE);
            client.subscribe(SUBSCRIBE_TOPIC_LIGHT_SET);
            client.subscribe(SUBSCRIBE_TOPIC_LIGHT_STATE);
            client.subscribe(SUBSCRIBE_TOPIC_DIRECTION_SET);
            client.subscribe(SUBSCRIBE_TOPIC_DIRECTION_STATE);
        }
        else
        {
            Serial.print("failed, rc=");
            Serial.print(client.state());
            Serial.println(" try again in 5 seconds");
            // Wait 5 seconds before retrying
            delay(5000);
        }
    }
}

void setup()
{
    Serial.begin(500000);

    // initialize fan struct
    for (int i = 0; i < 16; i++)
    {
        fans[i].lightOn = false;
        fans[i].fanOn = false;
        fans[i].fanSpeed = FAN_LOW;
        fans[i].forward = true;
        fans[i].configured = false;
    }

    ELECHOUSE_cc1101.Init();
    ELECHOUSE_cc1101.setClb(1, 13, 15);
    ELECHOUSE_cc1101.setMHZ(FREQUENCY);
    //ELECHOUSE_cc1101.setDeviation( 47.60 ) ;   // frequency deviation in kHz. Value from 1.58 to 380.85. Default is 47.60 kHz
    //ELECHOUSE_cc1101.setChannel( 0 ) ;         // channel number from 0 to 255. Default is 0
    //ELECHOUSE_cc1101.setChsp( 199.95 ) ;       // channel spacing is multiplied by channel number CHAN & added to the base freq in kHz. Range: 25.39..405.45. Default 199.95 kHz
    ELECHOUSE_cc1101.setRxBW(58.03); // receive bandwidth in kHz. Range: 58.03..812.50. Default 812.50 kHz
    ELECHOUSE_cc1101.setDRate(5);    // data rate in kBaud. Range: 0.02..1621.83. Default 99.97 kBaud!
    //ELECHOUSE_cc1101.setPA( 12 ) ;             // TxPower. Possible values depending on the frequency band. (-30 -20 -15 -10 -6 0 5 7 10 11 12 ) Default = max!
    // ELECHOUSE_cc1101.setSyncMode( 2 ) ;        // Combined sync-word qualifier mode
    // 0 = No preamble/sync
    // 1 = 16 sync word bits detected
    // 2 = 16/16 sync word bits detected
    // 3 = 30/32 sync word bits detected
    // 4 = No preamble/sync, carrier-sense above threshold
    // 5 = 15/16 + carrier-sense above threshold
    // 6 = 16/16 + carrier-sense above threshold
    // 7 = 30/32 + carrier-sense above threshold
    // ELECHOUSE_cc1101.setSyncWord( 211, 145 ) ; // Syncword-H, Syncword-L. Must be the same for the transmitter and receiver.
    //ELECHOUSE_cc1101.setAdrChk( 0 ) ;          // Address check of received packages
    // 0=No check, 1=Addr check/no broadcast, 2=Addr check + 0x00 broadcast, 3=Addr check + 0x00 + 0xFF broadcast
    //ELECHOUSE_cc1101.setAddr( 0 ) ;            // Address used for packet filtration. Optional broadcast addresses are 0 ( 0x00 ) and 255 ( 0xFF )
    //ELECHOUSE_cc1101.setWhiteData( 0 ) ;       // Turn data whitening on/off. 0=Whitening off. 1 = Whitening on
    //ELECHOUSE_cc1101.setPktFormat( 0 ) ;       // Format of RX and TX data. 0=Normal mode, use FIFOs for RX and TX
    //                           1=Synchronous serial mode, Data in on GDO0 and data out on either of the GDOx pins
    //                           2=Random TX test mode; sends random data using PN9 generator. Works as normal mode, setting 0 in RX
    //                           3=Asynchronous serial mode, Data in on GDO0 and data out on either of the GDOx pins
    //ELECHOUSE_cc1101.setLengthConfig( 0 ) ;    // 0=Fixed packet length mode. 1=Variable packet length mode. 2=Infinite packet length mode. 3=Reserved
    //ELECHOUSE_cc1101.setPacketLength( 12 ) ;    // Indicates the packet length in fixed packet length mode or max packet length in variable packet length mode
    //ELECHOUSE_cc1101.setCrc( 1 ) ;             // 1=CRC calculation in TX and CRC check in RX enabled. 0=CRC disabled for TX and RX
    //ELECHOUSE_cc1101.setCRC_AF( 0 ) ;          // automatic flush RX FIFO if wrong CRC. Requires that only 1 packet is in the RXIFIFO and packet length is limited to FIFO size
    ELECHOUSE_cc1101.setDcFilterOff(0); // Disable digital DC blocking filter before demodulator. Only for data rates < 250 kBaud.
                                        // The recommended IF frequency changes when DC blocking is disabled. 1=Disable (current optimized). 0=Enable (better sensitivity)
    //ELECHOUSE_cc1101.setManchester( 0 ) ;      // Enables Manchester encoding/decoding. 0=Disable. 1=Enable
    //ELECHOUSE_cc1101.setFEC( 0 ) ;             // Enable Forward Error Correction (FEC) with interleaving for packet payload (Only supported for fixed packet length mode)
    //ELECHOUSE_cc1101.setPQT( 0 ) ;             // Preamble quality estimator threshold. The preamble quality estimator increases an internal counter by one each time
    //   a bit is received that is different from the previous bit
    //   and decreases the counter by 8 each time a bit is received that is the same as the last bit
    // A threshold of 4*PQT for this counter is used to gate sync word detection. When PQT=0 a sync word is always accepted
    //ELECHOUSE_cc1101.setAppendStatus( 0 ) ;    // When enabled, two status bytes will be appended to the payload of the packet.
    // The status bytes contain RSSI and LQI values, as well as CRC OK
    ELECHOUSE_cc1101.SetRx();
    mySwitch.enableReceive(RX_PIN);

    setup_wifi();
    client.setServer(MQTT_HOST, MQTT_PORT);
    client.setCallback(callback);
}

void loop()
{

    if (!client.connected())
    {
        reconnect();
    }
    client.loop();

#ifdef ENABLE_OTA
    ArduinoOTA.handle();
#endif

    // Handle received transmissions
    if (mySwitch.available())
    {
        long value = mySwitch.getReceivedValue();   // save received Value
        int prot = mySwitch.getReceivedProtocol();  // save received Protocol
        int bits = mySwitch.getReceivedBitlength(); // save received Bitlength
        int d = mySwitch.getReceivedDelay();
        int rssi = ELECHOUSE_cc1101.getRssi();

        if (prot == 11 && bits == 12)
        {
            unsigned long t = millis();
            if (value == lastvalue)
            {
                if (t - lasttime < NO_RF_REPEAT_TIME)
                {
                    mySwitch.resetAvailable();
                    return;
                }
            }
            lastvalue = value;
            lasttime = t;
            Serial.print(d);
            Serial.print(" - ");
            Serial.print(prot);
            Serial.print(" - ");
            Serial.print(value, BIN);
            Serial.print(" - ");
            Serial.print(bits);
            Serial.print(" - ");
            Serial.print(rssi);
            Serial.println("dbm");
            int id = value >> 7;
            // Got a correct id in the correct protocol
            if (id < 16)
            {
                // Convert to dip id
                int dipId = dipToRfIds[id];
                Serial.print("Received command from ID - ");
                Serial.println(dipId);

#ifdef ENABLE_DEBUGTOPIC
                char buf[128];
                char bitstring[16];
                itoa(value, bitstring, 2);
                sprintf(buf, "ID %d - %s @ -%ddBm", id, bitstring, rssi);
                client.publish(MESSAGE_TOPIC, buf, true);
#endif

                int command = value & 0b0111111;
                switch (command)
                {
                case FAN_HI:
                case FAN_MED:
                case FAN_LOW:
                    fans[dipId].fanSpeed = command;
                    fans[dipId].fanOn = true;
                    break;
                case FAN_REV:
                    fans[dipId].forward = !fans[dipId].forward;
                    break;
                case FAN_OFF:
                    fans[dipId].fanOn = false;
                    break;
                case LIGHT_TOGGLE:
                    fans[dipId].lightOn = !fans[dipId].lightOn;
                    break;
                }

                postStateUpdate(dipId);
            }
        }
        mySwitch.resetAvailable();
    }
}