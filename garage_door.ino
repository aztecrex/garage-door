
#include "WiFi.h"
#include <WiFiClientSecure.h>
#include <MQTTClient.h>
#include <ArduinoJson.h>

#include "secrets.h"

WiFiClientSecure net = WiFiClientSecure();
MQTTClient client = MQTTClient(512);

const int actl = 26;
const int bots = 32;
const int tops = 33;
const int upl = 27;
const int downl = 14;
enum position {
  UNKNOWN, UP, DOWN, ERROR, FREE
};

position calc_position(int b, int t) {
  if (b && t) {
    return ERROR;
  } else if (b) {
    return DOWN;
  } else if (t) {
    return UP;
  } else {
    return UNKNOWN;
  }
}


position last = UNKNOWN;
position desired = UNKNOWN;

void setup() {
  Serial.begin(115200);
  Serial.println("on board");
  connectToWiFi();
  connectToAWS();
  pinMode(actl, OUTPUT);
  digitalWrite(actl, LOW);

  pinMode(upl, OUTPUT);
  digitalWrite(upl, LOW);

  pinMode(downl, OUTPUT);
  digitalWrite(downl, LOW);
  
  pinMode(tops, INPUT_PULLUP);
  pinMode(bots, INPUT_PULLUP);
  reportPosition(UNKNOWN, UNKNOWN);

}

position decode_position() {
  int b = !digitalRead(bots);
  int t = !digitalRead(tops);
  return calc_position(b, t);
}

void encode_status(position cur, position want)  {
  int u = LOW;
  int d = LOW;
  if (cur != want) {
    if (want == UP) {
      u = HIGH;
    } else if (want == DOWN) {
      d = HIGH;
    }
  }
  digitalWrite(upl, u);
  digitalWrite(downl, d);
}

char const * position_name(position pos) {
  switch (pos) {
    case UNKNOWN: return "UNKNOWN";
    case ERROR: return "ERROR";
    case UP: return "UP";
    case DOWN: return "DOWN";
    case FREE: return "FREE";
  }
}

position position_value(char const *n) {
  if (!strcmp(n, "UNKNOWN")) return UNKNOWN;
  if (!strcmp(n, "UP")) return UP;
  if (!strcmp(n, "DOWN")) return DOWN;
  if (!strcmp(n, "ERROR")) return ERROR;
  if (!strcmp(n, "FREE")) return FREE;
}

char buffer[100];
int operate = 0;

int stabilize = 0;

void pulse(int duty) {
    digitalWrite(actl, HIGH);
    delay(500);
    digitalWrite(actl, LOW);  
}

void loop() {

  position current = decode_position();
  if (current != last) {
    char const * lasts = position_name(last);
    char const * currents = position_name(current);
    sprintf(buffer, "transition from %s to %s\n", lasts, currents);
    Serial.print(buffer);
    reportPosition(current, desired);
    last = current;
  }
  client.loop();

  bool targeted = (stabilize == 0) && (desired != FREE) && (desired != UNKNOWN) && (current != desired);
  
  encode_status(current, desired);
  if ((operate || targeted) && current != ERROR) {
    pulse(500);
    operate = 0;
    stabilize = 6;
    delay(500);
  } else {
    delay(1000);
  }

  if (stabilize > 0) stabilize = stabilize - 1;
}

// The name of the device. This MUST match up with the name defined in the AWS console
#define DEVICE_NAME "iot-codecraft-thing-Garage-Device-GSBODF67Q7W0"

// The MQTTT endpoint for the device (unique for each AWS account but shared amongst devices within the account)
#define AWS_IOT_ENDPOINT "ad78o9k6p57sk.iot.us-east-1.amazonaws.com"

// The MQTT topic that this device should publish to
#define AWS_IOT_TOPIC "$aws/things/" DEVICE_NAME "/shadow/update"

#define AWS_IOT_DELTA_TOPIC "$aws/things/" DEVICE_NAME "/shadow/update/delta"
#define AWS_IOT_OPERATE_TOPIC "control/" DEVICE_NAME "/operate"


// How many times we should attempt to connect to AWS
#define AWS_MAX_RECONNECT_TRIES 50

