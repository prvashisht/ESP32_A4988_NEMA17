#include <Credentials.h> // contains WiFi credentials, could contain any secret key
#include <CustomOTA.h> // enables WiFi, sets up OTA updates
#include <AsyncTCP.h> // Base for Async Web Server
#include <ESPAsyncWebServer.h> // Creates asynchronous web servers
#include <WebSerial.h> // enables reading "Serial" output over web (acts as an emulator)
#include <Stepper_Motor.h>

#define DEVICE_HOSTNAME "LastRoomCurtain" // change this for every new device
#define STATIC_IP_HOST_ADDRESS 85 // change this for every new device

const byte PIN_SLEEP = 5;
const byte PIN_DIR = 16;
const byte PIN_STEP = 17;
const byte PIN_RESET = 18;
const byte PIN_MS3 = 19;
const byte PIN_MS2 = 21;
const byte PIN_MS1 = 22;
const byte PIN_ENABLE = 23;
const byte PIN_LED = 25;
const byte PIN_LIMIT_SWITCH = 26;
const byte PIN_BUTTONS = 34;

bool isCurtainClosed = false;
bool isLimitSwitchPressed = false;
int BUTTON_RIGHT_THRESHOLD = 1000;
int BUTTON_LEFT_THRESHOLD = 3000;

// Enum for which input button was pressed
enum inputButtons { INPUT_NONE, INPUT_BUTTON_CLOCKWISE, INPUT_BUTTON_ANTI_CLOCKWISE };
inputButtons pressedButton = INPUT_NONE;

// Enum for type of override from the web
// TODO: Replace this with a proper priority list.
enum webOverrideStatus { OVERRIDE_NONE, OVERRIDE_OPEN_CURTAIN, OVERRIDE_CLOSE_CURTAIN };
webOverrideStatus webOverride = OVERRIDE_NONE;

// Enum for current state of the motor
enum motorStates { MOTOR_DISABLE, MOTOR_CLOCKWISE, MOTOR_ANTI_CLOCKWISE };
motorStates motorState = MOTOR_DISABLE;

// Enum for current entity controlling the motor
// TODO: Use CONTROL_WEBSERIAL_MONITOR for a debug mode with interactive inputs.
enum motorControllers { CONTROL_NONE, CONTROL_BUTTON, CONTROL_WEB_API, CONTROL_WEBSERIAL_MONITOR };
motorControllers motorController = CONTROL_NONE;

Stepper_Motor motor(PIN_ENABLE, PIN_DIR, PIN_STEP, PIN_SLEEP, PIN_RESET, PIN_MS1, PIN_MS2, PIN_MS3, WebSerial);

void setupPins() {
    pinMode(PIN_LED, OUTPUT);
    pinMode(PIN_BUTTONS, INPUT);
    pinMode(PIN_LIMIT_SWITCH, INPUT);

    digitalWrite(PIN_LED, HIGH);
    delay(2000);
    digitalWrite(PIN_LED, LOW); // check if the LED is working fine.
}
void setupWiFi() {
    const char *ssid = WIFI_SSID;
    const char *password = WIFI_PW;

    IPAddress staticIP(192, 168, 178, STATIC_IP_HOST_ADDRESS);
    IPAddress gateway(192, 168, 178, 1);
    IPAddress subnet(255, 255, 255, 0);
    IPAddress dns1(192, 168, 178, 13); // optional
    IPAddress dns2(1, 1, 1, 1);        // optional

    if (!WiFi.config(staticIP, gateway, subnet, dns1, dns2)) {
        Serial.println("STA Failed to configure");
    }

    setupOTA(DEVICE_HOSTNAME, WIFI_SSID, WIFI_PW);
}