void reportPosition(position cur, position des)
{
  StaticJsonDocument<256> jsonDoc;
  JsonObject stateObj = jsonDoc.createNestedObject("state");
  JsonObject reportedObj = stateObj.createNestedObject("reported");

  reportedObj["position"] = position_name(cur);
  reportedObj["wifi_strength"] = WiFi.RSSI();

  // once up or down is reached, go into "FREE" mode
  // so door can be operated by standard controls
  // this is still not entirely safe. e.g. if someone
  // operates the manual control to prevent the door
  // from going down, the Shadow will still have its
  // desired state as down. You definitely don't want
  // to use target positioning if you don't have
  // obstruction detection.
  if ((cur == DOWN && des != UP) || (cur == UP && des != DOWN) || cur == ERROR) {
    JsonObject desiredObj = stateObj.createNestedObject("desired");
    desiredObj["position"] = position_name(FREE);
  }

  // Publish the message to AWS
  char jsonBuffer[512];
  serializeJson(jsonDoc, jsonBuffer);
//  Serial.println(AWS_IOT_TOPIC);
//  Serial.println(jsonBuffer);
  client.publish(AWS_IOT_TOPIC, jsonBuffer);
}

void handleMessage(String &topic, String &payload) {
  if (topic == AWS_IOT_OPERATE_TOPIC) {
    Serial.println("operate");
    operate = 1;
  } else if (topic == AWS_IOT_DELTA_TOPIC) {
    StaticJsonDocument<256> jsonDoc;
    deserializeJson(jsonDoc, payload);
    char const *desired_s = jsonDoc["state"]["position"];
    desired = position_value(desired_s);
    Serial.println("want: " + String(desired_s));
  }
  Serial.println("incoming: " + topic + " - " + payload);
}


void connectToAWS()
{
    // Configure WiFiClientSecure to use the AWS certificates we generated
    net.setCACert(AWS_CERT_CA);
    net.setCertificate(AWS_CERT_CRT);
    net.setPrivateKey(AWS_CERT_PRIVATE);

    // Connect to the MQTT broker on the AWS endpoint we defined earlier
    client.begin(AWS_IOT_ENDPOINT, 8883, net);
    client.onMessage(handleMessage);

    // Try to connect to AWS and count how many times we retried.
    int retries = 0;
    Serial.print("Connecting to AWS IOT");

    while (!client.connect(DEVICE_NAME) && retries < AWS_MAX_RECONNECT_TRIES) {
        Serial.print(".");
        delay(100);
        retries++;
    }

    // Make sure that we did indeed successfully connect to the MQTT broker
    // If not we just end the function and wait for the next loop.
    if(!client.connected()){
        Serial.println(" Timeout AWS!");
        return;
    }

    client.subscribe(AWS_IOT_DELTA_TOPIC);
    client.subscribe(AWS_IOT_OPERATE_TOPIC);

    // If we land here, we have successfully connected to AWS!
    // And we can subscribe to topics and send messages.
    Serial.println("Connected AWS!");
}

void connectToWiFi()
{
  WiFi.mode(WIFI_STA);

  char const *ssid = "ssid";
  char const *pass = "pass";
  int nets = WiFi.scanNetworks();
  for(int i = 0; i < nets; i += 1) {
    Serial.println(WiFi.SSID(i));
    if (WiFi.SSID(i) == WIFI_SSID_1) {
      ssid = WIFI_SSID_1;
      pass = WIFI_PASSWORD_1;
      break;
    } else if (WiFi.SSID(i) == WIFI_SSID_2) {
      ssid = WIFI_SSID_2;
      pass = WIFI_PASSWORD_2;
      break;
    }
  }
  Serial.println("using ssid " + String(ssid));
  WiFi.begin(ssid, pass);

  // Only try 15 times to connect to the WiFi
  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 15){
    delay(500);
    Serial.print(".");
    retries++;
  }

  // If we still couldn't connect to the WiFi, go to deep sleep for a minute and try again.
  if(WiFi.status() != WL_CONNECTED){
    esp_sleep_enable_timer_wakeup(1 * 60L * 1000000L);
    esp_deep_sleep_start();
  } else {
    Serial.println(" Connected WiFi");
  }
}