// Set up Web Server
AsyncWebServer server(80);
// HTML page
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML><html>
<head>
  <title>ESP Web Server</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <link rel="icon" href="data:,">
  <style>
    html {font-family: Arial; display: inline-block; text-align: center;}
    h2 {font-size: 3.0rem;}
    p {font-size: 3.0rem;}
    body {max-width: 600px; margin:0px auto; padding-bottom: 25px;}
    .switch {position: relative; display: inline-block; width: 120px; height: 68px} 
    .switch input {display: none}
    .slider {position: absolute; top: 0; left: 0; right: 0; bottom: 0; background-color: #ccc; border-radius: 6px}
    .slider:before {position: absolute; content: ""; height: 52px; width: 52px; left: 8px; bottom: 8px; background-color: #fff; -webkit-transition: .4s; transition: .4s; border-radius: 3px}
    input:checked+.slider {background-color: #b30000}
    input:checked+.slider:before {-webkit-transform: translateX(52px); -ms-transform: translateX(52px); transform: translateX(52px)}
  </style>
</head>
<body>
  <h2>ESP Web Server</h2>
  %BUTTONPLACEHOLDER%
<script>function toggleCheckbox(element) {
  var xhr = new XMLHttpRequest();
//  if(element.checked){ xhr.open("GET", "/update?output="+element.id+"&state=1", true); }
//  else { xhr.open("GET", "/update?output="+element.id+"&state=0", true); }
  if(element.checked){ xhr.open("GET", "/open", true); }
  else { xhr.open("GET", "/close", true); }
  xhr.send();
}
</script>
</body>
</html>
)rawliteral";
// Replaces placeholder with button section in your web page
String processor(const String &var) {
    if (var == "BUTTONPLACEHOLDER") {
        String checkedString = isCurtainClosed ? "checked" : "";
        String buttons = "";
        buttons += "<h4>Curtain</h4><label class='switch'><input type='checkbox' onchange='toggleCheckbox(this)' id='2' " + checkedString + "><span class='slider'></span></label>";
        return buttons;
    }
    return String();
}
void createServerEndpoints() {
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send_P(200, "text/html", index_html, processor);
    });

    server.on("/open", HTTP_GET, [](AsyncWebServerRequest *request) {
        webOverride = OVERRIDE_OPEN_CURTAIN;
        request->send(200, "text/plain", "OK");
    });

    server.on("/close", HTTP_GET, [](AsyncWebServerRequest *request) {
        webOverride = OVERRIDE_CLOSE_CURTAIN; // TODO: Not working, only opens with api calls
        request->send(200, "text/plain", "OK");
    });
}
void beginOnlineServer() {
    server.begin();
}

void enableWebSerial() {
    WebSerial.begin(&server);
    WebSerial.msgCallback(processWebSerialInput);
}
void processWebSerialInput(uint8_t *data, size_t len) {
    WebSerial.println("Received Data...");
    String d = "";
    for (int i = 0; i < len; i++) {
        d += char(data[i]);
    }
    Serial.println(d);
    if (d == "open") webOverride = OVERRIDE_OPEN_CURTAIN;
    if (d == "close") webOverride = OVERRIDE_CLOSE_CURTAIN;
    if (d == "gateway") WebSerial.println(WiFi.gatewayIP().toString());
    if (d == "filename") WebSerial.println(__FILE__);
}

void readButtonInputs() {
    int buttonValue = analogRead(PIN_BUTTONS);
    if (buttonValue > BUTTON_LEFT_THRESHOLD) {
        pressedButton = INPUT_BUTTON_ANTI_CLOCKWISE;
    } else if (buttonValue > BUTTON_RIGHT_THRESHOLD) {
        pressedButton = INPUT_BUTTON_CLOCKWISE;
    } else {
        pressedButton = INPUT_NONE;
    }

    isLimitSwitchPressed = digitalRead(PIN_LIMIT_SWITCH);
}
void respondToButtonInputs() {
    // move logic inside common move_curtain function
    if (isLimitSwitchPressed || !(digitalRead(PIN_ENABLE) || pressedButton != INPUT_NONE) && webOverride == OVERRIDE_NONE) {
        motor.disable();
    } else if (digitalRead(PIN_ENABLE)) {
        if (pressedButton == INPUT_BUTTON_CLOCKWISE) {
            motor.clockwise();
        } else if (pressedButton == INPUT_BUTTON_ANTI_CLOCKWISE) {
            motor.antiClockwise();
        }

        if (pressedButton != INPUT_NONE) {
            motor.enable();
            motor.takeSteps();
            motor.disable();
        }
    }
}

void processWebControls() {
    // move logic inside common move_curtain function
    switch (webOverride) {
        case OVERRIDE_OPEN_CURTAIN:
            WebSerial.println("opening -- DO NOT UPDATE THE CODE RIGHT NOW");
            motor.enable();
            motor.takeSteps(200 * 15);
            motor.disable();
            isCurtainClosed = false;
            webOverride = OVERRIDE_NONE;
            WebSerial.print("SAFE TO UPDATE -- isCurtainClosed: ");
            WebSerial.println(isCurtainClosed);
            break;
        case OVERRIDE_CLOSE_CURTAIN:
            WebSerial.println("closing -- DO NOT UPDATE THE CODE RIGHT NOW");
            motor.enable();
            motor.takeSteps(200 * 15);
            motor.disable();
            isCurtainClosed = true;
            webOverride = OVERRIDE_NONE;
            WebSerial.print("SAFE TO UPDATE -- isCurtainClosed: ");
            WebSerial.println(isCurtainClosed);
            break;
    }
}

void setLEDState() {
    if (!digitalRead(PIN_ENABLE)) {
        digitalWrite(PIN_LED, HIGH);
    } else {
        digitalWrite(PIN_LED, LOW);
    }
}

void setup() {
    setCpuFrequencyMhz(80);
    Serial.begin(115200);

    setupPins();
    setupWiFi();
    createServerEndpoints();
    enableWebSerial();
    beginOnlineServer();

    WebSerial.print("Running ");
    WebSerial.println(__FILE__);
}

void loop() {
    ArduinoOTA.handle();
    readButtonInputs();
    respondToButtonInputs();

    processWebControls();

    setLEDState();
}
